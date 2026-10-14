#include "eawr/sim/tactical/fighters.hpp"

#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "../math/wide.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include "fighters_algorithms.hpp"

namespace eawr::sim::tactical {
using namespace fighters_detail;

namespace detail {

core::Result<SquadronTargetCommit> commit_squadron_target(const CombatWorld& world,
    const CombatUnit& unit, CombatState scanned, CombatState& member,
    const std::span<CombatState* const> roster, SquadronState& team,
    const Vec3 team_position, const Fixed divert, const Vec3 anchor,
    const FormationAttackContext& formation, const bool same_cell, const bool scanning) {
    SquadronTargetCommit result;
    const auto previous = member.attack_target;
    member = std::move(scanned);
    const auto parent = world.container_of(member.attack_target);
    const auto selected = parent != invalid_entity_id ? parent : member.attack_target;
    if (!scanning) {
        // Held, not found: the tail below retains or clears the member's own target.
    } else if (selected != invalid_entity_id) {
        // WSQ-43/47: a find is shared when it is not the team's held target, or when the member
        // had none; a member that already held it (an earlier tick's find, or a player's attack
        // adopted before the order) shares it to a team that does not.
        result.shared = selected != team.target || previous == invalid_entity_id;
        member.attack_target = selected;
        if (result.shared) {
            team.target = selected;
            team.target_hardpoint = member.direct ? member.attack_hardpoint : attack_hull;
            for (auto* recipient : roster) {
                recipient->attack_target = selected;
                recipient->attack_hardpoint = team.target_hardpoint;
                recipient->direct = member.direct;
            }
        }
        const auto destination = team.formation ? team.formation->attack_target : invalid_entity_id;
        result.notified = !member.direct && selected != destination;
        result.admitted = member.direct;
        if (result.notified) {
            if (const auto* target = world.find(selected)) {
                auto admitted = formation_can_attack(world, *target, divert, anchor, formation);
                if (!admitted) return core::Result<SquadronTargetCommit>::failure(admitted.error());
                result.admitted = admitted.value();
            }
        }
        if (result.admitted && team.formation) {
            auto& destination_state = *team.formation;
            if (selected != destination_state.attack_target) {
                // WMV-18: only an autonomous find splits the team's base destination; a player's
                // attack changes the attack destination alone. Both start the FA-07 approach.
                if (!member.direct) {
                    if (destination_state.complete && destination_state.base_target == invalid_entity_id)
                        destination_state.base_position = team_position;
                    destination_state.attack_override = true;
                    destination_state.complete = false;
                }
                team.approach = true;
                destination_state.attack_hardpoint = member.direct ? member.attack_hardpoint : attack_hull;
            }
            destination_state.attack_target = selected;
        }
    } else if (previous != invalid_entity_id && team.target == previous) {
        // WSQ-44 (debug build, EWSQ-26): a member that loses the team's held target (fog, stealth,
        // limbo) shares the loss: the team and every member still holding it let go, and the
        // formation's destination is cleared as an unmatched find would. The caller ends the combat.
        result.lost = true;
        team.target = invalid_entity_id;
        team.target_hardpoint = attack_hull;
        team.approach = false;
        for (auto* recipient : roster) {
            if (recipient->attack_target != previous) continue;
            recipient->attack_target = invalid_entity_id;
            recipient->attack_hardpoint = no_hardpoint;
            recipient->direct = false;
        }
        if (team.formation) {
            auto& held = *team.formation;
            if (held.attack_override) {
                held.attack_override = false;
                held.complete = held.has_reached_done;
            }
            held.attack_target = invalid_entity_id;
            held.attack_hardpoint = attack_hull;
        }
    }
    const auto destination = team.formation ? team.formation->attack_target : invalid_entity_id;
    auto tail = finish_squadron_target(world, unit, member, formation, destination, same_cell);
    if (!tail) return core::Result<SquadronTargetCommit>::failure(tail.error());
    member = std::move(tail).value();
    return core::Result<SquadronTargetCommit>::success(result);
}

namespace {

// Squared distance of two points as an exact wide integer of raw units (ties compare exactly).
[[nodiscard]] math::detail::UInt192 squared(const Vec3& left, const Vec3& right, const bool planar) {
    math::detail::UInt192 total = math::detail::from_u64(0);
    const auto term = [&total](const Fixed a, const Fixed b) {
        const auto magnitude = math::detail::unsigned_magnitude(a.raw() - b.raw());
        static_cast<void>(math::detail::add_magnitude(total, math::detail::multiply_u64(magnitude, magnitude)));
    };
    term(left.x, right.x);
    term(left.y, right.y);
    if (!planar) term(left.z, right.z);
    return total;
}

} // namespace

SquadronScan squadron_target(const CombatWorld& world, const SquadronState& state, const SquadronProfile& squadron,
    const CombatUnit& leader, const Vec3 anchor) {
    // WST-11: a stealth-invisible enemy is no squadron target, kept or acquired, unless the leader's type
    // targets stealth units: the same rule as ship targeting. It also ends a planned approach.
    const auto stealth_hidden = [&leader](const CombatUnit& unit) {
        return unit.team != leader.team && unit.stealth_invisible && !(leader.profile && leader.profile->target_stealth);
    };
    const auto visible = [&leader, &stealth_hidden](const CombatUnit& unit) {
        return !stealth_hidden(unit)
            && (unit.team == leader.team || (unit.visible_to & (std::uint64_t{1} << leader.player_index)) != 0U);
    };
    // FT-01: a live target the owner sees is kept, and so is a fogged one while the squadron still
    // flies the approach its attack planned (FA-07, #633).
    if (state.target != invalid_entity_id) {
        // #424: a squadron target (a player's attack order) is kept while one of its craft is.
        const auto* current = world.resolve_target(world.find(state.target), leader.position);
        if (current != nullptr && world.hostile(leader.owner, current->owner)
            && !stealth_hidden(*current) && (visible(*current) || state.approach)) return SquadronScan{state.target, state.next_scan_frame};
    }
    SquadronScan result{invalid_entity_id, state.next_scan_frame};
    if (world.frame < state.next_scan_frame || leader.profile == nullptr || !leader.profile->max_attack_distance) {
        return result;
    }
    // FT-02, FT-03: one scan a second (project cadence) within the chase range plus the leader's
    // attack distance of the anchor. FO-05, FO-06: a player attack-move or guard sets the range.
    result.next_scan_frame = world.frame + logical_frames_per_second;
    Fixed chase = squadron.idle_chase_range;
    if (state.diversion == SquadronDiversion::attack_move) {
        chase = squadron.attack_move_response_range;
    } else if (state.mode == SquadronMode::escort || state.diversion == SquadronDiversion::guard) {
        chase = squadron.guard_chase_range;
    }
    const auto reach_raw = static_cast<std::uint64_t>(chase.raw() + leader.profile->max_attack_distance->raw());
    const auto reach = math::detail::multiply_u64(reach_raw, reach_raw);
    const PrioritySet* set = leader.profile->priority_set ? &world.table->priority_sets[*leader.profile->priority_set] : nullptr;
    const CombatUnit* best = nullptr;
    Fixed best_priority;
    math::detail::UInt192 best_distance{};
    for (const auto& unit : world.units) {
        if (!world.hostile(leader.owner, unit.owner) || unit.durability_profile == nullptr || !visible(unit)) continue;
        if (math::detail::compare(squared(unit.position, anchor, true), reach) > 0) continue;
        std::optional<Fixed> priority = Fixed::from_raw(Fixed::scale);
        if (set != nullptr) {
            const auto found = std::lower_bound(set->rows.begin(), set->rows.end(), unit.type_id,
                [](const PriorityRow& row, const TypeId id) { return row.type_id < id; });
            priority = found != set->rows.end() && found->type_id == unit.type_id ? found->priority : set->unlisted;
        }
        if (!priority) continue;
        const auto distance = squared(unit.position, leader.position, false);
        if (best == nullptr || *priority < best_priority
            || (*priority == best_priority && math::detail::compare(distance, best_distance) < 0)) {
            best = &unit;
            best_priority = *priority;
            best_distance = distance;
        }
    }
    if (best != nullptr) result.target = best->id;
    return result;
}

} // namespace detail

} // namespace eawr::sim::tactical
