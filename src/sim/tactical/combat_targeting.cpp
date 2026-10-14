#include "combat_algorithms.hpp"

namespace eawr::sim::tactical {
using detail::combat_detail::one_raw;

bool players_allied(const std::span<const SnapshotPlayer> players, const PlayerId owner, const PlayerId target) noexcept {
    const auto find = [&](const PlayerId id) -> const SnapshotPlayer* {
        const auto entry = std::lower_bound(players.begin(), players.end(), id,
            [](const SnapshotPlayer& player, const PlayerId value) { return player.player_id < value; });
        return entry != players.end() && entry->player_id == id ? &*entry : nullptr;
    };
    const auto* source = find(owner);
    const auto* recipient = find(target);
    // WPR-33/WHZ-51: neutral relationships are bound faction data, never an alliance
    // inferred solely from a matching numeric team (including the neutral team).
    return source != nullptr && recipient != nullptr && (owner == target
        || (!source->neutral && !recipient->neutral && source->team_id == recipient->team_id));
}

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

namespace {

// WWP-21: a type's soft radius outside a formation: its loaded combat geometry, else its
// movement footprint, else the larger planar half extent of its collision box.
[[nodiscard]] math::Fixed type_radius(const TargetFootprint* geometry, const MotionTable* motion, const TypeId type_id,
    const CombatProfile* profile) noexcept {
    if (geometry != nullptr) return geometry->radius;
    if (const auto* footprint = motion != nullptr ? motion->footprint(type_id) : nullptr) return footprint->radius;
    if (profile == nullptr || !profile->collision) return {};
    const auto& box = *profile->collision;
    return math::Fixed::from_raw(std::max(box.max.x.raw() - box.min.x.raw(), box.max.y.raw() - box.min.y.raw()) / 2);
}

// WWP-21: the ideal team box over `count` roster slots; `extents(slot)` gives a live member's
// planar hard half extents, or nothing for a slot without a member.
template <typename Extents>
[[nodiscard]] core::Result<TargetFootprint> ideal_team_footprint(const std::size_t count, const SquadronProfile& profile,
    Extents&& extents) {
    motion_detail::Calc calc;
    math::Fixed low_x{}, low_y{}, high_x{}, high_y{};
    // WWP-21: the ideal team box includes the origin and live members' compacted base slots,
    // widened by each member's hard extents. Actual scattered flight positions do not enter it.
    for (std::size_t slot = 0; slot < count; ++slot) {
        const std::optional<std::pair<math::Fixed, math::Fixed>> half = extents(slot, calc);
        if (!half) continue;
        const auto offset = slot < profile.offsets.size() ? profile.offsets[slot] : math::Vec3{};
        const auto [x, y] = *half;
        low_x = std::min(low_x, calc.sub(offset.x, x));
        high_x = std::max(high_x, calc.add(offset.x, x));
        low_y = std::min(low_y, calc.sub(offset.y, y));
        high_y = std::max(high_y, calc.add(offset.y, y));
    }
    const auto x = math::Fixed::from_raw(calc.sub(high_x, low_x).raw() / 2);
    const auto y = math::Fixed::from_raw(calc.sub(high_y, low_y).raw() / 2);
    const auto radius = calc.length(x, y);
    if (!calc.ok()) return core::Result<TargetFootprint>::failure(calc.error("team target footprint"));
    return core::Result<TargetFootprint>::success({profile.type_id, radius, x, y});
}

// A member's planar hard half extents from its combat geometry, else its movement footprint,
// else half its collision box.
[[nodiscard]] std::pair<math::Fixed, math::Fixed> member_extents(const TargetFootprint* geometry,
    const MotionTable* motion, const TypeId type_id, const CombatProfile* profile, motion_detail::Calc& calc) {
    const auto* movement = motion != nullptr ? motion->footprint(type_id) : nullptr;
    auto x = geometry != nullptr ? geometry->x_extent : movement != nullptr ? movement->x_extent : math::Fixed{};
    auto y = geometry != nullptr ? geometry->y_extent : movement != nullptr ? movement->y_extent : math::Fixed{};
    if (geometry == nullptr && movement == nullptr && profile != nullptr && profile->collision) {
        const auto& box = *profile->collision;
        x = math::Fixed::from_raw(calc.sub(box.max.x, box.min.x).raw() / 2);
        y = math::Fixed::from_raw(calc.sub(box.max.y, box.min.y).raw() / 2);
    }
    return {x, y};
}

} // namespace

math::Fixed target_soft_radius(const MotionTable* motion, const CombatUnit& target) noexcept {
    if (target.formation_footprint) return target.formation_footprint->radius;
    return type_radius(target.footprint, motion, target.type_id, target.profile);
}

math::Fixed type_soft_radius(const CombatTable& table, const MotionTable* motion, const TypeId type_id) noexcept {
    return type_radius(table.footprint(type_id), motion, type_id, table.find_target_type(type_id));
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

core::Result<TargetFootprint> formation_target_footprint(const CombatWorld& world,
    const std::span<const EntityId> members, const SquadronProfile& profile) {
    return ideal_team_footprint(members.size(), profile,
        [&](const std::size_t slot, motion_detail::Calc& calc) -> std::optional<std::pair<math::Fixed, math::Fixed>> {
            const auto* member = world.find(members[slot]);
            if (member == nullptr) return std::nullopt;
            return member_extents(member->footprint, world.motion, member->type_id, member->profile, calc);
        });
}

core::Result<TargetFootprint> formation_target_footprint(const CombatTable& table, const MotionTable* motion,
    const std::span<const std::optional<TypeId>> member_types, const SquadronProfile& profile) {
    return ideal_team_footprint(member_types.size(), profile,
        [&](const std::size_t slot, motion_detail::Calc& calc) -> std::optional<std::pair<math::Fixed, math::Fixed>> {
            const auto type_id = member_types[slot];
            if (!type_id) return std::nullopt;
            return member_extents(table.footprint(*type_id), motion, *type_id, table.find_target_type(*type_id), calc);
        });
}

core::Result<math::Fixed> projectile_target_radius(const CombatWorld& world,
    const CombatUnit& shooter, const CombatUnit* target) {
    auto radius = target != nullptr ? target_soft_radius(world.motion, *target) : math::Fixed{};
    // WWP-27: a craft contributes its own radius and its team's, except for a melee shooter.
    const bool melee = shooter.profile != nullptr && shooter.profile->max_attack_distance
        && world.table != nullptr && *shooter.profile->max_attack_distance <= world.table->melee_cutoff;
    if (target != nullptr && !melee) {
        const auto parent_id = world.container_of(target->id);
        if (parent_id != target->id) {
            if (const auto* parent = world.find(parent_id)) {
                return math::add(radius, target_soft_radius(world.motion, *parent));
            }
        }
    }
    return core::Result<math::Fixed>::success(radius);
}

core::Result<math::Fixed> target_attack_distance(const MotionTable* motion, const CombatUnit& target,
    const math::Vec3 from, const math::Fixed distance, const bool hardpoint_aim) {
    motion_detail::Calc calc;
    auto extension = target_soft_radius(motion, target);
    if (!hardpoint_aim) {
        const auto* footprint = motion != nullptr ? motion->footprint(target.type_id) : nullptr;
        math::Fixed x{}, y{};
        const auto* geometry = target.formation_footprint ? &*target.formation_footprint : target.footprint;
        if (geometry != nullptr) {
            x = geometry->x_extent;
            y = geometry->y_extent;
        } else if (footprint != nullptr) {
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

const CombatUnit* CombatWorld::random_member(const CombatUnit* target, CombatRandom& random) const {
    if (target == nullptr) return nullptr;
    const auto team = std::lower_bound(teams.begin(), teams.end(), target->id,
        [](const std::pair<EntityId, std::vector<EntityId>>& entry, const EntityId value) { return entry.first < value; });
    if (team == teams.end() || team->first != target->id) return target;
    std::vector<const CombatUnit*> live;
    live.reserve(team->second.size());
    for (const auto member : team->second) {
        if (const auto* craft = find(member)) live.push_back(craft);
    }
    if (live.empty()) return nullptr;
    return live[random.uniform(0, static_cast<std::uint32_t>(live.size() - 1))];
}

std::vector<EntityId> CombatWorld::candidates(const math::Vec3& centre, const math::Fixed half, const PlayerId owner) const {
    if (collection == nullptr) return index.box(centre, math::Vec3{half, half, half}, owner);
    const auto low = [&](const math::Fixed value) { return math::Fixed::from_raw(value.raw() - half.raw()); };
    const auto high = [&](const math::Fixed value) { return math::Fixed::from_raw(value.raw() + half.raw()); };
    return collection->collect(owner, CullBox{{low(centre.x), low(centre.y), low(centre.z)},
        {high(centre.x), high(centre.y), high(centre.z)}});
}


core::Result<bool> formation_can_attack(const CombatWorld& world, const CombatUnit& target,
    const math::Fixed divert, const math::Vec3 anchor, const FormationAttackContext& formation) {
    if (!formation.present || !formation.allows_divert) return core::Result<bool>::success(false);
    if (formation.unbounded || within_range(anchor, target.position, divert, RangeMetric::planar))
        return core::Result<bool>::success(true);
    const auto* leader = formation.leader;
    if (leader == nullptr || leader->profile == nullptr || !leader->profile->max_attack_distance)
        return core::Result<bool>::success(false);
    // WMV-17: test attack reach from the allowance circle, using the formation's leader.
    motion_detail::Calc calc;
    auto direction = math::normalize(math::Vec2{
        calc.sub(target.position.x, anchor.x), calc.sub(target.position.y, anchor.y)});
    if (!direction) return core::Result<bool>::failure(direction.error());
    const math::Vec3 edge{calc.add(anchor.x, calc.mul(direction.value().x, divert)),
        calc.add(anchor.y, calc.mul(direction.value().y, divert)), anchor.z};
    if (!calc.ok()) return core::Result<bool>::failure(calc.error("formation attack admission"));
    auto reach = target_attack_distance(world.motion, target, edge, *leader->profile->max_attack_distance, false);
    if (!reach) return core::Result<bool>::failure(reach.error());
    return core::Result<bool>::success(within_range(edge, target.position, reach.value(), RangeMetric::planar));
}

core::Result<CombatState> finish_squadron_target(const CombatWorld& world, const CombatUnit& unit,
    CombatState state, const FormationAttackContext& formation, const EntityId formation_target, const bool same_cell) {
    if (!formation.present || state.attack_target == invalid_entity_id || state.direct || same_cell
        || state.attack_target == formation_target) return core::Result<CombatState>::success(std::move(state));
    const auto* target = world.find(state.attack_target);
    bool in_reach = false;
    if (target != nullptr && unit.profile != nullptr && unit.profile->max_attack_distance) {
        // WDF-14: reach reads the held container's position and extent, not an aimed member.
        auto reach = target_attack_distance(world.motion, *target, unit.position, *unit.profile->max_attack_distance, false);
        if (!reach) return core::Result<CombatState>::failure(reach.error());
        in_reach = within_range(unit.position, target->position, reach.value(), RangeMetric::planar);
    }
    if (!in_reach) {
        state.attack_target = invalid_entity_id;
        state.attack_hardpoint = no_hardpoint;
        state.direct = false;
    }
    return core::Result<CombatState>::success(std::move(state));
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

void UnitCombat::point_object_turret_target() {
    for (std::size_t slot = 0; slot < profile_.weapons.size(); ++slot) {
        const auto& weapon = profile_.weapons[slot];
        auto& state = state_.weapons[slot];
        if (!state.turret) continue;
        auto& turret = *state.turret;
        turret.ready = false;
        const auto* target = world_.resolve_target(world_.find(state_.attack_target), unit_.position);
        if (!target || !hostile(*target)) { turret.target = invalid_entity_id; continue; }
        turret.target = target->id;
        // FC-60: retained range loss holds the last pose; only target clear starts FC-61 idle.
        if (!profile_.max_attack_distance) continue;
        const auto reach = take(target_attack_distance(world_.motion, *target, unit_.position,
            *profile_.max_attack_distance, has_aim_hardpoint(*target)));
        if (!within_range(unit_.position, target->position, reach, RangeMetric::planar)) continue;
        if (profile_.min_attack_distance.raw() > 0 && within_range(unit_.position, target->position,
                math::Fixed::from_raw(profile_.min_attack_distance.raw() - 1), RangeMetric::planar)) continue;
        // FS-33/FC-60/FD-51: geometry refusal clears; aiming keeps the target for later calls.
        if (!point_object_turret(weapon, turret, *target)) {
            state_.attack_target = turret.target = invalid_entity_id;
            state_.attack_hardpoint = no_hardpoint;
            state_.direct = false;
        }
    }
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
        // WCC-25: the held team object bypasses type admission; its members do not.
        // Use the immutable team index rather than resolving or allocating an aim candidate.
        const auto team = std::lower_bound(world_.teams.begin(), world_.teams.end(), target.id,
            [](const auto& entry, const EntityId id) { return entry.first < id; });
        const bool container = team != world_.teams.end() && team->first == target.id;
        if ((!container && (!target.profile->living_projectile_collision || !target.profile->valid_target))
            || target.in_limbo || (target.durability && target.durability->hull.raw() <= 0)
            || (target.profile->special_weapon && !target.profile->star_base)) return result;
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
            result.suitable = take(formation_can_attack(world_, target, scan_divert_, *diversion_anchor_, *formation_context_));
        } else if (!result.suitable && diversion_anchor_ && profile_.max_attack_distance && formation_context_ == nullptr) {
            const auto reach = math::Fixed::from_raw(profile_.max_attack_distance->raw() + scan_divert_.raw());
            result.suitable = within_range(*diversion_anchor_, target.position, reach, RangeMetric::planar)
                || within_range(*diversion_anchor_, target.position,
                    math::Fixed::from_raw(member_attack_reach(target, *diversion_anchor_).raw() + scan_divert_.raw()), RangeMetric::planar);
        }
        return result;
    }

core::Result<CombatState> UnitCombat::retaliation_state(const CombatUnit& attacker,
    const math::Fixed divert, const math::Vec3 anchor, const FormationAttackContext* formation) {
    formation_context_ = formation;
    scan_divert_ = divert;
    diversion_anchor_ = anchor;
    if (state_.direct || !unit_.weapons_enabled || (profile_.attack_category_restrictions & categories(attacker)) != 0
        || (attacker.profile && attacker.profile->special_weapon)
        || (unit_.durability && unit_.durability->hull.raw() <= 0)
        || (formation && formation->present && !formation->allows_divert)) {
        return core::Result<CombatState>::success(std::move(state_));
    }
    const auto admitted = ship_suitable(attacker);
    if (error_) return core::Result<CombatState>::failure(*error_);
    const auto* current = world_.find(state_.attack_target);
    const auto held_priority = current ? priority(*current) : std::nullopt;
    // WTA-29: strictly lower priority wins; neither damage nor distance breaks a tie.
    if (admitted.suitable && admitted.priority && (!current || !held_priority
        || *admitted.priority < *held_priority)) {
        state_.attack_target = attacker.id;
        state_.attack_hardpoint = no_hardpoint;
        state_.direct = false;
    }
    return core::Result<CombatState>::success(std::move(state_));
}

[[nodiscard]] bool UnitCombat::damaged(const CombatUnit& target) {
        if (target.durability_profile == nullptr || target.durability == nullptr) return false;
        // hull / max hull <= Health_Low_Percent_Threshold, exactly: hull <= fraction x max hull.
        const auto limit = take(math::multiply(world_.rules->damaged_fraction, target.durability_profile->max_hull));
        if (target.durability->hull <= limit) return true;
        // WCC-16: a type that dies with its hardpoints shows the lesser of the hull fraction and the
        // combined destroyable-hardpoint fraction S/T, so a low S/T also counts as damaged.
        if (!target.durability_profile->destroyed_with_hardpoints) return false;
        std::int64_t maximum = 0;
        std::int64_t current = 0;
        const auto& profile_hardpoints = target.durability_profile->hardpoints;
        for (std::size_t index = 0; index < profile_hardpoints.size() && index < target.durability->hardpoints.size();
             ++index) {
            if (!profile_hardpoints[index].destroyable) continue;
            maximum += profile_hardpoints[index].max_health.raw();
            current += target.durability->hardpoints[index].raw();
        }
        if (maximum <= 0) return false;
        const auto hardpoint_limit = take(math::multiply(world_.rules->damaged_fraction, math::Fixed::from_raw(maximum)));
        return current <= hardpoint_limit.raw();
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
        // WST-10: a cloaked unit does not scan, and its next scan frame stays as it was.
        if (unit_.stealth_on) return {nullptr, std::nullopt};
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

namespace {

// WCC-15: a tracked unit's estimated time to death (durability.hpp).
[[nodiscard]] std::optional<math::Fixed> time_to_death(const CombatUnit& unit) noexcept {
    return tactical::time_to_death(*unit.durability_profile, *unit.durability);
}

[[nodiscard]] bool damage_tracked(const CombatUnit& unit) noexcept {
    return unit.durability_profile != nullptr && unit.durability != nullptr && unit.durability_profile->damage_tracking;
}

} // namespace

bool UnitCombat::sticky(const CombatUnit& target) const {
    // A squadron's team container sums its tracked craft (an untracked craft adds nothing); any
    // other target without damage tracking is never sticky.
    std::optional<math::Fixed> time{math::Fixed{}};
    const auto team = std::lower_bound(world_.teams.begin(), world_.teams.end(), target.id,
        [](const auto& entry, const EntityId id) { return entry.first < id; });
    if (team != world_.teams.end() && team->first == target.id) {
        for (const auto member_id : team->second) {
            const auto* member = world_.find(member_id);
            if (member == nullptr || !damage_tracked(*member)) continue;
            const auto member_time = time_to_death(*member);
            // Both are at least 0: a sum beyond Fixed's range is "never" too.
            if (!member_time || member_time->raw() > std::numeric_limits<std::int64_t>::max() - time->raw()) return false;
            time = math::Fixed::from_raw(time->raw() + member_time->raw());
        }
    } else {
        if (!damage_tracked(target)) return false;
        time = time_to_death(target);
        if (!time) return false;
    }
    // The threshold is the scanner's type's (a squadron craft: its leader's type); a runtime
    // override from Lua is not bound. A type that authors none has 0 (debug build).
    const auto* threshold_profile = &profile_;
    if (formation_context_ != nullptr && formation_context_->leader != nullptr
        && formation_context_->leader->profile != nullptr) threshold_profile = formation_context_->leader->profile;
    return *time <= threshold_profile->stickiness;
}

void UnitCombat::ship_target() {
        const CombatUnit* current = nullptr;
        if (state_.attack_target != invalid_entity_id) {
            // WSQ-45: suitability and replacement read the held object. FO-04's nearest
            // live member is resolved separately when the weapon aims and fires.
            current = world_.find(state_.attack_target);
            // FS-09/FD-06: discovering a dead held target clears and ends this
            // targeting turn. Detachment never makes the retained scan due.
            if (current == nullptr || (current->durability && current->durability->hull.raw() <= 0)) {
                state_.attack_target = invalid_entity_id;
                state_.attack_hardpoint = no_hardpoint;
                state_.direct = false;
                return;
            }
            if (!hostile(*current) || !visible_to_me(*current)) {
                state_.attack_target = invalid_entity_id;
                state_.attack_hardpoint = no_hardpoint;
                state_.direct = false;
                return;
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
        // WCC-14: a suitable target is kept without a scan at priority 1.0 or while sticky (WCC-15).
        // Stickiness is only read when a scan is due: before that the scan finds nothing anyway.
        if (current_state.suitable && current_state.priority && (current_state.priority->raw() == one_raw
            || (world_.frame >= state_.next_scan_frame && sticky(*current)))) return;
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

core::Result<CombatState> retaliate_combat(const CombatWorld& world, const CombatUnit& unit,
    const CombatUnit& attacker, const math::Fixed divert, const math::Vec3 anchor,
    const FormationAttackContext* formation) {
    if (!unit.profile || !unit.combat) return core::Result<CombatState>::success(
        unit.combat ? *unit.combat : CombatState{});
    return combat_detail::UnitCombat(world, unit).retaliation_state(attacker, divert, anchor, formation);
}

} // namespace detail
} // namespace eawr::sim::tactical
