#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Typed, deterministic unit tables for the pinned M2 fleet (P2-02, #65;
// plan/phase-2/m2-skirmish.md). The tables are built from the P1 catalog
// (#5), the XML files outside the catalog registries (targeting priority sets,
// gameconstants.xml) and the ALO models, and hold only Q24 values, strings and
// indices. Nothing here reads a clock, a thread or host files; the loader is a
// pure function of its inputs. Loading rules are in docs/unit-data.md.
namespace eawr::units {

using sim::math::Fixed;
using sim::math::Vec3;

inline constexpr std::uint32_t no_index = 0xffffffffU;

// The #64 scan list in its unit-table order: stations, ships, the squadrons
// the lobby or the fleet places, then the SK-23 spawned squadrons. Craft are
// reached through squadron composition.
[[nodiscard]] std::span<const std::string_view> pinned_m2_types() noexcept;
// The M2 map's object types that the path finder avoids or may avoid (#71): the non-marker,
// non-prop placements of the fixture map (SK-04) that are not already pinned types.
[[nodiscard]] std::span<const std::string_view> pinned_m2_obstacles() noexcept;

enum class UnitKind : std::uint8_t { station, ship, squadron, craft };

enum class HardpointType : std::uint8_t {
    unknown,
    weapon_laser,
    weapon_missile,
    weapon_torpedo,
    weapon_ion_cannon,
    shield_generator,
    engine,
    fighter_bay,
    tractor_beam,
    gravity_well,
    enable_special_ability,
    dummy_art,
    weapon_mass_driver, // MD-01: append to preserve existing content-identity enum values.
    weapon_special, // WAD-31: ordinary weapon type, distinct from enable_special_ability.
};

struct InaccuracyEntry final {
    std::string category;
    Fixed distance;
    bool operator==(const InaccuracyEntry&) const = default;
};

// One way of firing: a weapon hardpoint's Fire_* tags, or a craft's own
// Projectile_* tags (object weapon). Missing values stay nullopt and appear in
// the unresolved list when the table requires them.
struct Weapon final {
    std::string projectile;
    std::uint32_t projectile_index{no_index};
    std::optional<Fixed> min_recharge_seconds;
    std::optional<Fixed> max_recharge_seconds;
    std::optional<std::uint32_t> pulse_count;
    std::optional<Fixed> pulse_delay_seconds;
    std::optional<Fixed> range;              // Fire_Range_Distance (hardpoint weapons only)
    std::optional<Fixed> cone_width_degrees; // Fire_Cone_Width
    std::optional<Fixed> cone_height_degrees;
    std::string damage_type;                 // authored Damage_Type, empty when absent
    std::optional<Fixed> damage;             // a hardpoint's own Projectile_Damage (DG-25)
    std::vector<std::string> category_restrictions;
    std::vector<InaccuracyEntry> inaccuracy;
    bool opportunity_fire_when_targeting{};
    bool opportunity_fire_when_idle{};
    std::optional<std::uint32_t> appearance_delay_frames; // WAD-37/40, hardpoint only
    bool operator==(const Weapon&) const = default;
};

// A model-space point of a named ALO bone: the translation of the bone's bind
// frame composed root-to-bone in Q24 (docs/unit-data.md), in asset units and
// before the owner's Scale_Factor.
struct BonePoint final {
    std::string bone;
    std::optional<Vec3> position;
    bool from_attached_model{}; // found in Model_To_Attach, placed at the attachment bone
    // The same bind frame's x, y and z axes in model space (its rotation columns, unnormalized);
    // set whenever position is. A fixed hardpoint's fire cone opens along Fire_Bone_A's x axis
    // (docs/behaviour/space-weapon-fire.md W-07).
    std::optional<std::array<Vec3, 3>> axes;
    bool operator==(const BonePoint&) const = default;
};

struct Hardpoint final {
    std::string id;
    HardpointType type{HardpointType::unknown};
    std::string type_name;
    bool targetable{};
    bool destroyable{};
    std::optional<Fixed> health;
    std::optional<Fixed> repair_amount_per_frame; // Repair_Amount_Per_Frame (station hardpoints, #72)
    std::optional<Fixed> repair_cost_per_frame;   // Repair_Cost_Per_Frame
    std::optional<Weapon> weapon;
    std::string model_to_attach;
    std::string collision_mesh; // Collision_Mesh: the mesh whose hits damage it (#536, DG-11)
    BonePoint attachment;
    BonePoint fire_a;
    BonePoint fire_b;
    std::string special_ability_name;
    std::optional<Fixed> fighter_bay_flyout_distance;
    // HARD_POINT_FIGHTER_BAY (#75): the X axis of the attachment bone's bind frame, model space.
    std::optional<Vec3> bay_axis;
    bool requires_manual_target{}; // WAD-39: no automatic shot without a manual assignment
    std::optional<Fixed> manual_cooldown_seconds;
    bool manual_is_turret{};
    BonePoint manual_turret;
    BonePoint manual_barrel;
    Vec3 manual_rest{};
    Vec3 manual_offset{};
    std::optional<Fixed> manual_rotate_speed;
    std::optional<Fixed> manual_yaw_extent;
    std::optional<Fixed> manual_pitch_extent;
    bool operator==(const Hardpoint&) const = default;
};

struct Projectile final {
    std::string id;
    std::optional<Fixed> damage;
    std::string damage_type;
    std::optional<Fixed> max_speed;
    std::optional<Fixed> max_rate_of_turn;
    std::optional<Fixed> max_flight_distance;
    std::string category;
    bool does_shield_damage{};
    bool does_energy_damage{};
    bool disables_engines_when_power_drained{};
    std::optional<Fixed> disable_engines_duration;
    bool does_hitpoint_damage{};
    std::optional<Fixed> energy_per_shot;
    std::optional<Fixed> ai_combat_power; // weighs a hardpoint's share of its unit's (#361)
    // Ion stun on detonation (#561, docs/behaviour/space-damage.md IS-01 to IS-09): the
    // Projectile_Ion_Stun_* tags. The reductions are fractions (0.5 halves the value).
    bool ion_stun{};
    std::optional<Fixed> ion_stun_duration; // seconds
    std::optional<Fixed> ion_stun_speed_reduction;
    std::optional<Fixed> ion_stun_shot_rate_reduction;
    bool ion_stun_stack_duration{};
    std::optional<Fixed> ion_stun_radius;
    sim::tactical::BlastProfile blast{};
    std::string blast_immune_faction;
    std::optional<Fixed> max_lifetime{};
    std::optional<bool> explode_at_target_radius{};
    std::optional<Fixed> rocket_curve_distance{};
    std::optional<Fixed> rocket_curve_offset{};
    std::optional<Fixed> rocket_straight_distance{};
    sim::tactical::WeakenProfile weaken{};
    bool operator==(const Projectile&) const = default;
};

struct ModMultiplier final {
    std::string modifier;
    Fixed value;
    bool operator==(const ModMultiplier&) const = default;
};

struct Ability final {
    std::string type;          // upper-case, as the engine's ability enum is spelled
    std::string authored_type; // as written (`power_to_weapons` on the Acclamator)
    std::optional<Fixed> expiration_seconds;
    std::optional<Fixed> recharge_seconds;
    std::vector<ModMultiplier> modifiers;
    bool supports_autofire{};
    // #561: the projectile the ability fires instead of the weapons' own (Projectile_Types_Override),
    // resolved to its index in the projectile table.
    std::string projectile_override;
    std::uint32_t projectile_index{no_index};
    std::optional<Fixed> effective_radius;
    std::string gui_activated_ability_name;
    std::optional<Fixed> fixed_inaccuracy{};
    std::optional<Fixed> target_z_offset{};
    std::string spawned_object;
    std::uint32_t spawned_projectile_index{no_index};
    std::optional<Fixed> bomb_countdown_seconds, target_position_z_offset;
    std::string replenish_particle;
    bool operator==(const Ability&) const = default;
};

struct SpawnEntry final {
    std::string squadron;
    std::uint32_t squadron_index{no_index};
    std::int32_t count{};
    bool operator==(const SpawnEntry&) const = default;
};

// SPAWN_SQUADRON lists. SK-23 launches `starting` once after
// `delay_seconds`; SK-36 keeps `reserves` read but unused (reserves_used is
// always false in M2).
struct Spawner final {
    std::vector<SpawnEntry> starting;
    std::optional<Fixed> delay_seconds;
    std::vector<SpawnEntry> reserves;
    bool reserves_used{false};
    bool operator==(const Spawner&) const = default;
};

struct SquadronMember final {
    std::string craft;
    std::uint32_t craft_index{no_index};
    std::optional<Vec3> offset; // Squadron_Offsets by member order
    bool operator==(const SquadronMember&) const = default;
};

struct Movement final {
    std::optional<Fixed> max_speed;
    std::optional<Fixed> min_speed;
    std::optional<Fixed> max_rate_of_turn;
    std::optional<Fixed> max_rate_of_roll;
    std::optional<Fixed> bank_turn_angle;
    std::optional<Fixed> max_thrust;
    std::optional<Fixed> max_lift;
    std::optional<Fixed> acceleration; // OverrideAcceleration
    std::optional<Fixed> deceleration; // OverrideDeceleration
    std::string space_layer;
    std::optional<Fixed> layer_z_adjust;
    bool operator==(const Movement&) const = default;
};

// A model-space box before Scale_Factor: the union of the model's collidable meshes, each
// mesh's bounds placed by its bone's bind frame (docs/behaviour/space-damage.md DG-31).
struct CollisionBounds final {
    Vec3 min;
    Vec3 max;
    bool operator==(const CollisionBounds&) const = default;
};

// A collidable mesh a projectile hits (#536, docs/behaviour/space-damage.md DG-36): its ALO name
// and its triangles in model space, bind pose, before Scale_Factor. `hardpoint` is the index of
// the hardpoint whose Model_To_Attach it comes from (no_index: the unit's own model).
struct CollisionMesh final {
    std::string name;
    std::uint32_t hardpoint{no_index};
    std::vector<std::array<Vec3, 3>> triangles;
    bool operator==(const CollisionMesh&) const = default;
};

// How the tracking system and the path finder see a type (#71, docs/unit-data.md, research
// E71-15, E71-17, E71-18). The collision box is the model's collision bounds: the union of its
// collidable meshes' boxes in the bind pose, half extents before Scale_Factor; a model without
// a collidable mesh has the box +-1.
struct HazardProfile final {
    std::vector<std::string> behavior;
    std::vector<std::string> space_behavior;
    bool asteroid_field{};
    bool ion_storm{};
    bool nebula{};
    bool impassable_asteroid{};
    bool asteroid_damage{};
    bool nebula_service{};
    Vec3 obstacle_offset{};
    bool operator==(const HazardProfile&) const = default;
};

struct SpaceFootprint final {
    bool space_obstacle{};                    // SPACE_OBSTACLE in Behavior or SpaceBehavior
    std::optional<Fixed> custom_hard_x;       // Custom_Hard_XExtent
    std::optional<Fixed> custom_hard_y;       // Custom_Hard_YExtent
    std::optional<Fixed> custom_soft_radius;  // Custom_Soft_Footprint_Radius
    std::optional<Fixed> obstacle_radius;     // Space_Obstacle_Radius
    std::optional<Fixed> collision_x;         // model collision half extents, unscaled
    std::optional<Fixed> collision_y;
    HazardProfile hazard; // WHZ-01: independent flags and authored service attachment
    bool operator==(const SpaceFootprint&) const = default;
};

// A map object type the unit tables load only for its footprint (#71).
struct ObstacleType final {
    std::string id;
    std::string xml_type;
    std::string space_layer;
    std::optional<Fixed> scale_factor;
    std::string model_path;
    SpaceFootprint footprint;
    bool influences_capture{true};
    bool construction_blocker{true};
    bool living_projectile_collision{}; // WBP-50: false by default for map objects
    bool selectable{}; // WSU-21: presentation fields do not enter content identity
    bool mouse_sensitive{}; // WSU-13: map props use the same admission as live units
    bool last_state_visible_under_fow{};
    bool initial_state_visible_under_fow{};
    bool operator==(const ObstacleType&) const = default;
};

// #530 (docs/behaviour/space-purchasing.md): a skirmish station's `Income_Stream_Ability`
// (PU-02, PU-03) and `Income_Stream_Mod_Ability` (PU-04) sub-objects, by Name.
struct IncomeStream final {
    std::string name;
    std::optional<Fixed> base_value;       // Base_Income_Value
    std::optional<Fixed> interval_seconds; // Base_Interval_In_Secs
    bool split_with_allies{};              // Split_Income_With_Allies
    bool full_amount_to_everyone{};        // Full_Amount_To_Everyone
    bool operator==(const IncomeStream&) const = default;
};

struct IncomeBonus final {
    std::string name;
    std::optional<Fixed> additive;   // Income_Additive_Value
    std::optional<Fixed> multiplier; // Income_Multiplier
    std::string target_source;       // Target_Stream_Source
    std::optional<Fixed> interval_multiplier;
    std::string activation_style;
    std::uint32_t stacking_category{};
    bool all_allied_sources{};
    bool reverse{};
    bool operator==(const IncomeBonus&) const = default;
};

// One faction's group of Tactical_Buildable_Objects_Multiplayer (PU-10), in authored order.
struct BuildGroup final {
    std::string faction;
    std::vector<std::string> types;
    bool operator==(const BuildGroup&) const = default;
};

// WPR-51: automatic combat bonuses authored on an upgrade object.
struct CombatBonus final {
    std::uint32_t stacking_category{};
    std::vector<std::string> types;
    std::vector<std::string> categories;
    std::array<Fixed, 6> percentages{}; // health, damage, energy, shield, defense, speed
    std::string name;
    sim::tactical::SpecialAbilityFilter filter;
    std::string specific_faction;
    bool enabled{true};
    std::vector<sim::tactical::TypeId> excluded_containers;
    bool operator==(const CombatBonus&) const = default;
};

// What a type builds and costs in a skirmish (#530, PU-10 to PU-21, PU-31).
struct Production final {
    std::vector<BuildGroup> buildable;                   // Tactical_Buildable_Objects_Multiplayer
    std::optional<Fixed> build_cost_multiplayer;         // Tactical_Build_Cost_Multiplayer
    std::optional<Fixed> build_time_seconds;             // Tactical_Build_Time_Seconds
    std::string production_queue;                        // Tactical_Production_Queue
    std::optional<std::uint32_t> population_value;       // Population_Value
    std::optional<Fixed> reinforcement_prevention_radius; // Reinforcement_Prevention_Radius
    std::vector<IncomeStream> income;
    std::vector<IncomeBonus> income_bonuses;
    std::optional<std::uint32_t> lifetime_player;
    std::optional<std::uint32_t> current_player;
    std::optional<std::uint32_t> lifetime_allies;
    std::optional<std::uint32_t> current_allies;
    std::vector<std::string> prerequisites;
    std::string next_level;
    bool upgrade_object{};
    bool level_up{};
    bool increments_tech{};
    std::string removes_previous;
    std::string next_upgrade; // WPR-63: menu metadata, outside content identity
    std::vector<CombatBonus> combat_bonuses;
    bool operator==(const Production&) const = default;
};

struct UnitType final {
    std::string id;
    UnitKind kind{UnitKind::ship};
    std::string xml_type; // SpaceUnit, StarBase, Squadron
    std::vector<std::string> variant_chain;
    std::string affiliation;
    std::string model;      // Space_Model_Name as authored
    std::string model_path; // logical ALO path, empty when unresolved
    std::optional<Fixed> scale_factor;
    std::optional<Fixed> hull;          // Tactical_Health
    std::optional<Fixed> shield_points;
    std::optional<Fixed> shield_refresh_rate;
    std::optional<Fixed> energy_capacity;
    std::optional<Fixed> energy_refresh_rate;
    std::string armor_type;
    std::string shield_armor_type;
    std::string damage_type;
    Movement movement;
    std::string targeting_priority_set;
    std::uint32_t targeting_priority_set_index{no_index};
    std::optional<Fixed> targeting_max_attack_distance;
    std::optional<Fixed> targeting_min_attack_distance;
    std::optional<Fixed> targeting_stickiness_seconds;
    std::vector<std::string> category_mask;
    std::uint64_t category_bits{};           // category_mask in GameObjectCategoryType bits
    std::vector<std::string> property_flags; // Property_Flags (#270)
    std::uint64_t property_bits{};           // property_flags in GameObjectPropertiesType bits
    std::optional<Fixed> ai_combat_power;
    // AI-only inputs (#449, docs/behaviour/foc-tactical-ai.md): the type is a space-mode goal target
    // (Has_Space_Evaluator) and its Tech_Level. The world never reads them, so the content
    // identity leaves them out.
    bool has_space_evaluator{};
    std::uint32_t tech_level{};
    std::uint32_t base_level{}; // SAE-02: Base_Level on live stations
    std::optional<Fixed> space_fow_reveal_range; // the type's own range (#68); used only with `reveal`
    bool reveal{}; // REVEAL in its Behavior or SpaceBehavior list (#271)
    // WSU-21, WSU-15: presentation selection eligibility; excluded from content identity.
    bool selectable{};
    bool mouse_sensitive{}; // WSU-13: behaviour, impassable asteroid or living-collidable valid target
    bool bar_admitted{}; // WSU-50: selectable, hero, construction or indigenous-spawner admission
    bool locomotion{};
    bool decoration{};
    // FW-25/26: presentation memory only; excluded from content identity.
    bool last_state_visible_under_fow{};
    bool initial_state_visible_under_fow{};
    std::optional<std::uint32_t> neutral_fog_animation_index; // FW-30: presentation only
    bool shielded{}; // SHIELDED in its Behavior or SpaceBehavior list (#74)
    bool powered{};  // POWERED in its Behavior or SpaceBehavior list: it has an energy pool (#361)
    bool ion_stun_effect{}; // ION_STUN_EFFECT in its Behavior or SpaceBehavior list: it can be ion stunned (#561)
    std::optional<CollisionBounds> collision; // collidable meshes of its model (#74)
    // #536: the model's collidable meshes, then each hardpoint's attached model's, placed at its
    // attachment bone; and Collision_Box_Modifier (a value above 1 adds the sphere of DG-37).
    std::vector<CollisionMesh> collision_meshes;
    std::optional<Fixed> collision_box_modifier;
    // #665 (WSU-10, WSU-12): Mouse_Collide_Override_Sphere_Radius, the pick sphere a pointer ray
    // hits when it misses the collision meshes. Presentation only: the content identity leaves it out.
    std::optional<Fixed> mouse_collide_sphere_radius;
    // WBP-35/36: presentation-only; excluded from simulation content identity.
    bool hides_when_built_on{};
    bool visible_to_enemies_when_empty{};
    std::uint32_t gui_row{};
    // Squadrons (#271): the team container the squadron spawns (Create_Team_Type, default
    // `Team`), and that container's Space_FOW_Reveal_Range when the container has REVEAL.
    std::string team_type;
    std::optional<Fixed> team_reveal_range;
    // WSQ-60: container trace metadata, independent of the craft's combat durability.
    std::optional<Fixed> team_hull; // Tactical_Health, including the debug-build default
    std::optional<Fixed> team_shield_points;
    // #561: the team container's own Unit_Abilities_Data (ION_CANNON_SHOT's recharge, autofire and
    // override projectile for the Y-wing squadron, AB-60).
    std::vector<Ability> team_abilities;
    std::vector<sim::tactical::SpecialAbilityProfile> team_special_abilities;
    bool victory_relevant{};
    bool destroyed_with_hardpoints{}; // Should_Be_Destroyed_When_All_Hardpoints_Destroyed (#72)
    std::vector<Hardpoint> hardpoints; // in HardPoints order
    std::optional<Weapon> weapon;      // object weapon (Projectile_Types)
    std::vector<BonePoint> target_bones;
    std::vector<Ability> abilities;
    std::vector<sim::tactical::SpecialAbilityProfile> special_abilities;
    // `Abilities` sub-objects (station income and radar) as authored; since #530 the income is
    // modelled through `production`, the radar stays off (SK-31):
    // element name and Name attribute.
    std::vector<std::string> inactive_abilities;
    SpaceFootprint footprint; // #71
    std::optional<Spawner> spawner;
    std::vector<SquadronMember> members;
    bool homogeneous{true}; // L-2: Is_Homogeneous controls card folding; presentation only.
    std::string lua_script;
    // #75: craft attack runs (Strafe_Distance) and a squadron's diversion ranges and formation
    // tolerance (Guard_Chase_Range, Idle_Chase_Range, Squadron_Formation_Error_Tolerance); #452
    // added the attack-move diversion range (Attack_Move_Response_Range).
    std::optional<Fixed> strafe_distance;
    std::optional<Fixed> guard_chase_range;
    std::optional<Fixed> idle_chase_range;
    std::optional<Fixed> attack_move_response_range;
    std::optional<Fixed> formation_error_tolerance;
    // #447 (space-fighter-deaths SP-01): with Spin_Away_On_Death, a killed craft spins away with
    // Spin_Away_On_Death_Chance for Spin_Away_On_Death_Time seconds. Both are read only then,
    // and a yes without both counts as no.
    bool spin_away_on_death{};
    std::optional<Fixed> spin_away_chance;
    std::optional<Fixed> spin_away_time;
    // #409: Out_Of_Combat_Defense_Adjustment, a craft's defense while idle or approaching (DG-26).
    std::optional<Fixed> out_of_combat_defense;
    Production production; // #530
    // WBP-01..19: live capture content, and the separate UC replacement path.
    bool capture_point{};
    bool build_pad{};
    bool under_construction{};
    bool living_projectile_collision{}; // WBP-50/51: effective living object types opt in
    bool influences_capture{true};
    bool ownership_sticks{};
    bool community_property{};
    // WSU-16: station selection metadata, separate from capture-pad simulation identity.
    bool station_community_property{};
    bool construction_blocker{true};
    bool child_persists{true};
    bool destroy_when_child_dies{};
    std::optional<Fixed> pad_rebuild_seconds;
    std::optional<Fixed> tactical_respawn_seconds;
    std::optional<Fixed> capture_radius;
    std::optional<Fixed> capture_seconds;
    std::string constructed_type;
    bool tactical_sale{};
    std::optional<Fixed> tactical_sell_percentage;
    BonePoint build_attachment;
    // #457: Minimum_Follow_Distance, how close a chasing craft closes before it slows (FD-05).
    std::optional<Fixed> follow_distance;
    std::optional<Fixed> score_cost_credits{}; // WBF-45: result/scoring input; no physics identity
    std::optional<Fixed> score_combat_power{}; // WBF-45: original scoring type's combat metric
    bool named_hero{}; // WHE-01: authored identity, independent of class/category
    bool generic_hero{};
    bool team_named_hero{};
    bool team_generic_hero{};
    struct CompanyMember {
        std::string type;
        bool named_hero{}, generic_hero{};
        bool attach_to_flagship{};
        std::string unique_space_unit;
        bool operator==(const CompanyMember&) const = default;
    };
    std::vector<CompanyMember> company_members;
    std::string company_transport;
    std::string deployed_space_type; // WHE-49: ship or automatically formed team
    bool creates_carried_heroes{};
    bool display_contained_hero_bars{};
    std::string replenish_team;
    bool redirect_damage_to_teammates{};
    bool operator==(const UnitType&) const = default;
};

// What an Attack_Priorities name matches (#270, docs/unit-data.md): a GameObjectCategoryType
// name, else a GameObjectPropertiesType name, else an exact object type.
enum class PriorityMatch : std::uint8_t { category, property, type };

struct PriorityEntry final {
    std::string name;
    Fixed weight;
    PriorityMatch match{PriorityMatch::category};
    std::uint64_t bits{}; // category or property bits; 0 for a type entry
    bool operator==(const PriorityEntry&) const = default;
};

struct TargetingPrioritySet final {
    std::string id;
    std::vector<PriorityEntry> attack_priorities;
    std::vector<std::string> hard_point_priorities;
    std::vector<std::string> category_exclusions;
    std::vector<std::string> property_exclusions;
    std::vector<std::string> unit_exclusions;
    std::vector<std::string> hard_point_exclusions;
    std::uint64_t category_exclusion_bits{};
    std::uint64_t property_exclusion_bits{};
    bool operator==(const TargetingPrioritySet&) const = default;
};

// One value of a dynamic enum file (data/xml/enum/*.xml). The x64 FoC build reads them as
// 64-bit masks (the Steam patch sets `All` to 0xFFFFFFFFFFFFFFFF).
struct EnumValue final {
    std::string name;
    std::uint64_t value{};
    bool operator==(const EnumValue&) const = default;
};

struct DamageToArmor final {
    std::string damage_type;
    std::string armor_type;
    Fixed multiplier;
    bool operator==(const DamageToArmor&) const = default;
};

// gameconstants.xml values the M2 combat tickets read (#70-#76). Scalars are
// listed by tag name in unit_tables.cpp; the damage table holds only the rows
// whose damage and armor types the fleet uses.
struct NamedConstant final {
    std::string tag;
    std::optional<Fixed> value;
    std::string text; // authored text, for the boolean constants
    bool operator==(const NamedConstant&) const = default;
};

struct CombatConstants final {
    std::vector<NamedConstant> scalars;
    std::vector<DamageToArmor> damage_to_armor;
    bool operator==(const CombatConstants&) const = default;
};

// One thing the scan could not resolve. `field` is a tag or a reference kind
// (`hardpoint`, `projectile`, `model`, `bone`, ...); `value` names what was
// looked for. Informational notes (duplicate tags, list-tag layers) use
// `note`.
struct Unresolved final {
    std::string owner;
    std::string field;
    std::string value;
    std::string reason;
    bool operator==(const Unresolved&) const = default;
};

struct InputFile final {
    std::string logical_path;
    std::string source_id;
    std::string layer_id;
    std::string sha256;
    bool operator==(const InputFile&) const = default;
};

struct UnitTables final {
    std::optional<Fixed> pad_ai_build_multiplier; // WBP-15: difficulty data, only with live capture content
    std::vector<std::uint64_t> pad_neutral_factions; // WHZ-51: authored Is_Neutral, sorted faction type CRCs
    std::vector<UnitType> units; // pinned types, then craft, in first-reference order
    std::vector<ObstacleType> obstacles; // pinned_m2_obstacles order (#71)
    std::vector<Projectile> projectiles;
    std::vector<TargetingPrioritySet> priority_sets;
    std::vector<EnumValue> categories; // GameObjectCategoryType.xml, file order
    std::vector<EnumValue> properties; // GameObjectPropertiesType.xml, file order
    CombatConstants constants;
    std::vector<Unresolved> unresolved;
    std::vector<Unresolved> notes;
    std::vector<InputFile> inputs; // XML and ALO files read, sorted by path

    [[nodiscard]] const UnitType* find(std::string_view id) const noexcept;
    bool operator==(const UnitTables&) const = default;
};

// Model lookup by logical ALO path (scene::VfsAssetCache::access().model fits).
using ModelLookup = std::function<const assets::Model*(std::string_view logical_path)>;
// SHA-256 hex of a logical file, for InputFile rows; may be empty.
using FileDigest = std::function<std::string(std::string_view logical_path)>;

struct LoadInput final {
    const data::Catalog* catalog{};
    const vfs::Vfs* filesystem{}; // targeting priority sets and gameconstants.xml
    ModelLookup model;
    FileDigest digest;
    std::vector<std::string> types; // empty = pinned_m2_types()
    std::vector<std::string> obstacles; // empty with empty `types` = pinned_m2_obstacles()
    std::string difficulty{"Normal_Default"}; // SK-42 default; alternate difficulties use their XML values
    std::string space_map; // WHZ-01: load effective profiles for this map's actual placed types
};

// Fails only when an input is missing (no catalog or filesystem); every data
// gap becomes an Unresolved row instead.
[[nodiscard]] core::Result<UnitTables> load_unit_tables(const LoadInput& input);

// The replay content identity of loaded tables: SHA-256 of their canonical
// encoding (docs/unit-data.md). It changes when any loaded value changes and
// ignores host paths, source layers and file bytes the tables do not read.
[[nodiscard]] std::array<std::uint8_t, 32> content_identity(const UnitTables& tables);
[[nodiscard]] std::vector<std::uint8_t> canonical_encoding(const UnitTables& tables);

// The durability table of loaded tables (#72, docs/behaviour/space-hardpoints.md): one profile
// per station, ship and craft with a Tactical_Health, keyed by the TED object-type CRC of its
// name (assets::object_type_crc) and sorted by it. Health is scaled by
// Object_Max_Health_Multiplier_Space. Fails when a scalar it needs is missing, a destroyable
// hardpoint has no Health, two types share a type ID or validate_durability rejects the table.
[[nodiscard]] core::Result<sim::tactical::DurabilityTable> durability_table(const UnitTables& tables);

// The damage and armor type names the tables use, lower case and sorted: the indices of the
// damage rules' armor table (#74). Both durability_table and combat_table index by it.
struct DamageTypeIndex final {
    std::vector<std::string> damage_types;
    std::vector<std::string> armor_types;
    // no_type_index (sim::tactical) for a name that is not listed; an empty name is Damage_Default.
    [[nodiscard]] std::uint32_t damage(std::string_view name) const;
    [[nodiscard]] std::uint32_t armor(std::string_view name) const;
};
[[nodiscard]] DamageTypeIndex damage_type_index(const UnitTables& tables);

// The space sensor range of a type (#271, docs/behaviour/space-visibility.md V-01, V-03): a
// squadron reveals through its team container (team_reveal_range); any other type reveals
// with its own Space_FOW_Reveal_Range only when it has REVEAL. Nothing otherwise.
[[nodiscard]] std::optional<Fixed> sensor_range(const UnitType& type) noexcept;
// Targeting priorities of one set (space-targeting R-09, docs/unit-data.md). Lower wins;
// nullopt is "no priority" (the candidate is not a target). A name the set does not list
// scores unlisted_priority, above every authored weight.
inline constexpr Fixed unlisted_priority = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
[[nodiscard]] std::optional<Fixed> attack_priority(const TargetingPrioritySet& set, const UnitType& candidate);
[[nodiscard]] std::optional<Fixed> hard_point_priority(const TargetingPrioritySet& set, HardpointType type);
// The motion table of loaded tables (#70, docs/behaviour/space-movement.md MV-01): one profile
// per ship with a Max_Speed, keyed like durability_table. With the #71 path finder constants it
// also carries the avoidance rules and one footprint per type with a Space_Layer (ships,
// stations and obstacles; AV-05). Speeds, accelerations and the rate of turn are multiplied by
// Object_Max_Speed_Multiplier_Space, and so is Max_Rate_Of_Roll (default 2; Bank_Turn_Angle
// default 70, #351); a missing OverrideAcceleration or
// OverrideDeceleration is the maximum speed; the turn-in-place slowdown follows Space_Layer
// (Corvette, Frigate, Capital or SuperCapital; 1 otherwise). The rules are 360 /
// MaxRotationsSpace and XYExpansionDistanceSpace. Fails when a scalar it needs is missing, two
// types share a type ID or validate_motion rejects the table. Craft fly with the squadron
// locomotor (#75) and have no profile.
[[nodiscard]] core::Result<sim::tactical::MotionTable> motion_table(const UnitTables& tables);

// The combat table of loaded tables (#73, docs/behaviour/space-weapon-fire.md): one profile per
// station, ship and craft, keyed like durability_table, with its CategoryMask bits,
// Targeting_Max_Attack_Distance, weapon hardpoints (Fire_* tags; recharge in truncated
// hundredths of a second, pulse delay in truncated frames), its object weapon (Projectile_Types,
// ranged by Targeting_Max_Attack_Distance), its target bones and hardpoint positions (scaled by
// Scale_Factor), and each used Targeting_Priority_Set resolved by attack_priority against every
// such type. Fails when a Fire_Category_Restrictions name is unknown, two types share a type ID
// or validate_combat rejects the table.
[[nodiscard]] core::Result<sim::tactical::CombatTable> combat_table(const UnitTables& tables);

// The ability table of loaded tables (#76, docs/behaviour/space-abilities.md AB-01 to AB-04): one
// profile per station, ship and craft type that authors a modelled Unit_Ability (DEFEND, TURBO,
// POWER_TO_WEAPONS, SPOILER_LOCK), keyed like durability_table, its seconds truncated to frames and
// its Mod_Multiplier rows by name; squadron types are skipped (their craft carry the ability,
// AB-15). `humans` are the session's human players (AB-41). Fails when an ability authors a
// modifier that is not modelled, two types share a type ID or validate_abilities rejects it.
// `allowed`, when supplied, excludes individual abilities under a caller's release policy (RG-03).
// AB-45: human owners use the enabled creation preference by default; false binds a
// session with the preference disabled. Individual ability commands still override each unit.
[[nodiscard]] core::Result<sim::tactical::AbilityTable> ability_table(
    const UnitTables& tables, std::span<const sim::tactical::PlayerId> humans = {},
    bool (*allowed)(std::string_view unit, std::string_view ability) = nullptr,
    bool default_autofire = true);
// Whether a type runs the PowerToShields object script (`Lua_Script`), whose DEFEND rule the
// ability table's stand-in carries (AB-41).
[[nodiscard]] bool runs_defend_script(const UnitType& type) noexcept;

// Q24 bind frames of every bone, root-to-bone in Q24 (docs/unit-data.md).
[[nodiscard]] core::Result<std::vector<sim::math::Mat3x4>> bind_frames(const assets::Model& model);

[[nodiscard]] std::string_view to_string(UnitKind kind) noexcept;
[[nodiscard]] std::string_view to_string(HardpointType type) noexcept;

namespace diagnostic_codes {
inline constexpr std::string_view missing_input = "EAWR-UNITS-0001";
inline constexpr std::string_view bone_frame = "EAWR-UNITS-0002";
inline constexpr std::string_view durability = "EAWR-UNITS-0003";
inline constexpr std::string_view motion = "EAWR-UNITS-0004";
inline constexpr std::string_view combat = "EAWR-UNITS-0005";
inline constexpr std::string_view abilities = "EAWR-UNITS-0006";
} // namespace diagnostic_codes

} // namespace eawr::units
