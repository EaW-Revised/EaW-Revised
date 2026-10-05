#include "combat_algorithms.hpp"

namespace eawr::sim::tactical {
using detail::combat_detail::one_raw;

bool players_hostile(const std::span<const SnapshotPlayer> players, const PlayerId owner, const PlayerId target) noexcept {
    const auto find = [&](const PlayerId id) -> const SnapshotPlayer* {
        const auto entry = std::lower_bound(players.begin(), players.end(), id,
            [](const SnapshotPlayer& player, const PlayerId value) { return player.player_id < value; });
        return entry != players.end() && entry->player_id == id ? &*entry : nullptr;
    };
    const auto* source = find(owner);
    const auto* recipient = find(target);
    return source != nullptr && recipient != nullptr && !source->neutral && !recipient->neutral
        && source->team_id != recipient->team_id;
}
OpportunityOutcome service_opportunity(
    const std::uint64_t frame, const OpportunityGates& gates, OpportunityState& state, OpportunityWorld& world) {
    OpportunityOutcome outcome;
    if (!gates.admitted) {
        return outcome; // R-01: no work, the reference stays
    }
    bool just_cleared = false;
    if (state.target != invalid_entity_id) {
        if (world.attempt(state.target, true)) {
            outcome.fired = true; // R-02: kept, no scan, no event
            return outcome;
        }
        state.target = invalid_entity_id; // R-03
        just_cleared = true;
    }
    // R-04: strictly more than trunc(fps * 0.5) frames since the last scan.
    const auto interval = static_cast<std::uint64_t>(gates.logical_fps / 2U);
    const bool due = frame >= state.last_scan_frame && frame - state.last_scan_frame > interval;
    if (!just_cleared && !due) {
        return outcome;
    }
    state.last_scan_frame = frame;
    if (gates.parent_suppressed) {
        return outcome; // R-05: before the player-start draw
    }
    const auto count = world.player_count();
    if (count == 0) {
        return outcome;
    }
    auto index = static_cast<std::size_t>(world.draw_player_start(static_cast<std::uint32_t>(count)));
    outcome.player_start_draws = 1;
    EntityId best = invalid_entity_id;
    math::Fixed best_priority{};
    bool terminal = false;
    for (std::size_t visit = 0; visit < count && !terminal; ++visit) {
        const auto relation = world.relation(index);
        // R-05, R-06: an absent slot and a nebula-fogged player spend the visit without advancing.
        if (relation == PlayerRelation::absent || world.nebula_fogged(index)) {
            continue;
        }
        if (relation == PlayerRelation::hostile) {
            for (const auto& candidate : world.candidates(index)) {
                // R-08: suitability, then the turret's pointing test, then the rest, then priority.
                if (!candidate.suitable || !candidate.pointable || !candidate.eligible || !candidate.priority) {
                    continue;
                }
                // R-09: lower wins, a tie keeps the first.
                if (best != invalid_entity_id && !(*candidate.priority < best_priority)) {
                    continue;
                }
                if (!world.aim_acceptable(candidate) || candidate.opportunity_fire_disabled) {
                    continue;
                }
                best = candidate.id;
                best_priority = *candidate.priority;
                if (best_priority.raw() == one_raw) {
                    terminal = true;
                    break;
                }
            }
        }
        index = (index + 1U) % count;
    }
    if (best == invalid_entity_id) {
        return outcome; // R-14
    }
    // R-12: tried at once; an event only when that attempt succeeds.
    state.target = best;
    if (world.attempt(best, false)) {
        outcome.fired = true;
        outcome.acquired = true;
    } else {
        state.target = invalid_entity_id;
    }
    return outcome;
}


namespace detail {
using namespace combat_detail;

math::Fixed target_soft_radius(const MotionTable* motion, const CombatUnit& target) noexcept {
    if (const auto* footprint = motion != nullptr ? motion->footprint(target.type_id) : nullptr) return footprint->radius;
    if (target.profile == nullptr || !target.profile->collision) return {};
    const auto& box = *target.profile->collision;
    return math::Fixed::from_raw(std::max(box.max.x.raw() - box.min.x.raw(), box.max.y.raw() - box.min.y.raw()) / 2);
}

bool has_aim_hardpoint(const CombatUnit& target) noexcept {
    if (target.profile == nullptr) return false;
    for (const auto& hardpoint : target.profile->hardpoints) {
        if (!hardpoint.targetable) continue;
        if (target.durability_profile != nullptr && target.durability != nullptr
            && hardpoint.hardpoint < target.durability_profile->hardpoints.size()
            && hardpoint_destroyed(*target.durability_profile, *target.durability, hardpoint.hardpoint)) continue;
        return true;
    }
    return false;
}

core::Result<math::Fixed> target_attack_distance(const MotionTable* motion, const CombatUnit& target,
    const math::Vec3 from, const math::Fixed distance, const bool hardpoint_aim) {
    motion_detail::Calc calc;
    auto extension = target_soft_radius(motion, target);
    if (!hardpoint_aim) {
        const auto* footprint = motion != nullptr ? motion->footprint(target.type_id) : nullptr;
        math::Fixed x{}, y{};
        if (footprint != nullptr) {
            x = footprint->x_extent;
            y = footprint->y_extent;
        } else if (target.profile != nullptr && target.profile->collision) {
            const auto& box = *target.profile->collision;
            x = math::Fixed::from_raw((box.max.x.raw() - box.min.x.raw()) / 2);
            y = math::Fixed::from_raw((box.max.y.raw() - box.min.y.raw()) / 2);
        }
        const auto dx = calc.sub(target.position.x, from.x);
        const auto dy = calc.sub(target.position.y, from.y);
        auto fx = target.transform.rows[0][0];
        const auto fy = target.transform.rows[1][0];
        if (fx.raw() == 0 && fy.raw() == 0) fx = math::Fixed::from_raw(one_raw);
        auto cosine = math::Fixed{};
        const auto length = calc.length(dx, dy);
        if (length.raw() != 0) cosine = calc.div(calc.dot(dx, dy, fx, fy), calc.mul(length, calc.length(fx, fy)));
        // AT-10: a centre aim uses the target's X extent along its bow/stern, Y at its beam.
        extension = calc.abs(cosine).raw() > one_raw * 7071 / 10000 ? x : y;
    }
    const auto reach = calc.add(distance, extension);
    if (!calc.ok()) return core::Result<math::Fixed>::failure(calc.error("target attack distance"));
    return core::Result<math::Fixed>::success(reach);
}

const CombatUnit* CombatWorld::find(const EntityId id) const noexcept {
    const auto found = std::lower_bound(
        units.begin(), units.end(), id, [](const CombatUnit& unit, const EntityId value) { return unit.id < value; });
    return found != units.end() && found->id == id ? &*found : nullptr;
}

EntityId CombatWorld::container_of(const EntityId unit) const noexcept {
    if (craft_containers != nullptr) {
        const auto found = craft_containers->find(unit);
        if (found != craft_containers->end()) return found->second;
    }
    // Worlds without flight profiles can still contain teams; retain their legacy lookup.
    for (const auto& [container, members] : teams) {
        if (std::find(members.begin(), members.end(), unit) != members.end()) return container;
    }
    return invalid_entity_id;
}

const CombatUnit* CombatWorld::resolve_target(const CombatUnit* target, const math::Vec3& from) const {
    if (target == nullptr) return nullptr;
    const auto team = std::lower_bound(teams.begin(), teams.end(), target->id,
        [](const std::pair<EntityId, std::vector<EntityId>>& entry, const EntityId value) { return entry.first < value; });
    if (team == teams.end() || team->first != target->id) return target;
    // FO-04: the smallest squared distance, the first in team order on a tie.
    const auto squared = [&from](const math::Vec3& point) {
        math::detail::UInt192 total{};
        for (const auto& [a, b] : {std::pair{from.x.raw(), point.x.raw()}, std::pair{from.y.raw(), point.y.raw()},
                 std::pair{from.z.raw(), point.z.raw()}}) {
            const auto d = a >= b ? static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)
                                  : static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
            static_cast<void>(math::detail::add_magnitude(total, math::detail::multiply_u64(d, d)));
        }
        return total;
    };
    const CombatUnit* nearest = nullptr;
    math::detail::UInt192 nearest_distance{};
    for (const auto member : team->second) {
        const auto* craft = find(member);
        if (craft == nullptr) continue;
        const auto distance = squared(craft->position);
        if (nearest == nullptr || math::detail::compare(distance, nearest_distance) < 0) {
            nearest = craft;
            nearest_distance = distance;
        }
    }
    return nearest;
}

std::vector<EntityId> CombatWorld::candidates(const math::Vec3& centre, const math::Fixed half, const PlayerId owner) const {
    if (collection == nullptr) return index.box(centre, math::Vec3{half, half, half}, owner);
    const auto low = [&](const math::Fixed value) { return math::Fixed::from_raw(value.raw() - half.raw()); };
    const auto high = [&](const math::Fixed value) { return math::Fixed::from_raw(value.raw() + half.raw()); };
    return collection->collect(owner, CullBox{{low(centre.x), low(centre.y), low(centre.z)},
        {high(centre.x), high(centre.y), high(centre.z)}});
}


namespace combat_detail {

CombatState UnitCombat::target_state(const math::Fixed divert, const EntityId formation_target,
    const math::Vec3 diversion_anchor, const FormationAttackContext* formation) {
    formation_context_ = formation;
    scan_divert_ = formation == nullptr || (formation->present && formation->allows_divert) ? divert : math::Fixed{};
    diversion_anchor_ = diversion_anchor;
    formation_target_ = formation_target;
    // WSQ-41 reads the base formation destination; WSQ-45/46 use its active override.
    // The caller already shares a player attack. An autonomous override over an idle/guard
    // base is not a base attack and must not be promoted to player-direct here.
    ship_target();
    return std::move(state_);
}

[[nodiscard]] math::Fixed UnitCombat::member_attack_reach(const CombatUnit& target, const math::Vec3& from) {
    return take(target_attack_distance(world_.motion, target, from, *profile_.max_attack_distance, false));
}

[[nodiscard]] bool UnitCombat::in_attack_range(const CombatUnit& target) {
        if (!profile_.max_attack_distance) return false;
        if (within_range(unit_.position, target.position, *profile_.max_attack_distance, RangeMetric::planar)) return true;
        return within_range(unit_.position, target.position, member_attack_reach(target, unit_.position), RangeMetric::planar);
    }

[[nodiscard]] UnitCombat::Suitability UnitCombat::ship_suitable(const CombatUnit& target) {
        Suitability result;
        if (target.profile == nullptr || !hostile(target) || !visible_to_me(target)) return result;
        bool armed = false;
        for (const auto& weapon : profile_.weapons) {
            if (weapon_up(weapon) && !restricted(weapon, target)) {
                armed = true;
                break;
            }
        }
        if (!armed) return result;
        result.priority = priority(target);
        if (!result.priority) return result;
        // WSQ-42/45: the member-centred collection box only supplies candidates. A held
        // formation target, own attack reach, or diversion from the formation's destination
        // admits a candidate; guard's destination is the guarded unit, not the scanning craft.
        const bool formation_held = (formation_context_ == nullptr || formation_context_->present)
            && formation_target_ != invalid_entity_id
            && target.id == formation_target_;
        const bool own_held = (formation_context_ == nullptr || !formation_context_->present)
            && target.id == state_.attack_target;
        result.suitable = own_held || formation_held || in_attack_range(target);
        if (!result.suitable && diversion_anchor_ && profile_.max_attack_distance && formation_context_ != nullptr
            && formation_context_->present && formation_context_->allows_divert) {
            if (formation_context_->unbounded || within_range(*diversion_anchor_, target.position, scan_divert_, RangeMetric::planar)) {
                result.suitable = true;
            } else {
                // WMV-17: outside the allowance, test attack reach from its circle toward the
                // candidate, using the team's leader rather than the scanning member's type.
                const auto direction = take(math::normalize(math::Vec2{
                    take(math::subtract(target.position.x, diversion_anchor_->x)),
                    take(math::subtract(target.position.y, diversion_anchor_->y))}));
                const math::Vec3 edge{take(math::add(diversion_anchor_->x, take(math::multiply(direction.x, scan_divert_)))),
                    take(math::add(diversion_anchor_->y, take(math::multiply(direction.y, scan_divert_)))), diversion_anchor_->z};
                auto reach = member_attack_reach(target, edge);
                const auto* leader = formation_context_->leader;
                if (leader != nullptr && leader->profile != nullptr && leader->profile->max_attack_distance) {
                    reach = math::Fixed::from_raw(reach.raw() - profile_.max_attack_distance->raw()
                        + leader->profile->max_attack_distance->raw());
                    result.suitable = within_range(edge, target.position, reach, RangeMetric::planar);
                }
            }
        } else if (!result.suitable && diversion_anchor_ && profile_.max_attack_distance && formation_context_ == nullptr) {
            const auto reach = math::Fixed::from_raw(profile_.max_attack_distance->raw() + scan_divert_.raw());
            result.suitable = within_range(*diversion_anchor_, target.position, reach, RangeMetric::planar)
                || within_range(*diversion_anchor_, target.position,
                    math::Fixed::from_raw(member_attack_reach(target, *diversion_anchor_).raw() + scan_divert_.raw()), RangeMetric::planar);
        }
        return result;
    }

[[nodiscard]] bool UnitCombat::damaged(const CombatUnit& target) {
        if (target.durability_profile == nullptr || target.durability == nullptr) return false;
        // hull / max hull <= Health_Low_Percent_Threshold, exactly: hull <= fraction x max hull.
        const auto limit = take(math::multiply(world_.rules->damaged_fraction, target.durability_profile->max_hull));
        return target.durability->hull <= limit;
    }

[[nodiscard]] bool UnitCombat::better(const CombatUnit& candidate, const std::optional<math::Fixed> candidate_priority,
        const CombatUnit* current, const std::optional<math::Fixed> current_priority) {
        if (!candidate_priority) return false;
        if (!current_priority || current == nullptr) return true;
        if (state_.direct && current->id == state_.attack_target) return false;
        const auto current_damaged = damaged(*current);
        if (current_damaged != damaged(candidate)) return !current_damaged;
        if (*current_priority > *candidate_priority) return true;
        for (const auto& weapon : profile_.weapons) {
            if (weapon.hardpoint != object_weapon) continue;
            const bool current_hit = !restricted(weapon, *current);
            if (current_hit != !restricted(weapon, candidate)) return !current_hit;
        }
        return math::detail::compare(squared(unit_.position, candidate.position, RangeMetric::planar),
                   squared(unit_.position, current->position, RangeMetric::planar)) < 0;
    }

[[nodiscard]] std::pair<const CombatUnit*, std::optional<math::Fixed>> UnitCombat::scan() {
        if (world_.frame < state_.next_scan_frame) return {nullptr, std::nullopt};
        CombatRandom random(world_.seed, world_.frame, unit_.id, ship_slot);
        state_.next_scan_frame = world_.frame + logical_frames_per_second
            + random.uniform(0, logical_frames_per_second / 2U);
        if (!profile_.max_attack_distance || profile_.max_attack_distance->raw() <= 0) return {nullptr, std::nullopt};
        // WSQ-42: a member's formation contributes the diversion allowance to its own scan.
        const auto range = math::Fixed::from_raw(profile_.max_attack_distance->raw() + scan_divert_.raw());
        const auto count = world_.players.size();
        auto index = static_cast<std::size_t>(random.uniform(0, static_cast<std::uint32_t>(count - 1U)));
        const CombatUnit* found = nullptr;
        std::optional<math::Fixed> carried;
        for (std::size_t visit = 0; visit < count; ++visit) {
            const auto& player = world_.players[index];
            index = (index + 1U) % count;
            if (!world_.hostile(unit_.owner, player.player_id)) continue;
            const CombatUnit* best = nullptr;
            bool stop = false;
            for (const auto id : world_.candidates(unit_.position, range, player.player_id)) {
                const auto* candidate = world_.find(id);
                if (candidate == nullptr) continue;
                const auto suitability = ship_suitable(*candidate);
                if (!suitability.suitable || !better(*candidate, suitability.priority, best, carried)) continue;
                best = candidate;
                carried = suitability.priority;
                if (candidate->id == state_.attack_target && carried->raw() == one_raw) {
                    stop = true;
                    break;
                }
            }
            if (best != nullptr) found = best;
            if (stop) break;
        }
        return {found, carried};
    }

void UnitCombat::ship_target() {
        const CombatUnit* current = nullptr;
        if (state_.attack_target != invalid_entity_id) {
            // WSQ-45: suitability and replacement read the held object. FO-04's nearest
            // live member is resolved separately when the weapon aims and fires.
            current = world_.find(state_.attack_target);
            if (current == nullptr || !hostile(*current) || !visible_to_me(*current)) {
                state_.attack_target = invalid_entity_id;
                state_.direct = false;
            }
        }
        // FT-07: an idle craft holds fire; target-only member preparation bypasses this gate.
        if (unit_.squadron_idle) {
            state_.attack_target = invalid_entity_id;
            state_.direct = false;
            return;
        }
        // WSQ-42/43: squadron members were prepared and shared before the firing phase.
        // Keep FT-07's clear above: a move/stop can remove the team's target between scans.
        if (unit_.target_prepared) return;
        if (!profile_.max_attack_distance) return;
        if (state_.attack_target == invalid_entity_id) {
            const auto [found, found_priority] = scan();
            static_cast<void>(found_priority);
            if (found != nullptr) {
                state_.attack_target = found->id;
                state_.direct = false;
            }
            return;
        }
        if (state_.direct) return;
        const auto current_state = ship_suitable(*current);
        if (current_state.suitable && current_state.priority && current_state.priority->raw() == one_raw) return;
        const auto [found, found_priority] = scan();
        if (found == nullptr || found->id == current->id) return;
        if (current_state.suitable && !better(*found, found_priority, current, current_state.priority)) return;
        state_.attack_target = found->id;
    }

} // namespace combat_detail

CombatState target_combat(const CombatWorld& world, const CombatUnit& unit, const math::Fixed divert,
    const EntityId formation_target, const math::Vec3 diversion_anchor, const FormationAttackContext* formation) {
    if (unit.profile == nullptr || unit.combat == nullptr) return unit.combat != nullptr ? *unit.combat : CombatState{};
    return combat_detail::UnitCombat(world, unit).target_state(divert, formation_target, diversion_anchor, formation);
}

} // namespace detail
} // namespace eawr::sim::tactical
