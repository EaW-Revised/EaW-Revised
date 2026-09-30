#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/ion.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Projectile flight, damage types against armor, shield absorption and regeneration (P2-11,
// #74; docs/behaviour/space-damage.md). Everything here is a pure function of Q24 values: no
// clock, thread or host input.
namespace eawr::sim::tactical {

inline constexpr std::size_t max_curve_points = 64;
inline constexpr std::uint32_t max_damage_types = 1024;
// Multipliers and curve values are bounded so every product of the pipeline stays in Q24: a
// projectile's damage (at most max_durability_health) times the curve value times an armor
// multiplier is at most 2^36, below Q24's 2^39. FoC's own data stays far inside (armor
// multipliers up to 10, curve values up to 1).
inline constexpr std::int64_t max_damage_multiplier = 256;

// Fails with EAWR-SIM-0305 unless the recharge interval is at least one frame, the depleted
// values are >= 0 and bounded, the curve has at most max_curve_points points with strictly
// increasing x in [0, 3600] and y in [0, max_damage_multiplier] whose spline evaluates in Q24
// between every pair of points, the table is damage_types x
// armor_types with both at most max_damage_types, every multiplier is in [0,
// max_damage_multiplier], and every profile's shields and armor indices fit the table.
[[nodiscard]] core::Result<void> validate_damage(const DurabilityTable& table);

// Damage_To_Armor_Mod for the pair; 1 when either index is not in the table.
[[nodiscard]] math::Fixed armor_multiplier(
    const DamageRules& rules, std::uint32_t damage_type, std::uint32_t armor_type) noexcept;

// The Diminishing_Firepower factor `frames` after the target's last projectile hit (DG-05): the
// natural cubic spline through the control points, the end values outside them, capped to
// [0, max_damage_multiplier] (FoC's curve peaks near 1.18, far below the cap).
[[nodiscard]] core::Result<math::Fixed> diminishing_factor(const DamageRules& rules, std::uint64_t frames);

// What arrives at a unit: a projectile's hit (DG-01) or scripted damage (DG-20).
struct Hit {
    math::Fixed amount{};
    std::uint32_t damage_type{no_type_index};
    bool projectile{};        // a projectile hit; scripted damage otherwise
    bool shield_damage{true}; // Projectile_Does_Shield_Damage
    bool hitpoint_damage{true}; // Projectile_Does_Hitpoint_Damage
    std::uint32_t hardpoint{hull_target}; // a destroyable hardpoint, or the hull
    // DG-05's two gates, both true in every M2 case: the shooter's allow-diminishing-firepower mode
    // flag when it fired (DurabilityProfile::allow_diminishing_firepower) and the projectile's
    // internal damage type being the misc type, the default (ShotProfile::internal_damage_misc).
    // Diminishing firepower applies, and the target's last-hit frame advances, only when both hold;
    // ignored for scripted damage.
    bool allow_diminishing_firepower{true};
    bool internal_damage_misc{true};
    math::Fixed defense{};    // DG-26: the target's defense modifier; a projectile does 1 - this times its damage
    bool energy_damage{};     // EN-07: Projectile_Does_Energy_Damage; ignored for scripted damage
};

struct HitOutcome {
    DamageOutcome damage;       // what the hull or hardpoint lost
    math::Fixed absorbed{};     // what the shield took
    math::Fixed drained{};      // what the energy pool lost (EN-07)
    bool shields_depleted{};    // this hit took the shield to zero
    // Presentation only, never state: the shield took all of the hit (FoC's absorbed flag, which
    // picks the shield-absorb effect), and the hull armor multiplier that scaled what went on
    // (one when nothing reached the armor stage; FoC's armor-reduced effect is <= 0.75).
    bool shield_absorbed{};
    math::Fixed armor_multiplier{math::Fixed::from_raw(math::Fixed::scale)};
};

// Applies one hit at `frame` (DG-01 to DG-11, DG-20). Needs the table's damage rules; a
// projectile hit's amount is in [0, max_durability_health] like a validated projectile's damage.
[[nodiscard]] core::Result<HitOutcome> apply_hit(const DurabilityProfile& profile, const DamageRules& rules,
    DurabilityState& state, const Hit& hit, std::uint64_t frame);

// The shield depletion effect (DG-08) at `frame`.
[[nodiscard]] bool shield_depleted(const DamageRules& rules, const DurabilityState& state, std::uint64_t frame) noexcept;

// One shield recharge (DG-13 to DG-16, EN-04); the session calls it on the unit's recharge
// frames. A unit with an energy pool pays for what the shield gains, or gains nothing.
// `multiplier` is an active ability's shield regen multiplier (AB-22); a negative one drains.
[[nodiscard]] core::Result<void> recharge_shields(const DurabilityProfile& profile, const DamageRules& rules,
    DurabilityState& state, std::uint64_t frame, math::Fixed multiplier = math::Fixed::from_raw(math::Fixed::scale));

// --- The energy pool (#361, docs/behaviour/space-damage.md EN-01 to EN-07) -----------------------

// Whether the unit has a pool: the rules bind one and the type is POWERED (EN-01).
[[nodiscard]] bool has_energy_pool(const DurabilityProfile& profile, const DamageRules& rules) noexcept;

// One energy recharge (EN-02): Energy_Refresh_Rate times an active ability's energy regen
// multiplier (AB-23), kept in [0, Energy_Capacity].
[[nodiscard]] core::Result<void> recharge_energy(const DurabilityProfile& profile, const DamageRules& rules,
    DurabilityState& state, math::Fixed multiplier = math::Fixed::from_raw(math::Fixed::scale));

// A shot that costs `amount` (EN-05, EN-06): true and the pool drained when the unit has a pool
// holding at least `amount`; true without a cost when `amount` is zero; false otherwise.
[[nodiscard]] bool draw_energy(
    const DurabilityProfile& profile, const DamageRules& rules, DurabilityState& state, math::Fixed amount) noexcept;

// After a unit lost hardpoints: once its last shield generator is gone the shield drops to zero
// and never recharges again (DG-17).
void shield_generators_lost(
    const DurabilityProfile& profile, const DamageRules& rules, DurabilityState& state, std::uint64_t frame) noexcept;

// --- Projectiles ------------------------------------------------------------------------------

// A model-space box, scaled by the type's Scale_Factor: the union of its collidable meshes
// (DG-31). A type without one cannot be hit by projectiles.
struct CollisionBox {
    math::Vec3 min{};
    math::Vec3 max{};
    friend constexpr bool operator==(const CollisionBox&, const CollisionBox&) noexcept = default;
};

// A projectile in flight (hashed state when the table binds damage rules).
struct Projectile {
    std::uint64_t id{};                   // creation order, from 1
    EntityId shooter{};
    PlayerId owner{};
    std::uint32_t weapon{};               // the shooter's weapon slot key (HardPoints index or object_weapon)
    EntityId target{};
    std::uint32_t target_hardpoint{};     // the hardpoint the shot aimed at, or no_hardpoint
    math::Vec3 position{};
    math::Vec3 step{};                    // displacement per frame
    math::Fixed speed{};                  // |step|
    math::Fixed travelled{};
    math::Fixed max_travel{};
    math::Fixed damage{};
    std::uint32_t damage_type{no_type_index};
    bool shield_damage{true};
    bool hitpoint_damage{true};
    // Captured at the shot (DG-05): the shooter's allow-diminishing-firepower flag and whether the
    // projectile's internal damage type is the misc type, the default.
    bool allow_diminishing_firepower{true};
    bool internal_damage_misc{true};
    // A homing projectile (#361, MS-01 to MS-07): its facing in degrees (yaw in [0, 360), pitch
    // positive downward, FoC's convention), its turn limit, whether it still holds its target,
    // and its aim offset in the target's frame (applied only when all three parts are nonzero).
    bool homing{};
    bool locked{};
    math::Fixed turn_rate{};
    math::Fixed yaw{};
    math::Fixed pitch{};
    math::Vec3 offset{};
    // Ion weapons (#561): the shot drains energy (EN-07) and stuns what it hits (IS-01).
    bool energy_damage{};
    std::optional<IonStunShot> ion_stun{};
    friend bool operator==(const Projectile&, const Projectile&) = default;
};

// Whether the segment from `from` to `to` enters `box` placed by `transform` (rotation and
// position), and the entry fraction in [0, 1] (Q24). A segment that starts inside enters at 0.
[[nodiscard]] core::Result<std::optional<math::Fixed>> segment_enters_box(
    const CollisionBox& box, const math::Mat3x4& transform, const math::Vec3& from, const math::Vec3& to);

} // namespace eawr::sim::tactical
