#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Hull and hardpoint health, hardpoint loss, unit death and hardpoint repair (P2-09, #72;
// docs/behaviour/space-hardpoints.md). Everything here is a pure function of Q24 values:
// no clock, thread or host input.
namespace eawr::sim::tactical {

// What losing a hardpoint switches off (rules HD-10 to HD-15). `other` covers dummy art and
// the hardpoint types the M2 roster does not use.
enum class HardpointRole : std::uint8_t {
    other = 0,
    weapon = 1,
    engine = 2,
    shield_generator = 3,
    fighter_bay = 4,
    special_ability = 5,
};

// An armor or damage type index that the table does not know: its multiplier is 1 (DG-12).
inline constexpr std::uint32_t no_type_index = 0xffffffffU;

// One control point of the Diminishing_Firepower curve: seconds since the target's last
// projectile hit, and the damage factor there.
struct CurvePoint {
    math::Fixed x{};
    math::Fixed y{};
    friend constexpr bool operator==(const CurvePoint&, const CurvePoint&) noexcept = default;
};

// gameconstants.xml values of the damage rules and the Damage_To_Armor_Mod table, reduced to the
// damage and armor types the table's units and weapons use.
struct DamageRules {
    std::uint32_t shield_recharge_frames{};  // trunc(ShieldRechargeIntervalInSecs x 30 + 0.5), at least 1
    math::Fixed depleted_disable_seconds{};  // Depleted_Shield_Disable_Time
    math::Fixed depleted_increment_seconds{};// Depleted_Shield_Damage_Increment
    math::Fixed depleted_regen_cap{};        // Depleted_Shield_Regen_Cap
    std::vector<CurvePoint> diminishing;     // Diminishing_Firepower, strictly increasing x
    std::uint32_t damage_types{};
    std::uint32_t armor_types{};
    std::vector<math::Fixed> armor_mods;     // damage_types x armor_types, row by damage type
    // The energy pool (#361, EN-01 to EN-07): trunc(EnergyRechargeIntervalInSecs x 30 + 0.5)
    // frames, and the energy a shield recharge costs per shield point (EnergyToShieldExchangeRate).
    // Zero frames: no unit has a pool, as before #361.
    std::uint32_t energy_recharge_frames{};
    math::Fixed energy_to_shield{};
    math::Fixed asteroid_damage{}; // WHZ-11/12: raw damage and probability per serviced logical frame
    math::Fixed asteroid_rate{};
    std::uint32_t asteroid_damage_type{no_type_index}; // ordinary Damage_Default armor route
    math::Fixed ion_storm_disable_seconds{}; // WHZ-31, Ion_Storm_Shield_Disable_Time
    friend bool operator==(const DamageRules&, const DamageRules&) = default;
};

struct HardpointProfile {
    HardpointRole role{HardpointRole::other};
    bool destroyable{};
    // Health x Object_Max_Health_Multiplier_Space; zero for a hardpoint that is not destroyable.
    math::Fixed max_health{};
    math::Fixed repair_amount_per_frame{}; // Repair_Amount_Per_Frame; zero when not authored
    math::Fixed repair_cost_per_frame{};   // Repair_Cost_Per_Frame; zero when not authored
    friend bool operator==(const HardpointProfile&, const HardpointProfile&) = default;
};

// One unit type's durability: content that the setup's content identity names (for M2 the #65
// unit tables, units::durability_table), neither replay data nor state.
struct DurabilityProfile {
    TypeId type_id{};
    math::Fixed max_hull{};                    // Tactical_Health x Object_Max_Health_Multiplier_Space
    std::optional<math::Fixed> max_speed{};    // Max_Speed as authored; stations have none
    bool destroyed_with_hardpoints{};          // Should_Be_Destroyed_When_All_Hardpoints_Destroyed
    std::vector<HardpointProfile> hardpoints;  // HardPoints order; the index is the hardpoint's ID
    // Shields and armor (#74, docs/behaviour/space-damage.md); used only when the table binds
    // damage rules. A type without the SHIELDED behaviour has no shield (zero maximum).
    math::Fixed max_shields{};                 // Shield_Points
    math::Fixed shield_refresh{};              // Shield_Refresh_Rate, per recharge
    std::uint32_t armor_type{no_type_index};   // Armor_Type, index in DamageRules
    std::uint32_t shield_armor_type{no_type_index}; // Shield_Armor_Type
    // The shooter's allow-diminishing-firepower mode flag (DG-05): on by default. FoC turns it off
    // only for a shooter under a MAXIMUM_FIREPOWER-style ability (#76); no M2 unit type does.
    bool allow_diminishing_firepower{true};
    // The energy pool (#361, EN-01): a POWERED type's Energy_Capacity and Energy_Refresh_Rate, per
    // energy recharge. A type without POWERED has no pool (zero maximum).
    bool powered{};
    math::Fixed max_energy{};
    math::Fixed energy_refresh{};
    // IS-02: the ION_STUN_EFFECT behaviour; a type without it is never ion stunned (#561).
    bool ion_stun_effect{};
    // DG-40: scenario staging only; ordinary unit loaders leave this false.
    // Blocks ordinary hits before shields/hardpoints; privileged scripted damage bypasses it.
    bool scenario_invulnerable{};
    friend bool operator==(const DurabilityProfile&, const DurabilityProfile&) = default;
};

// gameconstants.xml scalars of the durability rules.
struct DurabilityRules {
    math::Fixed hull_vs_hardpoints{};     // Hull_Vs_Hard_Points_Health_Constraint
    math::Fixed engines_disabled_speed{}; // Engines_Disabled_Speed_Modifier
    math::Fixed damaged_fraction{};       // Health_Low_Percent_Threshold (HD-04)
    friend constexpr bool operator==(const DurabilityRules&, const DurabilityRules&) noexcept = default;
};

// An empty table binds no durability: no unit has health, takes damage or dies. A table without
// damage rules keeps the #72 behaviour: shots are events only, scripted damage is raw and no
// unit has a shield.
struct DurabilityTable {
    DurabilityRules rules;
    std::optional<DamageRules> damage;
    std::vector<DurabilityProfile> profiles; // strictly increasing type_id
    [[nodiscard]] const DurabilityProfile* find(TypeId type_id) const noexcept;
    friend bool operator==(const DurabilityTable&, const DurabilityTable&) = default;
};

inline constexpr std::size_t max_hardpoints_per_type = 255;
// Health bound in whole source units: every exact product of the service rules fits 192 bits.
inline constexpr std::int64_t max_durability_health = std::int64_t{1} << 20;

// Fails with EAWR-SIM-0305 unless type IDs strictly increase, every hull is in
// (0, max_durability_health], a type has at most 255 hardpoints, a destroyable hardpoint's
// health is in (0, max_durability_health] and any other hardpoint's is zero, repair values,
// speeds and the three rules are >= 0 and bounded by max_durability_health, and the speed
// modifier and the damaged fraction are at most 1.
[[nodiscard]] core::Result<void> validate_durability(const DurabilityTable& table);

// A durable unit's health: its hull and each hardpoint's health by index (zero for a hardpoint
// that is not destroyable). Hashed state.
struct DurabilityState {
    math::Fixed hull{};
    std::vector<math::Fixed> hardpoints;
    // With damage rules only (#74): the shield, the frame it was last depleted (DG-08) and the
    // frame of the last projectile hit (DG-05).
    math::Fixed shields{};
    std::optional<std::uint64_t> depleted_frame;
    std::optional<std::uint64_t> last_hit_frame;
    // With damage rules (#361): the energy pool (EN-01), and the frames of the unit's next shield
    // and energy recharges, each on the unit's own phase (DG-13, EN-02).
    math::Fixed energy{};
    std::uint64_t next_shield_frame{};
    std::uint64_t next_energy_frame{};
    // WPR-52: disabled is independent of health (a restored hardpoint has 0.1 health).
    std::vector<bool> disabled;
    std::vector<std::vector<PlayerId>> repairing_players;
    std::optional<std::uint64_t> engines_disabled_until{}; // EN-08; independent of engine hardpoint health
    std::optional<std::uint64_t> ion_storm_contact{};
    friend bool operator==(const DurabilityState&, const DurabilityState&) = default;
};

// Every unit starts at full hull, hardpoint health, shield and energy (HD-01, DG-13, EN-01).
[[nodiscard]] DurabilityState full_durability(const DurabilityProfile& profile);

// WPR-52 step 2: carry repairing/disabled hardpoints by index into a full new station.
void carry_station_hardpoints(const DurabilityProfile& previous_profile, const DurabilityState& previous,
    DurabilityState& replacement);

// Presentation state of one hardpoint (the #136 hook): destroyed at zero health, damaged while
// alive below damaged_fraction of its maximum, intact otherwise. A hardpoint that is not
// destroyable is always intact.
enum class HardpointState : std::uint8_t { intact = 0, damaged = 1, destroyed = 2 };

[[nodiscard]] bool hardpoint_destroyed(
    const DurabilityProfile& profile, const DurabilityState& state, std::size_t index) noexcept;
[[nodiscard]] bool hardpoint_disabled(const DurabilityState& state, std::size_t index) noexcept;
[[nodiscard]] HardpointState hardpoint_state(const DurabilityProfile& profile, const DurabilityRules& rules,
    const DurabilityState& state, std::size_t index) noexcept;

// Consequences (HD-10 to HD-14), at the level the simulation exposes before movement, firing
// and squadrons exist: a weapon hardpoint may fire only while it is not destroyed; a type with
// engine hardpoints loses its engines when the last one is destroyed, and its maximum speed is
// then scaled by Engines_Disabled_Speed_Modifier; the same holds for shield generators and the
// shield; a spawner launches only while one of its fighter bays is not destroyed.
[[nodiscard]] bool weapon_enabled(
    const DurabilityProfile& profile, const DurabilityState& state, std::size_t index) noexcept;
[[nodiscard]] bool engines_online(const DurabilityProfile& profile, const DurabilityState& state) noexcept;
// EN-08: repeated drains keep the later deadline; permanent engine loss is never restored.
void disable_engines(DurabilityState& state, std::uint32_t frames, std::uint64_t frame) noexcept;
[[nodiscard]] bool service_disabled_engines(DurabilityState& state, std::uint64_t frame) noexcept;
[[nodiscard]] bool shields_online(const DurabilityProfile& profile, const DurabilityState& state) noexcept;
[[nodiscard]] bool launch_ready(const DurabilityProfile& profile, const DurabilityState& state) noexcept;
// 1 while the engines are online, Engines_Disabled_Speed_Modifier afterwards.
[[nodiscard]] math::Fixed max_speed_factor(
    const DurabilityProfile& profile, const DurabilityRules& rules, const DurabilityState& state) noexcept;

// Damage aimed at the hull rather than a hardpoint.
inline constexpr std::uint32_t hull_target = 0xffffffffU;

// A valid damage target is the hull or a destroyable hardpoint of the profile.
[[nodiscard]] bool damage_target_valid(const DurabilityProfile& profile, std::uint32_t target) noexcept;

struct DamageOutcome {
    std::optional<std::uint32_t> destroyed_hardpoint; // the target hardpoint reached zero health
    bool unit_destroyed{};                            // the hull reached zero (HD-20)
};

// Applies `amount` (>= 0) to a valid target (HD-02, HD-03, HD-20, HD-21). Hardpoint damage never
// reaches the hull and damage beyond a hardpoint's health is lost; a destroyed hardpoint takes
// no more damage.
[[nodiscard]] DamageOutcome apply_damage(
    const DurabilityProfile& profile, DurabilityState& state, std::uint32_t target, math::Fixed amount) noexcept;

struct ServiceOutcome {
    std::vector<std::uint32_t> destroyed_hardpoints; // ascending index
    bool unit_destroyed{};
};

// One frame of the hull/hardpoint coupling (HS-01 to HS-04), run for every live durable unit
// after the tick's commands. Fails only on an arithmetic overflow that validate_durability rules out.
[[nodiscard]] core::Result<ServiceOutcome> service_durability(
    const DurabilityProfile& profile, const DurabilityRules& rules, DurabilityState& state);

// One frame of one player's repair of hardpoint `index` (HR-01 to HR-05). The caller holds the
// player's credits and deducts `cost` when `paid`. `stopped` ends the repair: the hardpoint is
// destroyed, not destroyable, unaffordable or already back at full health.
struct RepairOutcome {
    bool paid{};
    bool stopped{};
    math::Fixed cost{};
};
[[nodiscard]] core::Result<RepairOutcome> repair_frame(
    const DurabilityProfile& profile, DurabilityState& state, std::size_t index, math::Fixed credits);

// WSL-41: bounded player-budget arbitration over a staged repair request, in registration order.
// Missing/insolvent payers are omitted. A completion reserves no payment from later payers.
struct RepairBudget {
    PlayerId player{};
    math::Fixed credits{};
};
[[nodiscard]] std::vector<std::vector<PlayerId>> reserve_hardpoint_repairs(
    const DurabilityProfile& profile, const DurabilityState& state, std::span<RepairBudget> budgets);

[[nodiscard]] std::string_view to_string(HardpointRole role) noexcept;
[[nodiscard]] std::string_view to_string(HardpointState state) noexcept;

} // namespace eawr::sim::tactical
