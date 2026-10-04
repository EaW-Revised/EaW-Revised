#include "eawr/assets/map.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <string>
#include <utility>

// The #73 combat table: weapons, target points, categories and resolved priority sets in the
// shape the tactical session binds (docs/behaviour/space-weapon-fire.md).
namespace eawr::units {
namespace {

namespace tactical = sim::tactical;

[[nodiscard]] core::Diagnostic failure(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::combat);
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool same_name(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [](const unsigned char a, const unsigned char b) {
               return std::tolower(a) == std::tolower(b);
           });
}

[[nodiscard]] bool is_weapon(const HardpointType type) noexcept {
    return type == HardpointType::weapon_laser || type == HardpointType::weapon_missile
        || type == HardpointType::weapon_torpedo || type == HardpointType::weapon_ion_cannon
        || type == HardpointType::weapon_mass_driver || type == HardpointType::weapon_special;
}

// trunc(value x factor) as a whole number, clamped at zero. The authored decimal is the value:
// its Q24 rounding (at most half a raw unit, times the factor) must not truncate a whole product
// one lower, as 0.2 x 30 would (retail's binary32 0.2 x 30 gives 6; S-15 fires 6 frames apart).
[[nodiscard]] core::Result<std::uint32_t> whole(const Fixed value, const std::int64_t factor, const std::string& what) {
    auto product = sim::math::multiply(value, Fixed::from_raw(factor * Fixed::scale));
    if (!product) return core::Result<std::uint32_t>::failure(failure(what + ": " + product.error().message));
    const auto tolerance = factor / 2 + 1;
    const auto truncated = Fixed::from_raw(product.value().raw() + (product.value().raw() >= 0 ? tolerance : 0))
                               .trunc_to_integer();
    if (truncated < 0 || truncated > std::int64_t{0xffffffff}) {
        return core::Result<std::uint32_t>::failure(failure(what + " is out of range"));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(truncated));
}

} // namespace

core::Result<tactical::CombatTable> combat_table(const UnitTables& tables) {
    using Result = core::Result<tactical::CombatTable>;
    const auto category_bits = [&tables](const std::vector<std::string>& names) -> core::Result<std::uint64_t> {
        std::uint64_t bits = 0;
        for (const auto& name : names) {
            const auto found = std::find_if(tables.categories.begin(), tables.categories.end(),
                [&](const EnumValue& value) { return same_name(value.name, name); });
            if (found == tables.categories.end()) {
                return core::Result<std::uint64_t>::failure(failure("unknown category '" + name + "'"));
            }
            bits |= found->value;
        }
        return core::Result<std::uint64_t>::success(bits);
    };

    const auto types = damage_type_index(tables);
    // The projectile a weapon fires (#74, DG-21 to DG-25): its damage (a hardpoint's own positive
    // damage before the projectile's), speed and flags; the weapon's own Damage_Type before the
    // projectile's; a hardpoint's range limits its travel.
    const auto shot_profile = [&](const Weapon& weapon, const bool hardpoint) -> core::Result<std::optional<tactical::ShotProfile>> {
        using ShotResult = core::Result<std::optional<tactical::ShotProfile>>;
        if (weapon.projectile_index == no_index || weapon.projectile_index >= tables.projectiles.size()) {
            return ShotResult::success(std::nullopt);
        }
        const auto& projectile = tables.projectiles[weapon.projectile_index];
        if (!projectile.max_speed || projectile.max_speed->raw() <= 0) return ShotResult::success(std::nullopt);
        tactical::ShotProfile shot;
        shot.damage = weapon.damage && weapon.damage->raw() > 0 ? *weapon.damage : projectile.damage.value_or(Fixed{});
        shot.blast = projectile.blast; // WAD-02/36: independent of instance direct damage
        shot.damage_type = types.damage(!weapon.damage_type.empty() ? weapon.damage_type : projectile.damage_type);
        shot.speed = *projectile.max_speed;
        shot.max_travel = hardpoint && weapon.range && weapon.range->raw() > 0
            ? *weapon.range
            : projectile.max_flight_distance.value_or(Fixed{});
        shot.shield_damage = projectile.does_shield_damage;
        shot.hitpoint_damage = projectile.does_hitpoint_damage;
        shot.energy_damage = projectile.does_energy_damage; // EN-07
        if (projectile.disables_engines_when_power_drained) {
            // EN-08: reject unsupported negative (indefinite) durations instead of inventing a timeout.
            const auto seconds = projectile.disable_engines_duration;
            if (!seconds || seconds->raw() < 0 || seconds->raw() > 3600 * Fixed::scale) {
                return ShotResult::failure(failure("projectile '" + projectile.id + "' has an invalid engine-disable duration (EN-08)"));
            }
            auto frames = whole(*seconds, 30, projectile.id + " Projectile_Disable_Engines_Duration");
            if (!frames) return ShotResult::failure(frames.error());
            shot.disable_engines_frames = frames.value();
        }
        // IS-01: a stun on detonation. IS-08: a stun with a radius is not modelled (no M2
        // projectile authors one); the loader refuses it rather than stun only the unit hit.
        if (projectile.ion_stun) {
            if (projectile.ion_stun_radius && projectile.ion_stun_radius->raw() != 0) {
                return ShotResult::failure(failure("projectile '" + projectile.id + "' has an ion stun radius (IS-08)"));
            }
            tactical::IonStunShot stun;
            const auto seconds = projectile.ion_stun_duration.value_or(Fixed{});
            stun.frames = static_cast<std::uint32_t>(std::max<std::int64_t>(seconds.raw(), 0) * 30 / Fixed::scale);
            stun.speed_reduction = projectile.ion_stun_speed_reduction.value_or(Fixed{});
            stun.rate_reduction = projectile.ion_stun_shot_rate_reduction.value_or(Fixed{});
            stun.stack = projectile.ion_stun_stack_duration;
            if (!tactical::valid_ion_stun(stun)) {
                return ShotResult::failure(failure("projectile '" + projectile.id + "' has an invalid ion stun (IS-01)"));
            }
            shot.ion_stun = stun;
        }
        // EN-05, EN-06: only the object weapon draws energy per shot.
        if (!hardpoint) shot.energy_per_shot = projectile.energy_per_shot.value_or(Fixed{});
        // MS-01: a MISSILE-category projectile homes, turning at its Max_Rate_Of_Turn.
        shot.homing = same_name(projectile.category, "MISSILE");
        if (shot.homing) shot.turn_rate = projectile.max_rate_of_turn.value_or(Fixed{});
        const bool rocket = same_name(projectile.category, "ROCKET");
        if (rocket || projectile.max_lifetime || projectile.explode_at_target_radius.value_or(false)) {
            tactical::FlightProfile flight;
            flight.kind = rocket ? tactical::FlightKind::rocket : same_name(projectile.category, "DEFAULT")
                ? tactical::FlightKind::default_projectile : tactical::FlightKind::ordinary;
            flight.target_radius = projectile.explode_at_target_radius.value_or(false);
            flight.lifetime = projectile.max_lifetime;
            flight.authored_distance = projectile.max_flight_distance.value_or(Fixed{});
            if (rocket) {
                if (!projectile.rocket_curve_distance || !projectile.rocket_curve_offset || !projectile.rocket_straight_distance) {
                    return ShotResult::failure(failure("projectile '" + projectile.id + "' lacks authored rocket path inputs (RFL-02/03)"));
                }
                flight.curve_distance = *projectile.rocket_curve_distance;
                flight.curve_offset = *projectile.rocket_curve_offset;
                flight.straight_distance = *projectile.rocket_straight_distance;
            }
            shot.flight = flight;
        }
        shot.appearance_delay_frames = weapon.appearance_delay_frames.value_or(0);
        for (const auto& row : weapon.inaccuracy) {
            auto bits = category_bits({row.category});
            if (!bits) return ShotResult::failure(bits.error());
            shot.inaccuracy.push_back({bits.value(), row.distance});
        }
        return ShotResult::success(std::move(shot));
    };

    // AB-66 (#561): the craft of a squadron whose team container has ION_CANNON_SHOT fire its
    // override projectile from their hardpoints while it is on; craft unit index -> projectile.
    std::map<std::size_t, std::uint32_t> ion_override;
    for (const auto& squadron : tables.units) {
        if (squadron.kind != UnitKind::squadron) continue;
        for (const auto& ability : squadron.team_abilities) {
            if (!same_name(ability.type, "ION_CANNON_SHOT") || ability.projectile_index == no_index) continue;
            for (const auto& member : squadron.members) {
                if (member.craft_index != no_index) ion_override.emplace(member.craft_index, ability.projectile_index);
            }
        }
    }

    tactical::CombatTable table;
    table.pad_neutral_factions = tables.pad_neutral_factions;
    std::map<std::uint32_t, std::uint32_t> set_index; // unit tables' set index -> combat table's
    for (std::size_t unit_index = 0; unit_index < tables.units.size(); ++unit_index) {
        const auto& unit = tables.units[unit_index];
        if (unit.kind == UnitKind::squadron) continue; // squadrons act through their craft (#75)
        const auto override_projectile = ion_override.find(unit_index);
        std::optional<std::uint32_t> barrage_projectile;
        for (const auto& ability : unit.abilities) {
            if (same_name(ability.type, "BARRAGE") && ability.projectile_index != no_index) {
                barrage_projectile = ability.projectile_index;
            }
        }
        const auto scale = unit.scale_factor.value_or(Fixed::from_raw(Fixed::scale));
        // A model point in the unit's frame: scaled by Scale_Factor, then turned by FoC's fixed
        // +90 degrees about Z (R-ROT-01, R-ROT-04: ship noses lie on model -Y, the unit faces +X),
        // so (x, y, z) becomes (-y, x, z).
        const auto place = [&](const Vec3& point) -> core::Result<Vec3> {
            auto x = sim::math::multiply(point.x, scale);
            auto y = sim::math::multiply(point.y, scale);
            auto z = sim::math::multiply(point.z, scale);
            if (!x || !y || !z) return core::Result<Vec3>::failure(failure(unit.id + ": scaled point overflows"));
            return core::Result<Vec3>::success(Vec3{Fixed::from_raw(-y.value().raw()), x.value(), z.value()});
        };
        tactical::CombatProfile profile;
        profile.type_id = assets::object_type_crc(unit.id);
        profile.living_projectile_collision = unit.living_projectile_collision;
        profile.capture_point = unit.capture_point;
        profile.category_bits = unit.category_bits;
        profile.hero = unit.named_hero || unit.generic_hero;
        profile.redirect_damage_to_teammates = unit.redirect_damage_to_teammates;
        profile.max_attack_distance = unit.targeting_max_attack_distance;
        profile.min_attack_distance = unit.targeting_min_attack_distance.value_or(Fixed{});
        if (unit.targeting_priority_set_index != no_index) {
            const auto [entry, inserted] =
                set_index.emplace(unit.targeting_priority_set_index, static_cast<std::uint32_t>(set_index.size()));
            static_cast<void>(inserted);
            profile.priority_set = entry->second;
        }
        const auto weapon_profile = [&](const Weapon& weapon, const std::uint32_t hardpoint,
                                        const std::string& what) -> core::Result<tactical::WeaponProfile> {
            using WeaponResult = core::Result<tactical::WeaponProfile>;
            tactical::WeaponProfile entry;
            entry.hardpoint = hardpoint;
            auto minimum = whole(weapon.min_recharge_seconds.value_or(Fixed{}), 100, what + " recharge");
            auto maximum = whole(weapon.max_recharge_seconds.value_or(Fixed{}), 100, what + " recharge");
            auto delay = whole(weapon.pulse_delay_seconds.value_or(Fixed{}), tactical::logical_frames_per_second,
                what + " pulse delay");
            auto restrictions = category_bits(weapon.category_restrictions);
            if (!minimum) return WeaponResult::failure(minimum.error());
            if (!maximum) return WeaponResult::failure(maximum.error());
            if (!delay) return WeaponResult::failure(delay.error());
            if (!restrictions) return WeaponResult::failure(restrictions.error());
            entry.min_recharge_hundredths = minimum.value();
            entry.max_recharge_hundredths = std::max(minimum.value(), maximum.value());
            entry.pulse_count = std::max<std::uint32_t>(1, weapon.pulse_count.value_or(1));
            entry.pulse_delay_frames = delay.value();
            entry.category_restrictions = restrictions.value();
            entry.opportunity_when_idle = weapon.opportunity_fire_when_idle;
            entry.opportunity_when_targeting = weapon.opportunity_fire_when_targeting;
            return WeaponResult::success(entry);
        };
        // Compute_AI_Combat_Power (A-06, research AT-09): a weapon hardpoint's share of the type's
        // AI_Combat_Power is its projectile's AI_Combat_Power over the sum of all its weapon
        // hardpoints' projectiles'.
        const auto projectile_power = [&](const Hardpoint& hardpoint) {
            if (!is_weapon(hardpoint.type) || !hardpoint.weapon || hardpoint.weapon->projectile_index == no_index
                || hardpoint.weapon->projectile_index >= tables.projectiles.size()) {
                return Fixed{};
            }
            return tables.projectiles[hardpoint.weapon->projectile_index].ai_combat_power.value_or(Fixed{});
        };
        Fixed power_sum{};
        for (const auto& hardpoint : unit.hardpoints) {
            auto sum = sim::math::add(power_sum, projectile_power(hardpoint));
            if (!sum) return Result::failure(failure(unit.id + " AI combat power: " + sum.error().message));
            power_sum = sum.value();
        }
        for (std::uint32_t index = 0; index < unit.hardpoints.size(); ++index) {
            const auto& hardpoint = unit.hardpoints[index];
            profile.hardpoint_meshes.push_back(hardpoint.collision_mesh);
            if (hardpoint.attachment.position) {
                auto position = place(*hardpoint.attachment.position);
                if (!position) return Result::failure(position.error());
                profile.hardpoints.push_back({index, position.value(), hardpoint.targetable});
            }
            if (!is_weapon(hardpoint.type) || !hardpoint.weapon) continue;
            const auto& weapon = *hardpoint.weapon;
            auto entry = weapon_profile(weapon, index, unit.id + " " + hardpoint.id);
            if (!entry) return Result::failure(entry.error());
            entry.value().range = weapon.range.value_or(Fixed{});
            entry.value().cone_width = weapon.cone_width_degrees.value_or(Fixed{});
            entry.value().cone_height = weapon.cone_height_degrees.value_or(Fixed{});
            entry.value().special = hardpoint.type == HardpointType::weapon_special;
            entry.value().requires_manual_target = hardpoint.requires_manual_target;
            if (hardpoint.requires_manual_target) {
                entry.value().manual_turret_required = hardpoint.manual_is_turret;
                const auto seconds = hardpoint.manual_cooldown_seconds.value_or(Fixed{});
                auto frames = sim::math::multiply(seconds, Fixed::from_raw(30 * Fixed::scale));
                if (!frames || frames.value().raw() < 0 || frames.value().raw() > 108000LL * Fixed::scale) {
                    return Result::failure(failure(unit.id + " manual cooldown out of range"));
                }
                // WAD-39: the player clock stores nearest whole frames, independently of weapon recharge.
                entry.value().manual_cooldown_frames = static_cast<std::uint32_t>(
                    (frames.value().raw() + Fixed::scale / 2) / Fixed::scale);
                if (hardpoint.manual_is_turret && hardpoint.manual_turret.position && hardpoint.manual_turret.axes
                    && hardpoint.attachment.position && hardpoint.attachment.axes) {
                    tactical::ManualTurretProfile turret;
                    auto pivot = place(*hardpoint.manual_turret.position);
                    if (!pivot) return Result::failure(pivot.error());
                    turret.pivot = pivot.value();
                    auto coordinate_pivot = place(*hardpoint.attachment.position);
                    if (!coordinate_pivot) return Result::failure(coordinate_pivot.error());
                    turret.coordinate_pivot = coordinate_pivot.value();
                    bool usable = true;
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const auto& value = (*hardpoint.attachment.axes)[axis];
                        auto turned = sim::math::normalize(Vec3{Fixed::from_raw(-value.y.raw()), value.x, value.z});
                        if (!turned) { usable = false; break; }
                        turret.coordinate_axes[axis] = turned.value();
                    }
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const auto& value = (*hardpoint.manual_turret.axes)[axis];
                        auto turned = sim::math::normalize(Vec3{Fixed::from_raw(-value.y.raw()), value.x, value.z});
                        if (!turned) { usable = false; break; }
                        turret.axes[axis] = turned.value();
                    }
                    turret.rest = hardpoint.manual_rest;
                    turret.offset = hardpoint.manual_offset;
                    turret.speed = hardpoint.manual_rotate_speed.value_or(Fixed{});
                    turret.yaw_extent = hardpoint.manual_yaw_extent.value_or(Fixed{});
                    turret.pitch_extent = hardpoint.manual_pitch_extent.value_or(Fixed{});
                    turret.barrel = hardpoint.manual_barrel.position.has_value() && hardpoint.manual_barrel.axes.has_value();
                    if (turret.barrel) {
                        auto barrel_pivot = place(*hardpoint.manual_barrel.position);
                        if (!barrel_pivot) return Result::failure(barrel_pivot.error());
                        turret.barrel_pivot = barrel_pivot.value();
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            const auto& value = (*hardpoint.manual_barrel.axes)[axis];
                            auto turned = sim::math::normalize(Vec3{Fixed::from_raw(-value.y.raw()), value.x, value.z});
                            if (!turned) { usable = false; break; }
                            turret.barrel_axes[axis] = turned.value();
                        }
                    }
                    if (usable) entry.value().manual_turret = turret;
                }
            }
            if (power_sum.raw() > 0 && unit.ai_combat_power) {
                auto share = sim::math::divide(projectile_power(hardpoint), power_sum);
                auto power = share ? sim::math::multiply(share.value(), *unit.ai_combat_power) : share;
                if (!power) return Result::failure(failure(unit.id + " " + hardpoint.id + " AI combat power: "
                                                           + power.error().message));
                entry.value().ai_combat_power = std::max(power.value(), Fixed{});
            }
            auto shot = shot_profile(weapon, true);
            if (!shot) return Result::failure(shot.error());
            entry.value().shot = std::move(shot).value();
            if (barrage_projectile) {
                auto swapped = weapon;
                swapped.projectile_index = *barrage_projectile;
                auto barrage_shot = shot_profile(swapped, true);
                if (!barrage_shot) return Result::failure(barrage_shot.error());
                entry.value().barrage_shot = std::move(barrage_shot).value();
            }
            if (override_projectile != ion_override.end()) {
                // The override keeps the hardpoint's damage type and range (DG-12, DG-23).
                auto swapped = weapon;
                swapped.projectile_index = override_projectile->second;
                auto ability_shot = shot_profile(swapped, true);
                if (!ability_shot) return Result::failure(ability_shot.error());
                entry.value().ability_shot = std::move(ability_shot).value();
            }
            // Fire bones fall back to the attachment bone, then to the unit's origin.
            const auto& fire_bone = hardpoint.fire_a.position ? hardpoint.fire_a : hardpoint.attachment;
            if (fire_bone.position) {
                auto placed = place(*fire_bone.position);
                if (!placed) return Result::failure(placed.error());
                entry.value().fire_a = placed.value();
            }
            // Its bind frame orients the cone (W-07): each axis turned like a point, unit length.
            if (fire_bone.axes) {
                std::array<Vec3, 3> turned{};
                bool usable = true;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto& value = (*fire_bone.axes)[axis];
                    auto unit_axis = sim::math::normalize(Vec3{Fixed::from_raw(-value.y.raw()), value.x, value.z});
                    if (!unit_axis) {
                        usable = false;
                        break;
                    }
                    turned[axis] = unit_axis.value();
                }
                if (usable) entry.value().fire_axes = turned;
            }
            entry.value().fire_b = entry.value().fire_a;
            if (!hardpoint.fire_b.bone.empty() && hardpoint.fire_b.position) {
                auto placed = place(*hardpoint.fire_b.position);
                if (!placed) return Result::failure(placed.error());
                entry.value().fire_b = placed.value();
                entry.value().has_fire_b = true;
            }
            profile.weapons.push_back(entry.value());
        }
        if (unit.weapon) {
            auto entry = weapon_profile(*unit.weapon, tactical::object_weapon, unit.id + " object weapon");
            if (!entry) return Result::failure(entry.error());
            entry.value().range = unit.targeting_max_attack_distance.value_or(Fixed{});
            // W-09: the turret extents, both authored, else no cone.
            if (unit.weapon->cone_width_degrees && unit.weapon->cone_height_degrees) {
                entry.value().cone_width = *unit.weapon->cone_width_degrees;
                entry.value().cone_height = *unit.weapon->cone_height_degrees;
            }
            auto shot = shot_profile(*unit.weapon, false);
            if (!shot) return Result::failure(shot.error());
            entry.value().shot = std::move(shot).value();
            if (barrage_projectile) {
                auto swapped = *unit.weapon;
                swapped.projectile_index = *barrage_projectile;
                auto barrage_shot = shot_profile(swapped, false);
                if (!barrage_shot) return Result::failure(barrage_shot.error());
                entry.value().barrage_shot = std::move(barrage_shot).value();
            }
            profile.weapons.push_back(entry.value());
        }
        if (unit.collision) {
            // The turned box: x from -max.y to -min.y, y from min.x to max.x.
            auto low = place(unit.collision->min);
            auto high = place(unit.collision->max);
            if (!low || !high) return Result::failure(failure(unit.id + ": scaled collision box overflows"));
            const auto& a = low.value();
            const auto& b = high.value();
            profile.collision = tactical::CollisionBox{Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)},
                Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}};
        }
        // #536 (DG-36 to DG-38): the collidable meshes, placed like the box. A mesh named as a
        // hardpoint's Collision_Mesh damages that hardpoint (the first such hardpoint in HardPoints
        // order); the mesh named "shield" is the shield mesh.
        if (unit.collision && !unit.collision_meshes.empty()) {
            std::optional<tactical::CollisionBox> bounds;
            for (const auto& mesh : unit.collision_meshes) {
                std::vector<tactical::CollisionTriangle> triangles;
                triangles.reserve(mesh.triangles.size());
                for (const auto& corners : mesh.triangles) {
                    std::array<Vec3, 3> placed{};
                    for (std::size_t corner = 0; corner < 3; ++corner) {
                        auto point = place(corners[corner]);
                        if (!point) return Result::failure(point.error());
                        placed[corner] = point.value();
                        const auto& p = placed[corner];
                        if (!bounds) {
                            bounds = tactical::CollisionBox{p, p};
                        } else {
                            bounds->min = {std::min(bounds->min.x, p.x), std::min(bounds->min.y, p.y), std::min(bounds->min.z, p.z)};
                            bounds->max = {std::max(bounds->max.x, p.x), std::max(bounds->max.y, p.y), std::max(bounds->max.z, p.z)};
                        }
                    }
                    triangles.push_back({placed[0], placed[1], placed[2]});
                }
                std::uint32_t damaged = tactical::no_hardpoint;
                for (std::uint32_t index = 0; index < unit.hardpoints.size(); ++index) {
                    if (!unit.hardpoints[index].collision_mesh.empty()
                        && same_name(unit.hardpoints[index].collision_mesh, mesh.name)) {
                        damaged = index;
                        break;
                    }
                }
                const auto source = mesh.hardpoint == no_index ? tactical::no_hardpoint : mesh.hardpoint;
                profile.meshes.push_back(
                    tactical::collision_mesh(std::move(triangles), damaged, source, same_name(mesh.name, "shield")));
            }
            profile.mesh_bounds = bounds;
            // #669 (DG-39): a shot aimed at a hardpoint damages the hardpoint its Collision_Mesh
            // names, whichever mesh it met.
            profile.aimed_routes.assign(unit.hardpoints.size(), tactical::no_hardpoint);
            for (std::uint32_t aimed = 0; aimed < unit.hardpoints.size(); ++aimed) {
                const auto& name = unit.hardpoints[aimed].collision_mesh;
                if (name.empty()) continue;
                for (std::uint32_t index = 0; index < unit.hardpoints.size(); ++index) {
                    if (same_name(unit.hardpoints[index].collision_mesh, name)) {
                        profile.aimed_routes[aimed] = index;
                        break;
                    }
                }
            }
            if (unit.collision_box_modifier && unit.collision_box_modifier->raw() > Fixed::scale && profile.collision) {
                if (unit.collision_box_modifier->raw() > tactical::max_sphere_modifier * Fixed::scale) {
                    return Result::failure(failure(unit.id + ": Collision_Box_Modifier above the limit"));
                }
                profile.sphere_modifier = *unit.collision_box_modifier;
            }
        }
        for (const auto& bone : unit.target_bones) {
            if (!bone.position) continue;
            auto placed = place(*bone.position);
            if (!placed) return Result::failure(placed.error());
            profile.target_bones.push_back(placed.value());
        }
        table.profiles.push_back(std::move(profile));
    }
    std::sort(table.profiles.begin(), table.profiles.end(),
        [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    for (std::size_t index = 1; index < table.profiles.size(); ++index) {
        if (table.profiles[index].type_id == table.profiles[index - 1].type_id) {
            return Result::failure(failure("two unit types share type ID " + std::to_string(table.profiles[index].type_id)));
        }
    }
    // Each used set, resolved against every type of the table (R-09 set side, attack_priority).
    table.priority_sets.resize(set_index.size());
    for (const auto& [source, target] : set_index) {
        auto& set = table.priority_sets[target];
        set.unlisted = unlisted_priority;
        for (const auto& unit : tables.units) {
            if (unit.kind == UnitKind::squadron) continue;
            set.rows.push_back({assets::object_type_crc(unit.id), attack_priority(tables.priority_sets[source], unit)});
        }
        std::sort(set.rows.begin(), set.rows.end(),
            [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    }
    auto valid = tactical::validate_combat(table);
    if (!valid) return Result::failure(failure(valid.error().message));
    return Result::success(std::move(table));
}

} // namespace eawr::units
