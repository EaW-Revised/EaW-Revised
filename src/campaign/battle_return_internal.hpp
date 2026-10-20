#pragma once

#include "eawr/campaign/conflict_policy.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace eawr::campaign {

// Both return paths supply their qualified station level and casualty list;
// transport survival remains qualified by the caller's retained roster.
[[nodiscard]] inline sim::galactic::ControlBattleReturn build_control_battle_return(
    const ConflictRecord& conflict, const BattleResult& result, const ConflictParties& parties,
    std::uint32_t station_level, std::vector<sim::EntityId> destroyed = {}) {
    sim::galactic::ControlBattleReturn returned;
    returned.planet = conflict.planet;
    returned.invader = parties.invader;
    returned.defender = parties.defender;
    returned.station_owner = parties.station_owner;
    returned.winner = result.winner;
    returned.land = conflict.layer == BattleLayer::land;
    returned.station_level = station_level;
    returned.destroyed = std::move(destroyed);
    return returned;
}

} // namespace eawr::campaign
