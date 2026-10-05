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
    const auto visible = [&leader](const CombatUnit& unit) {
        return unit.team == leader.team || (unit.visible_to & (std::uint64_t{1} << leader.player_index)) != 0U;
    };
    // FT-01: a live target the owner sees is kept, and so is a fogged one while the squadron still
    // flies the approach its attack planned (FA-07, #633).
    if (state.target != invalid_entity_id) {
        // #424: a squadron target (a player's attack order) is kept while one of its craft is.
        const auto* current = world.resolve_target(world.find(state.target), leader.position);
        if (current != nullptr && world.hostile(leader.owner, current->owner)
            && (visible(*current) || state.approach)) return SquadronScan{state.target, state.next_scan_frame};
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
