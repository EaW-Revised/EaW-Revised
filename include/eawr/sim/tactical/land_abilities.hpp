#pragma once

#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Land ability framework, group G1 (#3238, docs/behaviour/walks/land-abilities.md WLAB-01 to WLAB-11 and WLAB-77):
// the 75-name kind vocabulary and its flag table, the enabled gate, the timer choice per kind, the range context of a
// targeted order, and the effect registry the later groups (G2 movement and weapon modes, G3+ handlers) plug into.
// Pure functions of their inputs: no clock, thread or host input, nothing hashed or saved here.
namespace eawr::sim::tactical {

// WLAB-01: the engine numbers its authorable kinds 1 to 75 (the order the command bar already lists, AB-04).
inline constexpr std::uint32_t ability_vocabulary_count = 75;

struct AbilityVocabularyEntry {
    std::string_view name;
    AbilityKind kind{AbilityKind::none};
};
// The vocabulary in engine order; element 0 is engine index 1.
[[nodiscard]] std::span<const AbilityVocabularyEntry> ability_vocabulary() noexcept;
// The kind of an authored `Type` name, any case; none when the name is not one of the 75. Space `ability_kind()` is
// untouched by this and still names only the space kinds.
[[nodiscard]] AbilityKind land_ability_kind(std::string_view name) noexcept;
// The engine index (1 to 75) of a kind; 0 for none.
[[nodiscard]] std::uint32_t ability_vocabulary_index(AbilityKind kind) noexcept;
// The kind of an engine index (1 to 75); none outside it. WLAB-02 refuses such an index.
[[nodiscard]] AbilityKind ability_kind_of_index(std::uint32_t index) noexcept;

// WLAB-04: a kind that carries a behaviour of its own instead of only a slot.
enum class AbilityBehaviour : std::uint8_t { none, hunt, lure, deploy_troopers, self_destruct };
// WLAB-10, WLAB-11: how a no-target switch of the kind is handled.
enum class AbilityRoute : std::uint8_t {
    unit,            // the unit-level switch has a branch of its own for the kind
    handler_by_name, // the unit switch looks the nested handler up by GUI_Activated_Ability_Name and applies it with no target
    targeted_only,   // no unit-level branch: a no-target switch fails, only a targeted order reaches the handler
};
struct AbilityKindFlags {
    bool modal{true};                  // a state held on the slot; an instant kind never stays on
    bool nebula_disabled{true};        // set for every kind of the stock table; the enabled gate's nebula step reads it
    bool team{};                       // acts on the container
    bool force_cancelled{};            // refused while the unit is Force disabled (WLAB-06 step 3, WLAB-08)
    bool targeted{};                   // the command bar asks for a target first (AB-09): a switch-on carries an object or a point
    AbilityBehaviour behaviour{AbilityBehaviour::none};
    AbilityRoute route{AbilityRoute::unit};
    friend constexpr bool operator==(const AbilityKindFlags&, const AbilityKindFlags&) noexcept = default;
};
// The flags of a kind. A kind the table does not know (none, a value outside the vocabulary) reads as the defaults of
// an uninitialised entry: modal, nebula-disabled, not team.
[[nodiscard]] const AbilityKindFlags& ability_kind_flags(AbilityKind kind) noexcept;

// WLAB-09: which timer a switch-on of the kind starts.
enum class AbilityTimer : std::uint8_t {
    none,              // DEPLOY and SWAP_WEAPONS
    expiration,        // the shared countdown (WAB-03); the recharge starts when it ends (WAB-13)
    recharge_at_switch // instant kinds: the recharge starts at the switch and the slot does not stay on
};
// `has_expiration` is whether the authored Expiration_Seconds is above zero (only JET_PACK reads it).
[[nodiscard]] AbilityTimer ability_timer(AbilityKind kind, bool has_expiration) noexcept;

// WLAB-06, WLAB-07: the first gate a switch-on fails, in the documented order.
enum class AbilityRefusal : std::uint8_t {
    none = 0,
    kind_out_of_range,        // (1) a kind outside 1 to 75
    in_nebula,                // (2) when asked; no nebula exists on land
    force_disabled,           // (3) a kind the table marks cancelled by Force disabling
    limbo,                    // (4) in limbo and not garrisoned
    abilities_cancelled,      // (5) the special-ability manager's cancel flag
    excluded_behaviour,       // (6) the excluded behaviour class (unverified seam, DEV-14: nothing is excluded yet)
    recharging,               // (7) when asked
    team_member,              // (8) a team container needs every member to pass
    locomotor_hold,           // (9) a locomotor-level hold, for example a transit state
    movement_locked,          // WLAB-07: DEFEND, INTERDICT, HUNT
    in_limbo,                 // WLAB-07: the kinds that fail in limbo
    stunned,                  // WLAB-07: LUCKY_SHOT
    deploy_troopers,          // WLAB-07: the behaviour with resources left and its service off
    channelling,              // WLAB-07: FORCE_SIGHT while the unit channels another special ability
    special_hardpoint,        // WLAB-07: LEECH_SHIELDS and BLAST need a live special-weapon hardpoint
    remote_bomb_cap,          // WLAB-07: PLACE_REMOTE_BOMB at Max_Remote_Bombs_Per_Player
    summon,                   // WLAB-07: SUMMON needs no pending delay and no live summoned object
    base_power,               // WLAB-07: FIRE_LOBBING_SUPERWEAPON needs base power when the type requires it
    deploy_squad,             // WLAB-07: DEPLOY_SQUAD needs a ready spawn state and a fighter-bay hardpoint
};
[[nodiscard]] std::string_view to_string(AbilityRefusal refusal) noexcept;

// What the session knows of the unit for the gate. A subsystem the land session does not have yet is supplied as
// the failing value, never the passing one (see session_land_abilities.cpp): the per-kind tests below read these.
struct AbilityGateInput {
    bool in_nebula{};
    bool force_disabled{};
    bool limbo{}, garrisoned{};
    bool abilities_cancelled{};
    bool excluded_behaviour{};
    bool recharging{};
    bool team_member_fails{};       // some member of a team container fails the gate (recursion done by the caller)
    bool locomotor_hold{};
    bool movement_locked{};
    bool stunned{};
    bool deploy_troopers_ready{};   // behaviour present, resources left, service not already enabled
    bool channelling{};
    bool special_hardpoint_live{};
    std::uint32_t remote_bombs{}, remote_bomb_cap{};
    bool summon_ready{};            // no pending summon delay and no live summoned object
    bool base_power_ok{};
    bool deploy_squad_ready{};
};
// WLAB-06: kind in range; when `check_nebula` a unit inside a nebula fails; a Force-disabled unit fails the kinds the
// table marks; a unit in limbo that is not garrisoned fails; the cancel flag fails; one excluded behaviour class fails;
// when `check_recharge` a recharging unit fails; a team container fails unless every member passes; a locomotor hold
// fails; then the per-kind tests of WLAB-07. A switch-off never calls this.
[[nodiscard]] AbilityRefusal land_ability_enabled(AbilityKind kind, const AbilityGateInput& input,
    bool check_recharge, bool check_nebula) noexcept;

// WLAB-77: the range an ability supplies to the ordinary attack-range queries. An absent value leaves the weapon's own.
struct AbilityRangeContext {
    std::optional<math::Fixed> activation_min, activation_max; // Activation_Min_Range, Activation_Max_Range
    friend constexpr bool operator==(const AbilityRangeContext&, const AbilityRangeContext&) noexcept = default;
};
enum class AbilityRange : std::uint8_t { in_range, too_far, too_close };
// With no target a handler is close enough and not too close. With one, `distance` is checked against the context's
// maximum (the weapon's own `weapon_max` where the context has none or a non-positive one) and its minimum.
[[nodiscard]] AbilityRange land_ability_range(const AbilityRangeContext& context, bool has_target,
    math::Fixed distance, math::Fixed weapon_max) noexcept;

// What an effect may ask of the session beyond its own slot (#3239). The session supplies it on a switch; it is null on an
// expiry and in a test that registers a plain effect, and an effect that needs it does nothing without it.
class LandAbilityWorld {
public:
    virtual ~LandAbilityWorld() = default;
    // WLAB-15: takes `share` of the unit's maximum health from it as miscellaneous damage sourced to itself.
    virtual void damage_self(EntityId unit, math::Fixed share) = 0;
    // WLAB-16: the planar distance from the unit to a point; nothing when the unit is not live.
    [[nodiscard]] virtual std::optional<math::Fixed> planar_distance(EntityId unit, math::Vec3 point) const = 0;
    // WLAB-16: a jump to `point` was ordered: records the pending jump and, for a type that uses several locomotors, the
    // secondary locomotor in use. The land movement takes the request (#7084).
    virtual void request_jump(EntityId unit, math::Vec3 point) = 0;
    // WLAB-16 (#7084): whether the unit is flying or coming down. A second jump is refused until it has landed.
    [[nodiscard]] virtual bool jump_in_flight(EntityId unit) const = 0;
    // WLAB-17: the unit stops and its deploy phase becomes deployed (`on`) or stowed.
    virtual void set_deployed(EntityId unit, bool on) = 0;
    // WLAB-19: toggles the unit's current projectile index between 0 and 1 when its type has more than one projectile type;
    // otherwise nothing changes (the switch still succeeds).
    virtual void swap_weapon(EntityId unit) = 0;
};
struct AbilityUse {
    EntityId unit{};                    // the unit whose slot switches (a member when a container delegates)
    AbilityKind kind{AbilityKind::none};
    std::uint64_t tick{};
    EntityId target{};
    std::optional<math::Vec3> position{};
    const SpecialAbilityProfile* handler{}; // the nested handler GUI_Activated_Ability_Name names (WLAB-11), when one is bound
    const AbilityProfile* ability{};        // the authored row of the kind the unit holds
    LandAbilityWorld* world{};
};
// WLAB-77: what a targeted order of the kind may name. The default accepts an object or a point of any side.
enum class AbilityTargetRelation : std::uint8_t { any, hostile, friendly }; // friendly: the caster's own side and its allies
struct AbilityTargetRule {
    bool object{true}, point{true};
    AbilityTargetRelation relation{AbilityTargetRelation::any}; // applies to an object target
    friend constexpr bool operator==(const AbilityTargetRule&, const AbilityTargetRule&) noexcept = default;
};
// The hooks one kind's effect contributes to the switch. G1 owns the switch; a later group registers its kinds here
// and never edits the switch. A kind with no registered effect fails closed (nothing it does exists yet).
class LandAbilityEffect {
public:
    virtual ~LandAbilityEffect() = default;
    // Refuses a switch-on before anything changes.
    [[nodiscard]] virtual bool admit(const AbilityUse&) const { return true; }
    // After the slot changed: on is true for a switch-on, false for a switch-off or an expiry.
    virtual void switched(const AbilityUse&, bool /*on*/) const {}
    // The range the ability supplies to a targeted order (WLAB-77).
    [[nodiscard]] virtual AbilityRangeContext range() const { return {}; }
    // What a targeted order of the kind may name.
    [[nodiscard]] virtual AbilityTargetRule target_rule() const { return {}; }
};
class LandAbilityEffects {
public:
    // Strictly increasing kind.
    std::vector<std::pair<AbilityKind, std::shared_ptr<const LandAbilityEffect>>> rows;
    [[nodiscard]] const LandAbilityEffect* find(AbilityKind kind) const noexcept;
};
// #3239 G2: the effects of the movement and weapon-mode kinds (each PR of the group appends its rows, in ascending kind).
[[nodiscard]] std::shared_ptr<const LandAbilityEffects> make_land_ability_effects();

} // namespace eawr::sim::tactical
