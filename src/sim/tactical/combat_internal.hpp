#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/space.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/sim/world.hpp"
#include "collection.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace eawr::sim::tactical::detail {

// WPJ-17: activation owners publish copied sources in registration order. Radius
// precedence is independent of whether the shield ability itself is active.
struct ProjectileDefenceSource {
    EntityId id{};
    PlayerId owner{};
    math::Vec3 position{}, adjusted_position{};
    bool active_shield{}, passive_shield{}, sensor_jammed{};
    math::Fixed passive_radius{};
    std::optional<math::Fixed> shield_radius{}, jamming_radius{};
    [[nodiscard]] math::Fixed radius() const noexcept {
        return sensor_jammed && jamming_radius ? *jamming_radius : shield_radius.value_or(passive_radius);
    }
};

// Immutable per-phase registry; the existing spatial index handles candidate
// lookup, and the source's ordinal restores registration order after querying.
struct ProjectileDefenceRegistry {
    std::vector<ProjectileDefenceSource> sources;
    SpaceIndex index;
    math::Fixed query_radius{};
    [[nodiscard]] core::Result<void> rebuild(std::vector<ProjectileDefenceSource> registered);
    [[nodiscard]] std::vector<std::size_t> candidates(const math::Vec3& position,
        std::uint64_t* inspected = nullptr) const;
};

// Copied from the existing ability slots, aligned with CombatWorld::units.
// Declared radii remain present while inactive for WPJ-17's radius precedence.
struct ProjectileDefenceAbilityInput {
    bool active_shield{}, active_jamming{};
    std::optional<math::Fixed> shield_radius{}, jamming_radius{};
};

// One live unit as the targeting phase sees it: copied before the phase, never written by it.
struct CombatUnit {
    EntityId id{};
    TypeId type_id{};
    PlayerId owner{};
    TeamId team{};
    std::size_t player_index{};   // the owner's index in the ascending player table
    math::Vec3 position{};
    math::Vec3 previous_position{}; // before this frame's movement (W-10)
    math::Mat3x4 transform{};     // rotation and position
    std::uint64_t visible_to{};   // bit k: players[k] sees it (the last published snapshot)
    const CombatProfile* profile{};
    const DurabilityProfile* durability_profile{};
    const DurabilityState* durability{};
    const CombatState* combat{};
    bool squadron_idle{};         // FT-07: a squadron craft whose squadron has no target
    bool can_turn{};              // it has a motion profile and is at rest: no plan, no group wait
    // WMV-20: a single ship's unit destination can turn it without a direct combat target.
    std::optional<math::Vec3> facing_destination{};
    // AB-21: the weapon delay multiplier of its active abilities (1 without).
    math::Fixed weapon_delay{math::Fixed::from_raw(math::Fixed::scale)};
    // IS-06: its fire rate, 1 minus an ion stun's shot rate reduction (1 without a stun).
    math::Fixed fire_rate{math::Fixed::from_raw(math::Fixed::scale)};
    math::Fixed scatter_radius{math::Fixed::from_raw(math::Fixed::scale)}; // WAD-10, current source mode
    // AB-66 (#561): a craft whose squadron's ION_CANNON_SHOT is on (`ion_held`) fires only its
    // override shot, only at `ion_target` (a unit or a squadron's team container) and its hardpoint,
    // and only while its own shot is still due (`ion_armed`).
    bool ion_held{};
    bool ion_armed{};
    EntityId ion_target{};
    std::uint32_t ion_target_hardpoint{no_hardpoint};
    bool in_limbo{}; // WAD-11: hidden hyperspace arrivals are absent from blast admission
    bool in_nebula{};
    EntityId barrage_target{};
    std::optional<math::Fixed> fixed_inaccuracy{};
    math::Fixed object_fire_rate{math::Fixed::from_raw(math::Fixed::scale)};
    bool target_prepared{};       // WSQ-42/43: member targeting has already run before team sharing
    std::optional<math::Vec3> squadron_centre{}; // WWP-49: copied team position for object-weapon aim
    bool sensor_jammed{}; // WPJ-10: supplied by the recipient-stamping owner
};

// The immutable inputs of one tick's targeting phase.
struct CombatWorld {
    std::span<const Player> players; // ascending ID
    std::span<const SnapshotPlayer> relationships; // WHZ-51: shared ordinary-combat relationship gate
    std::vector<CombatUnit> units;   // ascending ID
    SpaceIndex index;                // the same units in the same order (#636 relies on it)
    const CollectionTrees* collection{}; // target scans' candidate order (space-targeting CO rules)
    const CollectionTrees* projectile_collection{}; // DG-30: separate membership and persistent history
    const CombatTable* table{};
    const DurabilityRules* rules{};
    const DamageRules* damage{};     // the energy pool's rules (EN-05); none without damage rules
    std::uint64_t seed{};
    std::uint64_t frame{};
    // #424: each squadron's team container (ascending) with its live craft in team order.
    std::vector<std::pair<EntityId, std::vector<EntityId>>> teams;
    [[nodiscard]] const CombatUnit* find(EntityId id) const noexcept;
    [[nodiscard]] bool hostile(PlayerId owner, PlayerId target) const noexcept {
        return players_hostile(relationships, owner, target);
    }
    // FO-04: an attack target that is a squadron's
    // team container stands for its live craft nearest `from` (spatial, the first in team order on
    // a tie); nothing when none is left. Any other target is itself.
    [[nodiscard]] const CombatUnit* resolve_target(const CombatUnit* target, const math::Vec3& from) const;
    // The team container of a squadron craft; invalid_entity_id for any other unit (#561).
    [[nodiscard]] EntityId container_of(EntityId unit) const noexcept;
    // One player's units whose boxes touch the box of `half` about `centre`, in collection order
    // (CO-03); without trees, the index's ascending-ID order.
    [[nodiscard]] std::vector<EntityId> candidates(const math::Vec3& centre, math::Fixed half, PlayerId owner) const;
    const MotionTable* motion{}; // WSQ-45 / AV-05: canonical hard extents, including custom overrides
    const std::map<EntityId, EntityId>* craft_containers{}; // immutable existing membership index for this tick
    ProjectileDefenceRegistry projectile_defences;
    std::vector<EntityId> static_projectile_defences; // creation registrations, including inactive declared shields
};

// WHE-32/WPJ-17: derive this frame's recipient stamps and immutable source records.
[[nodiscard]] core::Result<void> prepare_projectile_defences(CombatWorld& world,
    std::span<const ProjectileDefenceAbilityInput> abilities, const PartitionExecutor& executor,
    std::span<const EntityId> registration_order = {});

struct ManualFire {
    PlayerId player{};
    std::uint32_t cooldown_frames{};
};
struct ManualFeedback {
    PlayerId player{};
    std::uint32_t hardpoint{};
};

struct CombatStep {
    CombatState state;
    std::vector<CombatEvent> events;
    // A turn in place toward this point, from the next frame (A-04 to A-07): planned like a face
    // order in the serial commit.
    std::optional<math::Vec3> face;
    // The energy the unit's object weapon spent this frame (EN-05), drained in the serial commit.
    math::Fixed energy_spent{};
    // Per weapon_fired event, in order: the shot's scatter in its target's frame, which a
    // homing projectile keeps adding to its aim point (MS-03). Not state; not hashed.
    std::vector<math::Vec3> aim_offsets;
    std::vector<ManualFire> manual_fired;
    std::vector<ManualFeedback> manual_feedback;
};

// One unit's ship-level target choice and weapon service at world.frame (docs/behaviour/
// space-weapon-fire.md). Reads only `world`; the result goes to the unit's own slot.
[[nodiscard]] core::Result<CombatStep> step_combat(const CombatWorld& world, const CombatUnit& unit);
// Target-only preparation: no fire or pulse/recharge updates; the caller commits team sharing.
struct FormationAttackContext {
    bool present{};
    bool allows_divert{};
    bool unbounded{};
    const CombatUnit* leader{};
};
[[nodiscard]] CombatState target_combat(const CombatWorld& world, const CombatUnit& unit, math::Fixed divert,
    EntityId formation_target, math::Vec3 diversion_anchor, const FormationAttackContext* formation = nullptr);

// WAD-39: command admission reads one immutable target and the current weapon state.
[[nodiscard]] core::Result<bool> manual_target_admissible(const CombatWorld& world, const CombatUnit& unit,
    const CombatUnit& target, std::uint32_t hardpoint);

// A-04's aim point on `target` seen from `from`: its live targetable hardpoint nearest `from`
// (the first in HardPoints order on a tie), else its position. The approach checks of #452 use it
// (docs/behaviour/space-orders.md OR-04).
// With `hardpoint` (an attack order's, #531 OR-25) that hardpoint's position while it stands.
[[nodiscard]] math::Vec3 ordered_aim_point(
    const CombatUnit& target, const math::Vec3& from, std::uint32_t ordered_index = no_hardpoint);

// The hardpoint of `target` with HardPoints index `index` while it is targetable and not
// destroyed, else null (also for no_hardpoint and a target without a combat profile).
[[nodiscard]] const TargetHardpoint* standing_hardpoint(const CombatUnit& target, std::uint32_t index);

// AT-10 / OR-03: range includes the target's loaded dimensions. Hardpoint aim uses
// the soft radius; centre aim uses the hard extent along the incoming bearing.
[[nodiscard]] math::Fixed target_soft_radius(const MotionTable* motion, const CombatUnit& target) noexcept;
[[nodiscard]] core::Result<math::Fixed> target_attack_distance(const MotionTable* motion, const CombatUnit& target,
    math::Vec3 from, math::Fixed distance, bool hardpoint_aim);
[[nodiscard]] bool has_aim_hardpoint(const CombatUnit& target) noexcept;

// One projectile's flight for one frame (docs/behaviour/space-damage.md DG-30 to DG-34): the
// segment it flies this frame, the first eligible hostile unit in collection order,
// and whether it runs out of travel. Reads only `world`.
struct ProjectileStep {
    Projectile projectile;          // moved on by one frame
    std::optional<EntityId> hit;    // the unit it reached
    // #536 (DG-36): the hardpoint whose collision mesh it met, else no_hardpoint; `meshed` when
    // the unit it reached was tested by its meshes rather than its box.
    std::uint32_t mesh_hardpoint{no_hardpoint};
    bool meshed{};
    math::Vec3 contact{};
    math::Vec3 from{};              // its position at the start of the frame
    bool expired{};
    ProjectileExpiryReason expiry_reason{ProjectileExpiryReason::none};
};
// #636: a worker's candidate buffers, reused from one projectile to the next.
struct ProjectileScratch {
    std::uint64_t defence_inspected{}, defence_candidates{};
    std::vector<EntityId> contacts; // DG-30 ray matches, retained per partition
    std::vector<std::uint32_t> near;       // immutable unit positions in hostile-player/ray order
    std::uint64_t candidate_count{};       // the work done so far (TacticalTick's counters)
    std::uint64_t exact_count{};
};
[[nodiscard]] core::Result<ProjectileStep> step_projectile(
    const CombatWorld& world, const Projectile& projectile, ProjectileScratch& scratch,
    MeshCollisionWork* mesh_work = nullptr); // optional caller-owned diagnostics, never canonical state

// A new projectile for a weapon_fired event of a weapon with a shot profile (DG-21 to DG-25).
// `allow_diminishing_firepower` is the shooter's DG-05 mode flag, snapshotted at the shot; a
// homing one (MS-02) faces the shot's aim point and keeps `offset`, the scatter in its target's frame.
[[nodiscard]] core::Result<Projectile> launch_projectile(const CombatEvent& shot, const ShotProfile& profile,
    PlayerId owner, bool allow_diminishing_firepower, std::uint64_t id, const math::Vec3& offset,
    math::Fixed target_radius = {});

void append_projectile(std::vector<std::uint8_t>& bytes, const Projectile& projectile);

// WPJ-42: caller owns admission/probability, random draws and disposal of the old
// projectile. This helper creates a fresh instance; it never guesses callback odds.
struct ProjectileRedirect {
    std::uint64_t new_id{};
    const CombatUnit* source{};
    const CombatUnit* target{};
    math::Fixed yaw_spread_draw{};
    bool add_pitch{};
    math::Fixed pitch_draw{}; // caller's uniform draw in [-60,30]
};
[[nodiscard]] core::Result<Projectile> redirect_projectile(Projectile& original,
    const ShotProfile& projectile_type, const ProjectileRedirect& request);

// Canonical combat record of a unit (docs/replay-format.md): attack target, flags, next scan
// frame, then per weapon its opportunity target, last scan frame, countdown and pulses left.
void append_combat(std::vector<std::uint8_t>& bytes, const CombatState& state);
void append_combat_event(std::vector<std::uint8_t>& bytes, const CombatEvent& event);

} // namespace eawr::sim::tactical::detail
