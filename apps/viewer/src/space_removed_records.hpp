#pragma once

#include "eawr/battle/start.hpp"
#include "eawr/scene/space_population.hpp"

#include <algorithm>
#include <cstdint>

namespace eawr::presentation::godot_backend {

// A removed map record has no simulation object and must not be presented.
[[nodiscard]] inline bool removed_space_record(const battle::BattleStart* start, std::uint32_t record) {
    return start && std::any_of(start->removed.begin(), start->removed.end(),
        [record](const battle::RemovedObject& removed) { return removed.record == record; });
}

inline bool suppress_removed_space_placement(const battle::BattleStart* start, std::uint32_t record,
                                             scene::SpacePlacementDecision& decision) {
    if (!removed_space_record(start, record)) return false;
    decision.role = scene::SpaceRole::not_drawable;
    decision.drawn_surfaces.clear();
    decision.hardpoints.clear();
    decision.damage_decals.clear();
    decision.missing = {"removed at battle start"};
    return true;
}

} // namespace eawr::presentation::godot_backend
