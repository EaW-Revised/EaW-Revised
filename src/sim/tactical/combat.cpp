#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "motion_internal.hpp"
#include "rocket_internal.hpp"
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
    if (!std::is_sorted(table.pad_neutral_factions.begin(), table.pad_neutral_factions.end())
        || std::adjacent_find(table.pad_neutral_factions.begin(), table.pad_neutral_factions.end())
            != table.pad_neutral_factions.end()) {
        return invalid("neutral capture factions must be strictly ascending");
    }
    const auto distance_ok = [](const math::Fixed value) {
        return value.raw() >= 0 && value.raw() <= max_combat_distance * one_raw;
    };
    const auto cone_ok = [](const math::Fixed value) { return value.raw() >= 0 && value.raw() <= 720 * one_raw; };
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
        if (!distance_ok(profile.passive_missile_shield_radius)
            || std::llabs(profile.ranged_target_z_adjust.raw()) > max_combat_distance * one_raw) {
            return invalid(name + "projectile defence radius or height out of range");
        }
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
                || !distance_ok(weapon.manual_min_range) || weapon.manual_min_range > weapon.range
                || weapon.manual_cooldown_frames > 108000
                || weapon.ai_combat_power.raw() < 0 || weapon.ai_combat_power.raw() > max_weapon_combat_power * one_raw) {
                return invalid(name + "invalid weapon " + std::to_string(weapon.hardpoint));
            }
            if (weapon.manual_turret) {
                const auto& turret = *weapon.manual_turret;
                const auto angles_ok = [&](const math::Vec3& angles) {
                    return within_distance(angles.x, 720) && within_distance(angles.y, 720)
                        && within_distance(angles.z, 720);
                };
                if (!weapon.requires_manual_target || !point_in_bounds(turret.pivot)
                    || !point_in_bounds(turret.coordinate_pivot) || !axes_ok(turret.axes)
                    || !axes_ok(turret.coordinate_axes) || !angles_ok(turret.rest) || !angles_ok(turret.offset)
                    || !cone_ok(turret.speed) || !cone_ok(turret.yaw_extent) || !cone_ok(turret.pitch_extent)
                    || (turret.barrel && (!point_in_bounds(turret.barrel_pivot) || !axes_ok(turret.barrel_axes)))) {
                    return invalid(name + "invalid manual turret");
                }
            }
            for (const auto* candidate : {&weapon.shot, &weapon.ability_shot, &weapon.barrage_shot}) {
                if (!*candidate) continue;
                const auto& shot = **candidate;
                const auto damage_ok = shot.damage.raw() >= 0 && shot.damage.raw() <= max_durability_health * one_raw;
                const auto& blast = shot.blast;
                const auto blast_ok = blast.damage.raw() >= 0 && blast.damage.raw() <= max_durability_health * one_raw
                    && distance_ok(blast.radius) && blast.tiers >= 0 && blast.tiers <= 1024
                    && blast.max_delay.raw() >= 0 && blast.max_delay.raw() <= 3600 * one_raw;
                const auto inaccuracy_ok = std::all_of(shot.inaccuracy.begin(), shot.inaccuracy.end(),
                    [&](const InaccuracyRow& row) { return distance_ok(row.distance); });
                if (shot.damage_delay.raw() > 3600 * one_raw || shot.appearance_delay_frames > 108000 || !damage_ok || !blast_ok || shot.speed.raw() <= 0 || !distance_ok(shot.speed) || !distance_ok(shot.max_travel)
                    || !cone_ok(shot.turn_rate)
                    || !inaccuracy_ok || (shot.ion_stun && !valid_ion_stun(*shot.ion_stun))
                    || (shot.disable_engines_frames && *shot.disable_engines_frames > 30U * 3600U)

                    || (shot.flight && (!detail::valid_flight(*shot.flight)
                        || (shot.homing && shot.flight->kind == FlightKind::rocket)))) {
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
        if (profile.hardpoint_meshes.size() > max_hardpoints_per_type
            || profile.aimed_routes.size() > max_hardpoints_per_type
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
    CombatTable death_payloads;
    for (const auto& profile : table.profiles) {
        if (!profile.death_projectiles.empty()) {
            if (!within_distance(profile.ranged_target_z_adjust, max_combat_distance))
                return invalid("death projectile height adjustment out of range");
            CombatProfile payload;
            payload.type_id = profile.type_id;
            for (const auto& shot : profile.death_projectiles) {
                if (meshes && shot.speed.raw() > max_mesh_extent * one_raw)
                    return invalid("death projectile speed exceeds collision mesh bound");
                WeaponProfile weapon;
                weapon.shot = shot;
                payload.weapons.push_back(std::move(weapon));
            }
            death_payloads.profiles.push_back(std::move(payload));
        }
        for (const auto& weapon : profile.weapons) {
            if (meshes && weapon.shot && weapon.shot->speed.raw() > max_mesh_extent * one_raw) {
                return invalid("type " + std::to_string(profile.type_id) + ": a shot faster than "
                    + std::to_string(max_mesh_extent) + " units a frame with collision meshes");
            }
        }
    }
    // WNO-29: death payloads pass the same projectile validation as ordinary weapons.
    return death_payloads.profiles.empty() ? core::Result<void>::success() : validate_combat(death_payloads);
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
        if (weapon.requires_manual_target) {
            weapon_state.manual.emplace();
            if (weapon.manual_turret) {
                auto& manual = *weapon_state.manual;
                manual.yaw = manual.desired_yaw = weapon.manual_turret->rest.z;
                manual.pitch = manual.desired_pitch = weapon.manual_turret->rest.y;
            }
        }
        state.weapons.push_back(weapon_state);
    }
    return state;
}

std::string_view to_string(const CombatEventKind kind) noexcept {
    switch (kind) {
    case CombatEventKind::target_acquired: return "target_acquired";
    case CombatEventKind::weapon_fired: return "weapon_fired";
    case CombatEventKind::projectile_hit: return "projectile_hit";
    case CombatEventKind::projectile_expired: return "projectile_expired";
    }
    return "unknown";
}

namespace detail {

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


} // namespace detail
} // namespace eawr::sim::tactical
