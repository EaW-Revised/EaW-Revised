#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/damage.hpp"

#include <cstdint>
#include <array>
#include <functional>
#include <map>
#include <tuple>
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
    sensor_jamming, corrupt_systems, blast, stealth, find_weakness, maximum_firepower, laser_defense, leech_shields,
    redirect_blaster, force_healing = 15, enable_radar = 16,
};
enum class SpecialActivationStyle : std::uint8_t {
    unspecified, space_automatic, ground_automatic, ground_activated, galactic_automatic, user_input,
    skirmish_automatic = 6,
};
enum class SpecialAbilityMode : std::uint8_t { space, ground, galactic };
// BF-2 (#3779): the ability mode and the always-on activation style a battle domain uses.
[[nodiscard]] constexpr SpecialAbilityMode special_ability_mode(const BattleDomain domain) noexcept {
    return domain == BattleDomain::land ? SpecialAbilityMode::ground : SpecialAbilityMode::space;
}
[[nodiscard]] constexpr SpecialActivationStyle automatic_special_style(const BattleDomain domain) noexcept {
    return domain == BattleDomain::land ? SpecialActivationStyle::ground_automatic : SpecialActivationStyle::space_automatic;
}
inline constexpr std::uint32_t healing_initial_poll_slot = 0xfffe000eU; // coordinator-reserved, WSH-06

struct SpecialAbilityFilter {
    std::vector<TypeId> applicable_types{}; // sorted exact identities, never variant expansion
    std::uint64_t applicable_categories{};
    std::vector<TypeId> excluded_types{};
    std::uint64_t excluded_categories{};
    friend bool operator==(const SpecialAbilityFilter&, const SpecialAbilityFilter&) = default;
};
struct ChargedBlastProfile {
    std::uint32_t charge_frames{}; // FC-62: admitted one-frame services, not an elapsed deadline
    math::Fixed damage_multiplier{math::Fixed::from_raw(math::Fixed::scale)};
    std::string charging_effect{}, charged_effect{}, activate_sfx{};
    friend bool operator==(const ChargedBlastProfile&, const ChargedBlastProfile&) = default;
};
struct SpecialAbilityProfile {
    std::string name;
    SpecialAbilityKind kind{};
    SpecialActivationStyle style{}; // unspecified stays unclassified until its policy is traced
    std::optional<bool> initially_enabled{};
    bool affects_all_allies{}; // WRAD-01: radar recipients, omitted means owner only.
    bool causes_despawn{};
    std::uint32_t service_interval{}; // WHE-09: code interval, not an XML timer
    SpecialAbilityFilter filter;
    std::optional<ChargedBlastProfile> charged_blast{}; // WHE-35/36: BLAST only
    // WHE-25: target contributions, keyed by source and authored stacking category.
    math::Fixed target_damage_increase{}, target_speed_decrease{};
    std::uint32_t concentrate_stacking_category{};
    // WHE-57..60: nested ranges override ordinary targeting only when positive.
    math::Fixed beam_min_range{}, beam_max_range{}, damage_per_frame{};
    std::uint64_t doubled_speed_categories{}; // nonhero Corvette exception, resolved from content
    std::string beam_owner_particle{}, beam_owner_bone{};
    // WST-12: the cloak fade's length in seconds and its end tint (bytes); presentation only.
    math::Fixed stealth_transition{};
    std::array<std::uint8_t, 4> stealth_colour{255, 255, 255, 255};
    // WLS-02/03: shield-only drain and a category-zero outgoing damage contribution; no transfer.
    math::Fixed leech_damage_bonus{};
    std::uint32_t leech_duration_frames{}, leech_beam_frames{};
    std::array<std::uint8_t, 4> leech_colour{};
    math::Fixed leech_beam_width{};
    std::string leech_texture{}, leech_lightning{}, leech_effect{};
    // WHE-33/34: the named area handler supplies one refreshable status per recipient.
    math::Fixed corrupt_min_range{}, corrupt_max_range{}, corrupt_drain_per_frame{}, corrupt_inaccuracy{};
    std::uint32_t corrupt_duration_frames{};
    std::string corrupt_target_particle{}, corrupt_owner_particle{}, corrupt_activate_sfx{};
    // WLD-02..WLD-09: the LASER_DEFENSE point-defence handler (kind laser_defense only).
    std::uint32_t defence_duration_frames{};  // trunc(Defense_Duration_In_Secs x 30)
    std::uint32_t defence_look_frames{};      // trunc(Recharge_Time_In_Secs x 30): looks come every this + 1 frames
    std::uint32_t defence_beam_frames{5};     // Laser_Beam_Frames (the engine default when absent)
    math::Fixed defence_radius{};             // Protection_Radius
    math::Fixed defence_min_travel{};         // Projectile_Dist_Travelled: a shot must have flown farther than this
    std::uint32_t defence_categories{};       // Projectile_Types_Targeted as defence_category_* bits
    // Presentation only (WLD-12): the beam, its origin bones and the zap sound.
    std::array<std::uint8_t, 4> defence_colour{};
    math::Fixed defence_beam_width{};
    std::vector<std::string> defence_bones{};
    std::string defence_beam_texture{}, defence_lightning_effect{}, defence_zap_sfx{};
    // WRB-01..WRB-12: the take-damage Redirect_Blaster handler (kind redirect_blaster only). The chances are
    // fractions in [0, 1], the arc and the angle degrees; `filter` names the shooters it reacts to.
    math::Fixed redirect_chance{}, block_chance{}, reaction_arc{}, max_redirect_angle{};
    bool turn_to_face_unblockable{};
    std::string redirect_activate_sfx{}; // presentation: SFXEvent_Activate
    // WSH-01..H08: automatic space healing; deadline uses the existing special slot.
    math::Fixed heal_range{}, heal_percent{}, heal_amount{};
    std::uint32_t heal_interval_frames{};
    bool single_target_heal{};
    std::string heal_sfx{}, heal_particle{}, heal_lightning{}, heal_owner_bone{};
    std::vector<std::string> heal_target_bones{};
    std::array<math::Fixed, 3> heal_target_colour{}, heal_owner_colour{};
    std::uint32_t heal_target_light_frames{}, heal_owner_light_frames{};
    std::uint32_t heal_target_light_pulses{1}, heal_owner_light_pulses{1};
    friend bool operator==(const SpecialAbilityProfile&, const SpecialAbilityProfile&) = default;
};
// Projectile_Types_Targeted spellings the space game can fly (WLD-04); other categories are land-only
// (MPTL_ROCKET) or unmodelled and keep the ability out of the table (AB-26).
inline constexpr std::uint32_t defence_category_missile = 1U;
inline constexpr std::uint32_t defence_category_rocket = 2U;
inline constexpr std::uint32_t defence_category_mptl_rocket = 4U;
// WLD-04: the defence_category_* bit a shot in flight belongs to: a homing shot is a MISSILE, a shot on
// a rocket path a ROCKET; every other shot belongs to none the space game can target.
[[nodiscard]] std::uint32_t projectile_defence_category(const Projectile& shot) noexcept;
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

// WAB-54: immutable candidates in the world's deterministic query order.
struct HuntEnemy {
    math::Vec3 position{};
    bool fogged{}, force_sensitive{};
};
class CombatRandom;
[[nodiscard]] core::Result<math::Vec3> hunt_destination(math::Vec3 position,
    const std::optional<std::array<math::Fixed, 4>>& bounds, math::Fixed reveal_range,
    bool force_sensitive, std::span<const HuntEnemy> enemies,
    const std::function<bool(math::Vec3)>& fogged, CombatRandom& random);

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
    // AB-26: retained authored data; victim and land consumers remain unmodelled.
    math::Fixed target_speed{math::Fixed::from_raw(math::Fixed::scale)};
    math::Fixed target_take_damage{math::Fixed::from_raw(math::Fixed::scale)};
    std::optional<bool> weapons_enabled, can_be_squashed, allow_target_diminishing_firepower;
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
    // WCB-02 and WFP-01: ordinary projectile flight, and owner-only fog source data.
    math::Fixed speed{}, max_travel{};
    std::optional<math::Fixed> lifetime{};
    std::uint32_t ring_count{};
    std::optional<math::Fixed> reveal_range{};
    math::Fixed dense_reveal_multiplier{math::Fixed::from_raw(math::Fixed::scale)};
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

// WAB-70..74: what a BUZZ_DROIDS activation creates and how the droids act. Ranges are planar
// from the activating unit to the chosen point; damages are per logical frame (the authored
// per-second values over 30); speed is the droid type's Max_Speed in units per frame (MV-01).
struct BuzzDroidsProfile {
    TypeId droid_type{};
    math::Fixed min_range{}, max_range{}, chase_radius{}, damage_radius{};
    math::Fixed enemy_damage{}, own_damage{}, speed{};
    std::uint32_t activation_frames{};
    std::uint64_t victim_categories{}; // the droid type's Special_Weapon_Valid_Targets
    std::uint32_t damage_type{0xffffffffU}; // the shared miscellaneous damage row
    friend bool operator==(const BuzzDroidsProfile&, const BuzzDroidsProfile&) = default;
};
// WAB-72..74: one live droid object: where it was created and the first frame it acts.
struct BuzzDroidState {
    EntityId droid{};
    TypeId source_type{}; // the activating type whose profile the droid copied (WAB-71)
    math::Vec3 start{};
    std::uint64_t active_from{};
    friend constexpr bool operator==(const BuzzDroidState&, const BuzzDroidState&) noexcept = default;
};

// WLU-02..WLU-06: a LURE craft's pulse. Every pulse_frames from the switch-on, up to max_pulses
// times while the ability is on, the craft orders each hostile object of a listed type within the
// radius (3D, inclusive) to attack it. Target types are exact identities, sorted and unique.
struct LureProfile {
    math::Fixed radius{};
    std::uint32_t pulse_frames{}; // trunc(Pulse_Frequency_Secs x 30); 0: every frame
    std::uint32_t max_pulses{};
    std::vector<TypeId> target_types{};
    std::string particle{}; // presentation only (WLU-09)
    friend bool operator==(const LureProfile&, const LureProfile&) = default;
};
// WLU-03: whether a LURE switched on at `started` pulses at `tick`.
[[nodiscard]] bool lure_pulses(const LureProfile& lure, std::uint64_t started, std::uint64_t tick) noexcept;

struct CorruptSystemsState {
    TypeId source_type{};
    std::uint64_t started_tick{}, expires_tick{};
    math::Fixed drain_per_frame{}, inaccuracy{};
    friend bool operator==(const CorruptSystemsState&, const CorruptSystemsState&) = default;
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
    // WLD-06: the countdown to the next use starts when the ability switches on, not when it ends.
    bool recharge_at_activation{};
    TypeId replenish_team{}; // WHE-63: authored Create_Team_Type
    std::string replenish_particle{}; // Particle_Effect, presentation only
    std::optional<BuzzDroidsProfile> buzz{}; // WAB-70: BUZZ_DROIDS only
    std::optional<LureProfile> lure{}; // WLU-01: LURE only
    std::optional<ChargedBlastProfile> charged_blast{}; // named nested BLAST handler
    math::Fixed expiration_seconds{}; // WST-03: STEALTH only, Expiration_Seconds as authored for the cloak's own count
    bool disable_movement{}; // WIN-02, WMS-02: the active deployment locks movement
    // WHD-01: typed dependency resolved from content, independent of visual bones.
    std::vector<std::uint32_t> required_hardpoints{};
    bool requires_hardpoint{}; // true with an empty list means unavailable
    // WHE-61: engaged on failed object resolution; empty means an omitted tag.
    // Session content, bound by content identity rather than replay/checkpoint slots.
    std::optional<std::string> unresolved_spawn{};
    std::optional<math::Fixed> targeting_max_attack_distance{}; // WRA-03: positive active override
    friend bool operator==(const AbilityProfile&, const AbilityProfile&) = default;
};

// A type's abilities in Unit_Abilities_Data order (the primary, then the secondary one), and
// whether it runs the DEFEND stand-in of its object script (AB-41).
struct UnitAbilityProfile {
    TypeId type_id{};
    std::vector<AbilityProfile> abilities;
    bool defend_script{};
    std::vector<SpecialAbilityProfile> special{};
    bool force_sensitive{}; // WAB-54: hunt preference and offset exemption
    math::Fixed hunt_reveal_range{}; // authored range even without REVEAL
    // WSD-09: the type runs the Krayt object script, whose SELF_DESTRUCT triggers the remake serves
    // in the ability service (as the DEFEND stand-in of AB-41).
    bool self_destruct_script{};
    bool mc30_script{}; // FS-29: visible CLUSTER_BOMB object-script consumer only.
    std::vector<std::uint32_t> gravity_well_hardpoints{}; // WIN-02: authored hardpoint indices
    std::uint32_t stealth_detection_frames{30}; // WST-09: trunc(GameConstants seconds x 30), default 1 s.
    friend bool operator==(const UnitAbilityProfile&, const UnitAbilityProfile&) = default;
};

// WHD-05: one retained boolean per owner, exact type and case-insensitive name.
// An explicit unlock remains present after the source object is removed.
using NamedAbilityLocks = std::map<std::tuple<PlayerId, TypeId, std::string>, bool>;
[[nodiscard]] bool set_named_ability_lock(NamedAbilityLocks&, PlayerId, const UnitAbilityProfile&,
    std::string_view name, bool locked);
[[nodiscard]] bool named_ability_enabled(const NamedAbilityLocks&, PlayerId, TypeId,
    const SpecialAbilityProfile&);

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
    // WSH-09: the Damage_Force_Corruption row a negative automatic Heal_Amount delivers; no_type_index when the
    // content's damage-type list lacks the name (no armor row then applies, the hit still lands).
    std::uint32_t passive_damage_type{0xffffffffU};
    // WSD-10: the AI combat rating (AI_Combat_Power metric) of every type that has one, for the
    // Krayt script's threat sum; a squadron type holds the sum over its craft. Strictly increasing
    // type IDs, session content like the profiles.
    std::vector<std::pair<TypeId, math::Fixed>> combat_ratings{};
    [[nodiscard]] const UnitAbilityProfile* find(TypeId type_id) const noexcept;
    [[nodiscard]] math::Fixed combat_rating(TypeId type_id) const noexcept;
    [[nodiscard]] bool human(PlayerId player) const noexcept;
    [[nodiscard]] bool autofire_default(PlayerId player) const noexcept;
    friend bool operator==(const AbilityTable&, const AbilityTable&) = default;
};

inline constexpr std::uint32_t max_ability_frames = 30U * 3600U;
// Every multiplier is in [-64, 64]; delay, interval and speed multipliers are positive.
inline constexpr std::int64_t max_ability_multiplier = 64;

// Fails with EAWR-SIM-0305 unless type IDs, humans and creation owners strictly increase,
// creation owners are human, every profile passes validate_ability_profile.
[[nodiscard]] core::Result<void> validate_abilities(const AbilityTable& table);
// One type's profile (#4666): any number of abilities (the table and session slots are vectors), all
// of distinct modelled kinds, frames at most max_ability_frames and every multiplier in bounds.
// The loader checks each type with it so a tolerant load can skip just that type.
[[nodiscard]] core::Result<void> validate_ability_profile(const UnitAbilityProfile& profile);

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
    std::optional<math::Vec3> position{}; // WHE-33: consumed marker position, retained for presentation
    friend constexpr bool operator==(const AbilitySlot&, const AbilitySlot&) noexcept = default;
};

// A unit's abilities (UnitAbilityProfile order). `replan_due`: a speed change the move plan has
// not taken yet (AB-24). Hashed state. The DEFEND stand-in's rate of damage taken (AB-42) is the
// unit's damage tracking (WCC-82, DurabilityState).
// WSD-09, WSD-10: the Krayt object script's own state, from its first service on. `last_service` is the
// script's LastService on the engine's single-precision clock in units of 2^-28 s (WFO-22, exact); `nearby`
// and `threat` are what its proximity callbacks counted at the end of the last service, which the next
// service reads; `armed` is the human script's autofire state. Hashed in the KSCR block.
struct SelfDestructScript {
    std::int64_t last_service{};
    std::uint32_t nearby{};
    math::Fixed threat{};
    bool armed{};
    friend bool operator==(const SelfDestructScript&, const SelfDestructScript&) = default;
};

struct ChargedBlastState {
    std::uint32_t charge_remaining{}, shots_remaining{};
    bool charged_presentation{}; // FD-55: retained until the next admitted poll at zero
    friend constexpr bool operator==(const ChargedBlastState&, const ChargedBlastState&) noexcept = default;
};
// FC-57/58, FD-47: the previous wrapper sample and the admitted script clock.
// FS-29 chooses human/AI once at initialization; ownership does not reselect it.
struct Mc30Script {
    std::int64_t last_service{};
    std::uint32_t nearby{};
    math::Fixed threat{};
    bool human{};
    bool armed{};
    friend bool operator==(const Mc30Script&, const Mc30Script&) = default;
};

struct AbilityState {
    std::vector<AbilitySlot> slots;
    bool replan_due{};
    // FS-23/FC-51: a team targeting turn retries this point until BUZZ_DROIDS applies.
    // Range refusal neither consumes the request nor starts recharge. Hashed only when present (BZQR).
    std::optional<math::Vec3> pending_buzz_position{};
    SpecialAbilityState special{};
    // WHE-24: activation-only recruitment; release drops these source-owned references.
    std::vector<EntityId> concentrate_recruits{};
    // WST-03 to WST-09: the cloak itself, apart from the STEALTH slot: firing or a stun ends it
    // while the slot's countdown runs on. `stealth_detected` is the last frame an enemy detector
    // was in range (0 from the switch-on). Hashed in the STLH block, only while on.
    bool stealth_on{};
    std::uint64_t stealth_detected{};
    // WST-03/04: the frame the cloak's own count ends, when a Stealth_Duration_Multiplier makes it
    // differ from the slot's countdown; 0: it ends with the slot. Hashed in the STLX block.
    std::uint64_t stealth_end{};
    std::optional<SelfDestructScript> self_destruct_script{};
    std::optional<ChargedBlastState> charged_blast{}; // optional canonical BLST block
    std::optional<Mc30Script> mc30_script{};
    SeatId pending_buzz_controller{}; // ADR-020: initiating seat survives an admitted delayed request.
    friend bool operator==(const AbilityState&, const AbilityState&) = default;
};

// FC-62, FSC-56/58/59: one decrement per admitted poll; completion snapshots eligible
// SPECIALs, including zero. A later poll cleans zero-shot presentation. No target/fire gate.
void service_charged_blast(std::optional<ChargedBlastState>&, std::uint32_t eligible_specials) noexcept;

// WST-09: the cloaked unit is hidden from its enemies' sight and from every enemy that cannot
// target stealth units, unless a detector was in range within the profile's reveal hold.
[[nodiscard]] inline bool stealth_invisible(const AbilityState& state, const std::uint64_t frame,
    const UnitAbilityProfile* profile = nullptr) noexcept {
    const auto hold = profile ? profile->stealth_detection_frames : 30U;
    return state.stealth_on && frame > state.stealth_detected && frame - state.stealth_detected > hold;
}

// WST-03: the cloak's own count, trunc(seconds x factor x 30) frames, from the exact product of the
// fixed-point values (the debug build multiplies floats; the stock values 60/80 s and 1.25/1.5 are
// exact in both). A factor of 0 or less counts as 1, as in the debug build.
[[nodiscard]] inline std::uint64_t stealth_frames(const math::Fixed seconds, const math::Fixed factor) noexcept {
    if (seconds.raw() <= 0) return 0;
    const auto scaled = static_cast<std::uint64_t>(seconds.raw()) * logical_frames_per_second; // < 2^41 within max_ability_frames
    const auto multiplier = factor.raw() > 0 ? static_cast<std::uint64_t>(factor.raw())
                                             : static_cast<std::uint64_t>(math::Fixed::scale);
    // floor(scaled x multiplier / 2^48) in two steps; each product stays below 2^64 for a factor below 2^14.
    constexpr int split = 16;
    const auto high = scaled * (multiplier >> split);
    const auto low = scaled * (multiplier & ((std::uint64_t{1} << split) - 1U));
    return (high + (low >> split)) >> (2 * math::Fixed::fractional_bits - split);
}

// WRA-03: first authored active positive range; absent/nonpositive leaves ordinary reach.
[[nodiscard]] std::optional<math::Fixed> ability_attack_distance(const UnitAbilityProfile& profile,
    const AbilityState& state) noexcept;

[[nodiscard]] AbilityState initial_abilities(const UnitAbilityProfile& profile, bool autofire_default = false);

// The slot of `kind` in the profile, if the type has it.
[[nodiscard]] std::optional<std::size_t> ability_slot(const UnitAbilityProfile& profile, AbilityKind kind) noexcept;
// WIN-02, WMS-02: simultaneous deployments retain their own movement locks.
[[nodiscard]] bool ability_movement_disabled(const UnitAbilityProfile& profile, const AbilityState& state) noexcept;

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
    bool corrupted{}; // WHE-65: new activations are disabled while the recipient status lives
    bool gravity_well_online{true}; // WIN-02: absent hardpoints do not disable structures
    const DurabilityProfile* hardpoint_profile{};
    const DurabilityState* hardpoint_state{};
};

[[nodiscard]] bool ability_hardpoints_online(const AbilityProfile&, const AbilityGate&) noexcept;

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

// AB-42, WCC-82: the damage-rate window: every rate_window_frames ticks the damage a tracked unit
// took since the last close becomes its rate per second; target stickiness (WCC-15) and the
// stand-in read it.
inline constexpr std::uint32_t rate_window_frames = 30;
// AB-41: the stand-in switches DEFEND on when the rate exceeds this.
inline constexpr std::int64_t defend_rate_threshold = 20;
[[nodiscard]] bool rate_window_closes(std::uint64_t tick) noexcept;
void close_rate_window(DurabilityState& state) noexcept;

// The canonical record of a unit's abilities (docs/replay-format.md).
void append_abilities(std::vector<std::uint8_t>& bytes, const AbilityState& state);

} // namespace eawr::sim::tactical
