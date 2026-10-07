#include "unit_tables_decode.hpp"

namespace eawr::units::unit_tables_detail {

void Loader::load_hardpoint(const std::string& id, UnitType& unit, const Frames* owner) {
        auto object = resolve(*input_.catalog, id, data::Category::hardpoint, unit.id, "hardpoint", report_);
        Hardpoint hardpoint;
        hardpoint.id = id;
        if (!object) {
            unit.hardpoints.push_back(std::move(hardpoint));
            return;
        }
        record_layers(*object);
        hardpoint.id = object->effective.object_id;
        const auto& owner_id = hardpoint.id;
        hardpoint.type_name = upper(object->text("Type"));
        if (hardpoint.type_name.empty()) {
            report_.missing(owner_id, "Type", {}, "required tag is absent");
        } else {
            for (const auto& [name, type] : hardpoint_types) {
                if (hardpoint.type_name == name) hardpoint.type = type;
            }
            if (hardpoint.type == HardpointType::unknown) {
                report_.missing(owner_id, "Type", hardpoint.type_name, "unknown hardpoint type");
            }
        }
        hardpoint.targetable = flag(*object, "Is_Targetable", true).value_or(false);
        hardpoint.destroyable = flag(*object, "Is_Destroyable", true).value_or(false);
        hardpoint.health = object->fixed("Health", report_, hardpoint.targetable || hardpoint.destroyable);
        hardpoint.repair_amount_per_frame = object->fixed("Repair_Amount_Per_Frame", report_, false);
        hardpoint.repair_cost_per_frame = object->fixed("Repair_Cost_Per_Frame", report_, false);
        hardpoint.model_to_attach = object->text("Model_To_Attach");
        hardpoint.collision_mesh = object->text("Collision_Mesh");
        hardpoint.special_ability_name = object->text("Special_Ability_Name");
        hardpoint.requires_manual_target = flag(*object, "Requires_Manual_Target_Assignment", false).value_or(false);
        hardpoint.fighter_bay_flyout_distance = object->fixed("Fighter_Bay_Flyout_Distance", report_, false);

        hardpoint.attachment = point(object->text("Attachment_Bone"), owner, owner_id, "Attachment_Bone");
        if (hardpoint.type == HardpointType::fighter_bay && owner != nullptr && hardpoint.attachment.position) {
            if (const auto index = bone_index(*owner->model, hardpoint.attachment.bone)) {
                const auto& frame = owner->frames[*index];
                hardpoint.bay_axis = Vec3{frame.rows[0][0], frame.rows[1][0], frame.rows[2][0]};
            }
        }
        std::string attached;
        std::optional<std::size_t> attach_bone;
        if (owner != nullptr && !hardpoint.model_to_attach.empty() && hardpoint.attachment.position) {
            attached = model_path(hardpoint.model_to_attach);
            attach_bone = bone_index(*owner->model, hardpoint.attachment.bone);
        }
        hardpoint.fire_a = point(object->text("Fire_Bone_A"), owner, owner_id, "Fire_Bone_A", attached, attach_bone);
        hardpoint.fire_b = point(object->text("Fire_Bone_B"), owner, owner_id, "Fire_Bone_B", attached, attach_bone);

        if (hardpoint.requires_manual_target) {
            hardpoint.manual_cooldown_seconds = object->fixed("Manual_Hardpoint_Firing_Cooldown_Secs", report_, false);
            hardpoint.manual_is_turret = flag(*object, "Is_Turret", false).value_or(false);
            if (hardpoint.manual_is_turret) {
                hardpoint.manual_turret = point(object->text("Turret_Bone_Name"), owner, owner_id,
                    "Turret_Bone_Name", attached, attach_bone);
                hardpoint.manual_barrel = point(object->text("Barrel_Bone_Name"), owner, owner_id,
                    "Barrel_Bone_Name", attached, attach_bone);
                hardpoint.manual_rotate_speed = object->fixed("Turret_Rotate_Speed", report_, false);
                hardpoint.manual_yaw_extent = object->fixed("Turret_Rotate_Extent_Degrees", report_, false);
                hardpoint.manual_pitch_extent = object->fixed("Turret_Elevate_Extent_Degrees", report_, false);
                const auto angle = [&](const std::string_view tag) {
                    const auto value = object->text(tag);
                    const auto row = tokens(value);
                    if (row.empty()) return Vec3{};
                    const auto x = row.size() == 3 ? number(row[0]) : std::nullopt;
                    const auto y = row.size() == 3 ? number(row[1]) : std::nullopt;
                    const auto z = row.size() == 3 ? number(row[2]) : std::nullopt;
                    if (!x || !y || !z) {
                        report_.missing(owner_id, std::string(tag), value, "expected three decimal angles");
                        return Vec3{};
                    }
                    return Vec3{*x, *y, *z};
                };
                hardpoint.manual_rest = angle("Turret_Rest_Angle");
                hardpoint.manual_offset = angle("Turret_Rotation_Offset");
            }
        }

        // WAD-31: projectile-free SPECIAL is an attachment/durability interface,
        // with no invented projectile or required recharge/cone records.
        if (is_weapon(hardpoint.type) && (hardpoint.type != HardpointType::weapon_special
            || !object->text("Fire_Projectile_Type").empty())) {
            Weapon weapon;
            weapon.projectile = object->text("Fire_Projectile_Type");
            if (weapon.projectile.empty()) {
                report_.missing(owner_id, "Fire_Projectile_Type", {}, "weapon hardpoint names no projectile");
            }
            want_projectile(weapon.projectile);
            weapon.min_recharge_seconds = object->fixed("Fire_Min_Recharge_Seconds", report_, true);
            weapon.max_recharge_seconds = object->fixed("Fire_Max_Recharge_Seconds", report_, true);
            weapon.pulse_count = count(*object, "Fire_Pulse_Count", true);
            weapon.pulse_delay_seconds = object->fixed("Fire_Pulse_Delay_Seconds", report_, false);
            weapon.appearance_delay_frames = count(*object, "Projectile_Appearance_Delay_Frames", false);
            weapon.range = object->fixed("Fire_Range_Distance", report_, true);
            weapon.cone_width_degrees = object->fixed("Fire_Cone_Width", report_, true);
            weapon.cone_height_degrees = object->fixed("Fire_Cone_Height", report_, true);
            weapon.damage_type = object->text("Damage_Type");
            weapon.damage = object->fixed("Projectile_Damage", report_, false);
            weapon.category_restrictions = mask(object->text("Fire_Category_Restrictions"));
            weapon.inaccuracy = inaccuracy(*object, "Fire_Inaccuracy_Distance");
            // Both default to yes in FoC's hardpoint data (#74, space-damage DG-35).
            weapon.opportunity_fire_when_targeting =
                flag(*object, "Allow_Opportunity_Fire_When_Targeting", false).value_or(true);
            weapon.opportunity_fire_when_idle = flag(*object, "Allow_Opportunity_Fire_When_Idle", false).value_or(true);
            hardpoint.weapon = std::move(weapon);
        }
        object->note_duplicates(report_);
        unit.hardpoints.push_back(std::move(hardpoint));
    }

std::vector<SpawnEntry> Loader::spawn_list(Object& object, const std::string_view tag, const bool enqueue_squadrons) {
        std::vector<SpawnEntry> result;
        for (const auto* node : object.list(tag, report_)) {
            const auto row = tokens(node->raw_text);
            const auto value = row.size() == 2 ? number(row[1]) : std::nullopt;
            const auto whole = value ? value->raw() / Fixed::scale : 0;
            if (!value || value->raw() % Fixed::scale != 0 || whole < std::numeric_limits<std::int32_t>::min() ||
                whole > std::numeric_limits<std::int32_t>::max()) {
                report_.missing(object.effective.object_id, std::string(tag), trim(node->raw_text),
                                "expected squadron, whole count");
                continue;
            }
            result.push_back({row[0], no_index, static_cast<std::int32_t>(whole)});
            if (enqueue_squadrons) enqueue(row[0], UnitKind::squadron);
        }
        return result;
    }

void Loader::load_spawner(Object& object, UnitType& unit) {
        bool spawns = false;
        for (const auto& token : tokens(object.text("SpaceBehavior"))) spawns = spawns || iequals(token, "SPAWN_SQUADRON");
        const bool has_list = !object.list("Starting_Spawned_Units_Tech_0", report_).empty();
        for (const auto tag : {"Starting_Spawned_Units_Tech_1", "Starting_Spawned_Units_Tech_2",
                               "Starting_Spawned_Units_Tech_3", "Starting_Spawned_Units_Tech_4"}) {
            if (!object.list(tag, report_).empty()) {
                report_.note(unit.id, tag, {}, "spawn list for a tech level other than Tech_0; not loaded (SK-23)");
            }
        }
        if (!spawns) {
            if (has_list) report_.note(unit.id, "Starting_Spawned_Units_Tech_0", {}, "list present without SPAWN_SQUADRON");
            return;
        }
        Spawner spawner;
        spawner.starting = spawn_list(object, "Starting_Spawned_Units_Tech_0", true);
        if (spawner.starting.empty()) {
            report_.missing(unit.id, "Starting_Spawned_Units_Tech_0", {}, "SPAWN_SQUADRON without a starting list");
        }
        spawner.delay_seconds = object.fixed("Spawned_Squadron_Delay_Seconds", report_, true);
        spawner.reserves = spawn_list(object, "Reserve_Spawned_Units_Tech_0", false);
        unit.spawner = std::move(spawner);
    }

void Loader::load_abilities(Object& object, UnitType& unit) {
        if (const auto* data = object.single("Unit_Abilities_Data")) {
            for (const auto& node : data->children) {
                if (!iequals(node.name, "Unit_Ability")) continue;
                data::tag_trace::used(node);
                Ability ability;
                const bool spatial_radius = std::any_of(node.children.begin(), node.children.end(), [](const auto& child) {
                    if (!iequals(child.name, "Type")) return false;
                    const auto kind = trim(child.raw_text);
                    return iequals(kind, "CONCENTRATE_FIRE") || iequals(kind, "WEAKEN_ENEMY")
                        || iequals(kind, "MISSILE_SHIELD") || iequals(kind, "SENSOR_JAMMING");
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
                        }
                    } else if (iequals(child.name, "Supports_Autofire")) {
                        data::tag_trace::used(child);
                        ability.supports_autofire = boolean(value).value_or(false);
                    } else if (iequals(child.name, "Projectile_Types_Override")) {
                        data::tag_trace::used(child);
                        ability.projectile_override = value;
                        want_projectile(value);
                    } else if ((ability.type == "HARMONIC_BOMB" || ability.type == "WEAKEN_ENEMY")
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
                const auto child_text = [&node](const std::string_view tag) -> std::string {
                    for (const auto& child : node.children) {
                        if (iequals(child.name, tag)) {
                            // WBP-22/23: nested ability reads also enter load-time tag diagnostics.
                            data::tag_trace::used(child);
                            return trim(child.raw_text);
                        }
                    }
                    return {};
                };
                const auto special_kind = sim::tactical::special_ability_kind(node.name);
                if (special_kind != sim::tactical::SpecialAbilityKind::none) {
                    sim::tactical::SpecialAbilityProfile special;
                    special.name = name;
                    special.kind = special_kind;
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
                    if (special_kind == sim::tactical::SpecialAbilityKind::concentrate_fire
                        || special_kind == sim::tactical::SpecialAbilityKind::tractor_beam
                        || special_kind == sim::tactical::SpecialAbilityKind::energy_weapon) {
                        const auto read_percent = [&](const std::string_view tag) {
                            const auto value = child_text(tag);
                            const auto parsed = number(value);
                            if (!value.empty() && !parsed) report_.missing(unit.id, std::string(tag), value, "not a decimal");
                            return parsed.value_or(Fixed{});
                        };
                        special.target_damage_increase = read_percent("Target_Damage_Increase_Percent");
                        special.target_speed_decrease = read_percent("Target_Speed_Decrease_Percent");
                        if (special_kind != sim::tactical::SpecialAbilityKind::concentrate_fire) {
                            special.beam_min_range = read_percent("Activation_Min_Range");
                            special.beam_max_range = read_percent("Activation_Max_Range");
                            special.damage_per_frame = read_percent("Damage_Per_Frame");
                            special.beam_owner_particle = child_text("Owner_Particle_Effect");
                            special.beam_owner_bone = child_text("Owner_Particle_Bone_Name");
                            if (special_kind == sim::tactical::SpecialAbilityKind::tractor_beam)
                                special.doubled_speed_categories = enum_bits(tables_.categories, {"Corvette"},
                                    unit.id, "Tractor_Corvette_Exception", report_);
                        }
                        const auto text = child_text("Stacking_Category");
                        const auto category = number(text);
                        if (category && category->raw() >= 0 && category->raw() % Fixed::scale == 0
                            && category->raw() / Fixed::scale <= std::numeric_limits<std::uint32_t>::max()) {
                            special.concentrate_stacking_category = static_cast<std::uint32_t>(category->raw() / Fixed::scale);
                        } else if (!text.empty()) report_.missing(unit.id, "Stacking_Category", text, "not an unsigned integer");
                    }
                    unit.special_abilities.push_back(std::move(special));
                }
                if (iequals(node.name, "Combat_Bonus_Ability") && iequals(child_text("Activation_Style"), "Space_Automatic")) {
                    CombatBonus bonus;
                    bonus.name = name;
                    bonus.filter = unit.special_abilities.back().filter;
                    bonus.enabled = unit.special_abilities.back().initially_enabled.value_or(true);
                    bonus.specific_faction = child_text("Specific_Faction");
                    for (const auto tag : {"Unique_Space_Unit", "Unique_Ground_Unit"}) {
                        const auto container = trim(object.text(tag));
                        if (!container.empty()) bonus.excluded_containers.push_back(assets::object_type_crc(container));
                    }
                    bonus.types = tokens(child_text("Applicable_Unit_Types"));
                    bonus.categories = mask(child_text("Applicable_Unit_Categories"));
                    const auto category = number(child_text("Stacking_Category"));
                    if (category && category->raw() >= 0 && category->raw() / Fixed::scale <= std::numeric_limits<std::uint32_t>::max()) {
                        bonus.stacking_category = static_cast<std::uint32_t>(category->raw() / Fixed::scale);
                    }
                    constexpr std::string_view tags[]{"Health_Bonus_Percentage", "Damage_Bonus_Percentage",
                        "Energy_Pool_Bonus_Percentage", "Shield_Bonus_Percentage", "Defense_Bonus_Percentage",
                        "Movement_Speed_Bonus_Percentage"};
                    for (std::size_t index = 0; index < bonus.percentages.size(); ++index) {
                        bonus.percentages[index] = number(child_text(tags[index])).value_or(Fixed{});
                    }
                    unit.production.combat_bonuses.push_back(std::move(bonus));
                } else if (iequals(node.name, "Starbase_Upgrade_Ability")
                    && iequals(child_text("Activation_Style"), "Skirmish_Automatic")) {
                    unit.production.level_up = true;
                } else if (iequals(node.name, "Income_Stream_Ability")) {
                    IncomeStream stream;
                    stream.name = name;
                    stream.base_value = number(child_text("Base_Income_Value"));
                    stream.interval_seconds = number(child_text("Base_Interval_In_Secs"));
                    stream.split_with_allies = boolean(child_text("Split_Income_With_Allies")).value_or(false);
                    stream.full_amount_to_everyone = boolean(child_text("Full_Amount_To_Everyone")).value_or(false);
                    if (!stream.base_value || !stream.interval_seconds) {
                        report_.missing(unit.id, "Income_Stream_Ability", name, "no Base_Income_Value or Base_Interval_In_Secs");
                    }
                    unit.production.income.push_back(std::move(stream));
                } else if (iequals(node.name, "Income_Stream_Mod_Ability")) {
                    IncomeBonus bonus;
                    bonus.name = name;
                    bonus.additive = number(child_text("Income_Additive_Value"));
                    bonus.multiplier = number(child_text("Income_Multiplier"));
                    bonus.target_source = child_text("Target_Stream_Source");
                    bonus.interval_multiplier = number(child_text("Interval_Multiplier"));
                    bonus.activation_style = child_text("Activation_Style");
                    bonus.all_allied_sources = boolean(child_text("Affects_All_Allied_Sources")).value_or(false);
                    bonus.reverse = boolean(child_text("Reverse_Application_Logic")).value_or(false);
                    const auto category = number(child_text("Stacking_Category"));
                    if (category && category->raw() >= 0
                        && category->raw() / Fixed::scale <= std::numeric_limits<std::uint32_t>::max()) {
                        bonus.stacking_category = static_cast<std::uint32_t>(category->raw() / Fixed::scale);
                    }
                    unit.production.income_bonuses.push_back(std::move(bonus));
                }
            }
        }
    }

    // #530 (docs/behaviour/space-purchasing.md PU-10 to PU-21, PU-31): what a station builds for
    // each faction, and what a type costs, takes and counts in a skirmish.
void Loader::load_production(Object& object, UnitType& unit) {
        auto& production = unit.production;
        const auto list = tokens(object.text("Tactical_Buildable_Objects_Multiplayer"));
        for (const auto& token : list) {
            const auto* definition = input_.catalog->find(token, data::Category::faction);
            if (definition != nullptr && iequals(definition->type_name, "Faction")) {
                production.buildable.push_back(BuildGroup{definition->id, {}});
            } else if (production.buildable.empty()) {
                report_.missing(unit.id, "Tactical_Buildable_Objects_Multiplayer", token, "type before any faction");
            } else {
                production.buildable.back().types.push_back(token);
            }
        }
        production.build_cost_multiplayer = object.fixed("Tactical_Build_Cost_Multiplayer", report_, false);
        production.build_time_seconds = object.fixed("Tactical_Build_Time_Seconds", report_, false);
        production.production_queue = object.text("Tactical_Production_Queue");
        // WPR-33 and WPR-22: production limits and completion side effects.
        production.lifetime_player = count(object, "Build_Limit_Lifetime_Per_Player", false, true);
        production.current_player = count(object, "Build_Limit_Current_Per_Player", false, true);
        production.lifetime_allies = count(object, "Build_Limit_Lifetime_For_All_Allies", false, true);
        production.current_allies = count(object, "Build_Limit_Current_For_All_Allies", false, true);
        production.prerequisites = tokens(object.text("Tactical_Build_Prerequisites"));
        production.next_level = trim(object.text("Next_Level_Base"));
        production.upgrade_object = iequals(unit.xml_type, "UpgradeObject") || has_behavior(object, "DUMMY_UPGRADE");
        production.increments_tech = flag(object, "Tactical_Build_Increments_Tech_Level", false).value_or(false);
        const auto previous_upgrade = trim(object.text("Previous_Upgrade_Level_Type"));
        production.next_upgrade = trim(object.text("Next_Upgrade_Level_Type"));
        if (flag(object, "Destroy_Previous_Upgrade_Level", false).value_or(true)) {
            production.removes_previous = previous_upgrade;
        }
        // WPR-50/52: load the authored station chain and every menu entry, once.
        for (const auto& group : production.buildable) for (const auto& name : group.types) production_ids_.push_back(name);
        if (!production.next_level.empty()) production_ids_.push_back(production.next_level);
        production.population_value = count(object, "Population_Value", false);
        production.reinforcement_prevention_radius = object.fixed("Reinforcement_Prevention_Radius", report_, false);
    }

void Loader::load_body(Object& object, UnitType& unit) {
        const bool station = unit.kind == UnitKind::station;
        // WBP-01/13/18: live space obstacles have durability and sensors, but no locomotor.
        const bool mobile = !station && !has_behavior(object, "SPACE_OBSTACLE");
        unit.model = object.text("Space_Model_Name");
        const Frames* model = nullptr;
        if (unit.model.empty()) {
            report_.missing(unit.id, "Space_Model_Name", {}, "required tag is absent");
        } else {
            unit.model_path = model_path(unit.model);
            model = frames(unit.model_path, unit.id);
            if (model == nullptr) unit.model_path.clear();
        }
        unit.scale_factor = object.fixed("Scale_Factor", report_, false);
        unit.hull = object.fixed("Tactical_Health", report_, true);
        unit.shield_points = object.fixed("Shield_Points", report_, station || mobile);
        const bool shielded = unit.shield_points && unit.shield_points->raw() > 0;
        unit.shield_refresh_rate = object.fixed("Shield_Refresh_Rate", report_, shielded);
        unit.energy_capacity = object.fixed("Energy_Capacity", report_, false);
        unit.energy_refresh_rate = object.fixed("Energy_Refresh_Rate", report_, false);
        unit.armor_type = object.text("Armor_Type");
        if (unit.armor_type.empty()) report_.missing(unit.id, "Armor_Type", {}, "required tag is absent");
        unit.shield_armor_type = object.text("Shield_Armor_Type");
        // WPR-54: the debug-build type constructor defaults this name before XML parsing.
        if (unit.shield_armor_type.empty()) unit.shield_armor_type = "Shield_Default";
        unit.damage_type = object.text("Damage_Type");

        auto& movement = unit.movement;
        movement.max_speed = object.fixed("Max_Speed", report_, mobile);
        movement.min_speed = object.fixed("Min_Speed", report_, false);
        movement.max_rate_of_turn = object.fixed("Max_Rate_Of_Turn", report_, mobile);
        movement.max_rate_of_roll = object.fixed("Max_Rate_Of_Roll", report_, false);
        movement.bank_turn_angle = object.fixed("Bank_Turn_Angle", report_, false);
        movement.max_thrust = object.fixed("Max_Thrust", report_, false);
        movement.max_lift = object.fixed("Max_Lift", report_, false);
        movement.acceleration = object.fixed("OverrideAcceleration", report_, false);
        movement.deceleration = object.fixed("OverrideDeceleration", report_, false);
        movement.space_layer = object.text("Space_Layer");
        unit.footprint = read_footprint(object, model);
        movement.layer_z_adjust = object.fixed("Layer_Z_Adjust", report_, false);

        unit.targeting_priority_set = object.text("Targeting_Priority_Set");
        if (unit.targeting_priority_set.empty()) {
            if (mobile) report_.missing(unit.id, "Targeting_Priority_Set", {}, "required tag is absent");
        } else if (std::none_of(priority_ids_.begin(), priority_ids_.end(),
                                [&](const std::string& id) { return iequals(id, unit.targeting_priority_set); })) {
            priority_ids_.push_back(unit.targeting_priority_set);
        }
        unit.targeting_max_attack_distance = object.fixed("Targeting_Max_Attack_Distance", report_, station || mobile);
        unit.targeting_min_attack_distance = object.fixed("Targeting_Min_Attack_Distance", report_, false);
        unit.targeting_stickiness_seconds = object.fixed("Targeting_Stickiness_Time_Threshold", report_, false);
        unit.strafe_distance = object.fixed("Strafe_Distance", report_, false);
        unit.spin_away_on_death = flag(object, "Spin_Away_On_Death", false).value_or(false);
        if (unit.spin_away_on_death) {
            unit.spin_away_chance = object.fixed("Spin_Away_On_Death_Chance", report_, true);
            unit.spin_away_time = object.fixed("Spin_Away_On_Death_Time", report_, true);
        }
        unit.out_of_combat_defense = object.fixed("Out_Of_Combat_Defense_Adjustment", report_, false);
        unit.follow_distance = object.fixed("Minimum_Follow_Distance", report_, false);
        if (unit.kind == UnitKind::craft) {
            unit.guard_chase_range = object.fixed("Guard_Chase_Range", report_, false);
            unit.idle_chase_range = object.fixed("Idle_Chase_Range", report_, false);
            unit.attack_move_response_range = object.fixed("Attack_Move_Response_Range", report_, false);
        }
        if (mask_flags(object, unit).empty()) report_.missing(unit.id, "CategoryMask", {}, "required tag is absent");
        unit.ai_combat_power = object.fixed("AI_Combat_Power", report_, station || mobile);
        unit.has_space_evaluator = flag(object, "Has_Space_Evaluator", false).value_or(false);
        unit.tech_level = count(object, "Tech_Level", false).value_or(0);
        unit.base_level = count(object, "Base_Level", false).value_or(0);
        unit.space_fow_reveal_range = object.fixed("Space_FOW_Reveal_Range", report_, has_reveal(object));
        unit.reveal = has_reveal(object);
        unit.force_sensitive = flag(object, "Is_Force_Sensitive", false).value_or(false);
        unit.dense_fow_multiplier = object.fixed("Dense_FOW_Reveal_Range_Multiplier", report_, false)
            .value_or(Fixed::from_raw(Fixed::scale / 2));
        unit.multisample_fow = flag(object, "Multisample_FOW_Check", false).value_or(false);
        unit.fog_box_offset.x = object.fixed("Custom_Hard_XExtent_Offset", report_, false).value_or(Fixed{});
        unit.fog_box_offset.y = object.fixed("Custom_Hard_YExtent_Offset", report_, false).value_or(Fixed{});
        load_selection(object, unit);
        // #74: the shield behaviour, and what projectiles hit.
        unit.shielded = has_behavior(object, "SHIELDED");
        unit.passive_missile_shield_radius = object.fixed("Passive_Missile_Shield_Radius", report_, false);
        unit.ranged_target_z_adjust = object.fixed("Ranged_Target_Z_Adjust", report_, false);
        unit.powered = has_behavior(object, "POWERED"); // #361: the energy pool
        unit.ion_stun_effect = has_behavior(object, "ION_STUN_EFFECT"); // #561: IS-02
        if (model != nullptr) unit.collision = collision_bounds(*model->model, model->frames);
        const auto victory = flag(object, "Victory_Relevant", station);
        unit.victory_relevant = victory.value_or(false);
        unit.destroyed_with_hardpoints =
            flag(object, "Should_Be_Destroyed_When_All_Hardpoints_Destroyed", false).value_or(true);

        for (const auto& id : tokens(object.text("HardPoints"))) load_hardpoint(id, unit, model);
        // #536 (DG-36): what a projectile hits, the model's collidable meshes and then each
        // hardpoint's attached model's, placed at its attachment bone.
        unit.collision_box_modifier = object.fixed("Collision_Box_Modifier", report_, false);
        unit.mouse_collide_sphere_radius = object.fixed("Mouse_Collide_Override_Sphere_Radius", report_, false);
        unit.hides_when_built_on = flag(object, "Hides_When_Built_On", false).value_or(false);
        unit.visible_to_enemies_when_empty = flag(object, "Visible_To_Enemies_When_Empty", false).value_or(false);
        unit.gui_row = count(object, "GUI_Row", false).value_or(0);
        if (model != nullptr) {
            append_collision_meshes(*model->model, model->frames, sim::math::identity_matrix(), no_index,
                unit.collision_meshes);
            for (std::uint32_t index = 0; index < unit.hardpoints.size(); ++index) {
                const auto& hardpoint = unit.hardpoints[index];
                if (hardpoint.model_to_attach.empty() || !hardpoint.attachment.position) continue;
                const auto bone = bone_index(*model->model, hardpoint.attachment.bone);
                const auto* attached = bone ? frames(model_path(hardpoint.model_to_attach), unit.id) : nullptr;
                if (attached == nullptr) continue;
                append_collision_meshes(*attached->model, attached->frames, model->frames[*bone], index,
                    unit.collision_meshes);
            }
        }
        for (const auto& bone : tokens(object.text("Target_Bones"))) {
            unit.target_bones.push_back(point(bone, model, unit.id, "Target_Bones"));
        }

        const auto projectiles = tokens(object.text("Projectile_Types"));
        if (!projectiles.empty()) {
            if (projectiles.size() > 1) {
                report_.note(unit.id, "Projectile_Types", object.text("Projectile_Types"), "several types; the first is loaded");
            }
            Weapon weapon;
            weapon.projectile = projectiles.front();
            want_projectile(weapon.projectile);
            weapon.min_recharge_seconds = object.fixed("Projectile_Fire_Recharge_Seconds", report_, true);
            weapon.max_recharge_seconds = weapon.min_recharge_seconds;
            weapon.pulse_count = count(object, "Projectile_Fire_Pulse_Count", true);
            weapon.pulse_delay_seconds = object.fixed("Projectile_Fire_Pulse_Delay_Seconds", report_, true);
            weapon.damage_type = unit.damage_type;
            // DG-24: the object weapon's scatter rows are the type's Targeting_Fire_Inaccuracy; a
            // unit's Fire_Inaccuracy_Distance (every FoC fighter authors it) feeds only hardpoints.
            weapon.inaccuracy = inaccuracy(object, "Targeting_Fire_Inaccuracy");
            // W-09: Fires_Forward defaults to no; absent extents default to 360/180 degrees
            // (debug build, OW-01, OW-02). Authored extents are compared whole.
            if (!flag(object, "Fires_Forward", false).value_or(false)) {
                weapon.cone_width_degrees = object.fixed("Turret_Rotate_Extent_Degrees", report_, false)
                    .value_or(Fixed::from_raw(360 * Fixed::scale));
                weapon.cone_height_degrees = object.fixed("Turret_Elevate_Extent_Degrees", report_, false)
                    .value_or(Fixed::from_raw(180 * Fixed::scale));
            }
            unit.weapon = std::move(weapon);
        }
        const bool armed = unit.weapon || std::any_of(unit.hardpoints.begin(), unit.hardpoints.end(),
            [](const Hardpoint& hardpoint) { return hardpoint.weapon.has_value(); });
        if (!armed && unit.kind == UnitKind::craft) {
            report_.missing(unit.id, "weapon", {}, "craft has neither Projectile_Types nor a weapon hardpoint");
        }
        load_spawner(object, unit);
    }

void Loader::load_squadron(Object& object, UnitType& unit) {
        // L-2 / WHE-SQ-01: this authored flag defaults to yes, independent of member names.
        unit.homogeneous = flag(object, "Is_Homogeneous", false).value_or(true);
        unit.footprint = read_footprint(object, nullptr);
        std::vector<std::string> members;
        for (const auto* node : object.list("Squadron_Units", report_)) {
            for (auto& id : tokens(node->raw_text)) members.push_back(std::move(id));
        }
        if (members.empty()) report_.missing(unit.id, "Squadron_Units", {}, "squadron has no members");
        std::vector<Vec3> offsets;
        for (const auto* node : object.list("Squadron_Offsets", report_)) {
            const auto row = tokens(node->raw_text);
            std::array<std::optional<Fixed>, 3> values{};
            for (std::size_t index = 0; index < 3 && index < row.size(); ++index) values[index] = number(row[index]);
            if (row.size() != 3 || !values[0] || !values[1] || !values[2]) {
                report_.missing(unit.id, "Squadron_Offsets", trim(node->raw_text), "expected x, y, z");
                continue;
            }
            offsets.push_back({*values[0], *values[1], *values[2]});
        }
        if (offsets.size() < members.size()) {
            report_.missing(unit.id, "Squadron_Offsets", std::to_string(offsets.size()),
                            "fewer offsets than the " + std::to_string(members.size()) + " members");
        } else if (offsets.size() > members.size()) {
            report_.note(unit.id, "Squadron_Offsets", std::to_string(offsets.size()), "more offsets than members");
        }
        for (std::size_t index = 0; index < members.size(); ++index) {
            unit.members.push_back({members[index], no_index,
                                    index < offsets.size() ? std::optional<Vec3>(offsets[index]) : std::nullopt});
            enqueue(members[index], UnitKind::craft);
        }
        mask_flags(object, unit);
        load_team(object, unit);
        unit.guard_chase_range = object.fixed("Guard_Chase_Range", report_, false);
        unit.idle_chase_range = object.fixed("Idle_Chase_Range", report_, false);
        unit.attack_move_response_range = object.fixed("Attack_Move_Response_Range", report_, false);
        unit.formation_error_tolerance = object.fixed("Squadron_Formation_Error_Tolerance", report_, false);
    }

    // CategoryMask and Property_Flags, as names and as enum bits (#270). Returns the categories.
const std::vector<std::string>& Loader::mask_flags(Object& object, UnitType& unit) {
        unit.category_mask = mask(object.text("CategoryMask"));
        unit.category_bits = enum_bits(tables_.categories, unit.category_mask, unit.id, "CategoryMask", report_);
        unit.property_flags = mask(object.text("Property_Flags"));
        unit.property_bits = enum_bits(tables_.properties, unit.property_flags, unit.id, "Property_Flags", report_);
        return unit.category_mask;
    }

    // #271: a squadron reveals through the team container it spawns.
void Loader::load_team(Object& object, UnitType& unit) {
        unit.team_type = object.text("Create_Team_Type");
        if (unit.team_type.empty()) unit.team_type = "Team";
        auto team = resolve(*input_.catalog, unit.team_type, data::Category::game_object, unit.id, "Create_Team_Type", report_);
        if (!team) return;
        record_layers(*team);
        unit.team_type = team->effective.object_id;
        // WBP-51: the runtime squadron object is this team, not the purchase template.
        // Its craft retain their own effective collision flags.
        unit.living_projectile_collision = flag(*team, "Collidable_By_Projectile_Living", false).value_or(false);
        unit.team_named_hero = flag(*team, "Is_Named_Hero", false).value_or(false);
        unit.team_generic_hero = flag(*team, "Is_Generic_Hero", false).value_or(false);
        // WSQ-60: the container owns its hull; the team's craft do not contribute to it.
        unit.team_hull = team->fixed("Tactical_Health", report_, false).value_or(Fixed::from_raw(100 * Fixed::scale));
        unit.team_shield_points = team->fixed("Shield_Points", report_, false);
        load_selection(*team, unit);
        if (has_reveal(*team)) unit.team_reveal_range = team->fixed("Space_FOW_Reveal_Range", report_, true);
        unit.team_dense_fow_multiplier = team->fixed("Dense_FOW_Reveal_Range_Multiplier", report_, false)
            .value_or(Fixed::from_raw(Fixed::scale / 2));
        // AB-60 (#561): a team ability's data lives on the team container.
        UnitType container;
        container.id = unit.team_type;
        load_abilities(*team, container);
        unit.team_abilities = std::move(container.abilities);
        unit.team_special_abilities = std::move(container.special_abilities);
        team->note_duplicates(report_);
    }

void Loader::load_selection(Object& object, UnitType& unit) {
        // WSU-21: behaviour presence, independent of authored or current speed.
        unit.selectable = has_behavior(object, "SELECTABLE");
        // WSU-13/50: presentation admission is independent of ownership and maximum hull.
        const bool impassable = flag(object, "Is_Impassable_Asteroid", false).value_or(false);
        const bool living_contact = flag(object, "Collidable_By_Projectile_Living", false).value_or(false);
        const bool valid_target = flag(object, "Is_Valid_Target", false).value_or(true);
        const bool generic_hero = flag(object, "Is_Generic_Hero", false).value_or(false);
        const bool named_hero = flag(object, "Is_Named_Hero", false).value_or(false);
        unit.mouse_sensitive = unit.selectable || has_behavior(object, "TACTICAL_BUILD_OBJECTS")
            || has_behavior(object, "DUMMY_TOOLTIP") || impassable || (living_contact && valid_target);
        unit.bar_admitted = unit.selectable || unit.under_construction
            || has_behavior(object, "SPAWN_INDIGENOUS_UNITS") || generic_hero || named_hero;
        unit.locomotion = has_behavior(object, "SIMPLE_SPACE_LOCOMOTOR")
            || has_behavior(object, "FIGHTER_LOCOMOTOR") || has_behavior(object, "TEAM_LOCOMOTOR");
        unit.decoration = flag(object, "Is_Decoration", false).value_or(false);
    }

} // namespace eawr::units::unit_tables_detail
