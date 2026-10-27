#include "unit_tables_decode.hpp"

#include "eawr/data/retail_values.hpp"

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
        hardpoint.health = object->fixed("Health", report_, hardpoint.targetable && !hardpoint.destroyable);
        // HD-06: omission keeps the retail zero default; invalid authored values stay unresolved.
        if (hardpoint.destroyable && object->single("Health") == nullptr) hardpoint.health = Fixed{};
        hardpoint.repair_amount_per_frame = object->fixed("Repair_Amount_Per_Frame", report_, false);
        hardpoint.repair_cost_per_frame = object->fixed("Repair_Cost_Per_Frame", report_, false);
        hardpoint.model_to_attach = object->text("Model_To_Attach");
        hardpoint.collision_mesh = object->text("Collision_Mesh");
        hardpoint.special_ability_name = object->text("Special_Ability_Name");
        hardpoint.requires_manual_target = flag(*object, "Requires_Manual_Target_Assignment", false).value_or(false);
        hardpoint.manual_is_turret = flag(*object, "Is_Turret", false).value_or(false);
        hardpoint.allows_special_weapon_use = flag(*object, "Allows_Special_Weapon_Use", false).value_or(false);
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
        const bool attached_turret = hardpoint.manual_is_turret && !hardpoint.requires_manual_target
            && !hardpoint.model_to_attach.empty();
        hardpoint.fire_a = point(object->text("Fire_Bone_A"), owner, owner_id, "Fire_Bone_A", attached, attach_bone, attached_turret);
        hardpoint.fire_b = point(object->text("Fire_Bone_B"), owner, owner_id, "Fire_Bone_B", attached, attach_bone, attached_turret);
        hardpoint.randomize_between_fire_bones = flag(*object, "Randomize_Between_Fire_Bones", false).value_or(true);

        if (hardpoint.requires_manual_target) {
            hardpoint.manual_cooldown_seconds = object->fixed("Manual_Hardpoint_Firing_Cooldown_Secs", report_, false);
        }
        // HPT-01: ordinary weapon hardpoints own the same authored turret geometry.
        {
            if (hardpoint.manual_is_turret) {
                hardpoint.manual_turret = point(object->text("Turret_Bone_Name"), owner, owner_id,
                    "Turret_Bone_Name", attached, attach_bone, attached_turret);
                hardpoint.manual_barrel = point(object->text("Barrel_Bone_Name"), owner, owner_id,
                    "Barrel_Bone_Name", attached, attach_bone, attached_turret);
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
            weapon.charged_projectile = object->text("Blast_Ability_Fire_Projectile_Type");
            if (!weapon.charged_projectile.empty()) want_projectile(weapon.charged_projectile);
            weapon.min_recharge_seconds = object->fixed("Fire_Min_Recharge_Seconds", report_, true);
            weapon.max_recharge_seconds = object->fixed("Fire_Max_Recharge_Seconds", report_, true);
            weapon.pulse_count = count(*object, "Fire_Pulse_Count", true);
            weapon.pulse_delay_seconds = object->fixed("Fire_Pulse_Delay_Seconds", report_, false);
            weapon.appearance_delay_frames = count(*object, "Projectile_Appearance_Delay_Frames", false);
            weapon.full_salvo_delay_multiplier = object->fixed("Full_Salvo_Weapon_Delay_Multiplier", report_, false);
            weapon.range = object->fixed("Fire_Range_Distance", report_, true);
            weapon.cone_width_degrees = object->fixed("Fire_Cone_Width", report_, true);
            weapon.cone_height_degrees = object->fixed("Fire_Cone_Height", report_, true);
            weapon.damage_type = object->text("Damage_Type");
            weapon.damage = object->fixed("Projectile_Damage", report_, false);
            weapon.category_restrictions = mask(object->text("Fire_Category_Restrictions"));
            // ML-53: a name outside the loaded category dictionary is reported and contributes no bit.
            for (std::size_t index = 0; index < weapon.category_restrictions.size(); ++index) {
                const auto& name = weapon.category_restrictions[index];
                const auto known = std::any_of(tables_.categories.begin(), tables_.categories.end(),
                    [&](const EnumValue& value) { return iequals(value.name, name); });
                const auto repeated = std::any_of(weapon.category_restrictions.begin(),
                    weapon.category_restrictions.begin() + static_cast<std::ptrdiff_t>(index),
                    [&](const std::string& earlier) { return iequals(earlier, name); });
                if (!known && !repeated) {
                    report_.missing(owner_id, "Fire_Category_Restrictions", name,
                        "unknown category; name ignored (ML-53)");
                }
            }
            weapon.inaccuracy = inaccuracy(*object, "Fire_Inaccuracy_Distance");
            // Both default to yes in FoC's hardpoint data (#74, space-damage DG-35).
            weapon.opportunity_fire_when_targeting =
                flag(*object, "Allow_Opportunity_Fire_When_Targeting", false).value_or(true);
            weapon.opportunity_fire_when_idle = flag(*object, "Allow_Opportunity_Fire_When_Idle", false).value_or(true);
            weapon.fire_in_rocket_mode = flag(*object, "Fire_When_In_Rocket_Attack_Mode", false).value_or(true);
            weapon.fire_in_normal_mode = flag(*object, "Fire_When_In_Normal_Attack_Mode", false).value_or(true);
            hardpoint.weapon = std::move(weapon);
        }
        object->note_duplicates(report_);
        unit.hardpoints.push_back(std::move(hardpoint));
    }

std::vector<SpawnEntry> Loader::spawn_list(Object& object, const std::string_view tag, const bool enqueue_squadrons) {
        std::vector<SpawnEntry> result;
        for (const auto* node : object.list(tag, report_)) {
            const auto row = tokens(node->raw_text);
            // AU-82: each occurrence reads its first type and count pair only.
            const auto value = row.size() >= 2
                ? read_integer(row[1], report_, object.effective.object_id, tag) : std::nullopt;
            if (row.size() > 2) {
                report_.note(object.effective.object_id, std::string(tag), trim(node->raw_text),
                             "only the first type and count pair of an occurrence is read (AU-82)");
            }
            if (!value) {
                if (row.size() < 2) report_.missing(object.effective.object_id, std::string(tag), trim(node->raw_text),
                                "expected squadron, count");
                continue;
            }
            result.push_back({row[0], no_index, *value});
            if (enqueue_squadrons) enqueue(row[0], UnitKind::squadron);
        }
        return result;
    }

void Loader::load_spawner(Object& object, UnitType& unit) {
        bool spawns = false;
        for (const auto& token : tokens(object.text("SpaceBehavior"))) spawns = spawns || iequals(token, "SPAWN_SQUADRON");
        const bool has_list = !object.list("Starting_Spawned_Units_Tech_0", report_).empty();
        if (!input_.campaign_scene) {
            for (const auto tag : {"Starting_Spawned_Units_Tech_1", "Starting_Spawned_Units_Tech_2",
                                   "Starting_Spawned_Units_Tech_3", "Starting_Spawned_Units_Tech_4"}) {
                if (!object.list(tag, report_).empty()) {
                    report_.note(unit.id, tag, {}, "spawn list for a tech level other than Tech_0; not loaded (SK-23)");
                }
            }
        }
        if (!spawns) {
            if (has_list) report_.note(unit.id, "Starting_Spawned_Units_Tech_0", {}, "list present without SPAWN_SQUADRON");
            return;
        }
        Spawner spawner;
        spawner.starting = spawn_list(object, "Starting_Spawned_Units_Tech_0", true);
        if (spawner.starting.empty() && !input_.campaign_scene) {
            report_.missing(unit.id, "Starting_Spawned_Units_Tech_0", {}, "SPAWN_SQUADRON without a starting list");
        }
        spawner.delay_seconds = object.fixed("Spawned_Squadron_Delay_Seconds", report_, true);
        spawner.reserves = spawn_list(object, "Reserve_Spawned_Units_Tech_0", input_.campaign_scene);
        if (input_.campaign_scene) {
            spawner.campaign_starting[0] = spawner.starting;
            spawner.campaign_reserves[0] = spawner.reserves;
            for (unsigned tech = 1; tech < 6; ++tech) {
                spawner.campaign_starting[tech] = spawn_list(object, "Starting_Spawned_Units_Tech_" + std::to_string(tech), true);
                spawner.campaign_reserves[tech] = spawn_list(object, "Reserve_Spawned_Units_Tech_" + std::to_string(tech), true);
            }
        }
        unit.spawner = std::move(spawner);
    }

void Loader::load_abilities(Object& object, UnitType& unit) {
        load_unit_abilities(object, unit);
        load_nested_abilities(object, unit);
    }

    // #530 (docs/behaviour/space-purchasing.md PU-10 to PU-21, PU-31): what a station builds for
    // each faction, and what a type costs, takes and counts in a skirmish.
void Loader::load_production(Object& object, UnitType& unit) {
        if (input_.campaign_scene) {
            unit.bounty_sp = object.fixed("Override_Bounty_SP", report_, false);
            unit.bounty_mp = object.fixed("Override_Bounty_MP", report_, false);
        }
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
        // WGT-26: campaign menus are independent of multiplayer menus.
        for (const auto& token : tokens(object.text("Tactical_Buildable_Objects_Campaign"))) {
            const auto* definition = input_.catalog->find(token, data::Category::faction);
            if (definition != nullptr && iequals(definition->type_name, "Faction")) {
                production.buildable_campaign.push_back(BuildGroup{definition->id, {}});
            } else if (production.buildable_campaign.empty()) {
                report_.missing(unit.id, "Tactical_Buildable_Objects_Campaign", token, "type before any faction");
            } else production.buildable_campaign.back().types.push_back(token);
        }
        production.build_cost_campaign = object.fixed("Tactical_Build_Cost_Campaign", report_, false);
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
        for (const auto& group : production.buildable_campaign) for (const auto& name : group.types) production_ids_.push_back(name);
        if (!production.next_level.empty()) production_ids_.push_back(production.next_level);
        production.population_value = count(object, "Population_Value", false);
        production.space_override_population_value = count(object, "Space_Override_Population_Value", false, true);
        production.reinforcement_prevention_radius = object.fixed("Reinforcement_Prevention_Radius", report_, false);
    }

void Loader::load_planetary_weapon(Object& object, UnitType& unit) {
    // WSW-01/14/21: this hidden source uses object firing data, not ground
    // locomotion, durability or muzzle/appearance data in the space simulation.
    UnitType::PlanetaryWeapon special;
    special.index = count(object, "Special_Weapon_Index", true).value_or(no_index);
    special.target_action = count(object, "Special_Weapon_Target_Action_Index", true).value_or(no_index);
    special.valid_targets = enum_bits(tables_.categories, mask(object.text("Special_Weapon_Valid_Targets")),
        unit.id, "Special_Weapon_Valid_Targets", report_);
    unit.planetary_weapon = special;
    mask_flags(object, unit);
    unit.movement.layer_z_adjust = object.fixed("Layer_Z_Adjust", report_, false);
    unit.targeting_max_attack_distance = object.fixed("Targeting_Max_Attack_Distance", report_, false);
    unit.damage_type = object.text("Damage_Type");
    unit.ai_combat_power = object.fixed("AI_Combat_Power", report_, false);
    unit.has_space_evaluator = flag(object, "Has_Space_Evaluator", false).value_or(false);
    Weapon weapon;
    const auto projectiles = tokens(object.text("Projectile_Types"));
    if (projectiles.empty()) report_.missing(unit.id, "Projectile_Types", {}, "planetary source needs a projectile");
    else { weapon.projectile = projectiles.front(); want_projectile(weapon.projectile); }
    weapon.min_recharge_seconds = object.fixed("Projectile_Fire_Recharge_Seconds", report_, true);
    weapon.max_recharge_seconds = weapon.min_recharge_seconds;
    weapon.pulse_count = count(object, "Projectile_Fire_Pulse_Count", true);
    weapon.pulse_delay_seconds = object.fixed("Projectile_Fire_Pulse_Delay_Seconds", report_, true);
    weapon.damage_type = unit.damage_type;
    weapon.damage = object.fixed("Projectile_Damage", report_, false);
    weapon.inaccuracy = inaccuracy(object, "Targeting_Fire_Inaccuracy");
    // WSW-45: the source's unauthored object turret extents default to 360/180.
    if (!flag(object, "Fires_Forward", false).value_or(false)) {
        weapon.cone_width_degrees = object.fixed("Turret_Rotate_Extent_Degrees", report_, false)
            .value_or(Fixed::from_raw(360 * Fixed::scale));
        weapon.cone_height_degrees = object.fixed("Turret_Elevate_Extent_Degrees", report_, false)
            .value_or(Fixed::from_raw(180 * Fixed::scale));
    }
    unit.weapon = std::move(weapon);
}

void Loader::read_object_weapon(Object& object, UnitType& unit, const Frames* model, const bool object_damage) {
    const auto projectiles = tokens(object.text("Projectile_Types"));
    if (projectiles.empty()) return;
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
    // LS shoot PR 1: a land object authors its own Projectile_Damage; a positive value replaces the
    // projectile row's damage in the shot profile (space object weapons take the row's).
    if (object_damage) weapon.damage = object.fixed("Projectile_Damage", report_, false);
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
    // WWP-47: enumerate the muzzle bones in turn; the first absent suffix ends the list, and an
    // absent _00 leaves it empty (the shot then leaves the unit's origin). Not reported as
    // unresolved: many object weapons author no muzzle bone.
    const auto prefix_tokens = tokens(object.text("Muzzle_Bone_Name"));
    const std::string prefix = prefix_tokens.empty() ? std::string{"MUZZLEA"} : prefix_tokens.front();
    for (std::uint32_t suffix = 0; model != nullptr && suffix < 100; ++suffix) {
        const auto name = prefix + (suffix < 10 ? "_0" : "_") + std::to_string(suffix);
        if (!bone_index(*model->model, name)) break;
        unit.muzzle_bones.push_back(point(name, model, unit.id, "Muzzle_Bone_Name"));
    }
}

void Loader::load_body(Object& object, UnitType& unit) {
        unit.creates_team = flag(object, "Create_Team", false).value_or(false);
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
        if (has_behavior(object, "TURRET")) {
            auto& turret = unit.object_turret.emplace(UnitType::ObjectTurret{});
            turret.turret = point(object.text("Turret_Bone_Name"), model, unit.id, "Turret_Bone_Name");
            turret.barrel = point(object.text("Barrel_Bone_Name"), model, unit.id, "Barrel_Bone_Name");
            turret.speed = object.fixed("Turret_Rotate_Speed", report_, true);
            // FC-61, EFSB-06: the stock-unauthored delay defaults to two seconds.
            turret.rest_delay = object.fixed("Turret_Rotate_To_Rest_Delay_Sec", report_, false)
                .value_or(sim::tactical::default_object_turret_rest_delay);
            // WSW-45: type defaults apply when these extents are unauthored.
            turret.yaw_extent = object.fixed("Turret_Rotate_Extent_Degrees", report_, false)
                .value_or(Fixed::from_raw(360 * Fixed::scale));
            turret.pitch_extent = object.fixed("Turret_Elevate_Extent_Degrees", report_, false)
                .value_or(Fixed::from_raw(180 * Fixed::scale));
            turret.xy_only = flag(object, "Turret_XY_Only", false).value_or(false);
            const auto authored = tokens(object.text("Turret_Rest_Angle"));
            if (!authored.empty()) {
                const auto x = authored.size() == 3 ? number(authored[0]) : std::nullopt;
                const auto y = authored.size() == 3 ? number(authored[1]) : std::nullopt;
                const auto z = authored.size() == 3 ? number(authored[2]) : std::nullopt;
                if (x && y && z) turret.rest = {*x, *y, *z};
                else report_.missing(unit.id, "Turret_Rest_Angle", object.text("Turret_Rest_Angle"), "expected three decimal angles");
            }
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
        movement.space_layer = object.text(domain_tag(input_.domain, DomainTag::layer));
        if (input_.campaign_scene) {
            unit.campaign_ship_class = trim(object.text("Ship_Class"));
            unit.campaign_hyperspace = flag(object, "Hyperspace", false).value_or(false);
        }
        unit.footprint = read_footprint(object, model);
        movement.layer_z_adjust = object.fixed("Layer_Z_Adjust", report_, false);
        if (has_behavior(object, "TACTICAL_SUPER_WEAPON")) {
            // TSW-01: the debug build's type constructor leaves each of these at zero or off.
            UnitType::TacticalSuperWeapon weapon;
            weapon.start_distance = object.fixed("TSW_Start_Distance_From_Target", report_, false).value_or(Fixed{});
            weapon.attack_distance = object.fixed("TSW_Attack_Distance_From_Target", report_, false).value_or(Fixed{});
            weapon.z_adjust = object.fixed("TSW_Z_Adjust_Relative_To_Target_Pos", report_, false).value_or(Fixed{});
            weapon.start_pos_match_target_z = flag(object, "TSW_Start_Pos_Match_Targets_Z", false).value_or(false);
            unit.tactical_super_weapon = weapon;
        }

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
        if (unit.star_base) {
            // SAE-02: only level metadata is needed for the AI's surviving-station query;
            // predecessor references do not add physical unit or model entries.
            std::set<std::string> seen{lower(unit.id)};
            std::string previous_name = trim(object.text("Prev_Level_Base"));
            while (!previous_name.empty()) {
                if (!seen.insert(lower(previous_name)).second) {
                    report_.missing(unit.id, "Prev_Level_Base", previous_name, "reference cycle");
                    break;
                }
                auto previous = resolve(*input_.catalog, previous_name, data::Category::game_object,
                    unit.id, "Prev_Level_Base", report_);
                if (!previous) break;
                record_layers(*previous);
                unit.previous_base_levels.push_back(count(*previous, "Base_Level", false).value_or(0));
                previous_name = trim(previous->text("Prev_Level_Base"));
            }
        }
        unit.fow_reveal_range = object.fixed(domain_tag(input_.domain, DomainTag::reveal_range), report_, has_reveal(object));
        unit.reveal = has_reveal(object);
        unit.force_sensitive = flag(object, "Is_Force_Sensitive", false).value_or(false);
        unit.target_stealth_units = flag(object, "Target_Stealth_Units", false).value_or(false); // WST-07
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
        unit.damage_tracking = has_behavior(object, "DAMAGE_TRACKING"); // WCC-82
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

        read_object_weapon(object, unit, model, false);
        const bool armed = unit.weapon || std::any_of(unit.hardpoints.begin(), unit.hardpoints.end(),
            [](const Hardpoint& hardpoint) { return hardpoint.weapon.has_value(); });
        if (!armed && unit.kind == UnitKind::craft) {
            report_.missing(unit.id, "weapon", {}, "craft has neither Projectile_Types nor a weapon hardpoint");
        }
        load_spawner(object, unit);
    }

void Loader::load_squadron(Object& object, UnitType& unit) {
        // FL-06: nonpositive maxima do not make progress in debug-build creation.
        // Refuse them at validation rather than inventing a fallback or hanging.
        if (const auto* node = object.single("Max_Squad_Size")) {
            const auto maximum = number(trim(node->raw_text));
            if (!maximum || maximum->raw() <= 0 || maximum->raw() % Fixed::scale != 0
                || maximum->raw() / Fixed::scale > std::numeric_limits<std::int32_t>::max()) {
                report_.missing(unit.id, "Max_Squad_Size", trim(node->raw_text), "type skipped: expected a positive signed 32-bit whole team size");
                unit.max_squad_size = 0;
                return;
            } else unit.max_squad_size = static_cast<std::uint32_t>(maximum->raw() / Fixed::scale);
        }
        // L-2 / WHE-SQ-01: this authored flag defaults to yes, independent of member names.
        unit.homogeneous = flag(object, "Is_Homogeneous", false).value_or(true);
        unit.footprint = read_footprint(object, nullptr);
        std::vector<std::string> members;
        // ML-26: retain every authored variant occurrence, or inherit when all are placeholders.
        for (const auto* node : data::retail::list(object.layers, "Squadron_Units",
                                                 data::retail::ListRule::replace_by_variant_all)) {
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
        // FM-10: slots beyond the authored offsets use zero when binding motion.
        if (offsets.size() > members.size()) {
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
        unit.autonomous_move_extension_vs_attacker = object.fixed("Autonomous_Move_Extension_Vs_Attacker", report_, false);
        unit.attack_move_response_range = object.fixed("Attack_Move_Response_Range", report_, false);
        unit.formation_error_tolerance = object.fixed("Squadron_Formation_Error_Tolerance", report_, false);
    }

    // CategoryMask and Property_Flags, as names and as enum bits (#270). Returns the categories.
const std::vector<std::string>& Loader::mask_flags(Object& object, UnitType& unit) {
        unit.category_mask = mask(object.text("CategoryMask"));
        unit.category_bits = enum_bits(tables_.categories, unit.category_mask, unit.id, "CategoryMask", report_);
        if (const auto restrictions = mask(object.text("Attack_Category_Restrictions")); !restrictions.empty())
            unit.attack_category_restrictions = enum_bits(tables_.categories, restrictions, unit.id, "Attack_Category_Restrictions", report_);
        // WAB-73: the buzz-droid victim filter reads the droid type's own mask.
        if (const auto targets = mask(object.text("Special_Weapon_Valid_Targets")); !targets.empty())
            unit.special_weapon_targets = enum_bits(tables_.categories, targets, unit.id, "Special_Weapon_Valid_Targets", report_);
        unit.property_flags = mask(object.text("Property_Flags"));
        unit.property_bits = enum_bits(tables_.properties, unit.property_flags, unit.id, "Property_Flags", report_);
        return unit.category_mask;
    }

    // #271: a squadron reveals through the team container it spawns.
void Loader::load_team(Object& object, UnitType& unit) {
        unit.team_type = object.text("Create_Team_Type");
        if (unit.team_type.empty()) unit.team_type = "Team";
        auto team = resolve(*input_.catalog, unit.team_type, data::Category::game_object, unit.id, "Create_Team_Type", report_);
        // SOS-01: an unresolved team stays an unresolved row and attaches no object
        // script; the purchase type's own Lua_Script is not substituted (no retail
        // fallback is established).
        if (!team) return;
        record_layers(*team);
        unit.team_type = team->effective.object_id;
        unit.team_movement.max_speed = team->fixed("Max_Speed", report_, false);
        unit.team_movement.deceleration = team->fixed("OverrideDeceleration", report_, false);
        unit.lua_script = team->text("Lua_Script"); // SOS-01/19: inherited runtime attachment; a blank override keeps the parent's (ML-24)
        unit.unit_ai = has_behavior(*team, "UNIT_AI"); // WTA-29: the runtime container, not the purchase template
        // WBP-51: the runtime squadron object is this team, not the purchase template.
        // Its craft retain their own effective collision flags.
        unit.living_projectile_collision = flag(*team, "Collidable_By_Projectile_Living", false).value_or(false);
        unit.team_named_hero = flag(*team, "Is_Named_Hero", false).value_or(false);
        unit.team_generic_hero = flag(*team, "Is_Generic_Hero", false).value_or(false);
        // WSQ-60: the container owns its hull; the team's craft do not contribute to it.
        unit.team_hull = team->fixed("Tactical_Health", report_, false).value_or(Fixed::from_raw(100 * Fixed::scale));
        unit.team_shield_points = team->fixed("Shield_Points", report_, false);
        load_selection(*team, unit);
        if (has_reveal(*team)) unit.team_reveal_range = team->fixed(domain_tag(input_.domain, DomainTag::reveal_range), report_, true);
        unit.team_dense_fow_multiplier = team->fixed("Dense_FOW_Reveal_Range_Multiplier", report_, false)
            .value_or(Fixed::from_raw(Fixed::scale / 2));
        // AB-60 (#561): a team ability's data lives on the team container.
        UnitType container;
        container.id = unit.team_type;
        load_abilities(*team, container);
        unit.team_abilities = std::move(container.abilities);
        unit.team_special_abilities = std::move(container.special_abilities);
        unit.team_buzz_droids = std::move(container.buzz_droids);
        team->note_duplicates(report_);
    }

void Loader::load_selection(Object& object, UnitType& unit) {
        const auto selection_behavior = [&](const std::string_view behavior) {
            const auto domain_list = input_.domain == sim::tactical::BattleDomain::land
                ? std::string_view{"LandBehavior"} : std::string_view{"SpaceBehavior"};
            for (const auto tag : {std::string_view{"Behavior"}, domain_list})
                for (const auto& token : tokens(object.text(tag)))
                    if (iequals(token, behavior)) return true;
            return false;
        };
        // WSU-21: behaviour presence, independent of authored or current speed.
        unit.selectable = selection_behavior("SELECTABLE");
        // WSU-13/50: presentation admission is independent of ownership and maximum hull.
        const bool impassable = flag(object, "Is_Impassable_Asteroid", false).value_or(false);
        const bool living_contact = flag(object, "Collidable_By_Projectile_Living", false).value_or(false);
        const bool valid_target = flag(object, "Is_Valid_Target", false).value_or(true);
        const bool generic_hero = flag(object, "Is_Generic_Hero", false).value_or(false);
        const bool named_hero = flag(object, "Is_Named_Hero", false).value_or(false);
        unit.mouse_sensitive = unit.selectable || selection_behavior("TACTICAL_BUILD_OBJECTS")
            || selection_behavior("DUMMY_TOOLTIP") || impassable || (living_contact && valid_target);
        unit.bar_admitted = unit.selectable || unit.under_construction
            || selection_behavior("SPAWN_INDIGENOUS_UNITS") || generic_hero || named_hero;
        unit.locomotion = selection_behavior("SIMPLE_SPACE_LOCOMOTOR")
            || selection_behavior("FIGHTER_LOCOMOTOR") || selection_behavior("TEAM_LOCOMOTOR");
        unit.decoration = flag(object, "Is_Decoration", false).value_or(false);
    }

} // namespace eawr::units::unit_tables_detail
