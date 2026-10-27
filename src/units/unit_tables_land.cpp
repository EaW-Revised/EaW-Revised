#include "unit_tables_decode.hpp"

#include "eawr/data/retail_values.hpp"

namespace eawr::units::unit_tables_detail {

namespace {

// The engine's object classes (schema names, not data names) that make a land object a structure.
bool structure_class(const std::string_view xml_type) {
    for (const auto name : {"GroundStructure", "GroundBase", "GroundBuildable", "SpecialStructure",
                            "SecondaryStructure", "TechBuilding"}) {
        if (iequals(xml_type, name)) return true;
    }
    return false;
}

// LC-08: the authored movement classes. Infantry has its own; the other ground classes move vehicles.
bool vehicle_movement_class(const std::string_view movement_class) {
    for (const auto name : {"Wheeled", "Tracked", "Walker", "Hover", "LargeWalker"}) {
        if (iequals(movement_class, name)) return true;
    }
    return false;
}

LandObjectKind land_kind(const std::string_view xml_type, const bool hero, const std::string_view movement_class) {
    if (hero || iequals(xml_type, "HeroUnit") || iequals(xml_type, "GenericHeroUnit")) return LandObjectKind::hero;
    if (structure_class(xml_type)) return LandObjectKind::structure;
    if (iequals(xml_type, "GroundInfantry")) return LandObjectKind::infantry;
    if (iequals(xml_type, "GroundVehicle")) return LandObjectKind::vehicle;
    if (iequals(movement_class, "Infantry")) return LandObjectKind::infantry;
    if (vehicle_movement_class(movement_class)) return LandObjectKind::vehicle;
    return LandObjectKind::other;
}

std::string locomotor_behavior(Object& object) {
    for (const auto tag : {"LandBehavior", "Behavior"}) {
        for (const auto& name : tokens(object.text(tag))) {
            if (upper(name).ends_with("_LOCOMOTOR")) return name;
        }
    }
    return {};
}

// A behaviour in the object's land or shared behaviour list.
bool land_behavior(Object& object, const std::string_view behavior) {
    for (const auto tag : {"LandBehavior", "Behavior"}) {
        for (const auto& name : tokens(object.text(tag))) {
            if (iequals(name, behavior)) return true;
        }
    }
    return false;
}

} // namespace

bool land_ground_object(Object& object) {
    const auto hero = boolean(object.text("Is_Named_Hero")).value_or(false) || boolean(object.text("Is_Generic_Hero")).value_or(false);
    if (land_kind(object.effective.type_name, hero, trim(object.text("MovementClass"))) != LandObjectKind::other) return true;
    return !trim(object.text("Land_Model_Name")).empty() && trim(object.text("Space_Model_Name")).empty();
}

void Loader::load_land_body(Object& object, UnitType& unit) {
    // Shared combat profile classification; the land session supplies its own locomotion.
    unit.kind = UnitKind::ship;
    // WSU-10/13/21: the land body also publishes the authored selection and pick flags.
    load_selection(object, unit);
    unit.locomotion = !locomotor_behavior(object).empty();
    unit.movement.max_speed = object.retail_float("Max_Speed", report_);
    unit.mouse_collide_sphere_radius = object.fixed("Mouse_Collide_Override_Sphere_Radius", report_, false);
    unit.valid_target = flag(object, "Is_Valid_Target", false).value_or(true);
    unit.named_hero = flag(object, "Is_Named_Hero", false).value_or(false);
    unit.generic_hero = flag(object, "Is_Generic_Hero", false).value_or(false);
    unit.living_projectile_collision = flag(object, "Collidable_By_Projectile_Living", false).value_or(false);
    unit.dead_projectile_collision = flag(object, "Collidable_By_Projectile_Dead", false).value_or(false);
    // The domain selector names the model tag; a land object without one simply has no model.
    unit.model = object.text(domain_tag(input_.domain, DomainTag::model_name));
    const Frames* model = nullptr;
    if (!unit.model.empty()) {
        unit.model_path = model_path(unit.model);
        model = frames(unit.model_path, unit.id);
        if (model == nullptr) unit.model_path.clear();
    }
    unit.scale_factor = object.retail_float("Scale_Factor", report_); // LSL-P1
    // Durability: a positive Tactical_Health makes a damageable type; anything else leaves no row.
    const auto hull = object.fixed("Tactical_Health", report_, false);
    if (hull && hull->raw() > 0) unit.hull = hull;
    else if (hull) report_.note(unit.id, "Tactical_Health", trim(object.text("Tactical_Health")), "non-positive health; the type has no durability row");
    unit.shielded = land_behavior(object, "SHIELDED");
    unit.shield_points = object.fixed("Shield_Points", report_, false);
    const bool shielded = unit.shielded && unit.shield_points && unit.shield_points->raw() > 0;
    unit.shield_refresh_rate = object.fixed("Shield_Refresh_Rate", report_, shielded);
    unit.armor_type = object.text("Armor_Type");
    unit.shield_armor_type = object.text("Shield_Armor_Type");
    // WPR-54: the debug-build type constructor defaults this name before XML parsing.
    if (unit.shield_armor_type.empty()) unit.shield_armor_type = "Shield_Default";
    unit.damage_type = object.text("Damage_Type");
    // Targeting: the priority set and the range, chase and stickiness rows of the object.
    unit.targeting_priority_set = object.text("Targeting_Priority_Set");
    if (!unit.targeting_priority_set.empty() && std::none_of(priority_ids_.begin(), priority_ids_.end(),
            [&](const std::string& id) { return iequals(id, unit.targeting_priority_set); }))
        priority_ids_.push_back(unit.targeting_priority_set);
    unit.targeting_max_attack_distance = object.fixed("Targeting_Max_Attack_Distance", report_, false);
    unit.targeting_min_attack_distance = object.fixed("Targeting_Min_Attack_Distance", report_, false);
    unit.targeting_stickiness_seconds = object.fixed("Targeting_Stickiness_Time_Threshold", report_, false);
    unit.idle_chase_range = object.fixed("Idle_Chase_Range", report_, false);
    unit.guard_chase_range = object.fixed("Guard_Chase_Range", report_, false);
    unit.attack_move_response_range = object.fixed("Attack_Move_Response_Range", report_, false);
    mask_flags(object, unit);
    unit.ranged_target_z_adjust = object.fixed("Ranged_Target_Z_Adjust", report_, false);
    // Projectile contact: the authored footprint rows, then the model's collision bounds and meshes.
    unit.footprint = read_footprint(object, model);
    if (model != nullptr) {
        unit.collision = collision_bounds(*model->model, model->frames);
        append_collision_meshes(*model->model, model->frames, sim::math::identity_matrix(), no_index, unit.collision_meshes);
    }
    read_object_weapon(object, unit, model, true);
    // Death presentation rows (Q4: selection and persistence rules stay unverified until a capture).
    for (const auto* node : object.list("Death_Clone", report_)) unit.death_clones.push_back(trim(node->raw_text));
    unit.death_fade_time = object.fixed("Death_Fade_Time", report_, false);
    unit.death_persistence_duration = object.fixed("Death_Persistence_Duration", report_, false);
}

LandCompanyMember Loader::land_member(Object& object) {
    LandCompanyMember member;
    member.type = object.effective.object_id;
    member.named_hero = flag(object, "Is_Named_Hero", false).value_or(false);
    member.generic_hero = flag(object, "Is_Generic_Hero", false).value_or(false);
    member.movement_class = trim(object.text("MovementClass"));
    member.kind = land_kind(object.effective.type_name, member.named_hero || member.generic_hero, member.movement_class);
    member.locomotor = locomotor_behavior(object);
    member.create_team = flag(object, "Create_Team", false).value_or(false);
    member.unique_ground_unit = trim(object.text("Unique_Ground_Unit"));
    member.max_speed = object.retail_float("Max_Speed", report_); // LSL-P1; absent = LSL-02 type default
    member.stay_in_transport = flag(object, "Stay_In_Transport_During_Ground_Battle", false).value_or(false);
    return member;
}

void Loader::load_land_static(Object& object) {
    if (std::any_of(tables_.land_static_objects.begin(), tables_.land_static_objects.end(),
        [&](const auto& row) { return iequals(row.id, object.effective.object_id); })) return;
    LandStaticObject row;
    row.id = object.effective.object_id; row.xml_type = object.effective.type_name;
    for (const auto tag : {"Behavior", "LandBehavior"})
        for (const auto& behavior : tokens(object.text(tag))) {
            row.behaviors.push_back(behavior);
            if (iequals(behavior, "LAND_OBSTACLE")) row.land_obstacle = true;
        }
    if (row.land_obstacle) {
        row.proxy_type = trim(object.text("Obstacle_Proxy_Type"));
        row.secondary_passability = trim(object.text("SecondaryOccupationPassability"));
        row.override_passability = trim(object.text("OverridePassability"));
        Object* geometry = &object;
        auto proxy = row.proxy_type.empty() ? std::optional<Object>{} : resolve(*input_.catalog,
            row.proxy_type, data::Category::game_object, row.id, "Obstacle_Proxy_Type", report_);
        if (proxy) { record_layers(*proxy); geometry = &*proxy; }
        // LSL-P1: retail float parse for every static geometry tag.
        row.width = geometry->retail_float("Obstacle_Width", report_);
        row.height = geometry->retail_float("Obstacle_Height", report_);
        row.x_offset = geometry->retail_float("Obstacle_X_Offset", report_);
        row.y_offset = geometry->retail_float("Obstacle_Y_Offset", report_);
        if (geometry->single("Fine_Tune_Occupied_Passability"))
            row.fine_tune = flag(*geometry, "Fine_Tune_Occupied_Passability", false);
        row.scale_factor = geometry->retail_float("Scale_Factor", report_);
        row.model_path = model_path(trim(geometry->text("Land_Model_Name")));
        if (!trim(geometry->text("Land_Model_Name")).empty() && input_.model) {
            if (const auto* model = frames(row.model_path, geometry->effective.object_id)) {
                row.model_present = true;
                auto bounds = prepare_galactic_collision_bounds(*model->model);
                if (!bounds) report_.missing(row.id, "Land_Model_Name", row.model_path, bounds.error().message);
                else row.collision_bounds = bounds.value();
                append_collision_meshes(*model->model, model->frames, sim::math::identity_matrix(), no_index, row.collision_meshes);
            }
        }
        if (proxy) {
            const auto original_model = trim(object.text("Land_Model_Name"));
            // WLPA-64: proxy collision uses the original instance transform and requires its model.
            row.model_present = row.model_present && !original_model.empty()
                && frames(model_path(original_model), object.effective.object_id) != nullptr;
        }
        geometry->note_duplicates(report_);
    }
    object.note_duplicates(report_);
    tables_.land_static_objects.push_back(std::move(row));
}

LandLocomotionData Loader::land_locomotion(Object& object) {
    LandLocomotionData member;
    // LSL-P1: the retail float parse; the authored value is kept as parsed, its meaning applies in the adapter.
    member.acceleration = object.retail_float("OverrideAcceleration", report_);
    member.deceleration = object.retail_float("OverrideDeceleration", report_);
    member.rate_of_turn = object.retail_float("Max_Rate_Of_Turn", report_);
    member.stopped_rate_of_turn = object.retail_float("Stopped_Rate_Of_Turn", report_);
    member.scale_factor = object.retail_float("Scale_Factor", report_);
    member.soft_radius = object.retail_float("Custom_Soft_Footprint_Radius", report_);
    member.hard_x = object.retail_float("Custom_Hard_XExtent", report_);
    member.hard_y = object.retail_float("Custom_Hard_YExtent", report_);
    member.formation_spacing = object.retail_float("FormationSpacing", report_);
    // WLPA-73: an engine-fixed name set; absent = None. LSL-06: an unknown name fails only that key, so the
    // class keeps its None default and the loader leaves a note.
    if (const auto* node = object.single("UnitCollisionClass")) {
        const auto name = trim(node->raw_text);
        if (const auto parsed = parse_land_collision_class(name)) member.collision_class = *parsed;
        else report_.note(object.effective.object_id, "UnitCollisionClass", name, "not a collision class name; None stays (LSL-06)");
    }
    member.walk_transition = flag(object, "Walk_Transition", false).value_or(false);
    member.has_pre_turn_animation = flag(object, "Has_Pre_Turn_Anim", false).value_or(false);
    const auto model_name = trim(object.text("Land_Model_Name"));
    if (!model_name.empty()) {
        if (const auto* model = frames(model_path(model_name), object.effective.object_id)) {
            if (const auto extents = collision_half_extents(*model->model, model->frames)) {
                member.collision_x = extents->first;
                member.collision_y = extents->second;
            }
        }
    }
    return member;
}

void Loader::load_land_movement_classes() {
    // Engine table path, like the category/property enum paths; its classes and memberships are data.
    auto document = data::load_document(*input_.filesystem, "data/xml/movementclasstypedefs.xml");
    if (!document) {
        report_.missing("MovementClassType", "file", "data/xml/movementclasstypedefs.xml", document.error().message);
        return;
    }
    const auto& value = document.value();
    report_.input({value.source.logical_path, value.source.source_id, value.source.layer_id, value.input_sha256});
    data::tag_trace::document(value.root);
    if (!iequals(value.root.name, "MovementClassType")) {
        report_.missing("MovementClassType", "root", value.root.name, "expected MovementClassType");
        return;
    }
    for (const auto& child : value.root.children) {
        data::tag_trace::used(child);
        std::string text = child.raw_text;
        std::replace(text.begin(), text.end(), '|', ' ');
        auto names = tokens(text);
        const auto duplicate = std::any_of(tables_.land_movement_classes.begin(), tables_.land_movement_classes.end(),
            [&](const auto& row) { return iequals(row.name, child.name); });
        if (names.empty() || duplicate) {
            report_.missing("MovementClassType", child.name, trim(child.raw_text), "empty or duplicate movement class");
            continue;
        }
        tables_.land_movement_classes.push_back({child.name, std::move(names)});
    }
}

// LC-01..LC-06: the ordered member list, the squad size, the team type and its speed source. Any
// member or reference that does not resolve leaves an unresolved row and the company out of the table.
void Loader::load_land_company(Object& object) {
    LandCompany company;
    company.id = object.effective.object_id;
    company.xml_type = object.effective.type_name;
    company.transport = trim(object.text("Company_Transport_Unit"));
    bool complete = true;
    std::string first_member_team;
    std::vector<std::string> ids;
    for (const auto* node : object.list("Company_Units", report_)) {
        if (data::retail::is_placeholder(node->raw_text)) continue; // AU-80/81: an empty or TBD value is no value
        for (auto& id : tokens(node->raw_text)) ids.push_back(std::move(id));
    }
    // LSL-05: an empty or placeholder list leaves the member list at its empty default, exactly as when the tag
    // is absent, so the company takes the LC-01 direct-object path.
    if (ids.empty()) {
        company.direct_object = true; // LC-01: no member list, the one object is attempted
        company.members.push_back(land_member(object));
        company.locomotion.push_back(land_locomotion(object));
        first_member_team = trim(object.text("Create_Team_Type"));
    }
    for (const auto& id : ids) {
        auto member = resolve(*input_.catalog, id, data::Category::game_object, company.id, "Company_Units", report_);
        if (!member) {
            complete = false;
            continue;
        }
        record_layers(*member);
        if (company.members.empty()) first_member_team = trim(member->text("Create_Team_Type"));
        enqueue(member->effective.object_id, UnitKind::ship); // the member's combat and durability body
        company.members.push_back(land_member(*member));
        company.locomotion.push_back(land_locomotion(*member));
        member->note_duplicates(report_);
    }
    // LSL-P2/LSL-07: a signed integer; a fraction truncates. A non-positive size has no verified grouping
    // meaning: it is noted and the company groups as with no authored size (one squad holds every member).
    if (const auto* node = object.single("Max_Squad_Size")) {
        if (const auto size = data::retail::integer_value(node->raw_text)) {
            if (size->value >= 1) company.max_squad_size = static_cast<std::uint32_t>(size->value);
            else report_.note(company.id, "Max_Squad_Size", trim(node->raw_text), "non-positive squad size groups as unsized (LSL-07, unverified)");
            if (!size->exact) report_.note(company.id, "Max_Squad_Size", trim(node->raw_text), "read as the retail integer parse does (LSL-P2)");
        }
    }
    // The company authors the team type in stock data; a first member that authors another is noted.
    company.team_type = trim(object.text("Create_Team_Type"));
    company.team_type_authored = !company.team_type.empty();
    if (!company.team_type.empty() && !first_member_team.empty() && !iequals(first_member_team, company.team_type))
        report_.note(company.id, "Create_Team_Type", first_member_team, "first member names another team type; the company's is used");
    if (!company.team_type.empty() || std::any_of(company.members.begin(), company.members.end(), [](const auto& member) { return member.create_team; })) {
        const auto selected_team = company.team_type.empty() ? std::string("Team") : company.team_type; // LC-81 schema fallback
        if (auto team = resolve(*input_.catalog, selected_team, data::Category::game_object, company.id,
                "Create_Team_Type", report_)) {
            record_layers(*team);
            company.team_type = team->effective.object_id;
            company.team_locomotor = locomotor_behavior(*team);
            const auto team_speed = team->retail_float("Max_Speed", report_); // LSL-P1
            if (!team_speed)
                report_.note(company.id, "Max_Speed", selected_team, "team type authors no Max_Speed; the engine default is used");
            company.team_max_speed = team_speed.value_or(Fixed::from_raw(Fixed::scale / 4)); // xml-fallback: Max_Speed
            company.team_formation_spacing = team->retail_float("FormationSpacing", report_);
            team->note_duplicates(report_);
        } else {
            complete = false;
        }
    }
    if (!company.formation_spacing && !company.locomotion.empty())
        company.formation_spacing = company.locomotion.front().formation_spacing;
    company.formation_spacing = company.formation_spacing.value_or(Fixed{});
    if (complete) tables_.land_companies.push_back(std::move(company));
}

} // namespace eawr::units::unit_tables_detail

namespace eawr::units {
UnitTables load_land_static_tables(const data::Catalog& catalog, const std::span<const std::string> types,
    ModelLookup model, FileDigest digest) {
    LoadInput input; input.domain = sim::tactical::BattleDomain::land; input.catalog = &catalog;
    input.model = std::move(model); input.digest = std::move(digest);
    UnitTables tables; tables.domain = input.domain;
    unit_tables_detail::Report report;
    unit_tables_detail::Loader(input, tables, report).read_land_statics(types);
    tables.unresolved = std::move(report.unresolved); tables.notes = std::move(report.notes);
    for (auto& [path, file] : report.inputs) tables.inputs.push_back(std::move(file));
    return tables;
}
} // namespace eawr::units
