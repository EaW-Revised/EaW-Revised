#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/damage.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Unit abilities of the M2 fleet (#76, docs/behaviour/space-abilities.md AB-01 to AB-44): the
// four power modes DEFEND, TURBO, POWER_TO_WEAPONS and SPOILER_LOCK, their activation, duration
// and recharge, the multipliers they apply while active, and the DEFEND stand-in for the object
// script of the Nebulon-B and the MC80. Pure functions of Q24 values and ticks: no clock, thread or host input.
namespace eawr::sim::tactical {

// WHE-09/10/12/14: shared nested-handler data, independent of ordinary Unit_Ability kinds.
enum class SpecialAbilityKind : std::uint8_t {
    none, combat_bonus, production_price, concentrate_fire, tractor_beam, energy_weapon,
    sensor_jamming, corrupt_systems, blast, stealth, find_weakness, maximum_firepower,
};
enum class SpecialActivationStyle : std::uint8_t {
    unspecified, space_automatic, ground_automatic, ground_activated, galactic_automatic, user_input,
};
enum class SpecialAbilityMode : std::uint8_t { space, ground, galactic };

struct SpecialAbilityFilter {
    std::vector<TypeId> applicable_types{}; // sorted exact identities, never variant expansion
    std::uint64_t applicable_categories{};
    std::vector<TypeId> excluded_types{};
    std::uint64_t excluded_categories{};
    friend bool operator==(const SpecialAbilityFilter&, const SpecialAbilityFilter&) = default;
};
struct SpecialAbilityProfile {
    std::string name;
    SpecialAbilityKind kind{};
    SpecialActivationStyle style{}; // unspecified stays unclassified until its policy is traced
    std::optional<bool> initially_enabled{};
    bool causes_despawn{};
    std::uint32_t service_interval{}; // WHE-09: code interval, not an XML timer
    SpecialAbilityFilter filter;
    // WHE-25: target contributions, keyed by source and authored stacking category.
    math::Fixed target_damage_increase{}, target_speed_decrease{};
    std::uint32_t concentrate_stacking_category{};
    // WHE-57..60: nested ranges override ordinary targeting only when positive.
    math::Fixed beam_min_range{}, beam_max_range{}, damage_per_frame{};
    std::uint64_t doubled_speed_categories{}; // nonhero Corvette exception, resolved from content
    std::string beam_owner_particle{}, beam_owner_bone{};
    friend bool operator==(const SpecialAbilityProfile&, const SpecialAbilityProfile&) = default;
};
struct SpecialActivationContext {
    EntityId owner{}, target{};
    SpecialAbilityMode mode{SpecialAbilityMode::space};
    bool owner_exists{true}, type_exists{true}, death_clone{}, map_editor{};
    friend bool operator==(const SpecialActivationContext&, const SpecialActivationContext&) = default;
};
struct SpecialAbilitySlot {
    bool enabled{}, cancelled{}, despawn_success{};
    std::uint64_t next_service_frame{};
    std::optional<SpecialActivationContext> context{};
    std::vector<EntityId> targets{}; // sorted, unique tracked recipients
    friend bool operator==(const SpecialAbilitySlot&, const SpecialAbilitySlot&) = default;
};
struct SpecialAbilityState {
    bool service_cancelled{};
    std::vector<SpecialAbilitySlot> slots{};
    friend bool operator==(const SpecialAbilityState&, const SpecialAbilityState&) = default;
};

// A concrete handler supplies its own gates/effects. Calls may mutate only this owner's
// staged slot and effect output; world inputs are immutable during a partitioned phase.
struct SpecialAbilityHandler {
    virtual ~SpecialAbilityHandler() = default;
    virtual bool ready(std::size_t slot, const SpecialActivationContext&) const = 0;
    virtual bool appropriate_mode(std::size_t slot, SpecialAbilityMode) const = 0;
    virtual bool appropriate_target(std::size_t slot, const SpecialActivationContext&) const = 0;
    virtual bool available(std::size_t slot, const SpecialActivationContext&) const = 0;
    virtual bool apply(std::size_t slot, SpecialAbilitySlot&, const SpecialActivationContext&) = 0;
    virtual void service(std::size_t slot, SpecialAbilitySlot&, std::uint64_t frame) = 0;
    virtual bool target_live(EntityId target) const = 0;
    virtual void remove_effect(std::size_t slot, EntityId target) = 0;
    virtual void terminate(std::size_t slot) = 0;
};

[[nodiscard]] SpecialAbilityKind special_ability_kind(std::string_view name) noexcept;
[[nodiscard]] SpecialActivationStyle special_activation_style(std::string_view name) noexcept;
[[nodiscard]] std::uint32_t special_service_interval(SpecialAbilityKind kind) noexcept;
[[nodiscard]] bool special_type_matches(const SpecialAbilityFilter&, TypeId type, std::uint64_t categories) noexcept;
[[nodiscard]] SpecialAbilityState initial_special_abilities(std::span<const SpecialAbilityProfile>, bool default_enabled = false);
// Return the number of successful applications; despawn_success is set only by a successful Apply.
[[nodiscard]] std::size_t activate_special_abilities(std::span<const SpecialAbilityProfile>, SpecialAbilityState&,
    SpecialActivationStyle, const SpecialActivationContext&, bool first_only, SpecialAbilityHandler&);
[[nodiscard]] std::size_t service_special_abilities(std::span<const SpecialAbilityProfile>, SpecialAbilityState&,
    const SpecialActivationContext&, std::uint64_t frame, SpecialAbilityHandler&);
void delete_special_owner(std::span<const SpecialAbilityProfile>, SpecialAbilityState&, SpecialAbilityMode, SpecialAbilityHandler&);
void delete_special_target(SpecialAbilityState&, EntityId target);
void append_special_abilities(std::vector<std::uint8_t>&, const SpecialAbilityState&);

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
    math::Fixed scatter_radius{math::Fixed::from_raw(math::Fixed::scale)}; // WAD-10
    math::Fixed cause_damage{math::Fixed::from_raw(math::Fixed::scale)}; // WHE-22
    math::Fixed take_damage{math::Fixed::from_raw(math::Fixed::scale)}; // WHE-22
    math::Fixed fire_rate{math::Fixed::from_raw(math::Fixed::scale)}; // WAD-38
    friend constexpr bool operator==(const AbilityModifiers&, const AbilityModifiers&) noexcept = default;
};

struct WeakenProfile {
    bool on_detonation{};
    math::Fixed radius{}, take_damage_increase{}, cause_damage_reduction{};
    std::uint32_t duration_frames{};
    std::uint64_t categories{};
    std::string status_effect{};
    friend bool operator==(const WeakenProfile&, const WeakenProfile&) = default;
};
struct SpawnedAbilityProfile {
    TypeId type{};
    math::Fixed damage{}, reach{}, z_offset{};
    std::uint32_t damage_type{0xffffffffU}, countdown_frames{};
    bool shield_damage{true}, hitpoint_damage{true};
    BlastProfile blast;
    WeakenProfile weaken;
    friend bool operator==(const SpawnedAbilityProfile&, const SpawnedAbilityProfile&) = default;
};
struct WeakenRecipient {
    EntityId target{};
    std::uint64_t expires{};
    friend constexpr bool operator==(const WeakenRecipient&, const WeakenRecipient&) = default;
};
// WHE-28/29/61: independent spawned object identity and its surviving timed recipients.
struct AbilitySpawnState {
    std::uint64_t id{};
    TypeId type{};
    AbilityKind kind{};
    PlayerId owner{};
    EntityId source{};
    math::Vec3 position{};
    math::Quat rotation{};
    std::uint64_t due{};
    bool detonated{};
    std::vector<WeakenRecipient> recipients{};
    friend bool operator==(const AbilitySpawnState&, const AbilitySpawnState&) = default;
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
    math::Fixed effective_radius{};
    std::string gui_activated_ability_name{};
    std::optional<SpawnedAbilityProfile> spawned{};
    TypeId barrage_target_type{};
    std::optional<math::Fixed> fixed_inaccuracy{};
    math::Fixed target_z_offset{};
    TypeId replenish_team{}; // WHE-63: authored Create_Team_Type
    std::string replenish_particle{}; // Particle_Effect, presentation only
    friend bool operator==(const AbilityProfile&, const AbilityProfile&) = default;
};

// A type's abilities in Unit_Abilities_Data order (the primary, then the secondary one), and
// whether it runs the DEFEND stand-in of its object script (AB-41).
struct UnitAbilityProfile {
    TypeId type_id{};
    std::vector<AbilityProfile> abilities;
    bool defend_script{};
    std::vector<SpecialAbilityProfile> special{};
    friend bool operator==(const UnitAbilityProfile&, const UnitAbilityProfile&) = default;
};

// An empty table binds no abilities, as before #76. `humans` are the players a person commands:
// the DEFEND stand-in waits for autofire on their units (AB-41). Like the victory rules' human
// list it is session content, not replay data.
struct AbilityTable {
    std::vector<UnitAbilityProfile> profiles; // strictly increasing type_id
    std::vector<PlayerId> humans;             // strictly increasing
    // AB-45: owners whose new supported abilities start on autofire. Session content
    // supplies each local player's profile preference; existing units keep their toggles.
    std::vector<PlayerId> autofire_defaults{}; // strictly increasing, a subset of humans
    std::uint32_t beam_damage_type{0xffffffffU}; // shared Damage_Default armor index, bound from content
    [[nodiscard]] const UnitAbilityProfile* find(TypeId type_id) const noexcept;
    [[nodiscard]] bool human(PlayerId player) const noexcept;
    [[nodiscard]] bool autofire_default(PlayerId player) const noexcept;
    friend bool operator==(const AbilityTable&, const AbilityTable&) = default;
};

inline constexpr std::size_t max_abilities_per_type = 2;
inline constexpr std::uint32_t max_ability_frames = 30U * 3600U;
// Every multiplier is in [-64, 64]; delay, interval and speed multipliers are positive.
inline constexpr std::int64_t max_ability_multiplier = 64;

// Fails with EAWR-SIM-0305 unless type IDs, humans and creation owners strictly increase,
// creation owners are human, a type has one or two
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
    SpecialAbilityState special{};
    // WHE-24: activation-only recruitment; release drops these source-owned references.
    std::vector<EntityId> concentrate_recruits{};
    friend bool operator==(const AbilityState&, const AbilityState&) = default;
};

[[nodiscard]] AbilityState initial_abilities(const UnitAbilityProfile& profile, bool autofire_default = false);

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
    bool in_nebula{}; // WHZ-25: environmental gate precedes the ordinary gates
    bool in_ion_storm{}; // WHZ-32: DEFEND needs an effective shield
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
    scatter_radius,
    cause_damage,
    take_damage,
    fire_rate,
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
