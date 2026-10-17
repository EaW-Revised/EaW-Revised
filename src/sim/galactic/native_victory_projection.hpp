#pragma once

#include "eawr/sim/galactic/victory.hpp"

#include <algorithm>

namespace eawr::sim::galactic::session_detail {
// WGS-24: the live owner and private read projection share leader membership.
inline bool update_native_victory_actor(VictoryState& victory, const CensusObject& actor) {
    auto& rows = victory.context.leaders;
    auto existing = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
        return !row.scheduled_return && row.object == actor.id;
    });
    const auto selected = std::find_if(victory.context.players.begin(), victory.context.players.end(),
        [&](const auto& row) { return row.player == actor.owner; });
    const auto* owner = selected == victory.context.players.end() ? nullptr : &*selected;
    const bool matches_leader = owner && owner->leader_company && owner->leader_unit_type
        && !owner->leader_unit.empty() && actor.current_type == owner->leader_unit_type;
    if (!matches_leader) {
        if (existing != rows.end()) { rows.erase(existing); return true; }
        return false;
    }
    VictoryLeader current{actor.owner, owner->leader_company, owner->leader_unit,
        false, actor.dead, false, actor.delete_pending, 0, actor.id};
    if (existing != rows.end()) {
        if (existing->owner == current.owner && existing->company == current.company) {
            current.killer = existing->killer;
            current.marked_for_death = existing->marked_for_death;
        }
        if (*existing != current) { *existing = std::move(current); return true; }
    } else {
        // Active census order is ascending persistent identity. Only sparse
        // births/current-type changes touch this projection, never a frame scan.
        const auto position = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
            return !row.scheduled_return && row.object > actor.id;
        });
        rows.insert(position, std::move(current)); return true;
    }
    return false;
}
inline bool remove_native_victory_actor(VictoryState& victory, EntityId object) {
    return std::erase_if(victory.context.leaders,
        [object](const auto& row) { return !row.scheduled_return && row.object == object; }) != 0;
}
} // namespace eawr::sim::galactic::session_detail
