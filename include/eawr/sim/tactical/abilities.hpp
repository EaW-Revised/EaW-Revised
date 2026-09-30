#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Unit abilities of the M2 fleet (#76, docs/behaviour/space-abilities.md AB-01 to AB-44): the
// four power modes DEFEND, TURBO, POWER_TO_WEAPONS and SPOILER_LOCK, their activation, duration
// and recharge, the multipliers they apply while active, and the DEFEND stand-in for the object
// script of the Nebulon-B and the MC80. Pure functions of Q24 values and ticks: no clock, thread or host input.
namespace eawr::sim::tactical {

// The XML and Lua spelling (`DEFEND`, `TURBO`, `POWER_TO_WEAPONS`, `SPOILER_LOCK`,
// `ION_CANNON_SHOT`), any case; none for every other name.
[[nodiscard]] AbilityKind ability_kind(std::string_view name) noexcept;

// The multipliers one ability applies while it is active (AB-20 to AB-26). A Mod_Multiplier the
// ability does not author is 1.
struct AbilityModifiers {
    math::Fixed weapon_delay{math::Fixed::from_raw(math::Fixed::scale)};
    math::Fixed shield_regen{math::Fixed::from_raw(math::Fixed::scale)};
    math::Fixed shield_regen_interval{math::Fixed::from_raw(math::Fixed::scale)};
    math::Fixed energy_regen{math::Fixed::from_raw(math::Fixed::scale)};
    math::Fixed energy_regen_interval{math::Fixed::from_raw(math::Fixed::scale)};
    math::Fixed speed{math::Fixed::from_raw(math::Fixed::scale)};
    friend constexpr bool operator==(const AbilityModifiers&, const AbilityModifiers&) noexcept = default;
};

// One Unit_Ability of a type (content, neither replay data nor state).
struct AbilityProfile {
    AbilityKind kind{AbilityKind::none};
    std::uint32_t expiration_frames{}; // trunc(Expiration_Seconds x 30); zero: no time limit (AB-11)
    std::uint32_t recharge_frames{};   // trunc(Recharge_Seconds x 30); zero: no recharge (AB-12)
    AbilityModifiers modifiers;
    bool supports_autofire{};          // Supports_Autofire (AB-40)
    // AB-60 (#561): a team ability: the squadron's team container holds it, from the team type's
    // data; its craft's own slot of the kind only marks which of them still has to fire.
    bool team{};
    friend constexpr bool operator==(const AbilityProfile&, const AbilityProfile&) noexcept = default;
};

// A type's abilities in Unit_Abilities_Data order (the primary, then the secondary one), and
// whether it runs the DEFEND stand-in of its object script (AB-41).
struct UnitAbilityProfile {
    TypeId type_id{};
    std::vector<AbilityProfile> abilities;
    bool defend_script{};
    friend bool operator==(const UnitAbilityProfile&, const UnitAbilityProfile&) = default;
};

// An empty table binds no abilities, as before #76. `humans` are the players a person commands:
// the DEFEND stand-in waits for autofire on their units (AB-41). Like the victory rules' human
// list it is session content, not replay data.
struct AbilityTable {
    std::vector<UnitAbilityProfile> profiles; // strictly increasing type_id
    std::vector<PlayerId> humans;             // strictly increasing
    [[nodiscard]] const UnitAbilityProfile* find(TypeId type_id) const noexcept;
    [[nodiscard]] bool human(PlayerId player) const noexcept;
    friend bool operator==(const AbilityTable&, const AbilityTable&) = default;
};

inline constexpr std::size_t max_abilities_per_type = 2;
inline constexpr std::uint32_t max_ability_frames = 30U * 3600U;
// Every multiplier is in [-64, 64]; delay, interval and speed multipliers are positive.
inline constexpr std::int64_t max_ability_multiplier = 64;

// Fails with EAWR-SIM-0305 unless type IDs and humans strictly increase, a type has one or two
// abilities of distinct modelled kinds, frames are at most max_ability_frames and every
// multiplier is in bounds.
[[nodiscard]] core::Result<void> validate_abilities(const AbilityTable& table);

// One ability of a unit. `expires_tick` is the tick its time limit ends it (zero: none), and
// `ready_tick` the first tick it may be switched on again after a recharge (AB-11, AB-12).
struct AbilitySlot {
    bool active{};
    bool autofire{};
    std::uint64_t started_tick{};
    std::uint64_t expires_tick{};
    std::uint64_t ready_tick{};
    // A targeted ability's target while it is on (#561, AB-61): a unit, and its hardpoint or
    // 0xffffffff for the whole unit. Zero otherwise.
    EntityId target{};
    std::uint32_t target_hardpoint{0xffffffffU};
    friend constexpr bool operator==(const AbilitySlot&, const AbilitySlot&) noexcept = default;
};

// A unit's abilities (UnitAbilityProfile order) and the damage it took, for the DEFEND stand-in
// (AB-42): the hull and shield lost since the last rate window closed, and the last window's
// rate per second. `replan_due`: a speed change the move plan has not taken yet (AB-24).
// Hashed state.
struct AbilityState {
    std::vector<AbilitySlot> slots;
    math::Fixed window_damage{};
    math::Fixed damage_rate{};
    bool replan_due{};
    friend bool operator==(const AbilityState&, const AbilityState&) = default;
};

[[nodiscard]] AbilityState initial_abilities(const UnitAbilityProfile& profile);

// The slot of `kind` in the profile, if the type has it.
[[nodiscard]] std::optional<std::size_t> ability_slot(const UnitAbilityProfile& profile, AbilityKind kind) noexcept;

// What the unit's other systems say about switching an ability on (AB-14, AB-16): DEFEND needs a
// shield that is online and not in its depletion effect; TURBO and SPOILER_LOCK need engines.
struct AbilityGate {
    bool shielded{};
    bool shields_online{};
    bool shield_depleted{};
    bool engines_online{true};
    bool ion_stunned{}; // AB-14: DEFEND is refused under ion stun (#561)
};

// AB-13: the ability may be switched on (or is on): it is not recharging and, for DEFEND, the
// gate holds. This is what the Lua `Is_Ability_Ready` reports.
[[nodiscard]] bool ability_ready(const AbilityProfile& profile, const AbilitySlot& slot, const AbilityGate& gate,
    std::uint64_t tick) noexcept;

// The change an activation or deactivation made; `speed` when the unit's speed multiplier changed.
struct AbilitySwitch {
    bool changed{};
    bool speed{};
};

// AB-10, AB-11: switches the ability on at `tick` unless it is on already or not ready.
[[nodiscard]] AbilitySwitch activate_ability(const AbilityProfile& profile, AbilitySlot& slot,
    const AbilityGate& gate, std::uint64_t tick) noexcept;
// AB-12: switches the ability off at `tick`. A time-limited ability ended early recharges for
// the share of its duration that ran; one that ran out recharges fully (expire_abilities).
[[nodiscard]] AbilitySwitch deactivate_ability(const AbilityProfile& profile, AbilitySlot& slot, std::uint64_t tick) noexcept;
// AB-11: ends every ability whose time limit is up at `tick` and starts its full recharge.
[[nodiscard]] AbilitySwitch expire_abilities(
    const UnitAbilityProfile& profile, AbilityState& state, std::uint64_t tick) noexcept;

// AB-20: a unit's multiplier of one kind is the product over its active abilities.
enum class AbilityModifier : std::uint8_t {
    weapon_delay,
    shield_regen,
    shield_regen_interval,
    energy_regen,
    energy_regen_interval,
    speed,
};
[[nodiscard]] math::Fixed ability_multiplier(
    const UnitAbilityProfile& profile, const AbilityState& state, AbilityModifier modifier) noexcept;

// AB-21: a recharge countdown after a weapon's burst (`full`) or between two shots of one burst.
// The delay multiplier lengthens only the gap within a burst; the full recharge takes it only
// when it shortens it. Rounded half up.
[[nodiscard]] std::uint32_t scaled_weapon_delay(std::uint32_t frames, math::Fixed multiplier, bool full) noexcept;
// AB-22, AB-23: a recharge interval of `frames` under an interval multiplier, rounded half up,
// at least 1.
[[nodiscard]] std::uint32_t scaled_interval(std::uint32_t frames, math::Fixed multiplier) noexcept;

// AB-42: the damage-rate window: every rate_window_frames ticks the damage taken since the last
// close becomes the rate per second, and the stand-in reads it.
inline constexpr std::uint32_t rate_window_frames = 30;
// AB-41: the stand-in switches DEFEND on when the rate exceeds this.
inline constexpr std::int64_t defend_rate_threshold = 20;
[[nodiscard]] bool rate_window_closes(std::uint64_t tick) noexcept;
void close_rate_window(AbilityState& state) noexcept;

// The canonical record of a unit's abilities (docs/replay-format.md).
void append_abilities(std::vector<std::uint8_t>& bytes, const AbilityState& state);

} // namespace eawr::sim::tactical
