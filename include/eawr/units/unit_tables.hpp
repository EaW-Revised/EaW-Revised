#pragma once

#include "eawr/core/load_note.hpp"

#include "eawr/units/unit_tables_combat.hpp"
#include "eawr/units/unit_tables_abilities.hpp"
#include "eawr/units/unit_tables_spawner.hpp"
#include "eawr/units/unit_tables_galactic.hpp"
#include "eawr/units/land_companies.hpp"
#include "eawr/units/unit_tables_presentation.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/units/population.hpp"

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

// The #64 scan list in its unit-table order: stations, ships, the squadrons
// the lobby or the fleet places, then the SK-23 spawned squadrons. Craft are
// reached through squadron composition.
[[nodiscard]] std::span<const std::string_view> pinned_m2_types() noexcept;
// The M2 map's object types that the path finder avoids or may avoid (#71): the non-marker,
// non-prop placements of the fixture map (SK-04) that are not already pinned types.
[[nodiscard]] std::span<const std::string_view> pinned_m2_obstacles() noexcept;

// WGM-128: strict reference-pose collision box for model-backed GC objects.
// Reuses the shared model/bone loader; no collidable meshes yield [-1,1]^3.
// Malformed bounds, bones and arithmetic overflow refuse rather than falling back.
[[nodiscard]] core::Result<CollisionBounds> prepare_galactic_collision_bounds(const assets::Model&);

struct UnitType final {
    std::optional<Fixed> bounty_sp{}, bounty_mp{}; // WGCH-65: negative means category fallback
    using ObjectTurret = table_rows::ObjectTurret;
    std::string id;
    // Position in the type table: the object's file in registry order, then its place in the file.
    // The AI's category teams enumerate in this order (PL-20); it is not part of the content identity.
    std::uint64_t table_order{};
    UnitKind kind{UnitKind::ship};
    std::string campaign_ship_class; // WGT-45: opt-in Ship_Class, independent of movement layer
    bool campaign_hyperspace{};      // WSL-16a: opt-in Hyperspace tag; a station-spawned ship with it arrives by hyperspace
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
    std::vector<std::uint32_t> previous_base_levels{}; // SAE-02: resolved Prev_Level_Base chain, AI-only
    std::optional<Fixed> fow_reveal_range; // the type's own range (#68); used only with `reveal`
    bool force_sensitive{}; // WAB-54: Is_Force_Sensitive
    bool reveal{}; // REVEAL in its Behavior or SpaceBehavior list (#271)
    Fixed dense_fow_multiplier{Fixed::from_raw(Fixed::scale / 2)};
    bool multisample_fow{};
    Vec3 fog_box_offset{};
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
    std::optional<Fixed> passive_missile_shield_radius;
    std::optional<Fixed> ranged_target_z_adjust;
    bool powered{};  // POWERED in its Behavior or SpaceBehavior list: it has an energy pool (#361)
    bool ion_stun_effect{}; // ION_STUN_EFFECT in its Behavior or SpaceBehavior list: it can be ion stunned (#561)
    bool damage_tracking{}; // DAMAGE_TRACKING in its Behavior or SpaceBehavior list: its rate of damage taken (WCC-82)
    std::optional<CollisionBounds> collision; // collidable meshes of its model (#74)
    // #536: the model's collidable meshes, then each hardpoint's attached model's, placed at its
    // attachment bone; and Collision_Box_Modifier (a value above 1 adds the sphere of DG-37).
    std::vector<CollisionMesh> collision_meshes; // immutable indexed geometry shared by table copies
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
    Movement team_movement; // WMV-44: the spawned container's own formation inputs
    std::optional<Fixed> team_reveal_range;
    Fixed team_dense_fow_multiplier{Fixed::from_raw(Fixed::scale / 2)};
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
    // WWP-47 (docs/behaviour/walks/weapons.md U-06): the object weapon's muzzle bones, the
    // Muzzle_Bone_Name prefix (default MUZZLEA) with _00 and consecutive two-digit suffixes while
    // the model has them. Empty without an object weapon or without a _00 bone.
    std::vector<BonePoint> muzzle_bones;
    std::optional<ObjectTurret> object_turret{};
    std::vector<Ability> abilities;
    std::vector<sim::tactical::SpecialAbilityProfile> special_abilities;
    std::optional<BuzzDroids> buzz_droids; // WAB-70: its nested Buzz_Droids_Ability
    std::optional<ClusterBomb> cluster_bomb;
    std::optional<BuzzDroids> team_buzz_droids; // WAB-70: the team container's (AB-60)
    std::uint64_t special_weapon_targets{}; // WAB-73: Special_Weapon_Valid_Targets in category bits
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
    std::optional<Fixed> autonomous_move_extension_vs_attacker;
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
    // LS shoot PR 1 (land bodies only): Collidable_By_Projectile_Dead and the death presentation rows
    // (Death_Clone entries as authored, Death_Fade_Time, Death_Persistence_Duration).
    bool dead_projectile_collision{};
    std::vector<std::string> death_clones;
    std::optional<Fixed> death_fade_time;
    std::optional<Fixed> death_persistence_duration;
    Production production; // #530
    // WBP-01..19: live capture content, and the separate UC replacement path.
    bool capture_point{};
    bool build_pad{};
    bool under_construction{};
    bool living_projectile_collision{}; // WBP-50/51: effective living object types opt in
    bool valid_target{true}; // WCC-25: effective Is_Valid_Target, default true
    bool target_stealth_units{}; // WST-07: Target_Stealth_Units
    bool special_weapon{}; // effective SPECIAL_WEAPON behavior
    // WSW-01/14: a standalone planetary source, separate from station manual weapons.
    using PlanetaryWeapon = table_rows::PlanetaryWeapon;
    std::optional<PlanetaryWeapon> planetary_weapon;
    // TSW-01: an effective TACTICAL_SUPER_WEAPON behaviour's start placement data. The debug build repositions
    // the object once, at its first service, relative to the nearest enemy-owned super weapon target.
    using TacticalSuperWeapon = table_rows::TacticalSuperWeapon;
    std::optional<TacticalSuperWeapon> tactical_super_weapon;
    using SpecialUnlock = table_rows::SpecialUnlock;
    std::vector<SpecialUnlock> special_unlocks{}; // WSW-10/46: space-automatic battlefield effects.
    bool consume_after_special_fire{}; // WSW-09: authored marker, independent of unlock kind.
    bool star_base{}; // effective DUMMY_STAR_BASE behavior
    bool unit_ai{}; // WTA-29: damage retaliation belongs to the type, regardless of owner
    bool targeting{}; // EX-31: effective TARGETING behavior; AI attack blocks give such movers the attack order
    std::uint64_t attack_category_restrictions{}; // WTA-29: type gate, distinct from weapon restrictions
    bool influences_capture{true};
    bool ownership_sticks{};
    bool community_property{};
    bool ai_community_property{}; // PE-34: tactical perception only, excluded from simulation identity.
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
    // MAT-15: Build_Tab_Space_Units, the type is listed among the space reinforcements. Only the demo
    // attract's pool return reads it (the setup carries the set); outside it the tag changes nothing.
    bool build_tab_space_units{};
    BonePoint build_attachment;
    // #457: Minimum_Follow_Distance, how close a chasing craft closes before it slows (FD-05).
    std::optional<Fixed> follow_distance;
    std::optional<Fixed> score_cost_credits{}; // WBF-45: result/scoring input; no physics identity
    std::optional<Fixed> score_combat_power{}; // WBF-45: original scoring type's combat metric
    bool named_hero{}; // WHE-01: authored identity, independent of class/category
    bool generic_hero{};
    bool special_weapon_in_space{}; // WTA-36: Is_Special_Weapon_In_Space, a hero to the AI's type query
    bool team_named_hero{};
    bool team_generic_hero{};
    using CompanyMember = table_rows::CompanyMember;
    std::vector<CompanyMember> company_members;
    std::string company_transport;
    std::string deployed_space_type; // WHE-49: ship or automatically formed team
    bool creates_carried_heroes{};
    bool display_contained_hero_bars{};
    std::string replenish_team;
    bool replenish_via_members{}; // WHE-70: TEAM dispatches this ability to its members.
    bool redirect_damage_to_teammates{};
    // WNO-29: authored order, resolved independently of ordinary weapons.
    std::vector<std::string> death_projectiles{};
    std::vector<std::uint32_t> death_projectile_indices{};
    // RP-09: the projectile a station creates at its position when a retreat destroys it.
    std::string retreat_self_destruct{};
    std::uint32_t retreat_self_destruct_index{no_index};
    std::uint32_t max_squad_size{10}; // FL-06: debug-build default; positive authored team maximum
    bool creates_team{}; // PG-08: first member's Create_Team guards the tactical power clamp
    bool operator==(const UnitType&) const = default;
};

// One value of a dynamic enum file (data/xml/enum/*.xml). The x64 FoC build reads them as
// 64-bit masks (the Steam patch sets `All` to 0xFFFFFFFFFFFFFFFF).
struct EnumValue final {
    std::string name;
    std::uint64_t value{};
    bool operator==(const EnumValue&) const = default;
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
    sim::tactical::BattleDomain domain{sim::tactical::BattleDomain::space}; // BF-2: which variant of the domain tags was read
    bool campaign_scene{}; // opt-in identity for GC-only scene metadata
    bool campaign_multiplayer{}; // WGCH-65: tactical bounty table selection
    std::vector<CampaignAbilityLock> campaign_ability_locks;
    std::optional<Fixed> ai_credit_multiplier{Fixed::from_raw(Fixed::scale)}; // WPR-12: neutral for synthetic content without difficulty data
    std::optional<Fixed> pad_ai_build_multiplier; // WBP-15: difficulty data, only with live capture content
    std::vector<std::uint64_t> pad_neutral_factions; // WHZ-51: authored Is_Neutral, sorted faction type CRCs
    std::vector<UnitType> units; // pinned types, then craft, in first-reference order
    std::vector<LandCompany> land_companies; // L2a (#1450): land company objects of a land load, in first-reference order
    std::vector<LandTraversalDefinition> land_movement_classes;
    std::vector<LandStaticObject> land_static_objects;
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
    sim::tactical::BattleDomain domain{sim::tactical::BattleDomain::space}; // BF-2 (#3779): selects the domain tag variants
    bool campaign_scene{}; // GC extensions are never decoded by ordinary skirmish loads
    const data::Catalog* catalog{};
    const vfs::Vfs* filesystem{}; // targeting priority sets and gameconstants.xml
    ModelLookup model;
    FileDigest digest;
    std::vector<std::string> types; // empty = pinned_m2_types()
    std::vector<std::string> obstacles; // empty with empty `types` = pinned_m2_obstacles()
    std::string difficulty{"Normal_Default"}; // SK-42 default; alternate difficulties use their XML values
    std::vector<CampaignAbilityLock> campaign_ability_locks;
    std::string space_map; // WHZ-01: load effective profiles for this map's actual placed types
};

// Fails only when an input is missing (no catalog or filesystem); every data
// gap becomes an Unresolved row instead.
[[nodiscard]] core::Result<UnitTables> load_unit_tables(const LoadInput& input);

// Land-only scene/proxy decoding without combat, model or company expansion.
[[nodiscard]] UnitTables load_land_static_tables(const data::Catalog&, std::span<const std::string> types,
    ModelLookup model = {}, FileDigest digest = {});

// The replay content identity of loaded tables: SHA-256 of their canonical
// encoding (docs/unit-data.md). It changes when any loaded value changes and
// ignores host paths, source layers and file bytes the tables do not read.
[[nodiscard]] std::array<std::uint8_t, 32> content_identity(const UnitTables& tables);
// DG-36: prepared coordinate extrema admit usable collision without walking triangles;
// invalid geometry retains the original first-coordinate refusal text and order.
[[nodiscard]] std::optional<std::string> collision_capability_error(const UnitType& unit);
[[nodiscard]] std::vector<std::uint8_t> canonical_encoding(const UnitTables& tables);

// The durability table of loaded tables (#72, docs/behaviour/space-hardpoints.md): one profile
// per station, ship and craft with a Tactical_Health, keyed by the TED object-type CRC of its
// name (assets::object_type_crc) and sorted by it. Health is scaled by
// Object_Max_Health_Multiplier_Space. Missing globals use stock values; duplicate IDs
// retain the first profile. A destroyable hardpoint with an unparseable Health still fails.
// FoC has no health cap (SKI-64): a scaled hull above sim::tactical::max_durability_health is held
// at that bound and, when `notes` is given, reported there once per type as a warning.
[[nodiscard]] core::Result<sim::tactical::DurabilityTable> durability_table(const UnitTables& tables,
    std::vector<core::Diagnostic>* notes = nullptr, core::LoadNoteBuffer* load_notes = nullptr);

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
// WSL-16: generic campaign scenes allow ships but retain zero reserves.
// Full campaign space entries also bind authored reserves and owner tech tiers.
struct SpawnPolicy final {
    enum class Mode { skirmish, campaign_scene, campaign };
    struct OwnerTech final {
        sim::tactical::PlayerId owner{};
        std::uint32_t tech{};
    };
    Mode mode{Mode::skirmish};
    std::vector<OwnerTech> owners;
};
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
// locomotor (#75) and have no profile. Static footprints without a locomotor or profile
// emit one informational diagnostic per type when notes is supplied.
// The resulting squadrons.spawners and campaign_spawners are the shared arrival
// roster: skirmish uses starting squadrons and ships; campaign also includes authored
// reserves and each owner's selected tech tier (WGT-10, WSL-04/16a).
[[nodiscard]] core::Result<sim::tactical::MotionTable> motion_table(
    const UnitTables& tables, const SpawnPolicy& spawn_policy = {},
    std::vector<core::Diagnostic>* notes = nullptr, core::LoadNoteBuffer* load_notes = nullptr);

// What a content error in one object does to its table (#4589, docs/unit-data.md "Content errors").
// strict: the table is refused (our CI and data checks). skip_object: FoC's way, the object's rows
// are left out and reported, and only a battle that places it fails.
enum class ContentErrors : std::uint8_t { strict, skip_object };
// The combat table of loaded tables (#73, docs/behaviour/space-weapon-fire.md): one profile per
// station, ship and craft, keyed like durability_table, with its CategoryMask bits,
// Targeting_Max_Attack_Distance, weapon hardpoints (Fire_* tags; recharge in truncated
// hundredths of a second, pulse delay in truncated frames), its object weapon (Projectile_Types,
// ranged by Targeting_Max_Attack_Distance), its target bones and hardpoint positions (scaled by
// Scale_Factor), and each used Targeting_Priority_Set resolved by attack_priority against every
// such type. A Fire_Category_Restrictions name outside the category dictionary adds no bit (ML-53; the
// loader reports it in `unresolved`). Fails when two types share a type ID
// or validate_combat rejects the table. Under skip_object a type whose rows fail is left out and
// appended to `skipped` (its message names the type) instead.
[[nodiscard]] core::Result<sim::tactical::CombatTable> combat_table(const UnitTables& tables,
    ContentErrors policy = ContentErrors::strict, std::vector<core::Diagnostic>* skipped = nullptr,
    core::LoadNoteBuffer* load_notes = nullptr);

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
    bool default_autofire = true, ContentErrors policy = ContentErrors::strict,
    std::vector<core::Diagnostic>* skipped = nullptr, core::LoadNoteBuffer* load_notes = nullptr);
// Whether a type runs the PowerToShields object script (`Lua_Script`), whose DEFEND rule the
// ability table's stand-in carries (AB-41).
[[nodiscard]] bool runs_defend_script(const UnitType& type) noexcept;
// Whether a type runs the Krayt object script (`Lua_Script`), whose SELF_DESTRUCT triggers the ability table's
// stand-in carries (WSD-09).
[[nodiscard]] bool runs_self_destruct_script(const UnitType& type) noexcept;

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
