#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <span>
#include <vector>

namespace eawr::sim::galactic::session_detail {
// WST-31: the original types of a fleet's members and of everything inside them (heroes in transports, squadron
// craft), sorted and unique. `children` maps a container to its immediate children. A visited set bounds the walk:
// a containment cycle (malformed or modded data) is walked once and reported through `cyclic`, never looped.
// `fleet` is skipped when it shows up as somebody's child.
[[nodiscard]] inline std::vector<TypeId> contained_types(std::span<const FleetMember> members,
    const std::map<EntityId, std::vector<EntityId>>& children, const std::map<EntityId, CensusObject>& records,
    const EntityId fleet, bool& cyclic) {
    std::vector<TypeId> types;
    std::set<EntityId> visited;
    std::vector<EntityId> pending;
    for (const auto& member : members) {
        types.push_back(member.type);
        if (visited.insert(member.id).second) pending.push_back(member.id); else cyclic = true;
    }
    while (!pending.empty()) {
        const auto parent = pending.back(); pending.pop_back();
        const auto found = children.find(parent);
        if (found == children.end()) continue;
        for (const auto child : found->second) {
            if (child == fleet) continue;
            if (!visited.insert(child).second) { cyclic = true; continue; }
            if (const auto record = records.find(child); record != records.end()) types.push_back(record->second.original_type);
            pending.push_back(child);
        }
    }
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    return types;
}
} // namespace eawr::sim::galactic::session_detail
