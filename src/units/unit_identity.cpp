#include "eawr/units/unit_tables.hpp"

#include "eawr/core/sha256.hpp"

#include <span>

namespace eawr::units {
namespace {

// Little-endian, length-framed encoding of every loaded value in table order
// (docs/unit-data.md). Absent optionals encode as a 0 byte, present ones as a
// 1 byte followed by the value.
class Encoder final {
public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void u32(const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(const std::uint64_t value) { i64(static_cast<std::int64_t>(value)); }
    void i64(const std::int64_t value) {
        const auto bits = static_cast<std::uint64_t>(value);
        for (unsigned shift = 0; shift < 64; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
    void text(const std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        for (const char item : value) bytes_.push_back(static_cast<std::uint8_t>(item));
    }
    void flag(const bool value) { u8(value ? 1 : 0); }
    void fixed(const Fixed value) { i64(value.raw()); }
    void fixed(const std::optional<Fixed>& value) {
        flag(value.has_value());
        if (value) fixed(*value);
    }
    void vec3(const Vec3& value) {
        fixed(value.x);
        fixed(value.y);
        fixed(value.z);
    }
    void vec3(const std::optional<Vec3>& value) {
        flag(value.has_value());
        if (value) vec3(*value);
    }
    void texts(const std::vector<std::string>& values) {
        u32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) text(value);
    }
    template <typename T, typename F>
    void each(const std::vector<T>& values, F&& encode) {
        u32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) encode(value);
    }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

void encode(Encoder& out, const BonePoint& point) {
    out.text(point.bone);
    out.vec3(point.position);
    out.flag(point.from_attached_model);
    out.flag(point.axes.has_value());
    if (point.axes) {
        for (const auto& axis : *point.axes) out.vec3(axis);
    }
}

void encode(Encoder& out, const Weapon& weapon) {
    out.text(weapon.projectile);
    out.u32(weapon.projectile_index);
    out.fixed(weapon.min_recharge_seconds);
    out.fixed(weapon.max_recharge_seconds);
    out.flag(weapon.pulse_count.has_value());
    if (weapon.pulse_count) out.u32(*weapon.pulse_count);
    out.fixed(weapon.pulse_delay_seconds);
    out.fixed(weapon.range);
    out.fixed(weapon.cone_width_degrees);
    out.fixed(weapon.cone_height_degrees);
    out.text(weapon.damage_type);
    out.fixed(weapon.damage);
    out.texts(weapon.category_restrictions);
    out.each(weapon.inaccuracy, [&](const InaccuracyEntry& entry) {
        out.text(entry.category);
        out.fixed(entry.distance);
    });
    out.flag(weapon.opportunity_fire_when_targeting);
    out.flag(weapon.opportunity_fire_when_idle);
}

void encode(Encoder& out, const std::optional<Weapon>& weapon) {
    out.flag(weapon.has_value());
    if (weapon) encode(out, *weapon);
}

void encode(Encoder& out, const Hardpoint& hardpoint) {
    out.text(hardpoint.id);
    out.u8(static_cast<std::uint8_t>(hardpoint.type));
    out.text(hardpoint.type_name);
    out.flag(hardpoint.targetable);
    out.flag(hardpoint.destroyable);
    out.fixed(hardpoint.health);
    out.fixed(hardpoint.repair_amount_per_frame);
    out.fixed(hardpoint.repair_cost_per_frame);
    encode(out, hardpoint.weapon);
    out.text(hardpoint.model_to_attach);
    out.text(hardpoint.collision_mesh);
    encode(out, hardpoint.attachment);
    encode(out, hardpoint.fire_a);
    encode(out, hardpoint.fire_b);
    out.text(hardpoint.special_ability_name);
    out.fixed(hardpoint.fighter_bay_flyout_distance);
    out.vec3(hardpoint.bay_axis);
}

void encode(Encoder& out, const SpawnEntry& entry) {
    out.text(entry.squadron);
    out.u32(entry.squadron_index);
    out.u32(static_cast<std::uint32_t>(entry.count));
}

void encode(Encoder& out, const SpaceFootprint& footprint) {
    out.flag(footprint.space_obstacle);
    out.fixed(footprint.custom_hard_x);
    out.fixed(footprint.custom_hard_y);
    out.fixed(footprint.custom_soft_radius);
    out.fixed(footprint.obstacle_radius);
    out.fixed(footprint.collision_x);
    out.fixed(footprint.collision_y);
}

void encode(Encoder& out, const UnitType& unit) {
    out.text(unit.id);
    out.u8(static_cast<std::uint8_t>(unit.kind));
    out.text(unit.xml_type);
    out.texts(unit.variant_chain);
    out.text(unit.affiliation);
    out.text(unit.model);
    out.text(unit.model_path);
    out.fixed(unit.scale_factor);
    out.fixed(unit.hull);
    out.fixed(unit.shield_points);
    out.fixed(unit.shield_refresh_rate);
    out.fixed(unit.energy_capacity);
    out.fixed(unit.energy_refresh_rate);
    out.text(unit.armor_type);
    out.text(unit.shield_armor_type);
    out.text(unit.damage_type);
    const auto& movement = unit.movement;
    out.fixed(movement.max_speed);
    out.fixed(movement.min_speed);
    out.fixed(movement.max_rate_of_turn);
    out.fixed(movement.max_rate_of_roll);
    out.fixed(movement.bank_turn_angle);
    out.fixed(movement.max_thrust);
    out.fixed(movement.max_lift);
    out.fixed(movement.acceleration);
    out.fixed(movement.deceleration);
    out.text(movement.space_layer);
    out.fixed(movement.layer_z_adjust);
    out.text(unit.targeting_priority_set);
    out.u32(unit.targeting_priority_set_index);
    out.fixed(unit.targeting_max_attack_distance);
    out.fixed(unit.targeting_stickiness_seconds);
    out.texts(unit.category_mask);
    out.u64(unit.category_bits);
    out.texts(unit.property_flags);
    out.u64(unit.property_bits);
    out.fixed(unit.ai_combat_power);
    out.fixed(unit.space_fow_reveal_range);
    out.flag(unit.reveal);
    out.text(unit.team_type);
    out.fixed(unit.team_reveal_range);
    out.flag(unit.victory_relevant);
    out.flag(unit.destroyed_with_hardpoints);
    out.flag(unit.shielded);
    out.flag(unit.powered);
    out.flag(unit.ion_stun_effect);
    out.flag(unit.collision.has_value());
    if (unit.collision) {
        out.vec3(unit.collision->min);
        out.vec3(unit.collision->max);
    }
    out.each(unit.collision_meshes, [&](const CollisionMesh& mesh) {
        out.text(mesh.name);
        out.u32(mesh.hardpoint);
        out.each(mesh.triangles, [&](const std::array<Vec3, 3>& triangle) {
            for (const auto& corner : triangle) out.vec3(corner);
        });
    });
    out.fixed(unit.collision_box_modifier);
    out.each(unit.hardpoints, [&](const Hardpoint& hardpoint) { encode(out, hardpoint); });
    encode(out, unit.weapon);
    out.each(unit.target_bones, [&](const BonePoint& point) { encode(out, point); });
    out.each(unit.abilities, [&](const Ability& ability) {
        out.text(ability.type);
        out.text(ability.authored_type);
        out.fixed(ability.expiration_seconds);
        out.fixed(ability.recharge_seconds);
        out.each(ability.modifiers, [&](const ModMultiplier& modifier) {
            out.text(modifier.modifier);
            out.fixed(modifier.value);
        });
        out.flag(ability.supports_autofire);
    });
    out.each(unit.team_abilities, [&](const Ability& ability) {
        out.text(ability.type);
        out.fixed(ability.recharge_seconds);
        out.flag(ability.supports_autofire);
        out.text(ability.projectile_override);
        out.u32(ability.projectile_index);
    });
    out.texts(unit.inactive_abilities);
    encode(out, unit.footprint);
    out.flag(unit.spawner.has_value());
    if (unit.spawner) {
        out.each(unit.spawner->starting, [&](const SpawnEntry& entry) { encode(out, entry); });
        out.fixed(unit.spawner->delay_seconds);
        out.each(unit.spawner->reserves, [&](const SpawnEntry& entry) { encode(out, entry); });
        out.flag(unit.spawner->reserves_used);
    }
    out.each(unit.members, [&](const SquadronMember& member) {
        out.text(member.craft);
        out.u32(member.craft_index);
        out.vec3(member.offset);
    });
    out.text(unit.lua_script);
    out.fixed(unit.strafe_distance);
    out.fixed(unit.guard_chase_range);
    out.fixed(unit.idle_chase_range);
    out.fixed(unit.attack_move_response_range);
    out.fixed(unit.formation_error_tolerance);
    out.fixed(unit.out_of_combat_defense);
    out.fixed(unit.follow_distance);
}

} // namespace

std::vector<std::uint8_t> canonical_encoding(const UnitTables& tables) {
    Encoder out;
    out.text("eawr-unit-tables-v4");
    out.each(tables.units, [&](const UnitType& unit) { encode(out, unit); });
    out.each(tables.obstacles, [&](const ObstacleType& obstacle) {
        out.text(obstacle.id);
        out.text(obstacle.xml_type);
        out.text(obstacle.space_layer);
        out.fixed(obstacle.scale_factor);
        out.text(obstacle.model_path);
        encode(out, obstacle.footprint);
    });
    out.each(tables.projectiles, [&](const Projectile& projectile) {
        out.text(projectile.id);
        out.fixed(projectile.damage);
        out.text(projectile.damage_type);
        out.fixed(projectile.max_speed);
        out.fixed(projectile.max_rate_of_turn);
        out.fixed(projectile.max_flight_distance);
        out.text(projectile.category);
        out.flag(projectile.does_shield_damage);
        out.flag(projectile.does_energy_damage);
        out.flag(projectile.does_hitpoint_damage);
        out.fixed(projectile.energy_per_shot);
        out.fixed(projectile.ai_combat_power);
        out.flag(projectile.ion_stun);
        out.fixed(projectile.ion_stun_duration);
        out.fixed(projectile.ion_stun_speed_reduction);
        out.fixed(projectile.ion_stun_shot_rate_reduction);
        out.flag(projectile.ion_stun_stack_duration);
        out.fixed(projectile.ion_stun_radius);
    });
    out.each(tables.priority_sets, [&](const TargetingPrioritySet& set) {
        out.text(set.id);
        out.each(set.attack_priorities, [&](const PriorityEntry& entry) {
            out.text(entry.name);
            out.fixed(entry.weight);
            out.u32(static_cast<std::uint32_t>(entry.match));
            out.u64(entry.bits);
        });
        out.texts(set.hard_point_priorities);
        out.texts(set.category_exclusions);
        out.texts(set.property_exclusions);
        out.texts(set.unit_exclusions);
        out.texts(set.hard_point_exclusions);
        out.u64(set.category_exclusion_bits);
        out.u64(set.property_exclusion_bits);
    });
    const auto values = [&](const EnumValue& value) {
        out.text(value.name);
        out.u64(value.value);
    };
    out.each(tables.categories, values);
    out.each(tables.properties, values);
    out.each(tables.constants.scalars, [&](const NamedConstant& constant) {
        out.text(constant.tag);
        out.fixed(constant.value);
        out.text(constant.text);
    });
    out.each(tables.constants.damage_to_armor, [&](const DamageToArmor& row) {
        out.text(row.damage_type);
        out.text(row.armor_type);
        out.fixed(row.multiplier);
    });
    return out.take();
}

std::array<std::uint8_t, 32> content_identity(const UnitTables& tables) {
    const auto bytes = canonical_encoding(tables);
    return core::sha256(std::span<const std::uint8_t>(bytes));
}

} // namespace eawr::units
