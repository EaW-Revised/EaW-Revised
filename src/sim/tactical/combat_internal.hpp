#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/space.hpp"
#include "collection.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace eawr::sim::tactical::detail {

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
    // AB-21: the weapon delay multiplier of its active abilities (1 without).
    math::Fixed weapon_delay{math::Fixed::from_raw(math::Fixed::scale)};
    // IS-06: its fire rate, 1 minus an ion stun's shot rate reduction (1 without a stun).
    math::Fixed fire_rate{math::Fixed::from_raw(math::Fixed::scale)};
    // AB-66 (#561): a craft whose squadron's ION_CANNON_SHOT is on (`ion_held`) fires only its
    // override shot, only at `ion_target` (a unit or a squadron's team container) and its hardpoint,
    // and only while its own shot is still due (`ion_armed`).
    bool ion_held{};
    bool ion_armed{};
    EntityId ion_target{};
    std::uint32_t ion_target_hardpoint{no_hardpoint};
    // #636: no point a projectile can hit on it (its collision box, the box around its meshes, the
    // world box its DG-37 sphere needs a step to meet) lies further than this from its position,
    // whatever its rotation, with a margin for the Q24 rounding of the exact tests; 0 without a
    // collision box.
    math::Fixed collision_reach{};
};

// The immutable inputs of one tick's targeting phase.
struct CombatWorld {
    std::span<const Player> players; // ascending ID
    std::vector<CombatUnit> units;   // ascending ID
    SpaceIndex index;                // the same units in the same order (#636 relies on it)
    const CollectionTrees* collection{}; // target scans' candidate order (space-targeting CO rules)
    const CombatTable* table{};
    const DurabilityRules* rules{};
    const DamageRules* damage{};     // the energy pool's rules (EN-05); none without damage rules
    math::Fixed collision_reach{};   // no collision box reaches further from its unit's position (per axis)
    std::uint64_t seed{};
    std::uint64_t frame{};
    // #424: each squadron's team container (ascending) with its live craft in team order.
    std::vector<std::pair<EntityId, std::vector<EntityId>>> teams;
    [[nodiscard]] const CombatUnit* find(EntityId id) const noexcept;
    // FO-04: an attack target that is a squadron's
    // team container stands for its live craft nearest `from` (spatial, the first in team order on
    // a tie); nothing when none is left. Any other target is itself.
    [[nodiscard]] const CombatUnit* resolve_target(const CombatUnit* target, const math::Vec3& from) const;
    // The team container of a squadron craft; invalid_entity_id for any other unit (#561).
    [[nodiscard]] EntityId container_of(EntityId unit) const noexcept;
    // One player's units whose boxes touch the box of `half` about `centre`, in collection order
    // (CO-03); without trees, the index's ascending-ID order.
    [[nodiscard]] std::vector<EntityId> candidates(const math::Vec3& centre, math::Fixed half, PlayerId owner) const;
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
};

// One unit's ship-level target choice and weapon service at world.frame (docs/behaviour/
// space-weapon-fire.md). Reads only `world`; the result goes to the unit's own slot.
[[nodiscard]] core::Result<CombatStep> step_combat(const CombatWorld& world, const CombatUnit& unit);

// A-04's aim point on `target` seen from `from`: its live targetable hardpoint nearest `from`
// (the first in HardPoints order on a tie), else its position. The approach checks of #452 use it
// (docs/behaviour/space-orders.md OR-04).
[[nodiscard]] math::Vec3 ordered_aim_point(const CombatUnit& target, const math::Vec3& from);

// One projectile's flight for one frame (docs/behaviour/space-damage.md DG-30 to DG-34): the
// segment it flies this frame, the first unit of another team whose collision box it enters,
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
};
// #636: a worker's candidate buffers, reused from one projectile to the next.
struct ProjectileScratch {
    std::vector<std::uint32_t> candidates; // index positions in the broad phase's box
    std::vector<std::uint32_t> near;       // those that pass the reach test, then sorted
    std::uint64_t candidate_count{};       // the work done so far (TacticalTick's counters)
    std::uint64_t exact_count{};
};
[[nodiscard]] core::Result<ProjectileStep> step_projectile(
    const CombatWorld& world, const Projectile& projectile, TeamId owner_team, ProjectileScratch& scratch);

// A new projectile for a weapon_fired event of a weapon with a shot profile (DG-21 to DG-25).
// `allow_diminishing_firepower` is the shooter's DG-05 mode flag, snapshotted at the shot; a
// homing one (MS-02) faces the shot's aim point and keeps `offset`, the scatter in its target's frame.
[[nodiscard]] core::Result<Projectile> launch_projectile(const CombatEvent& shot, const ShotProfile& profile,
    PlayerId owner, bool allow_diminishing_firepower, std::uint64_t id, const math::Vec3& offset);

void append_projectile(std::vector<std::uint8_t>& bytes, const Projectile& projectile);

// Canonical combat record of a unit (docs/replay-format.md): attack target, flags, next scan
// frame, then per weapon its opportunity target, last scan frame, countdown and pulses left.
void append_combat(std::vector<std::uint8_t>& bytes, const CombatState& state);
void append_combat_event(std::vector<std::uint8_t>& bytes, const CombatEvent& event);

} // namespace eawr::sim::tactical::detail
