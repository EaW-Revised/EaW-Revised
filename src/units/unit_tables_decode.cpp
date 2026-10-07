#include "unit_tables_decode.hpp"

namespace eawr::units::unit_tables_detail {

[[nodiscard]] bool is_weapon(const HardpointType type) noexcept {
    return type == HardpointType::weapon_laser || type == HardpointType::weapon_missile ||
           type == HardpointType::weapon_torpedo || type == HardpointType::weapon_ion_cannon ||
           type == HardpointType::weapon_mass_driver || type == HardpointType::weapon_special;
}

// REVEAL in the object's Behavior or SpaceBehavior list (V-01, #271). A space object reveals
// only through this behaviour; there is no default.
[[nodiscard]] bool has_behavior(Object& object, const std::string_view behavior) {
    for (const auto tag : {std::string_view{"Behavior"}, std::string_view{"SpaceBehavior"}}) {
        for (const auto& token : tokens(object.text(tag))) {
            if (iequals(token, behavior)) return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_reveal(Object& object) { return has_behavior(object, "REVEAL"); }
// SPACE_OBSTACLE in the Behavior or SpaceBehavior list (#71, research E71-17).
[[nodiscard]] bool has_space_obstacle(Object& object) { return has_behavior(object, "SPACE_OBSTACLE"); }

[[nodiscard]] std::vector<std::string> mask(const std::string_view text) {
    std::string spaced(text);
    std::replace(spaced.begin(), spaced.end(), '|', ' ');
    return tokens(spaced);
}

void Loader::enqueue(const std::string& id, const UnitKind kind) {
        if (queued_.insert(lower(id)).second) queue_.emplace_back(id, kind);
    }

void Loader::want_projectile(const std::string& id) {
        if (!id.empty() && wanted_projectiles_.insert(lower(id)).second) projectile_ids_.push_back(id);
    }

void Loader::record_layers(const Object& object) {
        for (const auto* layer : object.layers) {
            const auto& source = layer->root.source;
            const auto digest = xml_digests_.find(source.logical_path);
            report_.input({source.logical_path, source.source_id, source.layer_id,
                           digest == xml_digests_.end() ? std::string{} : digest->second});
        }
    }

const Frames* Loader::frames(const std::string& path, const std::string& owner) {
        const auto cached = models_.find(path);
        if (cached != models_.end()) return cached->second.model == nullptr ? nullptr : &cached->second;
        Frames entry;
        entry.model = input_.model ? input_.model(path) : nullptr;
        if (entry.model == nullptr) {
            report_.missing(owner, "model", path, "model is not loadable");
        } else {
            const auto& source = entry.model->source;
            report_.input({path, source.source_id, source.layer_id, input_.digest ? input_.digest(path) : std::string{}});
            auto built = bind_frames(*entry.model);
            if (!built) {
                report_.missing(owner, "model", path, built.error().message);
                entry.model = nullptr;
            } else {
                entry.frames = std::move(built).value();
            }
        }
        const auto& stored = models_.insert_or_assign(path, std::move(entry)).first->second;
        return stored.model == nullptr ? nullptr : &stored;
    }

    // A bone of the owner model; failing that, a bone of the hardpoint's
    // attached model (loaded only then) placed at the owner's attachment bone.
BonePoint Loader::point(const std::string& bone, const Frames* owner, const std::string& owner_id,
                    const std::string& field, const std::string& attached_path,
                    const std::optional<std::size_t> attach_bone) {
        BonePoint result;
        result.bone = bone;
        if (bone.empty() || owner == nullptr) return result;
        if (const auto index = bone_index(*owner->model, bone)) {
            result.position = translation(owner->frames[*index]);
            result.axes = axes(owner->frames[*index]);
            return result;
        }
        const Frames* attached = attached_path.empty() || !attach_bone ? nullptr : frames(attached_path, owner_id);
        if (attached != nullptr) {
            if (const auto index = bone_index(*attached->model, bone)) {
                auto frame = sim::math::compose(owner->frames[*attach_bone], attached->frames[*index]);
                if (frame) {
                    result.position = translation(frame.value());
                    result.axes = axes(frame.value());
                    result.from_attached_model = true;
                    return result;
                }
            }
        }
        report_.missing(owner_id, field, bone, "bone is not in " + owner->model->source.logical_path +
                        (attached != nullptr ? " or " + attached->model->source.logical_path : std::string{}));
        return result;
    }

std::optional<std::uint32_t> Loader::count(Object& object, const std::string_view tag, const bool required,
        const bool negative_unbounded) {
        const auto value = object.fixed(tag, report_, required);
        if (!value) return std::nullopt;
        const auto whole = value->raw() / Fixed::scale;
        // WPR-33: the debug-build type defaults to -1 and only zero/positive limits gate.
        if (negative_unbounded && value->raw() < 0 && value->raw() % Fixed::scale == 0) return std::nullopt;
        if (value->raw() < 0 || value->raw() % Fixed::scale != 0 || whole > std::numeric_limits<std::uint32_t>::max()) {
            report_.missing(object.effective.object_id, std::string(tag), {}, "not a whole nonnegative 32-bit count");
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(whole);
    }

std::vector<InaccuracyEntry> Loader::inaccuracy(Object& object, const std::string_view tag) {
        std::vector<InaccuracyEntry> result;
        for (const auto* node : object.list(tag, report_)) {
            const auto row = tokens(node->raw_text);
            const auto distance = row.size() == 2 ? number(row[1]) : std::nullopt;
            if (!distance) {
                report_.missing(object.effective.object_id, std::string(tag), trim(node->raw_text),
                                "expected category, distance");
                continue;
            }
            result.push_back({row[0], *distance});
        }
        return result;
    }

std::optional<bool> Loader::flag(Object& object, const std::string_view tag, const bool required) {
        const auto* node = object.single(tag);
        if (node == nullptr) {
            if (required) report_.missing(object.effective.object_id, std::string(tag), {}, "required tag is absent");
            return std::nullopt;
        }
        const auto value = boolean(node->raw_text);
        if (!value) report_.missing(object.effective.object_id, std::string(tag), trim(node->raw_text), "not a boolean");
        return value;
    }

void Loader::load_unit(const std::string& id, const UnitKind hint) {
        UnitType unit;
        unit.id = id;
        unit.kind = hint;
        auto object = resolve(*input_.catalog, id, data::Category::game_object, id, "type", report_);
        if (!object) {
            tables_.units.push_back(std::move(unit));
            return;
        }
        record_layers(*object);
        unit.id = object->effective.object_id;
        unit.xml_type = object->effective.type_name;
        unit.variant_chain = object->effective.chain;
        unit.valid_target = flag(*object, "Is_Valid_Target", false).value_or(true);
        unit.special_weapon = has_behavior(*object, "SPECIAL_WEAPON");
        unit.star_base = has_behavior(*object, "DUMMY_STAR_BASE");
        // WAD-38: the target marker is content, without a hull or locomotor.
        // Its collision model remains an ordinary projectile recipient.
        if (iequals(unit.id, "Dummy_Barrage_Target") && has_behavior(*object, "MARKER")) {
            if (!flag(*object, "Immune_To_Damage", true).value_or(false)) {
                report_.missing(unit.id, "Immune_To_Damage", {}, "barrage target must be immune");
            }
            unit.model = object->text("Model_Name");
            unit.model_path = model_path(unit.model);
            const auto* model = frames(unit.model_path, unit.id);
            if (model == nullptr) unit.model_path.clear();
            unit.scale_factor = object->fixed("Scale_Factor", report_, false);
            unit.living_projectile_collision = flag(*object, "Collidable_By_Projectile_Living", false).value_or(true);
            unit.influences_capture = flag(*object, "Influences_Capture_Point", false).value_or(true);
            mask_flags(*object, unit);
            if (model != nullptr) {
                unit.collision = collision_bounds(*model->model, model->frames);
                append_collision_meshes(*model->model, model->frames, sim::math::identity_matrix(), no_index,
                    unit.collision_meshes);
            }
            object->note_duplicates(report_);
            tables_.units.push_back(std::move(unit));
            return;
        }
        if (iequals(unit.xml_type, "StarBase")) unit.kind = UnitKind::station;
        else if (iequals(unit.xml_type, "Squadron")) unit.kind = UnitKind::squadron;
        else if (unit.kind == UnitKind::squadron || unit.kind == UnitKind::station) unit.kind = UnitKind::ship;
        // WHE-SQ-02: flight follows the effective locomotor, including a solo hero transport.
        if (unit.kind == UnitKind::ship && has_behavior(*object, "FIGHTER_LOCOMOTOR"))
            unit.kind = UnitKind::craft;
        unit.affiliation = object->text("Affiliation");
        unit.last_state_visible_under_fow = flag(*object, "Last_State_Visible_Under_FOW", false).value_or(false);
        unit.initial_state_visible_under_fow = flag(*object, "Initial_State_Visible_Under_FOW", false).value_or(false);
        // FW-30: the first matching faction mapping wins; malformed rows are reported.
        for (const auto* node : object->list("Faction_Anim_Subindex", report_)) {
            const auto row = tokens(node->raw_text);
            const auto index = row.size() == 2 ? number(row[1]) : std::nullopt;
            if (!index || index->raw() < 0 || index->raw() % Fixed::scale != 0
                || index->raw() / Fixed::scale > 65535) {
                report_.missing(unit.id, "Faction_Anim_Subindex", trim(node->raw_text), "expected faction, unsigned 16-bit index");
                continue;
            }
            if (iequals(row[0], "Neutral") && !unit.neutral_fog_animation_index) {
                unit.neutral_fog_animation_index = static_cast<std::uint32_t>(index->raw() / Fixed::scale);
            }
        }
        // WBF-45/46: scoring uses the original type for squadron and upgrade objects too.
        // These derived result fields do not enter content identity or tactical decisions.
        unit.score_cost_credits = object->fixed("Score_Cost_Credits", report_, false);
        unit.score_combat_power = object->fixed("AI_Combat_Power", report_, false);
        unit.named_hero = flag(*object, "Is_Named_Hero", false).value_or(false);
        unit.generic_hero = flag(*object, "Is_Generic_Hero", false).value_or(false);
        unit.display_contained_hero_bars = flag(*object, "Display_Contained_Hero_Grab_Bars", false).value_or(false);
        unit.redirect_damage_to_teammates = flag(*object, "Redirect_Damage_To_Teammates", false).value_or(false);
        unit.death_projectiles = tokens(object->text("Death_Projectiles"));
        if (!unit.death_projectiles.empty()) {
            for (const auto& projectile : unit.death_projectiles) want_projectile(projectile);
        }
        unit.capture_point = has_behavior(*object, "CAPTURE_POINT");
        unit.build_pad = has_behavior(*object, "TACTICAL_BUILD_OBJECTS");
        unit.under_construction = has_behavior(*object, "TACTICAL_UNDER_CONSTRUCTION");
        unit.tactical_sale = has_behavior(*object, "TACTICAL_SELL");
        if (unit.tactical_sale) unit.tactical_sell_percentage = object->fixed("Tactical_Sell_Percentage", report_, false);
        unit.tactical_respawn_seconds = object->fixed("Tactical_Respawn_Time_In_Secs", report_, false);
        // WBP-50/51, WAD-14: admission is opt-in on each effective object type.
        unit.living_projectile_collision = flag(*object, "Collidable_By_Projectile_Living", false).value_or(false);
        unit.influences_capture = flag(*object, "Influences_Capture_Point", false).value_or(true);
        unit.construction_blocker = !has_behavior(*object, "BASE_SHIELD")
            && !has_behavior(*object, "DUMMY_GROUND_STRUCTURE");
        if (unit.capture_point) {
            unit.capture_radius = object->fixed("Capture_Point_Radius", report_, true);
            unit.capture_seconds = object->fixed("Capture_Point_Transition_Time_Seconds", report_, true);
            unit.ownership_sticks = flag(*object, "Ownership_Sticks", false).value_or(false);
            unit.community_property = flag(*object, "Is_Community_Property", false).value_or(false);
        }
        if (unit.kind == UnitKind::station) {
            // WSU-16: skirmish stations share selection without being capture points.
            unit.station_community_property = flag(*object, "Is_Community_Property", false).value_or(false);
        }
        if (unit.under_construction) {
            unit.constructed_type = trim(object->text("Tactical_Buildable_Constructed"));
            if (!unit.constructed_type.empty()) enqueue(unit.constructed_type, UnitKind::ship);
        }
        if (unit.build_pad) {
            unit.child_persists = flag(*object, "Tactically_Built_Child_Object_Persists", false).value_or(true);
            unit.destroy_when_child_dies = flag(*object, "Destroy_When_Child_Dies", false).value_or(false);
            unit.pad_rebuild_seconds = object->fixed("Minimum_Time_Before_Pad_Can_Build_Again", report_, false);
        }
        if (unit.affiliation.empty()) report_.missing(unit.id, "Affiliation", {}, "required tag is absent");
        const bool hero_company = iequals(unit.xml_type, "HeroCompany");
        const bool carried_hero = iequals(unit.xml_type, "HeroUnit") || iequals(unit.xml_type, "GenericHeroUnit");
        if (hero_company) load_company(*object, unit);
        if (iequals(unit.xml_type, "UpgradeObject") || has_behavior(*object, "DUMMY_UPGRADE") || hero_company || carried_hero) mask_flags(*object, unit);
        else if (unit.kind == UnitKind::squadron) load_squadron(*object, unit);
        else load_body(*object, unit);
        if (!hero_company) load_abilities(*object, unit);
        load_production(*object, unit);
        if (unit.build_pad) {
            for (const auto& group : unit.production.buildable) {
                for (const auto& child : group.types) enqueue(child, UnitKind::ship);
            }
            const auto attachment = trim(object->text("Tactical_Build_Attachment_Bone_Name"));
            if (!attachment.empty() && !unit.model_path.empty()) {
                unit.build_attachment = point(attachment, frames(unit.model_path, unit.id), unit.id,
                    "Tactical_Build_Attachment_Bone_Name");
            }
        }
        unit.lua_script = object->text("Lua_Script");
        object->note_duplicates(report_);
        tables_.units.push_back(std::move(unit));
    }

void Loader::load_company(Object& object, UnitType& unit) {
    unit.company_transport = trim(object.text("Company_Transport_Unit"));
    bool named_transport = false;
    if (!unit.company_transport.empty()) {
        auto transport = resolve(*input_.catalog, unit.company_transport, data::Category::game_object,
            unit.id, "Company_Transport_Unit", report_);
        if (transport) {
            record_layers(*transport);
            named_transport = flag(*transport, "Is_Named_Hero", false).value_or(false);
        }
    }
    for (const auto* node : object.list("Company_Units", report_)) {
        for (const auto& id : tokens(node->raw_text)) {
            auto member = resolve(*input_.catalog, id, data::Category::game_object, unit.id, "Company_Units", report_);
            if (!member) continue;
            record_layers(*member);
            UnitType::CompanyMember entry;
            entry.type = member->effective.object_id;
            entry.named_hero = flag(*member, "Is_Named_Hero", false).value_or(false);
            entry.generic_hero = flag(*member, "Is_Generic_Hero", false).value_or(false);
            entry.attach_to_flagship = flag(*member, "Attach_To_Flagship_During_Space_Battle", false).value_or(false);
            entry.unique_space_unit = trim(member->text("Unique_Space_Unit"));
            unit.company_members.push_back(std::move(entry));
        }
    }
    unit.deployed_space_type = unit.company_transport;
    // WHE-49: the first qualifying member wins, even when its unique mapping is absent.
    if (!named_transport) {
        for (const auto& member : unit.company_members) {
            if (!member.named_hero && !member.generic_hero) continue;
            unit.creates_carried_heroes = true;
            if (!member.unique_space_unit.empty()) unit.deployed_space_type = member.unique_space_unit;
            break;
        }
    }
    if (unit.creates_carried_heroes) {
        for (const auto& member : unit.company_members)
            if (member.named_hero || member.generic_hero) enqueue(member.type, UnitKind::ship);
    }
    if (unit.deployed_space_type.empty()) {
        report_.missing(unit.id, "Company_Transport_Unit", {}, "space company has no deployable ship");
        return;
    }
    auto ship = resolve(*input_.catalog, unit.deployed_space_type, data::Category::game_object,
        unit.id, "space deployment", report_);
    if (!ship) return;
    record_layers(*ship);
    // WHE-49: a unique ship may form an authored team during creation.
    if (flag(*ship, "Create_Team", false).value_or(false)) {
        const auto team = trim(ship->text("Create_Team_Type"));
        if (team.empty()) report_.missing(unit.id, "Create_Team_Type", {}, "created space team has no type");
        else {
            unit.deployed_space_type = team;
            enqueue(team, UnitKind::squadron);
        }
    } else enqueue(unit.deployed_space_type, UnitKind::ship);
}

    // #71: a map object type's footprint alone.
void Loader::load_obstacle(const std::string& id) {
        if (tables_.find(id) != nullptr) return; // live capture type already supplies its footprint
        auto object = resolve(*input_.catalog, id, data::Category::game_object, id, "obstacle", report_);
        if (!object) return;
        record_layers(*object);
        ObstacleType obstacle;
        obstacle.id = object->effective.object_id;
        obstacle.xml_type = object->effective.type_name;
        obstacle.space_layer = trim(object->text("Space_Layer"));
        obstacle.scale_factor = object->fixed("Scale_Factor", report_, false);
        obstacle.influences_capture = flag(*object, "Influences_Capture_Point", false).value_or(true);
        obstacle.living_projectile_collision = flag(*object, "Collidable_By_Projectile_Living", false).value_or(false);
        obstacle.construction_blocker = !has_behavior(*object, "BASE_SHIELD")
            && !has_behavior(*object, "DUMMY_GROUND_STRUCTURE");
        const Frames* model = nullptr;
        if (const auto name = object->text("Space_Model_Name"); !name.empty()) {
            obstacle.model_path = model_path(name);
            model = frames(obstacle.model_path, obstacle.id);
            if (model == nullptr) obstacle.model_path.clear();
        }
        obstacle.footprint = read_footprint(*object, model);
        // WSU-13: a drawn hazard volume does not automatically admit a mouse contact.
        UnitType selection;
        load_selection(*object, selection);
        obstacle.selectable = selection.selectable;
        obstacle.mouse_sensitive = selection.mouse_sensitive;
        obstacle.last_state_visible_under_fow = flag(*object, "Last_State_Visible_Under_FOW", false).value_or(false);
        obstacle.initial_state_visible_under_fow = flag(*object, "Initial_State_Visible_Under_FOW", false).value_or(false);
        object->note_duplicates(report_);
        tables_.obstacles.push_back(std::move(obstacle));
    }

SpaceFootprint Loader::read_footprint(Object& object, const Frames* model) {
        SpaceFootprint footprint;
        footprint.space_obstacle = has_space_obstacle(object);
        footprint.custom_hard_x = object.fixed("Custom_Hard_XExtent", report_, false);
        footprint.custom_hard_y = object.fixed("Custom_Hard_YExtent", report_, false);
        footprint.custom_soft_radius = object.fixed("Custom_Soft_Footprint_Radius", report_, false);
        footprint.obstacle_radius = object.fixed("Space_Obstacle_Radius", report_, false);
        auto& hazard = footprint.hazard;
        hazard.behavior = tokens(object.text("Behavior"));
        hazard.space_behavior = tokens(object.text("SpaceBehavior"));
        hazard.asteroid_field = flag(object, "Is_Asteroid_Field", false).value_or(false);
        hazard.ion_storm = flag(object, "Is_Ion_Storm", false).value_or(false);
        hazard.nebula = flag(object, "Is_Nebula", false).value_or(false);
        hazard.impassable_asteroid = flag(object, "Is_Impassable_Asteroid", false).value_or(false);
        hazard.asteroid_damage = has_behavior(object, "ASTEROID_FIELD_DAMAGE");
        hazard.nebula_service = has_behavior(object, "NEBULA");
        const auto offset = tokens(object.text("Space_Obstacle_Offset"));
        if (!offset.empty()) {
            if (offset.size() == 3 && number(offset[0]) && number(offset[1]) && number(offset[2])) {
                hazard.obstacle_offset = {*number(offset[0]), *number(offset[1]), *number(offset[2])};
            } else {
                report_.missing(object.effective.object_id, "Space_Obstacle_Offset", object.text("Space_Obstacle_Offset"),
                                "expected three fixed-point coordinates");
            }
        }
        if (model != nullptr) {
            if (const auto extents = collision_half_extents(*model->model, model->frames)) {
                footprint.collision_x = extents->first;
                footprint.collision_y = extents->second;
            }
        }
        return footprint;
    }

void Loader::load_projectile(const std::string& id) {
        auto object = resolve(*input_.catalog, id, data::Category::game_object, id, "projectile", report_);
        if (!object) return;
        record_layers(*object);
        Projectile projectile;
        projectile.id = object->effective.object_id;
        bool spawned = false;
        bool death_payload = false;
        bool fired = false;
        for (const auto& unit : tables_.units) {
            for (const auto& entry : unit.death_projectiles) death_payload |= iequals(entry, projectile.id);
            for (const auto* list : {&unit.abilities, &unit.team_abilities}) for (const auto& ability : *list) {
                spawned |= iequals(ability.spawned_object, projectile.id);
                fired |= iequals(ability.projectile_override, projectile.id);
            }
            if (unit.weapon) fired |= iequals(unit.weapon->projectile, projectile.id);
            for (const auto& point : unit.hardpoints) if (point.weapon)
                fired |= iequals(point.weapon->projectile, projectile.id);
        }
        // WHE-61/62: an ordinary ability creates its object directly; gun flight/category
        // fields are not prerequisites for that creation or its countdown detonation.
        const bool spawned_only = spawned && !fired;
        projectile.damage = object->fixed("Projectile_Damage", report_, true);
        projectile.damage_type = object->text("Damage_Type");
        projectile.max_speed = object->fixed("Max_Speed", report_, true);
        projectile.max_rate_of_turn = object->fixed("Max_Rate_Of_Turn", report_, false);
        projectile.max_flight_distance = object->fixed("Projectile_Max_Flight_Distance", report_, !spawned_only);
        projectile.category = object->text("Projectile_Category");
        // RFL-01..08: retain authored endpoints; unsupported rocket constructions
        // remain a combat-conversion error, never silently become straight missiles.
        projectile.max_lifetime = object->fixed("Projectile_Max_Lifetime", report_, false);
        projectile.explode_at_target_radius = flag(*object, "Explode_When_Reached_Target_Radius", false);
        if (iequals(projectile.category, "ROCKET")) {
            projectile.rocket_curve_distance = object->fixed("Projectile_Rocket_Curve_Distance", report_, false);
            projectile.rocket_curve_offset = object->fixed("Projectile_Rocket_Curve_Offset", report_, false);
            projectile.rocket_straight_distance = object->fixed("Projectile_Rocket_Straight_Distance", report_, false);
        }
        // WNO-30: the created death payload has flight/lifetime but no authored category.
        if (projectile.category.empty() && !spawned_only && !(death_payload && !fired))
            report_.missing(projectile.id, "Projectile_Category", {}, "required tag is absent");
        projectile.does_shield_damage = flag(*object, "Projectile_Does_Shield_Damage", true).value_or(false);
        projectile.does_energy_damage = flag(*object, "Projectile_Does_Energy_Damage", false).value_or(false);
        projectile.disables_engines_when_power_drained =
            flag(*object, "Projectile_Disables_Engines_When_Power_Drained", false).value_or(false);
        if (projectile.disables_engines_when_power_drained) {
            projectile.disable_engines_duration = object->fixed("Projectile_Disable_Engines_Duration", report_, true);
        }
        projectile.does_hitpoint_damage = flag(*object, "Projectile_Does_Hitpoint_Damage", true).value_or(false);
        projectile.energy_per_shot = object->fixed("Projectile_Energy_Per_Shot", report_, false);
        projectile.weaken.on_detonation = flag(*object, "Projectile_Weaken_Enemy_On_Detonation", false).value_or(false);
        if (projectile.weaken.on_detonation) {
            auto& weaken = projectile.weaken;
            weaken.radius = object->fixed("Projectile_Weaken_Enemy_Radius", report_, true).value_or(Fixed{});
            weaken.take_damage_increase = object->fixed("Projectile_Weaken_Enemy_Take_Damage_Increase_Percent", report_, false).value_or(Fixed{});
            weaken.cause_damage_reduction = object->fixed("Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent", report_, false).value_or(Fixed{});
            const auto duration = object->fixed("Projectile_Weaken_Enemy_Duration_Seconds", report_, true).value_or(Fixed{});
            weaken.duration_frames = static_cast<std::uint32_t>(std::clamp<std::int64_t>(duration.raw(), 0, 3600 * Fixed::scale)
                * sim::tactical::logical_frames_per_second / Fixed::scale);
            weaken.categories = enum_bits(tables_.categories, mask(object->text("Projectile_Weaken_Enemy_Targets_Category_Mask")),
                projectile.id, "Projectile_Weaken_Enemy_Targets_Category_Mask", report_);
            weaken.status_effect = object->text("Projectile_Weaken_Enemy_Spawn_Effect");
        }
        // WAD-01: resolve variants first, then apply the debug-build defaults.
        projectile.blast.damage = object->fixed("Projectile_Blast_Area_Damage", report_, false).value_or(Fixed{});
        projectile.blast.radius = object->fixed("Projectile_Blast_Area_Range", report_, false).value_or(Fixed{});
        projectile.blast.dropoff = flag(*object, "Projectile_Blast_Area_Dropoff", false).value_or(false);
        const auto signed_count = [&](const std::string_view tag, const std::int32_t fallback) {
            const auto value = object->fixed(tag, report_, false);
            if (!value) return fallback;
            const auto whole = value->trunc_to_integer();
            if (value->raw() % Fixed::scale != 0 || whole < std::numeric_limits<std::int32_t>::min()
                || whole > std::numeric_limits<std::int32_t>::max()) {
                report_.missing(projectile.id, std::string(tag), {}, "expected a signed integer");
                return fallback;
            }
            return static_cast<std::int32_t>(whole);
        };
        projectile.blast.tiers = signed_count("Projectile_Blast_Area_Dropoff_Tiers", 5);
        projectile.blast.max_victims = signed_count("Projectile_Blast_Area_Max_Victims", 5000);
        projectile.blast.max_delay = object->fixed("Max_Secs_For_AE_Delayed_Damage", report_, false)
            .value_or(projectile.blast.max_delay);
        projectile.blast_immune_faction = object->text("Projectile_Blast_Area_Immune_Faction");
        projectile.damage_delay = object->fixed("Projectile_Damage_Delay_Secs", report_, false).value_or(Fixed{});
        if (!projectile.blast_immune_faction.empty()) {
            auto faction = resolve(*input_.catalog, projectile.blast_immune_faction, data::Category::faction,
                projectile.id, "blast immune faction", report_);
            if (faction) projectile.blast.immune_faction = assets::object_type_crc(faction->effective.object_id);
        }
        projectile.ai_combat_power = object->fixed("AI_Combat_Power", report_, false);
        projectile.ion_stun = flag(*object, "Projectile_Ion_Stun_On_Detonation", false).value_or(false);
        if (projectile.ion_stun) {
            projectile.ion_stun_duration = object->fixed("Projectile_Ion_Stun_Duration", report_, false);
            projectile.ion_stun_speed_reduction = object->fixed("Projectile_Ion_Stun_Speed_Reduction_Percent", report_, false);
            projectile.ion_stun_shot_rate_reduction =
                object->fixed("Projectile_Ion_Stun_Shot_Rate_Reduction_Percent", report_, false);
            projectile.ion_stun_stack_duration = flag(*object, "Projectile_Ion_Stun_Stack_Duration", false).value_or(false);
            projectile.ion_stun_radius = object->fixed("Projectile_Ion_Stun_Radius", report_, false);
        }
        object->note_duplicates(report_);
        tables_.projectiles.push_back(std::move(projectile));
    }

} // namespace eawr::units::unit_tables_detail
