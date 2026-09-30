#include "unit_internal.hpp"

#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace eawr::units {
namespace {

using namespace detail;

constexpr std::string_view pinned[] = {
    "Skirmish_Rebel_Star_Base_1",
    "Skirmish_Empire_Star_Base_1",
    "Corellian_Corvette",
    "Nebulon_B_Frigate",
    "Tartan_Patrol_Cruiser",
    "Acclamator_Assault_Ship",
    "Calamari_Cruiser",
    "Rebel_X-Wing_Squadron",
    "Y-Wing_Squadron",
    "TIE_Interceptor_Squadron",
    "TIE_Fighter_Squadron",
    "TIE_Bomber_Squadron",
};

// The M2 map's other object types (#71): only their footprints load.
constexpr std::string_view pinned_obstacles[] = {
    "Skirmish_Merchant_Dock",
    "N_Gravity_Well_Station",
    "Defense_Satellite_Laser_Pad",
    "Mineral_Extractor_Pad",
    "Orbital_Resource_Container",
};

struct HardpointTypeName final {
    std::string_view name;
    HardpointType type;
};

constexpr HardpointTypeName hardpoint_types[] = {
    {"HARD_POINT_WEAPON_LASER", HardpointType::weapon_laser},
    {"HARD_POINT_WEAPON_MISSILE", HardpointType::weapon_missile},
    {"HARD_POINT_WEAPON_TORPEDO", HardpointType::weapon_torpedo},
    {"HARD_POINT_WEAPON_ION_CANNON", HardpointType::weapon_ion_cannon},
    {"HARD_POINT_SHIELD_GENERATOR", HardpointType::shield_generator},
    {"HARD_POINT_ENGINE", HardpointType::engine},
    {"HARD_POINT_FIGHTER_BAY", HardpointType::fighter_bay},
    {"HARD_POINT_TRACTOR_BEAM", HardpointType::tractor_beam},
    {"HARD_POINT_GRAVITY_WELL", HardpointType::gravity_well},
    {"HARD_POINT_ENABLE_SPECIAL_ABILITY", HardpointType::enable_special_ability},
    {"HARD_POINT_DUMMY_ART", HardpointType::dummy_art},
};

[[nodiscard]] bool is_weapon(const HardpointType type) noexcept {
    return type == HardpointType::weapon_laser || type == HardpointType::weapon_missile ||
           type == HardpointType::weapon_torpedo || type == HardpointType::weapon_ion_cannon;
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

struct Frames final {
    const assets::Model* model{};
    std::vector<sim::math::Mat3x4> frames;
};

class Loader final {
public:
    Loader(const LoadInput& input, UnitTables& tables, Report& report)
        : input_(input), tables_(tables), report_(report) {
        for (const auto& file : input.catalog->registry_files()) {
            if (file.source && file.input_sha256) xml_digests_.emplace(file.source->logical_path, *file.input_sha256);
        }
    }

    void run(const std::vector<std::string>& types, const std::vector<std::string>& obstacles) {
        obstacle_ids_ = obstacles;
        load_enums(*input_.filesystem, tables_, report_);
        for (const auto& id : types) enqueue(id, UnitKind::ship);
        for (std::size_t index = 0; index < queue_.size(); ++index) {
            const auto [id, kind] = queue_[index];
            load_unit(id, kind);
        }
        for (const auto& id : projectile_ids_) load_projectile(id);
        for (const auto& id : obstacle_ids_) load_obstacle(id);
        load_priority_sets(*input_.filesystem, priority_ids_, tables_, report_);
        link();
        std::set<std::string> damage;
        std::set<std::string> armor;
        const auto add = [](std::set<std::string>& set, const std::string& value) {
            if (!value.empty()) set.insert(lower(value));
        };
        for (const auto& unit : tables_.units) {
            add(armor, unit.armor_type);
            add(armor, unit.shield_armor_type);
            add(damage, unit.damage_type);
            if (unit.weapon) add(damage, unit.weapon->damage_type);
            for (const auto& hardpoint : unit.hardpoints) {
                if (hardpoint.weapon) add(damage, hardpoint.weapon->damage_type);
            }
        }
        for (const auto& projectile : tables_.projectiles) add(damage, projectile.damage_type);
        load_constants(*input_.filesystem, damage, armor, tables_, report_);
    }

private:
    void enqueue(const std::string& id, const UnitKind kind) {
        if (queued_.insert(lower(id)).second) queue_.emplace_back(id, kind);
    }

    void want_projectile(const std::string& id) {
        if (!id.empty() && wanted_projectiles_.insert(lower(id)).second) projectile_ids_.push_back(id);
    }

    void record_layers(const Object& object) {
        for (const auto* layer : object.layers) {
            const auto& source = layer->root.source;
            const auto digest = xml_digests_.find(source.logical_path);
            report_.input({source.logical_path, source.source_id, source.layer_id,
                           digest == xml_digests_.end() ? std::string{} : digest->second});
        }
    }

    const Frames* frames(const std::string& path, const std::string& owner) {
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
    BonePoint point(const std::string& bone, const Frames* owner, const std::string& owner_id,
                    const std::string& field, const std::string& attached_path = {},
                    const std::optional<std::size_t> attach_bone = std::nullopt) {
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

    std::optional<std::uint32_t> count(Object& object, const std::string_view tag, const bool required) {
        const auto value = object.fixed(tag, report_, required);
        if (!value) return std::nullopt;
        const auto whole = value->raw() / Fixed::scale;
        if (value->raw() < 0 || value->raw() % Fixed::scale != 0 || whole > std::numeric_limits<std::uint32_t>::max()) {
            report_.missing(object.effective.object_id, std::string(tag), {}, "not a whole nonnegative 32-bit count");
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(whole);
    }

    std::vector<InaccuracyEntry> inaccuracy(Object& object, const std::string_view tag) {
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

    std::optional<bool> flag(Object& object, const std::string_view tag, const bool required) {
        const auto* node = object.single(tag);
        if (node == nullptr) {
            if (required) report_.missing(object.effective.object_id, std::string(tag), {}, "required tag is absent");
            return std::nullopt;
        }
        const auto value = boolean(node->raw_text);
        if (!value) report_.missing(object.effective.object_id, std::string(tag), trim(node->raw_text), "not a boolean");
        return value;
    }

    void load_hardpoint(const std::string& id, UnitType& unit, const Frames* owner) {
        auto object = resolve(*input_.catalog, id, unit.id, "hardpoint", report_);
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

        if (is_weapon(hardpoint.type)) {
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

    std::vector<SpawnEntry> spawn_list(Object& object, const std::string_view tag, const bool enqueue_squadrons) {
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

    void load_spawner(Object& object, UnitType& unit) {
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

    void load_abilities(Object& object, UnitType& unit) {
        if (const auto* data = object.single("Unit_Abilities_Data")) {
            for (const auto& node : data->children) {
                if (!iequals(node.name, "Unit_Ability")) continue;
                data::tag_trace::used(node);
                Ability ability;
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
                    }
                }
                if (ability.type.empty()) report_.missing(unit.id, "Unit_Ability", {}, "ability has no Type");
                unit.abilities.push_back(std::move(ability));
            }
        }
        if (const auto* abilities = object.single("Abilities")) {
            for (const auto& node : abilities->children) {
                std::string name;
                for (const auto& attribute : node.attributes) {
                    if (iequals(attribute.name, "Name")) name = attribute.value;
                }
                unit.inactive_abilities.push_back(node.name + (name.empty() ? "" : " " + name));
            }
        }
    }

    void load_body(Object& object, UnitType& unit) {
        const bool station = unit.kind == UnitKind::station;
        const bool mobile = !station;
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
        unit.shield_points = object.fixed("Shield_Points", report_, true);
        const bool shielded = unit.shield_points && unit.shield_points->raw() > 0;
        unit.shield_refresh_rate = object.fixed("Shield_Refresh_Rate", report_, shielded);
        unit.energy_capacity = object.fixed("Energy_Capacity", report_, false);
        unit.energy_refresh_rate = object.fixed("Energy_Refresh_Rate", report_, false);
        unit.armor_type = object.text("Armor_Type");
        if (unit.armor_type.empty()) report_.missing(unit.id, "Armor_Type", {}, "required tag is absent");
        unit.shield_armor_type = object.text("Shield_Armor_Type");
        if (shielded && unit.shield_armor_type.empty()) {
            report_.missing(unit.id, "Shield_Armor_Type", {}, "shielded unit has no shield armor type");
        }
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
        unit.targeting_max_attack_distance = object.fixed("Targeting_Max_Attack_Distance", report_, true);
        unit.targeting_stickiness_seconds = object.fixed("Targeting_Stickiness_Time_Threshold", report_, false);
        unit.strafe_distance = object.fixed("Strafe_Distance", report_, false);
        unit.spin_away_on_death = flag(object, "Spin_Away_On_Death", false).value_or(false);
        if (unit.spin_away_on_death) {
            unit.spin_away_chance = object.fixed("Spin_Away_On_Death_Chance", report_, true);
            unit.spin_away_time = object.fixed("Spin_Away_On_Death_Time", report_, true);
        }
        unit.out_of_combat_defense = object.fixed("Out_Of_Combat_Defense_Adjustment", report_, false);
        unit.follow_distance = object.fixed("Minimum_Follow_Distance", report_, false);
        if (mask_flags(object, unit).empty()) report_.missing(unit.id, "CategoryMask", {}, "required tag is absent");
        unit.ai_combat_power = object.fixed("AI_Combat_Power", report_, true);
        unit.has_space_evaluator = flag(object, "Has_Space_Evaluator", false).value_or(false);
        unit.tech_level = count(object, "Tech_Level", false).value_or(0);
        unit.space_fow_reveal_range = object.fixed("Space_FOW_Reveal_Range", report_, true);
        unit.reveal = has_reveal(object);
        // #74: the shield behaviour, and what projectiles hit.
        unit.shielded = has_behavior(object, "SHIELDED");
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
            // A unit that aims its own weapon (Fires_Forward no) fires only within its turret
            // extents about its facing; the cone fields carry them (space-weapon-fire W-09).
            if (flag(object, "Fires_Forward", false) == false) {
                weapon.cone_width_degrees = object.fixed("Turret_Rotate_Extent_Degrees", report_, false);
                weapon.cone_height_degrees = object.fixed("Turret_Elevate_Extent_Degrees", report_, false);
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

    void load_squadron(Object& object, UnitType& unit) {
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
    const std::vector<std::string>& mask_flags(Object& object, UnitType& unit) {
        unit.category_mask = mask(object.text("CategoryMask"));
        unit.category_bits = enum_bits(tables_.categories, unit.category_mask, unit.id, "CategoryMask", report_);
        unit.property_flags = mask(object.text("Property_Flags"));
        unit.property_bits = enum_bits(tables_.properties, unit.property_flags, unit.id, "Property_Flags", report_);
        return unit.category_mask;
    }

    // #271: a squadron reveals through the team container it spawns.
    void load_team(Object& object, UnitType& unit) {
        unit.team_type = object.text("Create_Team_Type");
        if (unit.team_type.empty()) unit.team_type = "Team";
        auto team = resolve(*input_.catalog, unit.team_type, unit.id, "Create_Team_Type", report_);
        if (!team) return;
        record_layers(*team);
        unit.team_type = team->effective.object_id;
        if (has_reveal(*team)) unit.team_reveal_range = team->fixed("Space_FOW_Reveal_Range", report_, true);
        // AB-60 (#561): a team ability's data lives on the team container.
        UnitType container;
        container.id = unit.team_type;
        load_abilities(*team, container);
        unit.team_abilities = std::move(container.abilities);
        team->note_duplicates(report_);
    }

    void load_unit(const std::string& id, const UnitKind hint) {
        UnitType unit;
        unit.id = id;
        unit.kind = hint;
        auto object = resolve(*input_.catalog, id, id, "type", report_);
        if (!object) {
            tables_.units.push_back(std::move(unit));
            return;
        }
        record_layers(*object);
        unit.id = object->effective.object_id;
        unit.xml_type = object->effective.type_name;
        unit.variant_chain = object->effective.chain;
        if (iequals(unit.xml_type, "StarBase")) unit.kind = UnitKind::station;
        else if (iequals(unit.xml_type, "Squadron")) unit.kind = UnitKind::squadron;
        else if (unit.kind == UnitKind::squadron || unit.kind == UnitKind::station) unit.kind = UnitKind::ship;
        unit.affiliation = object->text("Affiliation");
        if (unit.affiliation.empty()) report_.missing(unit.id, "Affiliation", {}, "required tag is absent");
        if (unit.kind == UnitKind::squadron) load_squadron(*object, unit);
        else load_body(*object, unit);
        load_abilities(*object, unit);
        unit.lua_script = object->text("Lua_Script");
        object->note_duplicates(report_);
        tables_.units.push_back(std::move(unit));
    }

    // #71: a map object type's footprint alone.
    void load_obstacle(const std::string& id) {
        auto object = resolve(*input_.catalog, id, id, "obstacle", report_);
        if (!object) return;
        record_layers(*object);
        ObstacleType obstacle;
        obstacle.id = object->effective.object_id;
        obstacle.xml_type = object->effective.type_name;
        obstacle.space_layer = trim(object->text("Space_Layer"));
        obstacle.scale_factor = object->fixed("Scale_Factor", report_, false);
        const Frames* model = nullptr;
        if (const auto name = object->text("Space_Model_Name"); !name.empty()) {
            obstacle.model_path = model_path(name);
            model = frames(obstacle.model_path, obstacle.id);
            if (model == nullptr) obstacle.model_path.clear();
        }
        obstacle.footprint = read_footprint(*object, model);
        object->note_duplicates(report_);
        tables_.obstacles.push_back(std::move(obstacle));
    }

    SpaceFootprint read_footprint(Object& object, const Frames* model) {
        SpaceFootprint footprint;
        footprint.space_obstacle = has_space_obstacle(object);
        footprint.custom_hard_x = object.fixed("Custom_Hard_XExtent", report_, false);
        footprint.custom_hard_y = object.fixed("Custom_Hard_YExtent", report_, false);
        footprint.custom_soft_radius = object.fixed("Custom_Soft_Footprint_Radius", report_, false);
        footprint.obstacle_radius = object.fixed("Space_Obstacle_Radius", report_, false);
        if (model != nullptr) {
            if (const auto extents = collision_half_extents(*model->model, model->frames)) {
                footprint.collision_x = extents->first;
                footprint.collision_y = extents->second;
            }
        }
        return footprint;
    }

    void load_projectile(const std::string& id) {
        auto object = resolve(*input_.catalog, id, id, "projectile", report_);
        if (!object) return;
        record_layers(*object);
        Projectile projectile;
        projectile.id = object->effective.object_id;
        projectile.damage = object->fixed("Projectile_Damage", report_, true);
        projectile.damage_type = object->text("Damage_Type");
        projectile.max_speed = object->fixed("Max_Speed", report_, true);
        projectile.max_rate_of_turn = object->fixed("Max_Rate_Of_Turn", report_, false);
        projectile.max_flight_distance = object->fixed("Projectile_Max_Flight_Distance", report_, true);
        projectile.category = object->text("Projectile_Category");
        if (projectile.category.empty()) report_.missing(projectile.id, "Projectile_Category", {}, "required tag is absent");
        projectile.does_shield_damage = flag(*object, "Projectile_Does_Shield_Damage", true).value_or(false);
        projectile.does_energy_damage = flag(*object, "Projectile_Does_Energy_Damage", false).value_or(false);
        projectile.does_hitpoint_damage = flag(*object, "Projectile_Does_Hitpoint_Damage", true).value_or(false);
        projectile.energy_per_shot = object->fixed("Projectile_Energy_Per_Shot", report_, false);
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

    // Resolve cross-table references to indices once every table is loaded.
    void link() {
        const auto unit_index = [&](const std::string& id) {
            for (std::size_t index = 0; index < tables_.units.size(); ++index) {
                if (iequals(tables_.units[index].id, id) && !tables_.units[index].xml_type.empty()) {
                    return static_cast<std::uint32_t>(index);
                }
            }
            return no_index;
        };
        const auto projectile_index = [&](const std::string& id) {
            for (std::size_t index = 0; index < tables_.projectiles.size(); ++index) {
                if (iequals(tables_.projectiles[index].id, id)) return static_cast<std::uint32_t>(index);
            }
            return no_index;
        };
        for (auto& unit : tables_.units) {
            for (auto& member : unit.members) member.craft_index = unit_index(member.craft);
            if (unit.spawner) {
                for (auto& entry : unit.spawner->starting) entry.squadron_index = unit_index(entry.squadron);
                for (auto& entry : unit.spawner->reserves) entry.squadron_index = unit_index(entry.squadron);
            }
            if (unit.weapon) unit.weapon->projectile_index = projectile_index(unit.weapon->projectile);
            for (auto* list : {&unit.abilities, &unit.team_abilities}) {
                for (auto& ability : *list) ability.projectile_index = projectile_index(ability.projectile_override);
            }
            for (auto& hardpoint : unit.hardpoints) {
                if (hardpoint.weapon) hardpoint.weapon->projectile_index = projectile_index(hardpoint.weapon->projectile);
            }
            for (std::size_t index = 0; index < tables_.priority_sets.size(); ++index) {
                if (!unit.targeting_priority_set.empty() && iequals(tables_.priority_sets[index].id, unit.targeting_priority_set)) {
                    unit.targeting_priority_set_index = static_cast<std::uint32_t>(index);
                }
            }
        }
    }

    const LoadInput& input_;
    UnitTables& tables_;
    Report& report_;
    std::map<std::string, std::string> xml_digests_;
    std::map<std::string, Frames> models_;
    std::vector<std::pair<std::string, UnitKind>> queue_;
    std::set<std::string> queued_;
    std::vector<std::string> projectile_ids_;
    std::vector<std::string> obstacle_ids_;
    std::set<std::string> wanted_projectiles_;
    std::vector<std::string> priority_ids_;
};

} // namespace

std::optional<Fixed> sensor_range(const UnitType& type) noexcept {
    if (type.kind == UnitKind::squadron) return type.team_reveal_range;
    return type.reveal ? type.space_fow_reveal_range : std::nullopt;
}

std::span<const std::string_view> pinned_m2_types() noexcept {
    return pinned;
}

std::span<const std::string_view> pinned_m2_obstacles() noexcept {
    return pinned_obstacles;
}

const UnitType* UnitTables::find(const std::string_view id) const noexcept {
    for (const auto& unit : units) {
        if (iequals(unit.id, id)) return &unit;
    }
    return nullptr;
}

core::Result<UnitTables> load_unit_tables(const LoadInput& input) {
    if (input.catalog == nullptr || input.filesystem == nullptr) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::missing_input);
        diagnostic.message = "load_unit_tables needs a catalog and a filesystem";
        return core::Result<UnitTables>::failure(std::move(diagnostic));
    }
    std::vector<std::string> types = input.types;
    std::vector<std::string> obstacles = input.obstacles;
    if (types.empty()) {
        for (const auto id : pinned) types.emplace_back(id);
        if (obstacles.empty()) {
            for (const auto id : pinned_obstacles) obstacles.emplace_back(id);
        }
    }
    UnitTables tables;
    Report report;
    Loader(input, tables, report).run(types, obstacles);
    tables.unresolved = std::move(report.unresolved);
    tables.notes = std::move(report.notes);
    for (auto& [path, file] : report.inputs) tables.inputs.push_back(std::move(file));
    return core::Result<UnitTables>::success(std::move(tables));
}

std::string_view to_string(const UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::station: return "station";
    case UnitKind::ship: return "ship";
    case UnitKind::squadron: return "squadron";
    case UnitKind::craft: return "craft";
    }
    return "ship";
}

std::string_view to_string(const HardpointType type) noexcept {
    for (const auto& [name, value] : hardpoint_types) {
        if (value == type) return name;
    }
    return "unknown";
}

} // namespace eawr::units
