#pragma once

#include "eawr/sim/tactical/instance.hpp"
#include <cstdint>
#include <utility>

namespace eawr::presentation::godot_backend::ability_loop_audio {
using sim::tactical::AbilityKind;

[[nodiscard]] inline bool supported(const AbilityKind kind) noexcept {
    return kind == AbilityKind::leech_shields || kind == AbilityKind::tractor_beam
        || kind == AbilityKind::energy_weapon || kind == AbilityKind::super_laser;
}

// Resolve each ability's own authored event rather than sharing another kind's cue.
template <typename Read, typename Resolve>
[[nodiscard]] auto bind(const AbilityKind kind, Read read, Resolve resolve) -> decltype(resolve(read())) {
    if (!supported(kind)) return {};
    return resolve(read());
}

[[nodiscard]] inline bool playing(const sim::tactical::AbilityStatus& status,
                                  const std::uint64_t tick, const std::uint32_t fire_delay) noexcept {
    if (!status.active || !supported(status.kind) || status.target == sim::invalid_entity_id) return false;
    // WSPL-04: this snapshot's existing fire-delay seam begins the superlaser loop at firing.
    return status.kind != AbilityKind::super_laser
        || (tick >= status.started_tick && tick - status.started_tick >= fire_delay);
}
// One retained request per endpoint, even when admission declines a start.
template <typename ActiveMap, typename DesiredMap, typename Start, typename Stop>
void reconcile(ActiveMap& active, const DesiredMap& desired, Start start, Stop stop) {
    for (auto it = active.begin(); it != active.end();) {
        const auto next = desired.find(it->first);
        if (next != desired.end() && next->second == it->second.event) { ++it; continue; }
        stop(it->second);
        it = active.erase(it);
    }
    for (const auto& [id, cue] : desired) {
        if (active.contains(id)) continue;
        if (auto state = start(id, cue)) active.emplace(id, std::move(*state));
    }
}
} // namespace eawr::presentation::godot_backend::ability_loop_audio
