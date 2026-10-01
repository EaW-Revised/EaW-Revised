#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Ship-level target choice, hardpoint opportunity targeting and weapon fire (P2-10, #73;
// docs/behaviour/space-targeting.md and docs/behaviour/space-weapon-fire.md). Everything here is
// a pure function of Q24 values and keyed random draws: no clock, thread or host input.
namespace eawr::sim::tactical {

// A weapon slot that is the unit's own weapon (a craft's Projectile_Types), not a hardpoint.
inline constexpr std::uint32_t object_weapon = 0xffffffffU;
// No target hardpoint: the shot aims at the unit itself.
inline constexpr std::uint32_t no_hardpoint = 0xffffffffU;

// Fire_Inaccuracy_Distance: the aim scatter against targets of these categories (DG-24).
struct InaccuracyRow {
    std::uint64_t category_bits{};
    math::Fixed distance{};
    friend constexpr bool operator==(const InaccuracyRow&, const InaccuracyRow&) noexcept = default;
};

// The projectile a weapon fires (#74, docs/behaviour/space-damage.md). A weapon without one fires
// shots that are events only, as before #74.
struct ShotProfile {
    math::Fixed damage{};                      // Projectile_Damage
    std::uint32_t damage_type{no_type_index};  // the hardpoint's Damage_Type, else the projectile's
    math::Fixed speed{};                       // Max_Speed, source units per frame
    math::Fixed max_travel{};                  // Fire_Range_Distance; the object weapon's Projectile_Max_Flight_Distance
    bool shield_damage{true};                  // Projectile_Does_Shield_Damage
    bool hitpoint_damage{true};                // Projectile_Does_Hitpoint_Damage
    std::vector<InaccuracyRow> inaccuracy;     // Fire_Inaccuracy_Distance, authored order
    // Internal_Damage_Type is a hard-coded type separate from the XML armor damage types (DG-05);
    // true when it is the misc type, the default. No M2 projectile authors another one; #76
    // abilities (Force Whirlwind, Remote Bomb style effects) are the only ones that do in retail.
    bool internal_damage_misc{true};
    // The object weapon's Projectile_Energy_Per_Shot (#361, EN-05); a hardpoint's shot draws no
    // energy (EN-06), so its profile holds zero.
    math::Fixed energy_per_shot{};
    // A MISSILE-category projectile homes on its target (#361, MS-01 to MS-07), turning at most
    // `turn_rate` degrees (its Max_Rate_Of_Turn) in yaw and in pitch each frame.
    bool homing{};
    math::Fixed turn_rate{};
    // Ion weapons (#561): Projectile_Does_Energy_Damage drains the pool of the unit hit (EN-07),
    // and Projectile_Ion_Stun_On_Detonation stuns it (IS-01).
    bool energy_damage{};
    std::optional<IonStunShot> ion_stun{};
    friend bool operator==(const ShotProfile&, const ShotProfile&) = default;
};

// One way a unit type fires: a weapon hardpoint (Fire_* tags) or the type's object weapon.
// Points are model-space offsets already scaled by the type's Scale_Factor.
struct WeaponProfile {
    std::uint32_t hardpoint{object_weapon}; // index in the type's HardPoints list
    math::Fixed range{};                    // Fire_Range_Distance; the object weapon uses Targeting_Max_Attack_Distance
    std::uint32_t min_recharge_hundredths{}; // Fire_Min_Recharge_Seconds x 100, truncated
    std::uint32_t max_recharge_hundredths{};
    std::uint32_t pulse_count{1};           // shots per burst, at least 1
    std::uint32_t pulse_delay_frames{};     // Fire_Pulse_Delay_Seconds x 30, truncated
    math::Fixed cone_width{};               // full cone, degrees; the object weapon: Turret_Rotate_Extent_Degrees,
    math::Fixed cone_height{};              // Turret_Elevate_Extent_Degrees (each side), zero for no cone (W-09)
    std::uint64_t category_restrictions{};  // categories it may not fire at (Fire_Category_Restrictions)
    bool opportunity_when_idle{};
    bool opportunity_when_targeting{};
    math::Vec3 fire_a{};                    // Fire_Bone_A
    math::Vec3 fire_b{};                    // Fire_Bone_B; equal to fire_a when there is none
    bool has_fire_b{};
    // Fire_Bone_A's bind-frame x, y and z axes in the unit's frame, orthonormal: a fixed
    // hardpoint's cone opens along x, its width about z and its height toward z (W-07). Without
    // them (no fire bone frame) the cone is measured in the unit's own frame.
    std::optional<std::array<math::Vec3, 3>> fire_axes{};
    std::optional<ShotProfile> shot{};
    // #561 (AB-66): the shot this weapon fires instead while its squadron's ION_CANNON_SHOT is on:
    // the team ability's override projectile, with the weapon's damage type and range.
    std::optional<ShotProfile> ability_shot{};
    // The hardpoint's AI combat power: its projectile's AI_Combat_Power over the sum of the type's
    // weapon hardpoints', times the type's own. It weighs the directions a unit can fire in when
    // it turns toward an ordered target (A-06); zero for the object weapon.
    math::Fixed ai_combat_power{};
    friend bool operator==(const WeaponProfile&, const WeaponProfile&) = default;
};

// A hardpoint as a target (R-10, R-13): where it sits, and whether a shot may pick it.
struct TargetHardpoint {
    std::uint32_t hardpoint{}; // index in the type's HardPoints list
    math::Vec3 position{};
    bool targetable{};
    friend bool operator==(const TargetHardpoint&, const TargetHardpoint&) = default;
};

// #536 (space-damage DG-36 to DG-38): a collidable mesh in the type's frame (scaled and turned
// like its box) and a tree of boxes over its triangles for the segment test. `hardpoint` is the
// hardpoint whose Collision_Mesh it is: its hits damage that hardpoint. `source_hardpoint` is the
// hardpoint whose attached model it belongs to: it stops colliding once that hardpoint is
// destroyed. `shield` marks the shield mesh, which only a shield-damaging projectile meets, and
// only while the unit's shield is up.
struct CollisionTriangle {
    math::Vec3 a{};
    math::Vec3 b{};
    math::Vec3 c{};
    friend constexpr bool operator==(const CollisionTriangle&, const CollisionTriangle&) noexcept = default;
};

// A node of a mesh's tree: the box around its triangles; a leaf (count > 0) holds triangles
// [first, first + count), any other node has the children `first` and `second`, both after it.
struct CollisionNode {
    math::Vec3 min{};
    math::Vec3 max{};
    std::uint32_t first{};
    std::uint32_t second{};
    std::uint32_t count{};
    friend constexpr bool operator==(const CollisionNode&, const CollisionNode&) noexcept = default;
};

struct CollisionMesh {
    std::uint32_t hardpoint{no_hardpoint};
    std::uint32_t source_hardpoint{no_hardpoint};
    bool shield{};
    std::vector<CollisionTriangle> triangles{}; // in tree order
    std::vector<CollisionNode> nodes{};         // nodes[0] is the root
    friend bool operator==(const CollisionMesh&, const CollisionMesh&) = default;
};

// Builds the tree of a mesh from its triangles: each node splits its triangles at the median
// of their centres along its longest axis (ties keep the given order), down to leaves of at most
// four. Deterministic; the triangles come back in tree order.
[[nodiscard]] CollisionMesh collision_mesh(std::vector<CollisionTriangle> triangles, std::uint32_t hardpoint,
    std::uint32_t source_hardpoint, bool shield);

// Mesh coordinates the segment test accepts: every vertex within this many units of its unit's
// origin on each axis; a shot that may meet a mesh moves at most this far in a frame.
inline constexpr std::int64_t max_mesh_extent = 4096;

struct MeshHit {
    math::Fixed fraction{}; // where along the segment, in [0, 1], rounded down
    std::size_t mesh{};     // index in the meshes tested
};

// DG-36: the first triangle the segment from `from` to `to` meets among the meshes `enabled`
// admits, each placed by `transform` (rotation and position). The test is exact on a grid of
// 1/32 unit in the unit's frame; both faces of a triangle count.
[[nodiscard]] core::Result<std::optional<MeshHit>> segment_hits_meshes(std::span<const CollisionMesh> meshes,
    const std::function<bool(std::size_t)>& enabled, const math::Mat3x4& transform, const math::Vec3& from,
    const math::Vec3& to);

// One unit type's combat content (for M2 the #65 unit tables, units::combat_table): neither
// replay data nor state. A type without a profile (a squadron container, #271) never targets,
// fires or is a target.
struct CombatProfile {
    TypeId type_id{};
    std::uint64_t category_bits{};                  // CategoryMask, as a candidate
    std::optional<std::uint32_t> priority_set{};    // index in CombatTable::priority_sets
    std::optional<math::Fixed> max_attack_distance; // Targeting_Max_Attack_Distance (last authored value)
    std::vector<WeaponProfile> weapons;             // weapon hardpoints in HardPoints order, then the object weapon
    std::vector<math::Vec3> target_bones;           // Target_Bones, declared order
    std::vector<TargetHardpoint> hardpoints;        // every hardpoint with a known position, HardPoints order
    std::optional<CollisionBox> collision{};        // what projectiles hit (DG-31); none: they fly through
    // #536: with meshes, a projectile hits their triangles (DG-36), the box only gates the test
    // (together with `mesh_bounds`, the box around every mesh); without, the box itself is hit.
    std::vector<CollisionMesh> meshes{};
    std::optional<CollisionBox> mesh_bounds{};
    // #669 (DG-39): with meshes, per HardPoints index, the hardpoint a shot aimed at that
    // hardpoint damages when it hits this unit: the first hardpoint whose Collision_Mesh names
    // the aimed hardpoint's Collision_Mesh, else no_hardpoint (the hull).
    std::vector<std::uint32_t> aimed_routes{};
    // DG-37: Collision_Box_Modifier when above 1. A projectile step that meets the unit's world box
    // (the collision box turned by the unit, then axis-aligned) and starts, ends or has its
    // midpoint closer to the unit than the modifier times that box's largest half extent hits it.
    std::optional<math::Fixed> sphere_modifier{};
    friend bool operator==(const CombatProfile&, const CombatProfile&) = default;
};

// A Targeting_Priority_Set resolved against the table's types (R-09, units::attack_priority):
// the score of each candidate type, nullopt for "no priority". A type the set does not list
// here scores `unlisted`.
struct PriorityRow {
    TypeId type_id{};
    std::optional<math::Fixed> priority{};
    friend constexpr bool operator==(const PriorityRow&, const PriorityRow&) noexcept = default;
};

struct PrioritySet {
    std::vector<PriorityRow> rows; // strictly increasing type_id
    math::Fixed unlisted{};        // the score of a type without a row
    friend bool operator==(const PrioritySet&, const PrioritySet&) = default;
};

// An empty table binds no combat: nothing targets or fires, as before #73.
struct CombatTable {
    std::vector<CombatProfile> profiles; // strictly increasing type_id
    std::vector<PrioritySet> priority_sets;
    [[nodiscard]] const CombatProfile* find(TypeId type_id) const noexcept;
    friend bool operator==(const CombatTable&, const CombatTable&) = default;
};

inline constexpr std::size_t max_weapons_per_type = 255;
// Ranges, cone angles and point offsets the rules accept, in whole source units.
inline constexpr std::int64_t max_combat_distance = std::int64_t{1} << 18;
inline constexpr std::uint32_t max_recharge_hundredths = 100 * 3600;
// W-06a: the object weapon's recharge adds a synchronized draw of 0 to this many frames.
inline constexpr std::uint32_t object_recharge_jitter_frames = 10;
// The largest Collision_Box_Modifier the craft sphere (DG-37) accepts: FoC authors 2 on its craft
// and 250 on the Death Star.
inline constexpr std::int64_t max_sphere_modifier = 256;
// A hardpoint's AI combat power, in whole units; a type's directed sums of up to 255 weapons
// stay far inside Q24.
inline constexpr std::int64_t max_weapon_combat_power = std::int64_t{1} << 24;
// How far a fire bone frame's Q24 dot products may stray from orthonormal: 2^-12, a skew of
// about 0.014 degrees. FoC's frames, normalized per axis on load, stay far inside it.
inline constexpr std::int64_t frame_tolerance_raw = std::int64_t{1} << 12;

// Fails with EAWR-SIM-0305 unless type IDs strictly increase, every priority set index exists,
// set rows strictly increase, a type has at most 255 weapons, hardpoint indices are below 255,
// ranges and attack distances are in [0, max_combat_distance], cones in [0, 360], recharge
// bounds ordered and at most max_recharge_hundredths, pulse counts at least 1, AI combat powers
// in [0, max_weapon_combat_power] and points within max_combat_distance on each axis.
[[nodiscard]] core::Result<void> validate_combat(const CombatTable& table);

// W-10: where a shot leaving `muzzle` at `speed` units per frame meets a target now at `aim` that
// moves by `velocity` per frame: the least positive meeting time, the aim point moved on by it.
// None when no shot can meet it. `aim` itself when the target does not move, and when the
// solution leaves the fixed-point range (a project bound no M2 shot reaches).
[[nodiscard]] std::optional<math::Vec3> lead_point(
    math::Vec3 aim, math::Vec3 muzzle, math::Vec3 velocity, math::Fixed speed);

// Keyed synchronized draws (project rule, docs/behaviour/space-weapon-fire.md): a draw depends
// only on the session seed, the frame, the unit, the weapon slot and how many draws that slot
// made before in the frame, never on other units or the worker count.
class CombatRandom final {
public:
    CombatRandom(std::uint64_t seed, std::uint64_t frame, EntityId unit, std::uint32_t slot) noexcept;
    // Uniform whole number in [low, high]; low when high <= low.
    [[nodiscard]] std::uint32_t uniform(std::uint32_t low, std::uint32_t high) noexcept;
    // Uniform Q24 raw value in [-magnitude, magnitude] (one draw); 0 when magnitude <= 0.
    [[nodiscard]] std::int64_t symmetric_raw(std::int64_t magnitude) noexcept;
    [[nodiscard]] std::uint32_t draws() const noexcept { return count_; }

private:
    std::uint64_t key_{};
    std::uint32_t count_{};
};

// Slot keys of CombatRandom: a weapon's index, or these.
inline constexpr std::uint32_t ship_slot = 0xffff0000U;          // ship-level scan
inline constexpr std::uint32_t shield_phase_slot = 0xffff0001U;  // spawn: the shield recharge phase (DG-13)
inline constexpr std::uint32_t energy_phase_slot = 0xffff0002U;  // spawn: the energy recharge phase (EN-02)
inline constexpr std::uint32_t spawn_slot_flag = 0x80000000U;    // | weapon index: initial countdown

// --- Hardpoint opportunity service (space-targeting R-01 to R-15) -----------------------------

// Retained between invocations: the opportunity target and the frame of the last scan.
struct OpportunityState {
    EntityId target{};
    std::uint64_t last_scan_frame{};
    friend constexpr bool operator==(const OpportunityState&, const OpportunityState&) noexcept = default;
};

enum class PlayerRelation : std::uint8_t { absent = 0, owner = 1, hostile = 2, other = 3 };

// One broad-phase candidate as the scan sees it, in authoritative order. The stages are split
// where their order changes the random stream (R-08).
struct OpportunityCandidate {
    EntityId id{};
    bool suitable{};         // model, valid type, limbo, dead, transported, marker, stealth, fog, hero clash
    bool pointable{true};    // a turret can point at its position (true for a fixed hardpoint)
    bool eligible{};         // collidable, weapon category restriction
    std::optional<math::Fixed> priority{};
    bool opportunity_fire_disabled{};
    friend constexpr bool operator==(const OpportunityCandidate&, const OpportunityCandidate&) noexcept = default;
};

// What the service asks of the world. The session and the note's fixture implement it.
class OpportunityWorld {
public:
    virtual ~OpportunityWorld() = default;
    [[nodiscard]] virtual std::size_t player_count() const = 0;
    [[nodiscard]] virtual PlayerRelation relation(std::size_t player_index) const = 0;
    // R-05: the parent is in a nebula and fogged to that player.
    [[nodiscard]] virtual bool nebula_fogged(std::size_t player_index) const = 0;
    // R-07: the player's broad-phase candidates in authoritative order.
    [[nodiscard]] virtual std::vector<OpportunityCandidate> candidates(std::size_t player_index) = 0;
    // R-10, R-11: whether the candidate has an acceptable aim point (may draw).
    [[nodiscard]] virtual bool aim_acceptable(const OpportunityCandidate& candidate) = 0;
    // R-02, R-12: a firing attempt; on success the world records the shot.
    [[nodiscard]] virtual bool attempt(EntityId target, bool retained) = 0;
    // R-06: the synchronized player-start draw in [0, count - 1].
    [[nodiscard]] virtual std::uint32_t draw_player_start(std::uint32_t count) = 0;
};

struct OpportunityGates {
    bool admitted{true};           // R-01: outer gates and the idle/targeting enable flag
    bool parent_suppressed{};      // R-05: global opportunity fire off, or parent stealth active
    std::uint32_t logical_fps{logical_frames_per_second};
};

struct OpportunityOutcome {
    bool acquired{};               // R-12: one opportunity-target-acquired event
    bool fired{};                  // an attempt succeeded
    std::uint32_t player_start_draws{};
};

// One admitted invocation of the opportunity path at `frame` (R-01 to R-14).
[[nodiscard]] OpportunityOutcome service_opportunity(
    std::uint64_t frame, const OpportunityGates& gates, OpportunityState& state, OpportunityWorld& world);

// --- Unit combat state ------------------------------------------------------------------------

// One weapon's fire cycle and opportunity target. Hashed state.
struct WeaponState {
    OpportunityState opportunity{};
    std::uint32_t countdown{};   // frames before the weapon may be serviced again
    std::uint32_t pulses_left{}; // shots left in the current burst
    friend constexpr bool operator==(const WeaponState&, const WeaponState&) noexcept = default;
};

// A unit's ship-level target and its weapons. Hashed state.
struct CombatState {
    EntityId attack_target{};         // ship-level target, zero for none
    bool direct{};                    // set by a player attack order
    // #531 (space-orders OR-20 to OR-25): the hardpoint of `attack_target` a player attack order
    // named, else no_hardpoint. Cleared when the order ends, the target changes or it is destroyed.
    std::uint32_t attack_hardpoint{no_hardpoint};
    std::uint64_t next_scan_frame{};  // the ship-level scan waits until this frame
    std::vector<WeaponState> weapons; // CombatProfile::weapons order
    friend bool operator==(const CombatState&, const CombatState&) = default;
};

// A unit starts with no target and each weapon's countdown drawn from [0, trunc(max recharge x 30)]
// (the retail spawn rule), keyed by the spawn frame.
[[nodiscard]] CombatState initial_combat(
    const CombatProfile& profile, std::uint64_t seed, std::uint64_t frame, EntityId unit);

enum class CombatEventKind : std::uint8_t {
    target_acquired = 1, // R-12: a weapon's new opportunity target was fired at
    weapon_fired = 2,    // a shot for the projectile system (#74)
    projectile_hit = 3,  // a projectile reached a unit (#74)
};

// weapon is the slot's HardPoints index (object_weapon for the unit's own weapon); a shot's
// origin is its fire bone and aim the point it was fired at; target_hardpoint is the hardpoint
// it aimed at, if any. A projectile hit names the unit it reached as target, the hardpoint it
// damaged (no_hardpoint for the hull or the shield alone), the projectile's position at the
// start of the frame as origin and the contact point as aim, and in `outcome` how the hit was
// taken (hit_outcome_* bits, #80): presentation picks the impact effect from it.
inline constexpr std::uint32_t hit_outcome_shield_absorbed = 1U; // the shield took all of it
// A weapon_fired event's outcome: the shot is the weapon's ability shot (#561, AB-66).
inline constexpr std::uint32_t fired_ability_shot = 1U;
inline constexpr std::uint32_t hit_outcome_armor_reduced = 2U;   // hull armor multiplier <= 0.75
inline constexpr math::Fixed armor_reduced_limit = math::Fixed::from_raw(math::Fixed::scale * 3 / 4);

struct CombatEvent {
    std::uint64_t tick{};
    CombatEventKind kind{CombatEventKind::weapon_fired};
    EntityId shooter{};
    std::uint32_t weapon{};
    EntityId target{};
    std::uint32_t target_hardpoint{no_hardpoint};
    math::Vec3 origin{};
    math::Vec3 aim{};
    std::uint32_t outcome{};
    friend constexpr bool operator==(const CombatEvent&, const CombatEvent&) noexcept = default;
};

[[nodiscard]] std::string_view to_string(CombatEventKind kind) noexcept;

} // namespace eawr::sim::tactical
