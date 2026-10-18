#pragma once

#include "eawr/units/unit_tables_common.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eawr::units {

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
    std::string charged_projectile; // WAD-35: Blast_Ability_Fire_Projectile_Type
    std::uint32_t charged_projectile_index{no_index};
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
    std::optional<Fixed> full_salvo_delay_multiplier; // Full_Salvo_Weapon_Delay_Multiplier, hardpoint only (WAB-80)
    bool operator==(const Weapon&) const = default;
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
    bool allows_special_weapon_use{}; // WSW-48: allied permission lifecycle notifications
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
    Fixed damage_delay{}; // Projectile_Damage_Delay_Secs, WAD-26
    std::optional<Fixed> reveal_range{};
    Fixed dense_reveal_multiplier{Fixed::from_raw(Fixed::scale)};
    bool operator==(const Projectile&) const = default;
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
    struct Bounty { std::uint64_t category{}; Fixed value{}; bool operator==(const Bounty&) const = default; };
    std::vector<Bounty> bounty_sp{}, bounty_mp{};
    std::vector<NamedConstant> scalars;
    std::vector<DamageToArmor> damage_to_armor;
    bool operator==(const CombatConstants&) const = default;
};

namespace table_rows {
struct ObjectTurret {
    BonePoint turret, barrel;
    Vec3 rest{};
    std::optional<Fixed> speed, yaw_extent, pitch_extent, rest_delay;
    bool xy_only{};
    bool operator==(const ObjectTurret&) const = default;
};

struct PlanetaryWeapon {
    std::uint32_t index{}, target_action{};
    std::uint64_t valid_targets{};
    bool operator==(const PlanetaryWeapon&) const = default;
};

struct TacticalSuperWeapon {
    Fixed start_distance{}, attack_distance{}, z_adjust{};
    bool start_pos_match_target_z{};
    bool operator==(const TacticalSuperWeapon&) const = default;
};
} // namespace table_rows

} // namespace eawr::units
