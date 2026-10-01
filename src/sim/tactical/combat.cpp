#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/math/trig.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace eawr::sim::tactical {
namespace {

constexpr std::int64_t one_raw = math::Fixed::scale;

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] bool within_distance(const math::Fixed value, const std::int64_t limit_units) noexcept {
    const auto limit = limit_units * one_raw;
    return value.raw() >= -limit && value.raw() <= limit;
}

[[nodiscard]] bool point_in_bounds(const math::Vec3& point) noexcept {
    return within_distance(point.x, max_combat_distance) && within_distance(point.y, max_combat_distance)
        && within_distance(point.z, max_combat_distance);
}

} // namespace

const CombatProfile* CombatTable::find(const TypeId type_id) const noexcept {
    const auto found = std::lower_bound(profiles.begin(), profiles.end(), type_id,
        [](const CombatProfile& profile, const TypeId id) { return profile.type_id < id; });
    return found != profiles.end() && found->type_id == type_id ? &*found : nullptr;
}

core::Result<void> validate_combat(const CombatTable& table) {
    const auto invalid = [](const std::string& message) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "combat table: " + message));
    };
    const auto distance_ok = [](const math::Fixed value) {
        return value.raw() >= 0 && value.raw() <= max_combat_distance * one_raw;
    };
    const auto cone_ok = [](const math::Fixed value) { return value.raw() >= 0 && value.raw() <= 360 * one_raw; };
    // W-07's yaw and pitch assume an orthonormal frame: each axis of unit length and each pair
    // perpendicular, every dot product within frame_tolerance_raw of 1 or 0. Bounded components
    // keep the dot products in range.
    const auto axes_ok = [](const std::optional<std::array<math::Vec3, 3>>& axes) {
        if (!axes) return true;
        const auto small = [](const math::Fixed value) { return std::llabs(value.raw()) <= 2 * one_raw; };
        if (!std::all_of(axes->begin(), axes->end(),
                [&](const math::Vec3& axis) { return small(axis.x) && small(axis.y) && small(axis.z); })) {
            return false;
        }
        const auto near = [&](const std::size_t left, const std::size_t right, const std::int64_t expected) {
            const auto product = math::dot((*axes)[left], (*axes)[right]);
            return product && std::llabs(product.value().raw() - expected) <= frame_tolerance_raw;
        };
        return near(0, 0, one_raw) && near(1, 1, one_raw) && near(2, 2, one_raw) && near(0, 1, 0) && near(0, 2, 0)
            && near(1, 2, 0);
    };
    for (std::size_t set = 0; set < table.priority_sets.size(); ++set) {
        const auto& rows = table.priority_sets[set].rows;
        for (std::size_t row = 1; row < rows.size(); ++row) {
            if (rows[row - 1].type_id >= rows[row].type_id) {
                return invalid("priority set " + std::to_string(set) + " rows must strictly increase");
            }
        }
    }
    for (std::size_t index = 0; index < table.profiles.size(); ++index) {
        const auto& profile = table.profiles[index];
        const auto name = "type " + std::to_string(profile.type_id) + ": ";
        if (index != 0 && table.profiles[index - 1].type_id >= profile.type_id) {
            return invalid("type IDs must strictly increase");
        }
        if (profile.priority_set && *profile.priority_set >= table.priority_sets.size()) {
            return invalid(name + "unknown priority set");
        }
        if (profile.max_attack_distance && !distance_ok(*profile.max_attack_distance)) {
            return invalid(name + "attack distance out of range");
        }
        if (profile.weapons.size() > max_weapons_per_type) {
            return invalid(name + "too many weapons");
        }
        for (const auto& weapon : profile.weapons) {
            if ((weapon.hardpoint != object_weapon && weapon.hardpoint >= max_hardpoints_per_type)
                || !distance_ok(weapon.range) || !cone_ok(weapon.cone_width) || !cone_ok(weapon.cone_height)
                || weapon.min_recharge_hundredths > weapon.max_recharge_hundredths
                || weapon.max_recharge_hundredths > max_recharge_hundredths || weapon.pulse_count == 0
                || weapon.pulse_delay_frames > max_recharge_hundredths || !point_in_bounds(weapon.fire_a)
                || !point_in_bounds(weapon.fire_b) || !axes_ok(weapon.fire_axes)
                || weapon.ai_combat_power.raw() < 0 || weapon.ai_combat_power.raw() > max_weapon_combat_power * one_raw) {
                return invalid(name + "invalid weapon " + std::to_string(weapon.hardpoint));
            }
            for (const auto* candidate : {&weapon.shot, &weapon.ability_shot}) {
                if (!*candidate) continue;
                const auto& shot = **candidate;
                const auto damage_ok = shot.damage.raw() >= 0 && shot.damage.raw() <= max_durability_health * one_raw;
                const auto inaccuracy_ok = std::all_of(shot.inaccuracy.begin(), shot.inaccuracy.end(),
                    [&](const InaccuracyRow& row) { return distance_ok(row.distance); });
                if (!damage_ok || shot.speed.raw() <= 0 || !distance_ok(shot.speed) || !distance_ok(shot.max_travel)
                    || !inaccuracy_ok || (shot.ion_stun && !valid_ion_stun(*shot.ion_stun))) {
                    return invalid(name + "invalid projectile of weapon " + std::to_string(weapon.hardpoint));
                }
            }
        }
        if (profile.collision) {
            const auto& box = *profile.collision;
            if (!point_in_bounds(box.min) || !point_in_bounds(box.max) || box.max.x < box.min.x
                || box.max.y < box.min.y || box.max.z < box.min.z) {
                return invalid(name + "invalid collision box");
            }
        }
        // #536: meshes need a box to gate them, vertices within max_mesh_extent and a well-formed
        // tree (children after their parent, leaves inside the triangle list).
        if (!profile.meshes.empty() && (!profile.collision || !profile.mesh_bounds)) {
            return invalid(name + "collision meshes need a collision box and mesh bounds");
        }
        const auto in_mesh_extent = [](const math::Vec3& point) {
            const auto limit = max_mesh_extent * one_raw;
            return std::llabs(point.x.raw()) <= limit && std::llabs(point.y.raw()) <= limit
                && std::llabs(point.z.raw()) <= limit;
        };
        for (const auto& mesh : profile.meshes) {
            if (mesh.triangles.empty() || mesh.nodes.empty()
                || (mesh.hardpoint != no_hardpoint && mesh.hardpoint >= max_hardpoints_per_type)
                || (mesh.source_hardpoint != no_hardpoint && mesh.source_hardpoint >= max_hardpoints_per_type)) {
                return invalid(name + "invalid collision mesh");
            }
            for (const auto& triangle : mesh.triangles) {
                if (!in_mesh_extent(triangle.a) || !in_mesh_extent(triangle.b) || !in_mesh_extent(triangle.c)) {
                    return invalid(name + "collision mesh beyond " + std::to_string(max_mesh_extent) + " units");
                }
            }
            for (std::size_t slot = 0; slot < mesh.nodes.size(); ++slot) {
                const auto& node = mesh.nodes[slot];
                const bool leaf_ok = node.count > 0 && node.first <= mesh.triangles.size()
                    && node.count <= mesh.triangles.size() - node.first;
                const bool inner_ok = node.count == 0 && node.first > slot && node.second > slot
                    && node.first < mesh.nodes.size() && node.second < mesh.nodes.size();
                if ((!leaf_ok && !inner_ok) || !in_mesh_extent(node.min) || !in_mesh_extent(node.max)) {
                    return invalid(name + "invalid collision mesh tree");
                }
            }
        }
        // #669 (DG-39): each aimed route names the hull or a hardpoint within the limit.
        if (profile.aimed_routes.size() > max_hardpoints_per_type
            || std::any_of(profile.aimed_routes.begin(), profile.aimed_routes.end(), [](const std::uint32_t route) {
                   return route != no_hardpoint && route >= max_hardpoints_per_type;
               })) {
            return invalid(name + "invalid aimed hardpoint route");
        }
        if (profile.sphere_modifier && (!profile.collision || profile.sphere_modifier->raw() <= one_raw
                                           || profile.sphere_modifier->raw() > max_sphere_modifier * one_raw)) {
            return invalid(name + "invalid collision sphere");
        }
        if (profile.mesh_bounds) {
            const auto& box = *profile.mesh_bounds;
            if (!in_mesh_extent(box.min) || !in_mesh_extent(box.max) || box.max.x < box.min.x
                || box.max.y < box.min.y || box.max.z < box.min.z) {
                return invalid(name + "invalid mesh bounds");
            }
        }
        for (const auto& bone : profile.target_bones) {
            if (!point_in_bounds(bone)) return invalid(name + "target bone out of range");
        }
        for (const auto& hardpoint : profile.hardpoints) {
            if (hardpoint.hardpoint >= max_hardpoints_per_type || !point_in_bounds(hardpoint.position)) {
                return invalid(name + "invalid target hardpoint");
            }
        }
    }
    // A shot that may meet a mesh moves at most max_mesh_extent units a frame (the segment test's bound).
    const bool meshes = std::any_of(table.profiles.begin(), table.profiles.end(),
        [](const CombatProfile& profile) { return !profile.meshes.empty(); });
    for (const auto& profile : table.profiles) {
        for (const auto& weapon : profile.weapons) {
            if (meshes && weapon.shot && weapon.shot->speed.raw() > max_mesh_extent * one_raw) {
                return invalid("type " + std::to_string(profile.type_id) + ": a shot faster than "
                    + std::to_string(max_mesh_extent) + " units a frame with collision meshes");
            }
        }
    }
    return core::Result<void>::success();
}

CombatRandom::CombatRandom(
    const std::uint64_t seed, const std::uint64_t frame, const EntityId unit, const std::uint32_t slot) noexcept
    : key_(mix64(seed ^ mix64(frame ^ mix64(unit ^ mix64(0x656177722d636f6dULL ^ slot))))) {}

std::uint32_t CombatRandom::uniform(const std::uint32_t low, const std::uint32_t high) noexcept {
    const auto value = mix64(key_ + static_cast<std::uint64_t>(count_) * 0xd1b54a32d192ed03ULL);
    ++count_;
    if (high <= low) {
        return low;
    }
    const auto span = static_cast<std::uint64_t>(high - low) + 1U;
    return low + static_cast<std::uint32_t>(value % span);
}

std::int64_t CombatRandom::symmetric_raw(const std::int64_t magnitude) noexcept {
    const auto value = mix64(key_ + static_cast<std::uint64_t>(count_) * 0xd1b54a32d192ed03ULL);
    ++count_;
    if (magnitude <= 0) {
        return 0;
    }
    const auto span = static_cast<std::uint64_t>(magnitude) * 2U + 1U;
    return static_cast<std::int64_t>(value % span) - magnitude;
}

std::optional<math::Vec3> lead_point(
    const math::Vec3 aim, const math::Vec3 muzzle, const math::Vec3 velocity, const math::Fixed speed) {
    if (speed.raw() <= 0 || (velocity.x.raw() == 0 && velocity.y.raw() == 0 && velocity.z.raw() == 0)) return aim;
    motion_detail::Calc calc;
    // In frames and projectile flights: |u + w t| = t with u = (aim - muzzle) / speed and
    // w = velocity / speed, so (w.w - 1) t^2 + 2 (u.w) t + u.u = 0.
    const auto per_shot = [&](const math::Vec3& value) {
        return math::Vec3{calc.div(value.x, speed), calc.div(value.y, speed), calc.div(value.z, speed)};
    };
    const auto dot = [&](const math::Vec3& l, const math::Vec3& r) {
        return calc.add(calc.add(calc.mul(l.x, r.x), calc.mul(l.y, r.y)), calc.mul(l.z, r.z));
    };
    const auto u = per_shot(math::Vec3{calc.sub(aim.x, muzzle.x), calc.sub(aim.y, muzzle.y), calc.sub(aim.z, muzzle.z)});
    const auto w = per_shot(velocity);
    const auto a = calc.sub(dot(w, w), math::Fixed::from_raw(one_raw));
    const auto b = calc.mul(dot(u, w), math::Fixed::from_raw(2 * one_raw));
    const auto c = dot(u, u);
    std::optional<math::Fixed> time;
    if (a.raw() == 0) {
        if (b.raw() != 0) {
            const auto root = calc.div(calc.neg(c), b);
            if (root.raw() >= 0) time = root;
        }
    } else {
        const auto discriminant = calc.sub(calc.mul(b, b), calc.mul(calc.mul(a, c), math::Fixed::from_raw(4 * one_raw)));
        if (discriminant.raw() >= 0) {
            const auto root = calc.sqrt(discriminant);
            const auto twice = calc.add(a, a);
            const auto first = calc.div(calc.add(calc.neg(b), root), twice);
            const auto second = calc.div(calc.sub(calc.neg(b), root), twice);
            if (first.raw() > 0 && second.raw() > 0) {
                time = std::min(first, second);
            } else if (first.raw() > 0) {
                time = first;
            } else if (second.raw() > 0) {
                time = second;
            }
        }
    }
    if (!calc.ok()) return aim;
    if (!time) return std::nullopt;
    const math::Vec3 point{calc.add(aim.x, calc.mul(velocity.x, *time)), calc.add(aim.y, calc.mul(velocity.y, *time)),
        calc.add(aim.z, calc.mul(velocity.z, *time))};
    return calc.ok() ? point : aim;
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

CombatState initial_combat(
    const CombatProfile& profile, const std::uint64_t seed, const std::uint64_t frame, const EntityId unit) {
    CombatState state;
    state.weapons.reserve(profile.weapons.size());
    for (std::uint32_t slot = 0; slot < profile.weapons.size(); ++slot) {
        const auto& weapon = profile.weapons[slot];
        CombatRandom random(seed, frame, unit, spawn_slot_flag | slot);
        // Retail spawn rule: a countdown in [0, trunc(max recharge seconds x fps)].
        const auto limit = weapon.max_recharge_hundredths * logical_frames_per_second / 100U;
        WeaponState weapon_state;
        weapon_state.countdown = random.uniform(0, limit);
        weapon_state.pulses_left = weapon.pulse_count;
        state.weapons.push_back(weapon_state);
    }
    return state;
}

std::string_view to_string(const CombatEventKind kind) noexcept {
    switch (kind) {
    case CombatEventKind::target_acquired: return "target_acquired";
    case CombatEventKind::weapon_fired: return "weapon_fired";
    case CombatEventKind::projectile_hit: return "projectile_hit";
    }
    return "unknown";
}

namespace detail {

const CombatUnit* CombatWorld::find(const EntityId id) const noexcept {
    const auto found = std::lower_bound(
        units.begin(), units.end(), id, [](const CombatUnit& unit, const EntityId value) { return unit.id < value; });
    return found != units.end() && found->id == id ? &*found : nullptr;
}

EntityId CombatWorld::container_of(const EntityId unit) const noexcept {
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

void append_combat(std::vector<std::uint8_t>& bytes, const CombatState& state) {
    sim::detail::append_u64(bytes, state.attack_target);
    // #531: bit 1 is the direct flag; an ordered hardpoint rides above it (index plus one), so a
    // session without a hardpoint order hashes as before.
    sim::detail::append_u32(bytes,
        (state.direct ? 1U : 0U) | (state.attack_hardpoint == no_hardpoint ? 0U : (state.attack_hardpoint + 1U) << 1U));
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.weapons.size()));
    sim::detail::append_u64(bytes, state.next_scan_frame);
    for (const auto& weapon : state.weapons) {
        sim::detail::append_u64(bytes, weapon.opportunity.target);
        sim::detail::append_u64(bytes, weapon.opportunity.last_scan_frame);
        sim::detail::append_u32(bytes, weapon.countdown);
        sim::detail::append_u32(bytes, weapon.pulses_left);
    }
}

void append_combat_event(std::vector<std::uint8_t>& bytes, const CombatEvent& event) {
    sim::detail::append_u64(bytes, event.tick);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(event.kind));
    sim::detail::append_u32(bytes, event.weapon);
    sim::detail::append_u64(bytes, event.shooter);
    sim::detail::append_u64(bytes, event.target);
    sim::detail::append_u32(bytes, event.target_hardpoint);
    sim::detail::append_u32(bytes, event.outcome);
    for (const auto& point : {event.origin, event.aim}) {
        sim::detail::append_i64(bytes, point.x.raw());
        sim::detail::append_i64(bytes, point.y.raw());
        sim::detail::append_i64(bytes, point.z.raw());
    }
}

const TargetHardpoint* standing_hardpoint(const CombatUnit& target, const std::uint32_t index) {
    if (index == no_hardpoint || target.profile == nullptr) return nullptr;
    for (const auto& hardpoint : target.profile->hardpoints) {
        if (hardpoint.hardpoint != index) continue;
        if (!hardpoint.targetable) return nullptr;
        if (target.durability_profile != nullptr && target.durability != nullptr
            && index < target.durability_profile->hardpoints.size()
            && tactical::hardpoint_destroyed(*target.durability_profile, *target.durability, index)) {
            return nullptr;
        }
        return &hardpoint;
    }
    return nullptr;
}

namespace {

// One unit's targeting and fire for one frame. The first arithmetic failure is kept and fails
// the step; the rules never reach one for bounded content and positions.
class UnitCombat final {
public:
    UnitCombat(const CombatWorld& world, const CombatUnit& unit)
        : world_(world), unit_(unit), profile_(*unit.profile), state_(*unit.combat) {}

    core::Result<CombatStep> run() {
        if (state_.weapons.size() != profile_.weapons.size()) {
            return core::Result<CombatStep>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                "combat state does not match the type's weapons"));
        }
        ship_target();
        // OR-23: an ordered hardpoint lasts while the order's target stands and so does it; then
        // the unit goes back to picking the target's nearest hardpoint (OR-24).
        if (state_.attack_hardpoint != no_hardpoint) {
            const auto* ordered = state_.direct && state_.attack_target != invalid_entity_id
                ? world_.resolve_target(world_.find(state_.attack_target), unit_.position)
                : nullptr;
            if (ordered == nullptr || standing_hardpoint(*ordered, state_.attack_hardpoint) == nullptr) {
                state_.attack_hardpoint = no_hardpoint;
            }
        }
        face_target();
        for (std::uint32_t slot = 0; slot < profile_.weapons.size(); ++slot) {
            service_weapon(slot);
        }
        if (error_) {
            return core::Result<CombatStep>::failure(std::move(*error_));
        }
        return core::Result<CombatStep>::success(
            CombatStep{std::move(state_), std::move(events_), face_, energy_spent_, std::move(aim_offsets_)});
    }

    // --- shared helpers ---------------------------------------------------------------------

    template <typename T>
    T take(core::Result<T> result, T fallback = T{}) {
        if (!result) {
            if (!error_) error_ = result.error();
            return fallback;
        }
        return std::move(result).value();
    }

    [[nodiscard]] math::Vec3 world_point(const CombatUnit& owner, const math::Vec3& local) {
        return take(math::transform_point(owner.transform, local), owner.position);
    }

    [[nodiscard]] bool visible_to_me(const CombatUnit& target) const noexcept {
        return target.team == unit_.team || (target.visible_to & (std::uint64_t{1} << unit_.player_index)) != 0U;
    }

    [[nodiscard]] bool hostile(const CombatUnit& target) const noexcept { return target.team != unit_.team; }

    [[nodiscard]] std::uint64_t categories(const CombatUnit& target) const noexcept {
        return target.profile != nullptr ? target.profile->category_bits : 0U;
    }

    // R-09 through the resolved set; a parent without a set scores every candidate 1.0.
    [[nodiscard]] std::optional<math::Fixed> priority(const CombatUnit& target) const {
        if (!profile_.priority_set) {
            return math::Fixed::from_raw(one_raw);
        }
        const auto& set = world_.table->priority_sets[*profile_.priority_set];
        const auto found = std::lower_bound(set.rows.begin(), set.rows.end(), target.type_id,
            [](const PriorityRow& row, const TypeId id) { return row.type_id < id; });
        if (found != set.rows.end() && found->type_id == target.type_id) {
            return found->priority;
        }
        return set.unlisted;
    }

    [[nodiscard]] bool hardpoint_destroyed_on(const CombatUnit& target, const std::uint32_t index) const {
        return target.durability_profile != nullptr && target.durability != nullptr
            && index < target.durability_profile->hardpoints.size()
            && tactical::hardpoint_destroyed(*target.durability_profile, *target.durability, index);
    }

    [[nodiscard]] bool weapon_up(const WeaponProfile& weapon) const {
        if (weapon.hardpoint == object_weapon || unit_.durability_profile == nullptr || unit_.durability == nullptr
            || weapon.hardpoint >= unit_.durability_profile->hardpoints.size()) {
            return true;
        }
        return weapon_enabled(*unit_.durability_profile, *unit_.durability, weapon.hardpoint);
    }

    [[nodiscard]] bool restricted(const WeaponProfile& weapon, const CombatUnit& target) const noexcept {
        return (weapon.category_restrictions & categories(target)) != 0U;
    }

    [[nodiscard]] math::Vec3 local_midpoint(const WeaponProfile& weapon) const noexcept {
        if (!weapon.has_fire_b) return weapon.fire_a;
        return math::Vec3{math::Fixed::from_raw((weapon.fire_a.x.raw() + weapon.fire_b.x.raw()) / 2),
            math::Fixed::from_raw((weapon.fire_a.y.raw() + weapon.fire_b.y.raw()) / 2),
            math::Fixed::from_raw((weapon.fire_a.z.raw() + weapon.fire_b.z.raw()) / 2)};
    }

    // Can_Weapon_Point_At for a fixed hardpoint: the point's yaw and pitch in Fire_Bone_A's
    // frame placed at the weapon midpoint lie within half the cone width and height; a zero
    // (unauthored) cone accepts only a point dead ahead (W-07). The object weapon's cone is its
    // turret extents about the unit's own facing, compared whole; without them it has none (W-09).
    [[nodiscard]] bool pointable(const WeaponProfile& weapon, const math::Vec3& midpoint, const math::Vec3& point) {
        const bool turret = weapon.hardpoint == object_weapon;
        if (turret && (weapon.cone_width.raw() == 0 || weapon.cone_height.raw() == 0)) {
            return true;
        }
        const math::Vec3 delta{take(math::subtract(point.x, midpoint.x)), take(math::subtract(point.y, midpoint.y)),
            take(math::subtract(point.z, midpoint.z))};
        const auto& m = unit_.transform.rows;
        const auto axis = [&](const std::size_t column) {
            return take(math::dot(math::Vec3{m[0][column], m[1][column], m[2][column]}, delta));
        };
        math::Vec3 local{axis(0), axis(1), axis(2)};
        if (!turret && weapon.fire_axes) {
            const auto& bone = *weapon.fire_axes;
            local = math::Vec3{take(math::dot(bone[0], local)), take(math::dot(bone[1], local)),
                take(math::dot(bone[2], local))};
        }
        const auto x = local.x;
        const auto y = local.y;
        const auto z = local.z;
        // FoC's facing takes no atan2 without a planar part: a point on the bone's z axis has yaw 0
        // and pitch +-90 degrees, the weapon midpoint itself yaw and pitch 0 (research WA-14).
        const auto yaw = x.raw() == 0 && y.raw() == 0 ? math::Fixed{} : take(math::atan2_turn(y, x));
        const auto planar = take(math::length(math::Vec2{x, y}));
        const auto pitch = planar.raw() == 0 && z.raw() == 0 ? math::Fixed{} : take(math::atan2_turn(z, planar));
        // |angle in turns| <= cone degrees / 720 (a turret extent: / 360), compared exactly on raw values.
        const std::int64_t turn_degrees = turret ? 360 : 720;
        return std::llabs(yaw.raw()) * turn_degrees <= weapon.cone_width.raw()
            && std::llabs(pitch.raw()) * turn_degrees <= weapon.cone_height.raw();
    }

    // R-10, R-11: target bones in order, then the target's hardpoints that are not destroyed in
    // circular order from one draw, then its position. The first point in range and pointable.
    struct Aim {
        math::Vec3 point{};
        std::uint32_t hardpoint{no_hardpoint};
    };
    [[nodiscard]] std::optional<Aim> choose_aim(const WeaponProfile& weapon, const math::Vec3& midpoint,
        const CombatUnit& target, CombatRandom& random, const RangeMetric metric) {
        // The soft coordinate radius is not loaded; it is zero (docs/behaviour/space-weapon-fire.md).
        const auto acceptable = [&](const math::Vec3& point) {
            return within_range(midpoint, point, weapon.range, metric) && pointable(weapon, midpoint, point);
        };
        if (target.profile != nullptr) {
            for (const auto& bone : target.profile->target_bones) {
                const auto point = world_point(target, bone);
                if (acceptable(point)) return Aim{point, no_hardpoint};
            }
            const auto& hardpoints = target.profile->hardpoints;
            if (!hardpoints.empty()) {
                const auto start = random.uniform(0, static_cast<std::uint32_t>(hardpoints.size() - 1));
                for (std::size_t step = 0; step < hardpoints.size(); ++step) {
                    const auto& hardpoint = hardpoints[(start + step) % hardpoints.size()];
                    if (hardpoint_destroyed_on(target, hardpoint.hardpoint)) continue;
                    const auto point = world_point(target, hardpoint.position);
                    if (acceptable(point)) return Aim{point, hardpoint.hardpoint};
                }
            }
        }
        if (acceptable(target.position)) return Aim{target.position, no_hardpoint};
        return std::nullopt;
    }

    // Attempt_Fire_At_Target (R-13 and the audit's firing attempt): on success the shot is
    // recorded and the weapon's burst and recharge advance.
    bool attempt(const std::uint32_t slot, const EntityId target_id, CombatRandom& random, const bool burst_begun = false) {
        // AB-66 (#561): while the squadron's ION_CANNON_SHOT is on, a craft's hardpoint fires only
        // the override shot, at the ion target, until the craft has fired it; otherwise nothing.
        std::optional<WeaponProfile> ion;
        if (unit_.ion_held) {
            if (!unit_.ion_armed || !profile_.weapons[slot].ability_shot) return false;
            ion = profile_.weapons[slot];
            ion->shot = ion->ability_shot;
        }
        const auto& weapon = ion ? *ion : profile_.weapons[slot];
        // #424: a squadron's team container is fired at through its craft nearest this unit.
        const auto* target = world_.resolve_target(world_.find(target_id), unit_.position);
        if (target == nullptr || target->profile == nullptr || !visible_to_me(*target) || restricted(weapon, *target)) {
            return false;
        }
        if (ion && target->id != unit_.ion_target && world_.container_of(target->id) != unit_.ion_target) {
            return false;
        }
        const auto midpoint = world_point(unit_, local_midpoint(weapon));
        // The nearest live targetable hardpoint (spatial distance from the shooter, first on a
        // tie), else the aim-point search.
        std::optional<Aim> aim;
        // OR-25: the ordered hardpoint of the ordered target, with no fallback to another point.
        if (target_id == state_.attack_target && state_.direct) {
            if (const auto* ordered = standing_hardpoint(*target, state_.attack_hardpoint)) {
                aim = Aim{world_point(*target, ordered->position), ordered->hardpoint};
            }
        }
        if (!aim && target->profile != nullptr) {
            const TargetHardpoint* nearest = nullptr;
            math::Vec3 nearest_point{};
            for (const auto& hardpoint : target->profile->hardpoints) {
                if (!hardpoint.targetable || hardpoint_destroyed_on(*target, hardpoint.hardpoint)) continue;
                const auto point = world_point(*target, hardpoint.position);
                if (nearest == nullptr || closer(unit_.position, point, nearest_point)) {
                    nearest = &hardpoint;
                    nearest_point = point;
                }
            }
            if (nearest != nullptr) aim = Aim{nearest_point, nearest->hardpoint};
            // AB-61: an ion shot at a hardpoint aims at it while it stands (else as above).
            if (ion && unit_.ion_target_hardpoint != no_hardpoint) {
                for (const auto& hardpoint : target->profile->hardpoints) {
                    if (hardpoint.hardpoint != unit_.ion_target_hardpoint || !hardpoint.targetable
                        || hardpoint_destroyed_on(*target, hardpoint.hardpoint)) {
                        continue;
                    }
                    aim = Aim{world_point(*target, hardpoint.position), hardpoint.hardpoint};
                }
            }
        }
        if (!aim) {
            aim = choose_aim(weapon, midpoint, *target, random, RangeMetric::spatial);
            if (!aim) return false;
        }
        // Planar range from the weapon midpoint to the aim point (plus the zero soft radius).
        if (!within_range(midpoint, aim->point, weapon.range, RangeMetric::planar)) {
            return false;
        }
        auto origin = weapon.fire_a;
        if (weapon.has_fire_b && random.uniform(0, 99) >= 50U) origin = weapon.fire_b;
        const auto muzzle = world_point(unit_, origin);
        // W-10, W-11: the shot leads the target; the weapon must point at the led point.
        const auto led = lead(weapon, *target, aim->point, muzzle);
        if (!led) return false;
        math::Vec3 scattered{};
        if (weapon.hardpoint == object_weapon) {
            // DG-24, W-09: the object weapon scatters by the spatial distance from its muzzle to
            // the led point, and its cone is tested on the scattered point.
            const auto spatial = take(math::length(math::Vec3{take(math::subtract(led->x, muzzle.x)),
                take(math::subtract(led->y, muzzle.y)), take(math::subtract(led->z, muzzle.z))}));
            scattered = scatter(weapon, spatial, *target, *led, random);
            if (!pointable(weapon, midpoint, scattered)) return false;
        } else {
            if (!pointable(weapon, midpoint, *led)) return false;
            const auto planar = take(math::length(math::Vec2{
                take(math::subtract(aim->point.x, midpoint.x)), take(math::subtract(aim->point.y, midpoint.y))}));
            scattered = scatter(weapon, planar, *target, *led, random);
        }
        events_.push_back(CombatEvent{world_.frame, CombatEventKind::weapon_fired, unit_.id, weapon.hardpoint,
            target->id, aim->hardpoint, muzzle, scattered, ion ? fired_ability_shot : 0U});
        // MS-03: the scatter off the led point, taken into the target's frame for a homing
        // projectile to keep.
        const math::Vec3 off{take(math::subtract(scattered.x, led->x)),
            take(math::subtract(scattered.y, led->y)), take(math::subtract(scattered.z, led->z))};
        const auto& m = target->transform.rows;
        const auto column = [&](const std::size_t j) {
            return take(math::add(take(math::add(take(math::multiply(m[0][j], off.x)), take(math::multiply(m[1][j], off.y)))),
                take(math::multiply(m[2][j], off.z))));
        };
        aim_offsets_.push_back(math::Vec3{column(0), column(1), column(2)});
        // W-06: a hardpoint spends its pulse with the shot; the object weapon only the first of a
        // burst, whose later pulses it spends before it tries them (W-06a, service_weapon).
        auto& state = state_.weapons[slot];
        if (weapon.hardpoint != object_weapon || !burst_begun) {
            spend_pulse(weapon, state, random);
        }
        return true;
    }

    // One pulse of the burst (W-06, W-06a): while pulses remain the countdown becomes the pulse
    // delay; after the last the recharge, and the burst refills. The object weapon's recharge
    // adds a draw of 0 to 10 frames. AB-21: an active ability's weapon delay multiplier
    // lengthens the gap within a burst and shortens, never lengthens, the recharge after it.
    void spend_pulse(const WeaponProfile& weapon, WeaponState& state, CombatRandom& random) {
        if (state.pulses_left != 0) --state.pulses_left;
        // IS-06: an ion stun's fire rate lengthens a hardpoint's recharge and gaps and shortens its
        // next burst; 1 without a stun changes nothing.
        const bool stunned = weapon.hardpoint != object_weapon && unit_.fire_rate.raw() != math::Fixed::scale;
        if (state.pulses_left == 0) {
            const auto hundredths = random.uniform(weapon.min_recharge_hundredths, weapon.max_recharge_hundredths);
            auto frames = hundredths * logical_frames_per_second / 100U;
            if (weapon.hardpoint == object_weapon) frames += random.uniform(0, object_recharge_jitter_frames);
            state.countdown = scaled_weapon_delay(frames, unit_.weapon_delay, true);
            state.pulses_left = weapon.pulse_count;
            if (stunned) {
                state.countdown = fire_rate_recharge(state.countdown, unit_.fire_rate);
                state.pulses_left = fire_rate_pulses(weapon.pulse_count, unit_.fire_rate);
            }
        } else {
            state.countdown = scaled_weapon_delay(weapon.pulse_delay_frames, unit_.weapon_delay, false);
            if (stunned) {
                state.countdown = fire_rate_gap(state.countdown, unit_.fire_rate);
                if (unit_.fire_rate.raw() <= 0) state.pulses_left = 0;
            }
        }
    }

    // W-10: the shot leads the target, which keeps the velocity of its last frame's move. No lead
    // without a projectile.
    [[nodiscard]] std::optional<math::Vec3> lead(
        const WeaponProfile& weapon, const CombatUnit& target, const math::Vec3& aim, const math::Vec3& muzzle) {
        if (!weapon.shot || weapon.shot->speed.raw() <= 0) return aim;
        const math::Vec3 velocity{take(math::subtract(target.position.x, target.previous_position.x)),
            take(math::subtract(target.position.y, target.previous_position.y)),
            take(math::subtract(target.position.z, target.previous_position.z))};
        return lead_point(aim, muzzle, velocity, weapon.shot->speed);
    }

    // DG-24, W-11: a projectile shot is aimed off its led point by a draw in [-r, r] on each axis,
    // r = `distance` x the inaccuracy / range. A hardpoint measures the planar distance from the
    // weapon midpoint to the aim point before the lead, the object weapon (#607) the spatial
    // distance from its muzzle to the led point against its Targeting_Max_Attack_Distance. The
    // inaccuracy is the first Fire_Inaccuracy_Distance row that names one of the target's
    // categories, doubled: in space the authored distance counts twice (#536).
    [[nodiscard]] math::Vec3 scatter(const WeaponProfile& weapon, const math::Fixed distance, const CombatUnit& target,
        const math::Vec3& point, CombatRandom& random) {
        if (!weapon.shot || weapon.range.raw() <= 0) {
            return point;
        }
        const auto row = std::find_if(weapon.shot->inaccuracy.begin(), weapon.shot->inaccuracy.end(),
            [&](const InaccuracyRow& entry) { return (entry.category_bits & categories(target)) != 0U; });
        if (row == weapon.shot->inaccuracy.end() || row->distance.raw() <= 0) {
            return point;
        }
        const auto doubled = math::Fixed::from_raw(row->distance.raw() * 2); // validated to 2^18 units, so at most 2^19
        const auto radius = take(math::divide(take(math::multiply(distance, doubled)), weapon.range));
        const auto offset = [&](const math::Fixed value) {
            return take(math::add(value, math::Fixed::from_raw(random.symmetric_raw(radius.raw()))));
        };
        const auto x = offset(point.x);
        const auto y = offset(point.y);
        const auto z = offset(point.z);
        return math::Vec3{x, y, z};
    }

    // Exact: |from - p|^2 < |from - q|^2.
    [[nodiscard]] static bool closer(const math::Vec3& from, const math::Vec3& p, const math::Vec3& q) {
        return math::detail::compare(squared(from, p, RangeMetric::spatial), squared(from, q, RangeMetric::spatial)) < 0;
    }

    // Exact squared distance: at most three squares below 2^128 each, so the 192-bit sum cannot overflow.
    [[nodiscard]] static math::detail::UInt192 squared(const math::Vec3& a, const math::Vec3& b, const RangeMetric metric) {
        math::detail::UInt192 total{};
        const auto add_square = [&](const std::int64_t left, const std::int64_t right) {
            const auto d = left >= right ? static_cast<std::uint64_t>(left) - static_cast<std::uint64_t>(right)
                                         : static_cast<std::uint64_t>(right) - static_cast<std::uint64_t>(left);
            static_cast<void>(math::detail::add_magnitude(total, math::detail::multiply_u64(d, d)));
        };
        add_square(a.x.raw(), b.x.raw());
        add_square(a.y.raw(), b.y.raw());
        if (metric == RangeMetric::spatial) add_square(a.z.raw(), b.z.raw());
        return total;
    }

    // --- hardpoint opportunity world (space-targeting R-05 to R-12) ------------------------

    class Opportunity final : public OpportunityWorld {
    public:
        Opportunity(UnitCombat& owner, const std::uint32_t slot, CombatRandom& random)
            : owner_(owner), slot_(slot), random_(random) {}

        [[nodiscard]] std::size_t player_count() const override { return owner_.world_.players.size(); }
        [[nodiscard]] PlayerRelation relation(const std::size_t index) const override {
            const auto& player = owner_.world_.players[index];
            if (player.player_id == owner_.unit_.owner) return PlayerRelation::owner;
            return player.team_id != owner_.unit_.team ? PlayerRelation::hostile : PlayerRelation::other;
        }
        [[nodiscard]] bool nebula_fogged(std::size_t) const override { return false; }
        [[nodiscard]] std::vector<OpportunityCandidate> candidates(const std::size_t index) override {
            const auto& weapon = owner_.profile_.weapons[slot_];
            // R-07: box of half extent 1.1 x range on every axis, one player's objects.
            const auto half = math::Fixed::from_raw(weapon.range.raw() * 11 / 10);
            const auto ids = owner_.world_.candidates(owner_.unit_.position, half, owner_.world_.players[index].player_id);
            std::vector<OpportunityCandidate> result;
            result.reserve(ids.size());
            for (const auto id : ids) {
                const auto* target = owner_.world_.find(id);
                OpportunityCandidate candidate;
                candidate.id = id;
                candidate.suitable = target != nullptr && target->profile != nullptr && owner_.visible_to_me(*target);
                candidate.eligible = target != nullptr && !owner_.restricted(weapon, *target);
                candidate.priority = target != nullptr ? owner_.priority(*target) : std::nullopt;
                result.push_back(candidate);
            }
            return result;
        }
        [[nodiscard]] bool aim_acceptable(const OpportunityCandidate& candidate) override {
            const auto& weapon = owner_.profile_.weapons[slot_];
            const auto* target = owner_.world_.find(candidate.id);
            if (target == nullptr) return false;
            const auto midpoint = owner_.world_point(owner_.unit_, owner_.local_midpoint(weapon));
            return owner_.choose_aim(weapon, midpoint, *target, random_, RangeMetric::spatial).has_value();
        }
        [[nodiscard]] bool attempt(const EntityId target, bool) override { return owner_.attempt(slot_, target, random_); }
        [[nodiscard]] std::uint32_t draw_player_start(const std::uint32_t count) override {
            return random_.uniform(0, count - 1U);
        }

    private:
        UnitCombat& owner_;
        std::uint32_t slot_;
        CombatRandom& random_;
    };

    // Service_Hard_Point_Weapon, one frame (docs/behaviour/space-weapon-fire.md W-rules).
    void service_weapon(const std::uint32_t slot) {
        const auto& weapon = profile_.weapons[slot];
        auto& state = state_.weapons[slot];
        // A target that has left the session is dropped at once, whatever the countdown.
        if (state.opportunity.target != invalid_entity_id && world_.find(state.opportunity.target) == nullptr) {
            state.opportunity.target = invalid_entity_id;
        }
        if (!weapon_up(weapon)) return;
        if (state.countdown != 0) {
            --state.countdown;
            if (state.countdown != 0) return;
        }
        CombatRandom random(world_.seed, world_.frame, unit_.id, slot);
        // W-06a: once its burst has begun, the object weapon spends each later pulse when its
        // countdown runs out, before it looks for a target, whether or not the shot then fires.
        const bool burst_begun = weapon.hardpoint == object_weapon && state.pulses_left != weapon.pulse_count;
        if (burst_begun) spend_pulse(weapon, state, random);
        // FT-07: an idle squadron craft holds fire; its countdowns keep running.
        if (unit_.squadron_idle) return;
        if (weapon.hardpoint == object_weapon) {
            // The unit's own weapon fires at its ship-level target. EN-05: a shot that costs
            // energy waits, with its countdown at zero and its burst unspent, until the pool holds
            // the cost; a unit without a pool never fires it.
            if (state_.attack_target == invalid_entity_id) return;
            const auto cost = weapon.shot ? weapon.shot->energy_per_shot : math::Fixed{};
            if (cost.raw() > 0) {
                if (world_.damage == nullptr || unit_.durability_profile == nullptr || unit_.durability == nullptr) return;
                auto pool = *unit_.durability;
                pool.energy = math::Fixed::from_raw(pool.energy.raw() - energy_spent_.raw());
                if (!draw_energy(*unit_.durability_profile, *world_.damage, pool, cost)) return;
            }
            if (attempt(slot, state_.attack_target, random, burst_begun)) {
                energy_spent_ = math::Fixed::from_raw(energy_spent_.raw() + cost.raw());
            }
            return;
        }
        // Hardpoints are handed the target of a player attack order only.
        const auto attack = state_.direct ? state_.attack_target : invalid_entity_id;
        if (attack != invalid_entity_id) {
            const auto* target = world_.resolve_target(world_.find(attack), unit_.position);
            if (target == nullptr || restricted(weapon, *target)) return;
        }
        if (attack == invalid_entity_id && !weapon.opportunity_when_idle) return;
        if (attack != invalid_entity_id && attempt(slot, attack, random)) return;
        if (attack != invalid_entity_id && !weapon.opportunity_when_targeting) return;
        Opportunity opportunity(*this, slot, random);
        const auto outcome = service_opportunity(world_.frame, OpportunityGates{}, state.opportunity, opportunity);
        if (outcome.acquired) {
            // The acquisition event goes before the shot that confirmed it.
            events_.insert(events_.end() - 1, CombatEvent{world_.frame, CombatEventKind::target_acquired, unit_.id,
                weapon.hardpoint, state.opportunity.target, no_hardpoint, {}, {}});
        }
    }

    // --- ship-level target choice (docs/behaviour/space-weapon-fire.md T-rules) -------------

    struct Suitability {
        bool suitable{};
        std::optional<math::Fixed> priority{};
    };

    [[nodiscard]] bool in_attack_range(const CombatUnit& target) const {
        return profile_.max_attack_distance
            && within_range(unit_.position, target.position, *profile_.max_attack_distance, RangeMetric::planar);
    }

    [[nodiscard]] Suitability ship_suitable(const CombatUnit& target) const {
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
        result.suitable = target.id == state_.attack_target || in_attack_range(target);
        return result;
    }

    [[nodiscard]] bool damaged(const CombatUnit& target) {
        if (target.durability_profile == nullptr || target.durability == nullptr) return false;
        // hull / max hull <= Health_Low_Percent_Threshold, exactly: hull <= fraction x max hull.
        const auto limit = take(math::multiply(world_.rules->damaged_fraction, target.durability_profile->max_hull));
        return target.durability->hull <= limit;
    }

    // Is_Better_Target (T-04).
    [[nodiscard]] bool better(const CombatUnit& candidate, const std::optional<math::Fixed> candidate_priority,
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

    // Scan_For_New_Target_In_Attack_Range (T-02, T-03).
    [[nodiscard]] std::pair<const CombatUnit*, std::optional<math::Fixed>> scan() {
        if (world_.frame < state_.next_scan_frame) return {nullptr, std::nullopt};
        CombatRandom random(world_.seed, world_.frame, unit_.id, ship_slot);
        state_.next_scan_frame = world_.frame + logical_frames_per_second
            + random.uniform(0, logical_frames_per_second / 2U);
        if (!profile_.max_attack_distance || profile_.max_attack_distance->raw() <= 0) return {nullptr, std::nullopt};
        const auto range = *profile_.max_attack_distance;
        const auto count = world_.players.size();
        auto index = static_cast<std::size_t>(random.uniform(0, static_cast<std::uint32_t>(count - 1U)));
        const CombatUnit* found = nullptr;
        std::optional<math::Fixed> carried;
        for (std::size_t visit = 0; visit < count; ++visit) {
            const auto& player = world_.players[index];
            index = (index + 1U) % count;
            if (player.player_id == unit_.owner || player.team_id == unit_.team) continue;
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

    // T-01: the drop runs for every unit. Without an attack distance FoC's scan finds nothing,
    // so the remake skips it (its next-scan frame is then never read).
    void ship_target() {
        if (state_.attack_target != invalid_entity_id) {
            // #424: a squadron target lasts while one of its craft does (and is seen).
            const auto* current = world_.resolve_target(world_.find(state_.attack_target), unit_.position);
            if (current == nullptr || !visible_to_me(*current)) {
                state_.attack_target = invalid_entity_id;
                state_.direct = false;
            }
        }
        // FT-07: a squadron craft takes its target only from its squadron, so without one it
        // holds none and does not scan.
        if (unit_.squadron_idle) {
            state_.attack_target = invalid_entity_id;
            state_.direct = false;
            return;
        }
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
        const auto* current = world_.find(state_.attack_target);
        const auto current_state = ship_suitable(*current);
        if (current_state.suitable && current_state.priority && current_state.priority->raw() == one_raw) return;
        const auto [found, found_priority] = scan();
        if (found == nullptr || found->id == current->id) return;
        if (current_state.suitable && !better(*found, found_priority, current, current_state.priority)) return;
        state_.attack_target = found->id;
    }

    // --- turning to bring weapons to bear (docs/behaviour/space-weapon-fire.md A-04 to A-07) ----

    // Get_Total_Hard_Points: every hardpoint of the type, weapon or not.
    [[nodiscard]] std::size_t total_hardpoints() const noexcept {
        if (unit_.durability_profile != nullptr) return unit_.durability_profile->hardpoints.size();
        const auto weapons = static_cast<std::size_t>(std::count_if(profile_.weapons.begin(), profile_.weapons.end(),
            [](const WeaponProfile& weapon) { return weapon.hardpoint != object_weapon; }));
        return std::max(profile_.hardpoints.size(), weapons);
    }

    // Get_Max_Firepower_Z_Angle_Adjust (A-06): 0 to face the target, +90 to turn the right side
    // toward it, -90 the left, by the AI combat power each side's live weapon hardpoints bring.
    // `delta` is the bearing minus the unit's yaw, wrapped to [-180, 180).
    [[nodiscard]] std::int64_t firepower_adjust(motion_detail::Calc& calc, const math::Fixed delta) {
        if (total_hardpoints() == 0) return 0;
        math::Fixed forward{};
        math::Fixed left{};
        math::Fixed right{};
        for (const auto& weapon : profile_.weapons) {
            if (weapon.hardpoint == object_weapon || !weapon_up(weapon) || !weapon.shot || !weapon.shot->hitpoint_damage) {
                continue;
            }
            // The XY direction of Fire_Bone_A's x axis in the unit's frame (+X ahead, +Y left);
            // without a planar part FoC's facing has yaw 0.
            auto x = math::Fixed::from_raw(one_raw);
            auto y = math::Fixed{};
            if (weapon.fire_axes && ((*weapon.fire_axes)[0].x.raw() != 0 || (*weapon.fire_axes)[0].y.raw() != 0)) {
                x = (*weapon.fire_axes)[0].x;
                y = (*weapon.fire_axes)[0].y;
            }
            const auto planar = calc.length(x, y);
            const auto half = std::clamp<std::int64_t>(weapon.cone_width.raw(), 0, 180 * one_raw) / 2;
            // A direction is covered when the vector's cosine to it is at least cos(half the cone).
            const auto limit = calc.mul(calc.cos_deg(math::Fixed::from_raw(half)), planar);
            if (x >= limit) forward = calc.add(forward, weapon.ai_combat_power);
            if (calc.neg(y) >= limit) right = calc.add(right, weapon.ai_combat_power);
            if (y >= limit) left = calc.add(left, weapon.ai_combat_power);
        }
        std::int64_t adjust = 0;
        auto best = forward;
        if (forward < right) {
            adjust = 90;
            best = right;
        }
        if (left > best) {
            adjust = -90;
        } else if (left == best && adjust != 0) {
            const auto other = calc.abs(motion_detail::clamp180(calc.sub(motion_detail::whole(90), delta)));
            if (other < calc.abs(delta)) adjust = -90;
        }
        return adjust;
    }

    // Service_Space_Based_Targeting's facing branch (A-04 to A-07; research AT-01 to AT-13): a
    // unit at rest whose player-ordered target is within its attack distance turns in place
    // toward the target's nearest live hardpoint, adjusted for firepower, when that heading lies
    // more than 10 degrees from its own. FoC's Auto_Rotate_For_Space_Targeting is false, so
    // only an attack order (or a type without hardpoints) turns a unit.
    void face_target() {
        if (!unit_.can_turn || state_.attack_target == invalid_entity_id || !profile_.max_attack_distance) return;
        if (!state_.direct && total_hardpoints() != 0) return;
        const auto* target = world_.resolve_target(world_.find(state_.attack_target), unit_.position);
        if (target == nullptr) return;
        auto aim = target->position;
        // HO-08 (OR-25): a unit ordered to attack a hardpoint turns to that hardpoint.
        const auto* ordered = state_.direct ? standing_hardpoint(*target, state_.attack_hardpoint) : nullptr;
        if (ordered != nullptr) aim = world_point(*target, ordered->position);
        if (ordered == nullptr && target->profile != nullptr) {
            bool found = false;
            for (const auto& hardpoint : target->profile->hardpoints) {
                if (!hardpoint.targetable || hardpoint_destroyed_on(*target, hardpoint.hardpoint)) continue;
                const auto point = world_point(*target, hardpoint.position);
                if (!found || closer(unit_.position, point, aim)) {
                    aim = point;
                    found = true;
                }
            }
        }
        // Is_Close_Enough_To_Attack: the planar distance to the aim point; the target's soft
        // radius and hard extents are not loaded and count as zero (P-04).
        if (!within_range(unit_.position, aim, *profile_.max_attack_distance, RangeMetric::planar)) return;
        motion_detail::Calc calc;
        const auto dx = calc.sub(aim.x, unit_.position.x);
        const auto dy = calc.sub(aim.y, unit_.position.y);
        if (!calc.ok() || (dx.raw() == 0 && dy.raw() == 0)) return;
        const auto& m = unit_.transform.rows;
        const auto yaw = calc.atan2_deg(m[1][0], m[0][0]);
        const auto bearing = calc.atan2_deg(dy, dx);
        const auto adjust = firepower_adjust(calc, motion_detail::clamp180(calc.sub(bearing, yaw)));
        const auto heading = calc.add(bearing, motion_detail::whole(adjust));
        const auto off = calc.abs(motion_detail::clamp180(calc.sub(yaw, heading)));
        if (!calc.ok()) {
            if (!error_) error_ = calc.error("attack turn");
            return;
        }
        if (off <= motion_detail::whole(10)) return;
        // The point the turn faces: the aim point, or it turned by the adjustment about the unit.
        auto face_x = dx;
        auto face_y = dy;
        if (adjust == 90) {
            face_x = calc.neg(dy);
            face_y = dx;
        } else if (adjust == -90) {
            face_x = dy;
            face_y = calc.neg(dx);
        }
        face_ = math::Vec3{calc.add(unit_.position.x, face_x), calc.add(unit_.position.y, face_y), unit_.position.z};
        if (!calc.ok()) {
            if (!error_) error_ = calc.error("attack turn");
            face_.reset();
        }
    }

private:
    const CombatWorld& world_;
    const CombatUnit& unit_;
    const CombatProfile& profile_;
    CombatState state_;
    std::vector<CombatEvent> events_;
    std::optional<math::Vec3> face_;
    math::Fixed energy_spent_{};
    std::vector<math::Vec3> aim_offsets_;
    std::optional<core::Diagnostic> error_;
};

} // namespace

math::Vec3 ordered_aim_point(const CombatUnit& target, const math::Vec3& from, const std::uint32_t ordered_index) {
    auto aim = target.position;
    if (target.profile == nullptr) return aim;
    if (const auto* ordered = standing_hardpoint(target, ordered_index)) {
        if (auto point = math::transform_point(target.transform, ordered->position)) return point.value();
    }
    bool found = false;
    for (const auto& hardpoint : target.profile->hardpoints) {
        if (!hardpoint.targetable) continue;
        if (target.durability_profile != nullptr && target.durability != nullptr
            && hardpoint.hardpoint < target.durability_profile->hardpoints.size()
            && hardpoint_destroyed(*target.durability_profile, *target.durability, hardpoint.hardpoint)) {
            continue;
        }
        auto point = math::transform_point(target.transform, hardpoint.position);
        if (!point) continue;
        if (!found || UnitCombat::closer(from, point.value(), aim)) {
            aim = point.value();
            found = true;
        }
    }
    return aim;
}

core::Result<CombatStep> step_combat(const CombatWorld& world, const CombatUnit& unit) {
    if (unit.profile == nullptr || unit.combat == nullptr) {
        return core::Result<CombatStep>::success(CombatStep{});
    }
    return UnitCombat(world, unit).run();
}

} // namespace detail
} // namespace eawr::sim::tactical
