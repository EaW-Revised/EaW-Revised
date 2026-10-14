#pragma once

#include "combat_internal.hpp"
#include "eawr/sim/tactical/fighters.hpp"

#include <cstdint>

namespace eawr::sim::tactical::detail {

struct SquadronScan {
    EntityId target{};
    std::uint64_t next_scan_frame{};
};

// The squadron's attack target at world.frame (FT-01 to FT-04): the current one while it lives and
// its owner sees it; otherwise, on a scan frame, the best hostile unit by the leader's priority set
// within the chase range plus the leader's attack distance of `anchor`. Reads only `world`.
[[nodiscard]] SquadronScan squadron_target(const CombatWorld& world, const SquadronState& state,
    const SquadronProfile& squadron, const CombatUnit& leader, math::Vec3 anchor);

struct SquadronTargetCommit {
    bool shared{};
    bool notified{};
    bool admitted{};
    bool lost{}; // WSQ-44: the held team target was dropped; the caller ends the combat
};
// FS-42/FD-69: one rank-ordered commit of a previously staged member scan.
// Roster writes preserve clocks; the returned member state is its weapon input. A member
// whose team does not scan this frame (FO-01 move, FT-06 launch delay) shares and notifies
// nothing: only its targeting tail runs on the target it holds.
[[nodiscard]] core::Result<SquadronTargetCommit> commit_squadron_target(
    const CombatWorld& world, const CombatUnit& unit, CombatState scanned,
    CombatState& member, std::span<CombatState* const> roster, SquadronState& team,
    math::Vec3 team_position, math::Fixed divert, math::Vec3 anchor,
    const FormationAttackContext& formation, bool same_cell, bool scanning = true);

} // namespace eawr::sim::tactical::detail
