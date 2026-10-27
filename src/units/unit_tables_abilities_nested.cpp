#include "unit_tables_decode.hpp"

namespace eawr::units::unit_tables_detail {

std::string AbilityFields::operator()(const std::string_view tag) const {
    for (const auto& child : node.children) {
        if (iequals(child.name, tag)) {
            // WBP-22/23: nested ability reads also enter load-time tag diagnostics.
            data::tag_trace::used(child);
            return trim(child.raw_text);
        }
    }
    return {};
}

void Loader::load_unit_abilities(Object& object, UnitType& unit) {
    if (const auto* data = object.single("Unit_Abilities_Data")) {
        for (const auto& node : data->children) {
            if (!iequals(node.name, "Unit_Ability")) continue;
            data::tag_trace::used(node);
            Ability ability;
            const bool spatial_radius = std::any_of(node.children.begin(), node.children.end(), [](const auto& child) {
                if (!iequals(child.name, "Type")) return false;
                const auto kind = trim(child.raw_text);
                return iequals(kind, "CONCENTRATE_FIRE") || iequals(kind, "WEAKEN_ENEMY")
                    || iequals(kind, "MISSILE_SHIELD") || iequals(kind, "SENSOR_JAMMING") || iequals(kind, "LURE");
            });
            for (const auto& child : node.children) {
                const auto value = trim(child.raw_text);
                // #628: a field counts as used where it is read (each branch below), so a field
                // added here is read and traced together; no list of names to keep in step.
                if (iequals(child.name, "Type")) {
                    data::tag_trace::used(child);
                    ability.authored_type = value;
                    ability.type = upper(value);
                } else if (iequals(child.name, "Expiration_Seconds")) {
                    data::tag_trace::used(child);
                    ability.expiration_seconds = number(value);
                    if (!ability.expiration_seconds) report_.missing(unit.id, "Expiration_Seconds", value, "not a decimal");
                } else if (iequals(child.name, "Recharge_Seconds")) {
                    data::tag_trace::used(child);
                    ability.recharge_seconds = number(value);
                    if (!ability.recharge_seconds) report_.missing(unit.id, "Recharge_Seconds", value, "not a decimal");
                } else if (spatial_radius && iequals(child.name, "Effective_Radius")) {
                    data::tag_trace::used(child);
                    ability.effective_radius = number(value);
                    if (!ability.effective_radius) report_.missing(unit.id, "Effective_Radius", value, "not a decimal");
                } else if (iequals(child.name, "Targeting_Max_Attack_Distance")) {
                    data::tag_trace::used(child);
                    ability.targeting_max_attack_distance = number(value);
                    if (!ability.targeting_max_attack_distance) report_.missing(unit.id, child.name, value, "not a decimal");
                } else if (iequals(child.name, "GUI_Activated_Ability_Name")) {
                    data::tag_trace::used(child);
                    ability.gui_activated_ability_name = value;
                } else if (iequals(child.name, "Mod_Multiplier")) {
                    data::tag_trace::used(child);
                    const auto row = tokens(value);
                    const auto factor = row.size() == 2 ? number(row[1]) : std::nullopt;
                    if (!factor) {
                        report_.missing(unit.id, "Mod_Multiplier", value, "expected modifier, decimal");
                    } else {
                        ability.modifiers.push_back({row[0], *factor});
                        // AB-26: unknown names keep the ability; retained names with no
                        // current consumer also produce a load note.
                        const auto name = upper(row[0]);
                        if (name == "TARGET_SPEED_MULTIPLIER" || name == "TARGET_TAKE_DAMAGE_MULTIPLIER") {
                            report_.note(unit.id, child.name, value, "retained modifier has no consumer (AB-26)");
                        } else if (name != "WEAPON_DELAY_MULTIPLIER" && name != "SCATTER_RADIUS_MULTIPLIER"
                            && name != "FIRE_RATE_MULTIPLIER" && name != "SHIELD_REGEN_MULTIPLIER"
                            && name != "SHIELD_REGEN_INTERVAL_MULTIPLIER" && name != "ENERGY_REGEN_MULTIPLIER"
                            && name != "ENERGY_REGEN_INTERVAL_MULTIPLIER" && name != "SPEED_MULTIPLIER"
                            && name != "CAUSE_DAMAGE_MULTIPLIER" && name != "TAKE_DAMAGE_MULTIPLIER") {
                            ability.modifiers.pop_back();
                            report_.note(unit.id, child.name, value, "unknown modifier ignored; ability retained (AB-26)");
                        }
                    }
                } else if (iequals(child.name, "Mod_Flag")) {
                    data::tag_trace::used(child);
                    const auto row = tokens(value);
                    const auto flag = row.size() == 2 ? boolean(row[1]) : std::nullopt;
                    if (!flag) {
                        report_.missing(unit.id, child.name, value, "expected modifier, boolean");
                    } else {
                        const auto name = upper(row[0]);
                        if (name == "WEAPONS_ENABLED_FLAG" || name == "CAN_BE_SQUASHED"
                            || name == "ALLOW_TARGET_DIMINISHING_FIREPOWER") {
                            ability.modifier_flags.push_back({row[0], *flag});
                            report_.note(unit.id, child.name, value, "retained flag has no consumer (AB-26)");
                        } else {
                            report_.note(unit.id, child.name, value, "unknown modifier flag ignored; ability retained (AB-26)");
                        }
                    }
                } else if (iequals(child.name, "Supports_Autofire")) {
                    data::tag_trace::used(child);
                    ability.supports_autofire = boolean(value).value_or(false);
                } else if ((ability.type == "INTERDICT" || ability.type == "MISSILE_SHIELD")
                    && iequals(child.name, "Disable_Movement")) {
                    data::tag_trace::used(child);
                    ability.disable_movement = boolean(value).value_or(false);
                } else if (iequals(child.name, "Projectile_Types_Override")) {
                    data::tag_trace::used(child);
                    ability.projectile_override = value;
                    want_projectile(value);
                } else if ((ability.type == "HARMONIC_BOMB" || ability.type == "WEAKEN_ENEMY" || ability.type == "SELF_DESTRUCT"
                    || ability.type == "FOW_REVEAL_PING")
                    && iequals(child.name, "Spawned_Object_Type")) {
                    data::tag_trace::used(child);
                    ability.spawned_object = value;
                    want_projectile(value);
                } else if (ability.type == "HARMONIC_BOMB" && iequals(child.name, "Bomb_Countdown_Seconds")) {
                    data::tag_trace::used(child);
                    ability.bomb_countdown_seconds = number(value);
                } else if (ability.type == "WEAKEN_ENEMY" && iequals(child.name, "Target_Position_Z_Offset")) {
                    data::tag_trace::used(child);
                    ability.target_position_z_offset = number(value);
                } else if (ability.type == "CORRUPT_SYSTEMS" && iequals(child.name, "Area_Effect_Decal_Distance")) {
                    data::tag_trace::used(child);
                    ability.effective_radius = number(value);
                    if (!ability.effective_radius) report_.missing(unit.id, child.name, value, "not a decimal");
                } else if (iequals(child.name, "Targeting_Fire_Inaccuracy_Fixed_Radius_Override")) {
                    data::tag_trace::used(child);
                    ability.fixed_inaccuracy = number(value);
                    if (!ability.fixed_inaccuracy) report_.missing(unit.id, child.name, value, "not a decimal");
                } else if (iequals(child.name, "Target_Position_Z_Offset")) {
                    data::tag_trace::used(child);
                    ability.target_z_offset = number(value);
                    if (!ability.target_z_offset) report_.missing(unit.id, child.name, value, "not a decimal");
                } else if (ability.type == "REPLENISH_WINGMEN" && iequals(child.name, "Particle_Effect")) {
                    data::tag_trace::used(child);
                    ability.replenish_particle = value;
                } else if (ability.type == "CONCENTRATE_FIRE" && iequals(child.name, "Particle_Effect")) {
                    data::tag_trace::used(child);
                    ability.concentrate_particle = value;
                } else if (ability.type == "LURE" && iequals(child.name, "Target_Types")) {
                    // WLU-04: exact type names, in authored order (the service looks each one up).
                    data::tag_trace::used(child);
                    for (auto& name : tokens(value)) ability.target_types.push_back(std::move(name));
                } else if (ability.type == "LURE" && iequals(child.name, "Pulse_Frequency_Secs")) {
                    data::tag_trace::used(child);
                    ability.pulse_frequency_seconds = number(value);
                    if (!ability.pulse_frequency_seconds) report_.missing(unit.id, child.name, value, "not a decimal");
                } else if (ability.type == "LURE" && iequals(child.name, "Max_Number_Of_Pulses")) {
                    data::tag_trace::used(child);
                    if (const auto count = read_integer(child.raw_text, report_, unit.id, child.name)) {
                        if (*count < 0) report_.note(unit.id, child.name, value,
                            "PR-C4: signed pulse count retained in the unsigned table field");
                        ability.max_pulses = static_cast<std::uint32_t>(*count);
                    }
                } else if (ability.type == "LURE" && iequals(child.name, "Is_Pulsing")) {
                    // WLU-03: read for the registry; the lure service pulses whatever it says.
                    data::tag_trace::used(child);
                    ability.is_pulsing = boolean(value);
                } else if (ability.type == "LURE" && iequals(child.name, "Particle_Effect")) {
                    data::tag_trace::used(child);
                    ability.lure_particle = value;
                }
            }
            if (ability.type.empty()) report_.missing(unit.id, "Unit_Ability", {}, "ability has no Type");
            if (iequals(ability.type, "BARRAGE")) enqueue("Dummy_Barrage_Target", UnitKind::ship);
            unit.abilities.push_back(std::move(ability));
        }
    }
    if (std::any_of(unit.abilities.begin(), unit.abilities.end(), [](const Ability& ability) {
            return ability.type == "REPLENISH_WINGMEN";
        })) {
        unit.replenish_team = trim(object.text("Create_Team_Type"));
        if (unit.replenish_team.empty()) report_.missing(unit.id, "Create_Team_Type", {}, "wingman replenishment requires an authored team");
        else enqueue(unit.replenish_team, UnitKind::squadron);
    }
}

void Loader::load_bounty_ability(UnitType& unit, const AbilityFields& child_text) {
    const auto& node = child_text.node;
    if (input_.campaign_scene && iequals(node.name, "Bounty_On_Faction_Ability")) {
        Production::Bounty bounty;
        bounty.percentage = std::max(Fixed{}, number(child_text("Bounty_Percentage_Amount")).value_or(Fixed{}));
        const auto category = number(child_text("Stacking_Category"));
        if (category && category->trunc_to_integer() >= std::numeric_limits<std::int32_t>::min()
            && category->trunc_to_integer() <= std::numeric_limits<std::int32_t>::max())
            bounty.category = static_cast<std::int32_t>(category->trunc_to_integer());
        bounty.factions = tokens(child_text("Target_Faction_Names"));
        bounty.all_allies = boolean(child_text("Apply_To_All_Allies")).value_or(false);
        unit.production.bounties.push_back(std::move(bounty));
    }
}

void Loader::load_cluster_bomb_ability(UnitType& unit, const std::string& name, const AbilityFields& child_text) {
    const auto& node = child_text.node;
    if (iequals(node.name, "Cluster_Bomb_Ability")) {
        ClusterBomb cluster;
        cluster.name = name;
        cluster.style = sim::tactical::special_activation_style(child_text("Activation_Style"));
        cluster.bomb_type = child_text("Bomb_Type");
        const auto count = number(child_text("Number_Of_Bombs"));
        const auto seconds = number(child_text("Detonation_Time_In_Secs"));
        // WCB-03: invalid nonpositive inputs fall back to one bomb / one second.
        if (count && count->raw() > 0 && count->raw() / Fixed::scale <= 65536)
            cluster.count = static_cast<std::uint32_t>(std::max<std::int64_t>(1, count->raw() / Fixed::scale));
        else report_.missing(unit.id, "Number_Of_Bombs", {}, "expected a positive bounded count; using one");
        if (seconds && seconds->raw() > 0) cluster.detonation_seconds = *seconds;
        else report_.missing(unit.id, "Detonation_Time_In_Secs", {}, "expected positive seconds; using one");
        if (cluster.bomb_type.empty()) report_.missing(unit.id, "Bomb_Type", {}, "cluster bombs need a projectile type");
        else want_projectile(cluster.bomb_type);
        unit.cluster_bomb = std::move(cluster);
    }
}

void Loader::load_buzz_droids_ability(UnitType& unit, const std::string& name, const AbilityFields& child_text) {
    const auto& node = child_text.node;
    if (iequals(node.name, "Buzz_Droids_Ability")) {
        // WAB-70/71: the nested data Apply copies onto the droid object it creates.
        BuzzDroids buzz;
        buzz.name = name;
        const auto read = [&](const std::string_view tag) -> std::optional<Fixed> {
            const auto value = child_text(tag);
            const auto parsed = number(value);
            if (!parsed) report_.missing(unit.id, std::string(tag), value, "not a decimal");
            return parsed;
        };
        buzz.min_range = read("Activation_Min_Range");
        buzz.max_range = read("Activation_Max_Range");
        buzz.chase_radius = read("Chase_Radius");
        buzz.damage_radius = read("Damage_Radius");
        buzz.enemy_damage_per_second = read("Enemy_Damage_Per_Second");
        buzz.own_damage_per_second = read("Own_Damage_Per_Second");
        buzz.activation_seconds = read("Activation_Time_In_Seconds");
        buzz.object_type = child_text("Object_Type");
        buzz.activate_sfx = child_text("Activate_SFX");
        if (buzz.object_type.empty()) report_.missing(unit.id, "Object_Type", {}, "buzz droids need an object type");
        else enqueue(buzz.object_type, UnitKind::ship);
        unit.buzz_droids = std::move(buzz);
    }
}

bool Loader::load_special_ability(UnitType& unit, const std::string& name, const AbilityFields& child_text) {
    const auto& node = child_text.node;
    const auto special_kind = sim::tactical::special_ability_kind(node.name);
    // WSH-01: only authored automatic space healers belong to this consumer.
    if (special_kind == sim::tactical::SpecialAbilityKind::force_healing
        && std::none_of(node.children.begin(), node.children.end(), [this](const auto& child) {
            return iequals(child.name, "Activation_Style") && iequals(trim(child.raw_text), automatic_activation_name(input_.domain));
        })) return false;
    if (special_kind != sim::tactical::SpecialAbilityKind::none) {
        sim::tactical::SpecialAbilityProfile special;
        special.name = name;
        special.kind = special_kind;
        if (special_kind == sim::tactical::SpecialAbilityKind::enable_radar) {
            const auto allies = child_text("Affects_All_Allies");
            if (!allies.empty()) {
                const auto parsed = boolean(allies);
                if (!parsed) report_.missing(unit.id, "Affects_All_Allies", allies, "not a boolean");
                else special.affects_all_allies = *parsed;
            }
        }
        special.style = sim::tactical::special_activation_style(child_text("Activation_Style"));
        special.service_interval = sim::tactical::special_service_interval(special_kind);
        const auto enabled = child_text("Initially_Enabled");
        if (!enabled.empty()) {
            special.initially_enabled = boolean(enabled);
            if (!special.initially_enabled) report_.missing(unit.id, "Initially_Enabled", enabled, "not a boolean");
        }
        const auto despawn = child_text("Causes_Despawn");
        if (!despawn.empty()) {
            const auto parsed = boolean(despawn);
            if (!parsed) report_.missing(unit.id, "Causes_Despawn", despawn, "not a boolean");
            else special.causes_despawn = *parsed;
        }
        const auto exact_types = [&](const std::string_view tag) {
            std::vector<sim::tactical::TypeId> ids;
            for (const auto& id : tokens(child_text(tag))) ids.push_back(assets::object_type_crc(id));
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            return ids;
        };
        special.filter.applicable_types = exact_types("Applicable_Unit_Types");
        special.filter.excluded_types = exact_types("Excluded_Unit_Types");
        special.filter.applicable_categories = enum_bits(tables_.categories, mask(child_text("Applicable_Unit_Categories")),
            unit.id, "Applicable_Unit_Categories", report_);
        special.filter.excluded_categories = enum_bits(tables_.categories, mask(child_text("Excluded_Unit_Categories")),
            unit.id, "Excluded_Unit_Categories", report_);
        load_special_healing(unit, special, child_text);
        load_special_beam(unit, special, child_text);
        load_special_blast(unit, special, child_text);
        load_special_stealth(unit, special, child_text);
        load_special_leech(unit, special, child_text);
        load_special_corrupt(unit, special, child_text);
        load_special_laser_defense(unit, special, child_text);
        load_special_redirect(unit, special, child_text);
        // WRB-01: only SpaceUnit carriers; the hero and structure objects that author it (Darth_Vader,
        // IG-88, Urai_Fen, the ground structures) are land objects, out of scope.
        if (special_kind != sim::tactical::SpecialAbilityKind::redirect_blaster || iequals(unit.xml_type, "SpaceUnit"))
            unit.special_abilities.push_back(std::move(special));
    }
    return true;
}

void Loader::load_nested_abilities(Object& object, UnitType& unit) {
    for (const auto* abilities : object.list("Abilities", report_)) {
        for (const auto& node : abilities->children) {
            std::string name;
            for (const auto& attribute : node.attributes) {
                if (iequals(attribute.name, "Name")) {
                    data::tag_trace::used_attribute(node, attribute.name);
                    name = attribute.value;
                }
            }
            unit.inactive_abilities.push_back(node.name + (name.empty() ? "" : " " + name));
            // #530: a skirmish station's income stream and its bonus (PU-02 to PU-04).
            const AbilityFields child_text{node};
            load_bounty_ability(unit, child_text);
            load_cluster_bomb_ability(unit, name, child_text);
            load_buzz_droids_ability(unit, name, child_text);
            if (!load_special_ability(unit, name, child_text)) continue;
            load_bonus_abilities(object, unit, name, child_text);
        }
    }
}

} // namespace eawr::units::unit_tables_detail
