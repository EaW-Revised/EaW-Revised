#include "eawr/presentation/live/unit_fade.hpp"

#include "eawr/sim/tactical/types.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <span>

namespace eawr::presentation::live {
namespace {

// FW-16: a critically damped ease toward `target` with characteristic time `smooth_seconds`
// (no overshoot for a step target reached from rest), stepped by `dt` seconds. The closed-form
// coefficients are the standard third-order Pade approximation of the exponential decay; the
// state carries its own rate so a target flip before the ease settles keeps moving smoothly
// rather than restarting.
void ease(double& value, double& velocity, const double target, const double smooth_seconds,
          const double dt, const double steps = 1.0) {
    if (dt <= 0.0 || smooth_seconds <= 0.0 || steps <= 0.0) return;
    const double omega = 2.0 / smooth_seconds;
    const double x = omega * dt;
    const double one_decay = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
    const double decay = steps == 1.0 ? one_decay : std::pow(one_decay, steps);
    if (decay == 0.0) {
        value = target;
        velocity = 0.0;
        return;
    }
    const double change = value - target;
    // FW-16: the one-frame transition is decay * (I + B), with B*B = 0.
    // Repeating it N times gives decay^N * (I + N*B): constant catch-up work,
    // preserving the small-step curve even for a slow configurable ease.
    const double temp = (velocity + omega * change) * dt * steps;
    velocity = (velocity - omega * temp) * decay;
    value = target + (change + temp) * decay;
}

} // namespace

UnitFade::UnitFade(const UnitFadeLooks looks) : looks_(looks) {}

void ProjectileHide::advance(const double tick, const bool limbo, const bool immediate, const Query& visible) {
    if (limbo || !std::isfinite(tick) || (started_ && tick < last_tick_)) return;
    if (!started_) {
        started_ = true;
        target_ = visible && visible();
        value_ = target_ ? 1.0 : 0.0;
        last_tick_ = checked_tick_ = tick;
        return;
    }
    double frames = tick - last_tick_;
    if (frames == 0.0) return;
    last_tick_ = tick;
    // WPJ-38: the sourced settled boundary is an absolute difference < 0.0001.
    const bool transitioning = std::abs(value_ - (target_ ? 1.0 : 0.0)) >= 0.0001;
    // Settled objects poll every 30 logical frames; moving transitions service
    // every presented frame. Query work is bounded independently of catch-up.
    if (transitioning || tick - checked_tick_ >= 30.0) {
        target_ = visible && visible();
        checked_tick_ = tick;
    } else return;
    // A newly changed settled target starts this service, not 30 frames ago.
    if (!transitioning) frames = std::min(frames, 1.0);
    const double target = target_ ? 1.0 : 0.0;
    if (immediate) {
        value_ = target;
        velocity_ = 0.0;
        return;
    }
    // WPJ-39 (debug build EUS-02): the hide service shares the unit presentation
    // ease with a quarter-second smoothing time and initially zero velocity.
    const double seconds = 1.0 / static_cast<double>(sim::tactical::logical_frames_per_second);
    ease(value_, velocity_, target, UnitFadeLooks{}.smooth_seconds, seconds, std::floor(frames));
    ease(value_, velocity_, target, UnitFadeLooks{}.smooth_seconds, seconds * (frames - std::floor(frames)));
    value_ = std::clamp(value_, 0.0, 1.0);
}

void NebulaBlend::service(const bool contact) {
    ease(value_, velocity_, contact_ ? 1.0 : 0.0, 0.15,
        1.0 / static_cast<double>(sim::tactical::logical_frames_per_second));
    // A cached positive service settles the visual immediately after its first entry frame.
    if (contact && contact_) { value_ = 1.0; velocity_ = 0.0; }
    contact_ = contact;
}

void StealthBlend::service(const bool on, const double transition_seconds) {
    const double target = on ? 1.0 : 0.0;
    if (transition_seconds <= 0.0) { value_ = target; velocity_ = 0.0; return; }
    ease(value_, velocity_, target, transition_seconds,
        1.0 / static_cast<double>(sim::tactical::logical_frames_per_second));
}

namespace {

// The inputs of UnitFade::advance are strictly ascending by contract. A caller that breaks it gets the
// normalised (sorted, unique) copy, so the walks below decide as a sorted input would.
std::span<const sim::EntityId> ascending(const std::span<const sim::EntityId> ids, std::vector<sim::EntityId>& scratch) {
    bool ordered = true;
    for (std::size_t index = 1; index < ids.size() && ordered; ++index) ordered = ids[index - 1] < ids[index];
    if (ordered) return ids;
    scratch.assign(ids.begin(), ids.end());
    std::sort(scratch.begin(), scratch.end());
    scratch.erase(std::unique(scratch.begin(), scratch.end()), scratch.end());
    return scratch;
}

// Moves `cursor` to the first id not below `entity`; true when it stands on `entity`.
bool reaches(const std::span<const sim::EntityId> ids, std::size_t& cursor, const sim::EntityId entity) {
    while (cursor < ids.size() && ids[cursor] < entity) ++cursor;
    return cursor < ids.size() && ids[cursor] == entity;
}

} // namespace

void UnitFade::advance(const std::span<const sim::EntityId> visible_in, const std::span<const sim::EntityId> alive_in,
                       const double frames, const std::span<const sim::EntityId> immediate_in) {
    // The spans are ascending IDs (the snapshot's own contract); everything below is a merge walk over them and
    // the ascending entries, each entry independent of the others, so the result equals a keyed lookup per entity.
    const std::span<const sim::EntityId> visible = ascending(visible_in, sorted_visible_);
    const std::span<const sim::EntityId> alive = ascending(alive_in, sorted_alive_);
    const std::span<const sim::EntityId> immediate = ascending(immediate_in, sorted_immediate_);

    // Grow the lifetime set with whatever `alive` has not held before, remembering which entities were new to it.
    first_sight_.clear();
    {
        std::size_t seen = 0;
        for (const sim::EntityId entity : alive) {
            if (!reaches(ever_seen_, seen, entity)) first_sight_.push_back(entity);
        }
    }
    if (!first_sight_.empty()) {
        ever_seen_.insert(ever_seen_.end(), first_sight_.begin(), first_sight_.end());
        std::inplace_merge(ever_seen_.begin(), ever_seen_.end() - static_cast<std::ptrdiff_t>(first_sight_.size()), ever_seen_.end());
    }

    // Aggregate whole logical-frame steps, then apply at most one fractional step.
    // A stall never causes a loop proportional to the number of missed frames.
    const double elapsed = std::isfinite(frames) ? std::max(0.0, frames) : 0.0;
    const double whole = std::floor(elapsed);
    const double fraction = elapsed - whole;
    const double frame_seconds = 1.0 / static_cast<double>(sim::tactical::logical_frames_per_second);

    next_.clear();
    std::size_t old_at = 0, visible_at = 0, alive_at = 0, first_at = 0, immediate_at = 0;
    sim::EntityId previous_visible = 0;
    bool any_visible = false;
    // Walk the union of the tracked entries and the visible ids in ascending order.
    while (old_at < fading_.size() || visible_at < visible.size()) {
        const bool have_old = old_at < fading_.size();
        const bool have_visible = visible_at < visible.size();
        const sim::EntityId entity = have_old && (!have_visible || fading_[old_at].entity <= visible[visible_at])
            ? fading_[old_at].entity : visible[visible_at];
        const bool is_old = have_old && fading_[old_at].entity == entity;
        bool is_visible = false;
        while (visible_at < visible.size() && visible[visible_at] <= entity) {
            if (visible[visible_at] == entity && !(any_visible && previous_visible == entity)) is_visible = true;
            previous_visible = visible[visible_at];
            any_visible = true;
            ++visible_at;
        }
        // FW-18/FW-21: a unit the session no longer holds is forgotten at once, whatever its opacity.
        const bool is_alive = reaches(alive, alive_at, entity);
        State state;
        if (is_old && is_alive) {
            state = fading_[old_at].state;
            state.target_visible = is_visible; // FW-17: a shown unit eases up, every other tracked unit eases toward hidden
        } else if (is_visible) {
            // FW-17: a unit new to `alive` this call starts at full opacity when shown; one `alive` already held starts a real ease.
            const bool spawned_now = reaches(first_sight_, first_at, entity) || reaches(immediate, immediate_at, entity);
            state = State{spawned_now ? 1.0 : 0.0, 0.0, true};
        } else {
            if (is_old) ++old_at;
            continue;
        }
        if (is_old) ++old_at;
        const double target = state.target_visible ? 1.0 : 0.0;
        // WSU-05 / FW-25: tagged models jump; their dim copy is a separate draw.
        if (reaches(immediate, immediate_at, entity)) {
            state.value = target;
            state.velocity = 0.0;
        } else {
            ease(state.value, state.velocity, target, looks_.smooth_seconds, frame_seconds, whole);
            ease(state.value, state.velocity, target, looks_.smooth_seconds, fraction * frame_seconds);
        }
        state.value = std::clamp(state.value, 0.0, 1.0);
        // FW-18: fully faded out and still hidden; stop drawing and forget it.
        if (!state.target_visible && state.value <= looks_.hide_threshold) continue;
        next_.push_back({entity, state});
    }
    fading_.swap(next_);
}

std::optional<float> UnitFade::opacity(const sim::EntityId entity) const noexcept {
    const auto entry = std::lower_bound(fading_.begin(), fading_.end(), entity,
        [](const Entry& candidate, const sim::EntityId id) { return candidate.entity < id; });
    if (entry == fading_.end() || entry->entity != entity) return std::nullopt;
    return static_cast<float>(entry->state.value);
}

std::optional<float> UnitFade::opacity_walk(const sim::EntityId entity, std::size_t& cursor) const noexcept {
    if (cursor > fading_.size() || (cursor > 0 && fading_[cursor - 1].entity >= entity)) {
        cursor = static_cast<std::size_t>(std::lower_bound(fading_.begin(), fading_.end(), entity,
            [](const Entry& candidate, const sim::EntityId id) { return candidate.entity < id; }) - fading_.begin());
    }
    while (cursor < fading_.size() && fading_[cursor].entity < entity) ++cursor;
    if (cursor == fading_.size() || fading_[cursor].entity != entity) return std::nullopt;
    return static_cast<float>(fading_[cursor].state.value);
}

std::vector<sim::EntityId> UnitFade::drawn() const {
    std::vector<sim::EntityId> result;
    result.reserve(fading_.size());
    for (const Entry& entry : fading_) result.push_back(entry.entity);
    return result;
}

std::vector<UnitFade::Row> UnitFade::rows() const {
    std::vector<Row> result;
    result.reserve(fading_.size());
    for (const Entry& entry : fading_) result.push_back({entry.entity, entry.state.value, entry.state.velocity, entry.state.target_visible});
    return result;
}

void FogGhosts::advance(const std::span<const Observation> observations, const RevealedPoint& revealed) {
    // Input IDs are ascending, just like the immutable tactical snapshot.
    for (const Observation& observation : observations) {
        auto [entry, inserted] = states_.try_emplace(observation.entity);
        State& state = entry->second;
        if (inserted) {
            state.position = observation.position;
            state.known = observation.initially_known;
        }
        if (observation.visible) {
            state.position = observation.position;
            state.known = true;
        }
        state.ghost = state.known && !observation.visible;
    }
    for (auto entry = states_.begin(); entry != states_.end();) {
        const auto alive = std::lower_bound(observations.begin(), observations.end(), entry->first,
            [](const Observation& observation, const sim::EntityId id) { return observation.entity < id; });
        if (alive != observations.end() && alive->entity == entry->first) { ++entry; continue; }
        // FW-27: a dead object's copy survives until sight returns to its
        // saved sample point. No hidden destruction changes its appearance.
        if (!entry->second.known || !entry->second.ghost
            || (revealed && revealed(entry->second.position))) {
            entry = states_.erase(entry);
        } else {
            ++entry;
        }
    }
}

} // namespace eawr::presentation::live
