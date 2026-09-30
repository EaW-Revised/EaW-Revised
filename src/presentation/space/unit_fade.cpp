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
void ease(double& value, double& velocity, const double target, const double smooth_seconds, const double dt) {
    if (dt <= 0.0 || smooth_seconds <= 0.0) return;
    const double omega = 2.0 / smooth_seconds;
    const double x = omega * dt;
    const double decay = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
    const double change = value - target;
    const double temp = (velocity + omega * change) * dt;
    velocity = (velocity - omega * temp) * decay;
    value = target + (change + temp) * decay;
}

} // namespace

UnitFade::UnitFade(const UnitFadeLooks looks) : looks_(looks) {}

void UnitFade::advance(const std::span<const sim::EntityId> visible, const std::span<const sim::EntityId> alive,
                       const double frames) {
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
        const bool spawned_now = std::binary_search(first_sight.begin(), first_sight.end(), entity);
        fading_.emplace(entity, State{spawned_now ? 1.0 : 0.0, 0.0, true});
    }
    // FW-17: every other tracked unit starts or continues easing toward hidden.
    for (auto& [entity, state] : fading_) {
        if (!std::binary_search(visible.begin(), visible.end(), entity)) state.target_visible = false;
    }

    // The ease is stepped in whole-frame increments (FoC's own service runs it once a real
    // frame): the closed-form step's cubic approximation of the exponential decay only holds
    // close to a single logical frame's span, so a stall or fast forward spanning many frames in
    // one advance() call must still walk through them one at a time, not take one large step.
    double remaining_frames = std::max(0.0, frames);
    while (remaining_frames > 0.0) {
        const double step_frames = std::min(remaining_frames, 1.0);
        const double dt = step_frames / static_cast<double>(sim::tactical::logical_frames_per_second);
        for (auto& [entity, state] : fading_) {
            ease(state.value, state.velocity, state.target_visible ? 1.0 : 0.0, looks_.smooth_seconds, dt);
        }
        remaining_frames -= step_frames;
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

} // namespace eawr::presentation::space
