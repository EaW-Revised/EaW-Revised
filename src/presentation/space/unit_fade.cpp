#include "eawr/presentation/space/unit_fade.hpp"

#include "eawr/sim/tactical/types.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace eawr::presentation::space {
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

void NebulaBlend::service(const bool contact) {
    ease(value_, velocity_, contact_ ? 1.0 : 0.0, 0.15,
        1.0 / static_cast<double>(sim::tactical::logical_frames_per_second));
    // A cached positive service settles the visual immediately after its first entry frame.
    if (contact && contact_) { value_ = 1.0; velocity_ = 0.0; }
    contact_ = contact;
}

void UnitFade::advance(const std::span<const sim::EntityId> visible, const std::span<const sim::EntityId> alive,
                       const double frames, const std::span<const sim::EntityId> immediate) {
    // Both spans are ascending IDs (the snapshot's own contract); grow the lifetime set with
    // whatever `alive` has not held before, remembering which entities were new to it.
    std::vector<sim::EntityId> first_sight;
    for (const sim::EntityId entity : alive) {
        if (!std::binary_search(ever_seen_.begin(), ever_seen_.end(), entity)) first_sight.push_back(entity);
    }
    if (!first_sight.empty()) {
        ever_seen_.insert(ever_seen_.end(), first_sight.begin(), first_sight.end());
        std::sort(ever_seen_.begin(), ever_seen_.end());
    }

    // FW-18/FW-21: a unit the session no longer holds is forgotten at once, whatever its opacity.
    for (auto entry = fading_.begin(); entry != fading_.end();) {
        entry = std::binary_search(alive.begin(), alive.end(), entry->first) ? std::next(entry) : fading_.erase(entry);
    }

    // FW-17: a unit new to `alive` this call starts at full opacity when shown (the battle's
    // opening roster, or one born since); one `alive` already held before starts a real ease.
    for (const sim::EntityId entity : visible) {
        auto entry = fading_.find(entity);
        if (entry != fading_.end()) {
            entry->second.target_visible = true;
            continue;
        }
        const bool spawned_now = std::binary_search(first_sight.begin(), first_sight.end(), entity)
            || std::binary_search(immediate.begin(), immediate.end(), entity);
        fading_.emplace(entity, State{spawned_now ? 1.0 : 0.0, 0.0, true});
    }
    // FW-17: every other tracked unit starts or continues easing toward hidden.
    for (auto& [entity, state] : fading_) {
        if (!std::binary_search(visible.begin(), visible.end(), entity)) state.target_visible = false;
    }

    // Aggregate whole logical-frame steps, then apply at most one fractional step.
    // A stall never causes a loop proportional to the number of missed frames.
    const double elapsed = std::isfinite(frames) ? std::max(0.0, frames) : 0.0;
    const double whole = std::floor(elapsed);
    const double fraction = elapsed - whole;
    const double frame_seconds = 1.0 / static_cast<double>(sim::tactical::logical_frames_per_second);
    for (auto& [entity, state] : fading_) {
        const double target = state.target_visible ? 1.0 : 0.0;
        // WSU-05 / FW-25: tagged models jump; their dim copy is a separate draw.
        if (std::binary_search(immediate.begin(), immediate.end(), entity)) {
            state.value = target;
            state.velocity = 0.0;
            continue;
        }
        ease(state.value, state.velocity, target, looks_.smooth_seconds, frame_seconds, whole);
        ease(state.value, state.velocity, target, looks_.smooth_seconds, fraction * frame_seconds);
    }
    for (auto& [entity, state] : fading_) state.value = std::clamp(state.value, 0.0, 1.0);

    // FW-18: fully faded out and still hidden; stop drawing and forget it.
    for (auto entry = fading_.begin(); entry != fading_.end();) {
        const bool done = !entry->second.target_visible && entry->second.value <= looks_.hide_threshold;
        entry = done ? fading_.erase(entry) : std::next(entry);
    }
}

std::optional<float> UnitFade::opacity(const sim::EntityId entity) const noexcept {
    const auto entry = fading_.find(entity);
    if (entry == fading_.end()) return std::nullopt;
    return static_cast<float>(entry->second.value);
}

std::vector<sim::EntityId> UnitFade::drawn() const {
    std::vector<sim::EntityId> result;
    result.reserve(fading_.size());
    for (const auto& [entity, state] : fading_) result.push_back(entity);
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

} // namespace eawr::presentation::space
