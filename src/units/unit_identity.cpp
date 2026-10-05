#include "eawr/units/unit_tables.hpp"

#include "eawr/core/sha256.hpp"

#include <span>
#include <algorithm>

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
    if (weapon.appearance_delay_frames.value_or(0) != 0) {
        out.text("APPEARANCE-DELAY-v1");
        out.u32(*weapon.appearance_delay_frames);
    }
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
    if (hardpoint.requires_manual_target) {
        out.text("MANUAL-TARGET-v1");
        out.text("MANUAL-CANNON-v1");
        out.fixed(hardpoint.manual_cooldown_seconds);
        out.flag(hardpoint.manual_is_turret);
        if (hardpoint.manual_is_turret) {
            encode(out, hardpoint.manual_turret);
            encode(out, hardpoint.manual_barrel);
            out.vec3(hardpoint.manual_rest);
            out.vec3(hardpoint.manual_offset);
            out.fixed(hardpoint.manual_rotate_speed);
            out.fixed(hardpoint.manual_yaw_extent);
            out.fixed(hardpoint.manual_pitch_extent);
        }
    }
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
    out.texts(footprint.hazard.behavior);
    out.texts(footprint.hazard.space_behavior);
    out.flag(footprint.hazard.asteroid_field);
    out.flag(footprint.hazard.ion_storm);
    out.flag(footprint.hazard.nebula);
    out.flag(footprint.hazard.impassable_asteroid);
    out.flag(footprint.hazard.asteroid_damage);
    out.flag(footprint.hazard.nebula_service);
    out.vec3(footprint.hazard.obstacle_offset);
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
    if (unit.targeting_min_attack_distance) {
        out.text("targeting-min-distance-v1");
        out.fixed(unit.targeting_min_attack_distance);
    }
    out.fixed(unit.targeting_stickiness_seconds);
    out.texts(unit.category_mask);
    out.u64(unit.category_bits);
    out.texts(unit.property_flags);
    out.u64(unit.property_bits);
    out.fixed(unit.ai_combat_power);
    out.fixed(unit.space_fow_reveal_range);
    out.flag(unit.reveal);
    out.text(unit.team_type);
    if (!unit.replenish_team.empty() || unit.redirect_damage_to_teammates) {
        out.text("hero-wingmen-v1"); out.text(unit.replenish_team); out.flag(unit.redirect_damage_to_teammates);
    }
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
        if (!ability.spawned_object.empty()) {
            out.text("hero-spawn-v1"); out.text(ability.spawned_object); out.u32(ability.spawned_projectile_index);
            out.fixed(ability.bomb_countdown_seconds); out.fixed(ability.effective_radius); out.fixed(ability.target_position_z_offset);
        }
        if (ability.type == "CONCENTRATE_FIRE" || ability.type == "ENERGY_WEAPON" || ability.type == "TRACTOR_BEAM") {
            out.text("concentrate-fire-v1");
            out.fixed(ability.effective_radius);
            out.text(ability.gui_activated_ability_name);
        }
        if (ability.type == "BARRAGE") {
            out.text("BARRAGE-v1");
            out.text(ability.projectile_override);
            out.u32(ability.projectile_index);
            out.fixed(ability.fixed_inaccuracy);
            out.fixed(ability.target_z_offset);
        }
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
    // #530: production and income (v5).
    const auto& production = unit.production;
    out.each(production.buildable, [&](const BuildGroup& group) {
        out.text(group.faction);
        out.texts(group.types);
    });
    out.fixed(production.build_cost_multiplayer);
    out.fixed(production.build_time_seconds);
    out.text(production.production_queue);
    out.flag(production.population_value.has_value());
    out.u32(production.population_value.value_or(0));
    out.fixed(production.reinforcement_prevention_radius);
    out.each(production.income, [&](const IncomeStream& stream) {
        out.text(stream.name);
        out.fixed(stream.base_value);
        out.fixed(stream.interval_seconds);
        out.flag(stream.split_with_allies);
        out.flag(stream.full_amount_to_everyone);
    });
    out.each(production.income_bonuses, [&](const IncomeBonus& bonus) {
        out.text(bonus.name);
        out.fixed(bonus.additive);
        out.fixed(bonus.multiplier);
        out.text(bonus.target_source);
    });
    // WPR-22/33/51/52: gameplay content, including absent versus zero limits.
    for (const auto limit : {production.lifetime_player, production.current_player,
             production.lifetime_allies, production.current_allies}) {
        out.flag(limit.has_value());
        if (limit) out.u32(*limit);
    }
    out.texts(production.prerequisites);
    out.text(production.next_level);
    out.flag(production.upgrade_object);
    out.flag(production.level_up);
    out.flag(production.increments_tech);
    out.text(production.removes_previous);
    out.each(production.combat_bonuses, [&](const CombatBonus& bonus) {
        out.u32(bonus.stacking_category);
        out.texts(bonus.types);
        out.texts(bonus.categories);
        for (const auto percentage : bonus.percentages) out.fixed(percentage);
    });
    out.fixed(unit.follow_distance);
}

} // namespace

std::vector<std::uint8_t> canonical_encoding(const UnitTables& tables) {
    Encoder out;
    out.text("eawr-unit-tables-v7");
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
        out.fixed(projectile.blast.damage);
        out.fixed(projectile.blast.radius);
        out.flag(projectile.blast.dropoff);
        out.u32(static_cast<std::uint32_t>(projectile.blast.tiers));
        out.u32(static_cast<std::uint32_t>(projectile.blast.max_victims));
        out.text(projectile.blast_immune_faction);
        out.fixed(projectile.blast.max_delay);
        if (projectile.weaken.on_detonation) {
            const auto& weaken = projectile.weaken;
            out.text("hero-weaken-v1"); out.fixed(weaken.radius); out.fixed(weaken.take_damage_increase);
            out.fixed(weaken.cause_damage_reduction); out.u32(weaken.duration_frames); out.u64(weaken.categories);
            out.text(weaken.status_effect);
        }
        // RFL-01..08: conditional extension leaves tables without flight inputs byte-identical.
        if (projectile.max_lifetime || projectile.explode_at_target_radius.value_or(false)
            || projectile.rocket_curve_distance || projectile.rocket_curve_offset || projectile.rocket_straight_distance) {
            out.text("RFL1");
            out.fixed(projectile.max_lifetime);
            out.flag(projectile.explode_at_target_radius.value_or(false));
            out.fixed(projectile.rocket_curve_distance);
            out.fixed(projectile.rocket_curve_offset);
            out.fixed(projectile.rocket_straight_distance);
        }
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
    // WAD-14: collision eligibility matters even in content without live capture points.
    out.text("BLAST-COLLISION-v1");
    out.each(tables.units, [&](const UnitType& unit) {
        out.text(unit.id);
        out.flag(unit.living_projectile_collision);
    });
    // WBP-01: the optional live-pad content extension leaves tables without capture points unchanged.
    if (std::any_of(tables.units.begin(), tables.units.end(), [](const UnitType& unit) { return unit.capture_point; })) {
        out.text("PADS-v1");
        out.fixed(tables.pad_ai_build_multiplier);
        out.each(tables.pad_neutral_factions, [&](const std::uint64_t faction) { out.u64(faction); });
        out.each(tables.units, [&](const UnitType& unit) {
            out.text(unit.id);
            out.flag(unit.capture_point);
            out.flag(unit.build_pad);
            out.flag(unit.under_construction);
            out.flag(unit.living_projectile_collision);
            out.flag(unit.influences_capture);
            out.flag(unit.ownership_sticks);
            out.flag(unit.community_property);
            out.flag(unit.construction_blocker);
            out.flag(unit.child_persists);
            out.fixed(unit.capture_radius);
            out.fixed(unit.capture_seconds);
            out.text(unit.constructed_type);
            encode(out, unit.build_attachment);
        });
        // WBP-02/07: stock noncapture obstacles use both defaults. Bind mod opt-outs
        // when present without changing the stock identity for these additional reads.
        if (std::any_of(tables.obstacles.begin(), tables.obstacles.end(), [](const ObstacleType& obstacle) {
                return !obstacle.influences_capture || !obstacle.construction_blocker || obstacle.living_projectile_collision;
            })) {
            out.text("pad-obstacle-candidates-v1");
            out.each(tables.obstacles, [&](const ObstacleType& obstacle) {
                out.text(obstacle.id);
                out.flag(obstacle.influences_capture);
                out.flag(obstacle.construction_blocker);
                out.flag(obstacle.living_projectile_collision);
            });
        }
    }
    // SAE-02: bind station-level perceptions without changing profiles that have no station.
    if (std::any_of(tables.units.begin(), tables.units.end(), [](const UnitType& unit) { return unit.base_level != 0; })) {
        out.text("ai-station-levels-v1");
        out.each(tables.units, [&](const UnitType& unit) { out.text(unit.id); out.u32(unit.base_level); });
    }
    if (std::any_of(tables.units.begin(), tables.units.end(), [](const UnitType& unit) {
            return unit.destroy_when_child_dies || unit.pad_rebuild_seconds || unit.tactical_respawn_seconds;
        })) {
        // WHZ-52/WBP-27/28: bind authored lifecycle content to replay identity.
        out.text("pad-lifecycle-v1");
        out.each(tables.units, [&](const UnitType& unit) {
            out.text(unit.id);
            out.flag(unit.destroy_when_child_dies);
            out.fixed(unit.pad_rebuild_seconds);
            out.fixed(unit.tactical_respawn_seconds);
        });
    }
    if (std::any_of(tables.units.begin(), tables.units.end(), [](const UnitType& unit) { return unit.tactical_sale; })) {
        out.text("pad-sale-v1");
        out.each(tables.units, [&](const UnitType& unit) {
            out.text(unit.id);
            out.flag(unit.tactical_sale);
            out.fixed(unit.tactical_sell_percentage);
        });
    }
    if (std::any_of(tables.units.begin(), tables.units.end(), [](const UnitType& unit) {
            return std::any_of(unit.production.income_bonuses.begin(), unit.production.income_bonuses.end(),
                [](const IncomeBonus& bonus) { return !bonus.target_source.empty(); });
        })) {
        out.text("income-modifiers-v1");
        out.each(tables.units, [&](const UnitType& unit) {
            out.text(unit.id);
            out.each(unit.production.income_bonuses, [&](const IncomeBonus& bonus) {
                out.fixed(bonus.interval_multiplier);
                out.text(bonus.activation_style);
                out.u32(bonus.stacking_category);
                out.flag(bonus.all_allied_sources);
                out.flag(bonus.reverse);
            });
        });
    }
    // EN-08: bind the optional engine-disable content without changing profiles that omit it.
    if (std::any_of(tables.projectiles.begin(), tables.projectiles.end(), [](const Projectile& projectile) {
            return projectile.disables_engines_when_power_drained;
        })) {
        out.text("engine-disable-v1");
        out.each(tables.projectiles, [&](const Projectile& projectile) {
            out.text(projectile.id);
            out.flag(projectile.disables_engines_when_power_drained);
            out.fixed(projectile.disable_engines_duration);
        });
    }
    if (std::any_of(tables.units.begin(), tables.units.end(), [](const UnitType& unit) {
            return !unit.company_members.empty();
        })) {
        out.text("hero-deployment-v1");
        out.each(tables.units, [&](const UnitType& unit) {
            out.text(unit.id);
            out.flag(unit.named_hero);
            out.flag(unit.generic_hero);
            out.flag(unit.team_named_hero);
            out.flag(unit.team_generic_hero);
            out.text(unit.company_transport);
            out.text(unit.deployed_space_type);
            out.flag(unit.creates_carried_heroes);
            out.each(unit.company_members, [&](const UnitType::CompanyMember& member) {
                out.text(member.type);
                out.flag(member.named_hero);
                out.flag(member.generic_hero);
                out.text(member.unique_space_unit);
            });
        });
    }
    if (std::any_of(tables.units.begin(), tables.units.end(), [](const UnitType& unit) {
            return !unit.special_abilities.empty() || !unit.team_special_abilities.empty();
        })) {
        out.text("nested-special-abilities-v1");
        const auto special = [&](const sim::tactical::SpecialAbilityProfile& profile) {
            out.text(profile.name); out.u8(static_cast<std::uint8_t>(profile.kind));
            out.u8(static_cast<std::uint8_t>(profile.style));
            out.flag(profile.initially_enabled.has_value());
            if (profile.initially_enabled) out.flag(*profile.initially_enabled);
            out.flag(profile.causes_despawn); out.u32(profile.service_interval);
            out.each(profile.filter.applicable_types, [&](const auto id) { out.u64(id); });
            out.u64(profile.filter.applicable_categories);
            out.each(profile.filter.excluded_types, [&](const auto id) { out.u64(id); });
            out.u64(profile.filter.excluded_categories);
            if (profile.kind == sim::tactical::SpecialAbilityKind::concentrate_fire) {
                out.text("concentrate-fire-v1");
                out.fixed(profile.target_damage_increase);
                out.fixed(profile.target_speed_decrease);
                out.u32(profile.concentrate_stacking_category);
            }
            if (profile.kind == sim::tactical::SpecialAbilityKind::energy_weapon
                || profile.kind == sim::tactical::SpecialAbilityKind::tractor_beam) {
                out.text("hero-beam-v1");
                out.fixed(profile.beam_min_range);
                out.fixed(profile.beam_max_range);
                out.fixed(profile.damage_per_frame);
                out.fixed(profile.target_speed_decrease);
                out.u32(profile.concentrate_stacking_category);
                out.u64(profile.doubled_speed_categories);
                out.text(profile.beam_owner_particle);
                out.text(profile.beam_owner_bone);
            }
        };
        out.each(tables.units, [&](const UnitType& unit) {
            out.text(unit.id);
            out.each(unit.special_abilities, special);
            out.each(unit.team_special_abilities, special);
        });
    }
    if (std::any_of(tables.units.begin(), tables.units.end(), [](const UnitType& unit) {
            return !unit.production.upgrade_object && !unit.production.combat_bonuses.empty();
        })) {
        out.text("hero-command-bonuses-v1");
        out.each(tables.units, [&](const UnitType& unit) {
            out.text(unit.id);
            if (unit.production.upgrade_object) { out.u32(0); return; }
            out.each(unit.production.combat_bonuses, [&](const CombatBonus& bonus) {
                out.text(bonus.name); out.flag(bonus.enabled); out.text(bonus.specific_faction);
                out.each(bonus.excluded_containers, [&](const auto id) { out.u64(id); });
                out.each(bonus.filter.applicable_types, [&](const auto id) { out.u64(id); });
                out.u64(bonus.filter.applicable_categories);
                out.each(bonus.filter.excluded_types, [&](const auto id) { out.u64(id); });
                out.u64(bonus.filter.excluded_categories);
            });
        });
    }
    return out.take();
}

std::array<std::uint8_t, 32> content_identity(const UnitTables& tables) {
    const auto bytes = canonical_encoding(tables);
    return core::sha256(std::span<const std::uint8_t>(bytes));
}

} // namespace eawr::units
