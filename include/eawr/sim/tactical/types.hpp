#pragma once

#include "eawr/sim/commands.hpp"
#include "eawr/sim/seat.hpp"
#include "eawr/sim/tactical/retreat_action.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace eawr::sim::tactical {

// Selects the tactical provider; space remains the default for existing callers.
enum class BattleDomain : std::uint32_t { space = 0, land = 1 };

// Logical frame rate pinned by docs/behaviour/tactical-tick-rate.md: one tick is 1/30 s.
inline constexpr std::uint32_t logical_frames_per_second = 30;
inline constexpr std::uint32_t tick_numerator = 1;
inline constexpr std::uint32_t tick_denominator = logical_frames_per_second;
inline constexpr std::uint32_t tactical_rules_version = 1;
inline constexpr std::uint32_t tactical_state_encoding_version = 1;
inline constexpr std::uint32_t tactical_snapshot_encoding_version = 3;

inline constexpr std::uint32_t max_players = 64;
// Controller seats follow the GC envelope's resource bound, independently of faction count.
inline constexpr std::uint32_t max_controller_seats = 256;
inline constexpr std::uint32_t max_units_per_command = 1024;
inline constexpr std::uint64_t max_units = 1'000'000;
inline constexpr std::uint64_t max_commands = 1'000'000;
inline constexpr std::uint64_t max_ticks = 1'000'000;

using PlayerId = std::uint32_t;
using TeamId = std::uint32_t;
using FactionId = std::uint64_t;
using TypeId = std::uint64_t;

// ADR-020: faction ownership and temporary battle control are separate identities.
enum class UnitControl : std::uint32_t { none = 0, mobile = 1, shared_structure = 2 };
enum class CommandOrigin : std::uint32_t { ordinary = 0, seat = 1, engine = 2 };
enum class FactionController : std::uint32_t { humans = 1, ai = 2, passive = 3 };
struct ControllerBinding {
    SeatId seat{};
    PlayerId player{};
    SeatKind kind{SeatKind::human};
    bool active{true};
    friend bool operator==(const ControllerBinding&, const ControllerBinding&) = default;
};
struct ControllerFaction {
    PlayerId player{};
    FactionController mode{FactionController::humans};
    friend bool operator==(const ControllerFaction&, const ControllerFaction&) = default;
};
struct BattleControllerPolicy {
    std::vector<ControllerBinding> seats;
    std::vector<ControllerFaction> factions;
    friend bool operator==(const BattleControllerPolicy&, const BattleControllerPolicy&) = default;
};

// A fighter squadron (#271, docs/behaviour/space-visibility.md V-03): its team container,
// a unit of its own that carries the squadron's sensor, and its craft in strictly increasing
// ID. The container sits at the centre of its live craft's bounding box and leaves the
// session with its last craft.
struct Squadron {
    EntityId container{};
    std::vector<EntityId> members;
    friend bool operator==(const Squadron&, const Squadron&) = default;
};


// WDF-05 (WU-27): both fighter grids are laid over the space map's box from its low corner. The
// session carries that corner; the default is the world origin (a session built without a map).
struct MapBox {
    math::Fixed min_x{};
    math::Fixed min_y{};
    friend constexpr bool operator==(const MapBox&, const MapBox&) noexcept = default;
};

// WSS-27..30: the four applied match switches that bind replay purchase content.
struct SkirmishMatchPolicy {
    bool allow_heroes{true};
    bool allow_superweapons{true};
    bool free_starting_units{true};
    bool pre_built_base{true};
    [[nodiscard]] constexpr std::uint32_t disabled_flags() const noexcept {
        return (allow_heroes ? 0U : 1U) | (allow_superweapons ? 0U : 2U)
            | (free_starting_units ? 0U : 4U) | (pre_built_base ? 0U : 8U);
    }
    friend constexpr bool operator==(const SkirmishMatchPolicy&, const SkirmishMatchPolicy&) noexcept = default;
};

inline constexpr std::uint32_t player_flag_commandable = 1U;

struct Player {
    PlayerId player_id{};
    TeamId team_id{};
    FactionId faction_id{};
    std::uint32_t flags{};
    [[nodiscard]] constexpr bool commandable() const noexcept {
        return (flags & player_flag_commandable) != 0U;
    }
    friend constexpr bool operator==(const Player&, const Player&) noexcept = default;
};

// An attack that names no hardpoint: it is on the unit (#531). The same value as hull_target and
// no_hardpoint, which name it where damage and shots do.
inline constexpr std::uint32_t attack_hull = 0xffffffffU;

// PU-08 / legacy #6266: the tactical activation styles that leave an income stream silent until an
// event starts it. Retail admits six non-automatic styles; the galactic three belong to the campaign.
enum class IncomeEventStyle : std::uint8_t { none = 0, special_attack = 1, take_damage = 2, user_input = 3 };

enum class OrderKind : std::uint8_t {
    none = 0,
    stop = 1,
    move = 2,
    attack = 3,
    // Scripted damage (#72): an event's command kind only; it never becomes a unit's order.
    damage = 4,
    // Turn in place to face a point, the retail Lua `Turn_To_Face` (#70).
    face = 5,
    // Move and engage what comes into range (#452, docs/behaviour/space-orders.md OR-10 to OR-13).
    attack_move = 6,
    // Follow and escort a unit, or hold a point (#452, OR-14 to OR-17).
    guard = 7,
    // A unit ability switched on or off, or its autofire set (#76): a command kind only; it
    // never becomes a unit's order.
    ability = 8,
    // Skirmish purchasing (#530, docs/behaviour/space-purchasing.md): command kinds only; they
    // never become a unit's order.
    buy = 9,        // PU-10 to PU-15: queue a type at a station
    cancel = 10,    // PU-17: cancel a queue entry
    reinforce = 11, // PU-30 to PU-34: bring a pooled unit in at a point
    pad_build = 12, // WBP-09/10: replay opcode 13, separate from order kind
    credit_grant = 13, // SAE-07: replay opcode 14
    ai_reservation_debit = 19, // coordinator-reserved, WAS-25: replay opcode 23
    pad_sell = 14, // coordinator-reserved: WBP-30, replay opcode 15
    intentional_quit = 15, // coordinator-reserved: replay opcode 16, WBF-43/48
    area_ability = 16, // WAD-38: coordinator-reserved, replay opcode 17
    manual_target = 17, // WAD-39: coordinator-reserved, replay opcode 18
    reveal_all = 18, // coordinator-reserved: V-20, replay opcode 21
    repair_hardpoint = 21, // coordinator-reserved: WSL-40, replay opcode 25
    retreat = 23, // WR-43..47, replay opcode 28; campaign only.
    special_fire = 22, // coordinator-reserved: WSW-15/40/41, command only.
    campaign_credit_release = 24, // WGA-54: replay opcode 29
    beacon = 25, // coordinator-reserved: HCB-12, replay opcode 31, no unit order.
    gift = 26, // ADR-020: controller transfer only, replay opcode 32, no unit order.
    battle_control = 27, // ADR-020: trusted departure boundary, replay opcode 33.
    script_invulnerable = 28, // TL-03: Lua Make_Invulnerable, replay opcode 34, no unit order.
    land_garrison = 29,
    land_unload = 30,
    tactical_superweapon = 31, // TSW-10: replay opcode 37, no unit order.
    script_create_object = 33, // SSU-03: coordinator-reserved opcode 39, no unit order.
    script_despawn = 32, // #7886: Lua Object.Despawn(), replay opcode 40, no unit order.
    script_teleport = 34, // Lua Teleport_And_Face: replay opcode 41, no unit order.
    script_hold_fire = 35, // Lua Prevent_All_Fire: replay opcode 42, no unit order.
    script_change_owner = 36, // Lua Change_Owner: replay opcode 43, no unit order.
};

// The order a unit last accepted: a move or face keeps its point in `destination`, an attack its
// target in `target`, an attack-move or guard both (#452; a zero target orders the point).
struct Order {
    OrderKind kind{OrderKind::none};
    std::uint64_t issued_tick{};
    math::Vec3 destination{};
    EntityId target{};
    // An attack on one of the target's hardpoints (#531, space-orders OR-20): its index in the
    // target type's HardPoints list, or attack_hull for an attack on the unit.
    std::uint32_t hardpoint{attack_hull};
    bool through_hazards{}; // WHZ-08a: double-click move, conditional MVHZ state
    // MV-40: a move that carries a final facing (degrees in [0, 360), 0 along +X); the unit turns to it
    // in place after its path ends, at the authored rate (space ships). Cleared by any later order. Conditional MVYW state.
    std::optional<math::Fixed> final_yaw{};
    friend constexpr bool operator==(const Order&, const Order&) noexcept = default;
};

// WHE-06/07: contained identities are outside independent spatial/weapon services.
struct CarriedObject {
    EntityId entity_id{};
    TypeId type_id{};
    EntityId parent{};
    bool named_hero{}, generic_hero{};
    bool limbo{true};
    bool model_visible{}, collidable{}, selected{}, movement_coordinated{};
    bool combat_preserved{};
    std::vector<CarriedObject> members;
    friend bool operator==(const CarriedObject&, const CarriedObject&) = default;
};

struct UnitState {
    EntityId entity_id{};
    TypeId type_id{};
    PlayerId owner{};
    math::Vec3 position{};
    math::Quat rotation{math::identity_quat()};
    Order order{};
    TypeId purchase_type{}; // WHE-02/40: zero means the ordinary deployed identity
    std::vector<CarriedObject> contained{};
    std::uint64_t purchase_token{}; // SAE-11: admitted reserved purchase, zero for ordinary units
    EntityId barrage_source{}; // WAD-38: proxy owner entity; zero for ordinary units
    bool garrison_enabled{true}; // FL-13: marker-created skirmish stations disable authored garrisons
    SeatId controller_seat{};
    UnitControl control{UnitControl::none};
    friend constexpr bool operator==(const UnitState&, const UnitState&) noexcept = default;
};

[[nodiscard]] constexpr TypeId purchase_identity(const UnitState& unit) noexcept {
    return unit.purchase_type != 0 ? unit.purchase_type : unit.type_id;
}

// WHE-06: reject reuse before touching the carrier; setup visits team members first.
[[nodiscard]] inline bool contain_object(UnitState& carrier, CarriedObject& rider, const bool preserve_combat) {
    if (carrier.entity_id == invalid_entity_id || rider.entity_id == invalid_entity_id
        || rider.entity_id == carrier.entity_id || rider.parent != invalid_entity_id) return false;
    const auto setup = [&](auto&& self, CarriedObject& object, const EntityId parent) -> void {
        for (auto& member : object.members) self(self, member, object.entity_id);
        object.parent = parent;
        object.limbo = true;
        object.model_visible = object.collidable = object.selected = object.movement_coordinated = false;
        object.combat_preserved = preserve_combat;
    };
    setup(setup, rider, carrier.entity_id);
    carrier.contained.push_back(rider);
    return true;
}

struct StopPayload {
    friend constexpr bool operator==(const StopPayload&, const StopPayload&) noexcept = default;
};

struct MovePayload {
    math::Vec3 destination{};
    bool through_hazards{}; // coordinator-reserved opcode 20 when true; ordinary move stays 2
    bool append{}; // HCB-57: optional replay tail; an ordinary move keeps its original bytes.
    // MV-40: optional arrival heading (degrees in [0, 360), quantised to 0.1 by the producer).
    // Present: replay opcode 38. Absent: today's move, bit for bit.
    std::optional<math::Fixed> final_yaw{};
    friend constexpr bool operator==(const MovePayload&, const MovePayload&) noexcept = default;
};

// An attack names the unit, and optionally one of its hardpoints (#531, space-orders OR-20 to
// OR-25): `hardpoint` is the index in the target type's HardPoints list, `attack_hull` for an
// attack on the unit.
struct AttackPayload {
    EntityId target{};
    std::uint32_t hardpoint{attack_hull};
    friend constexpr bool operator==(const AttackPayload&, const AttackPayload&) noexcept = default;
};

// Scripted damage, the retail Lua `Take_Damage(amount[, hardpoint])` (#72,
// docs/behaviour/space-hardpoints.md HD-30): `amount` (>= 0) hits each listed unit's hull, or
// the hardpoint with that index in its type's HardPoints list. Only the script host issues it.
struct DamagePayload {
    math::Fixed amount{};
    std::uint32_t hardpoint{0xffffffffU}; // hull_target
    friend constexpr bool operator==(const DamagePayload&, const DamagePayload&) noexcept = default;
};

// Lua `Make_Invulnerable(enabled)` (TL-03): sets the per-unit script flag on each listed unit and,
// for a squadron, on each of its members. Idempotent, no expiry; only the script host issues it.
struct ScriptInvulnerablePayload {
    bool enabled{};
    friend constexpr bool operator==(const ScriptInvulnerablePayload&, const ScriptInvulnerablePayload&) noexcept = default;
};

// Lua `Object.Despawn()` (#7886): removes each listed unit from the battle silently (no death effects, no destruction
// event, no kill credit); only the script host issues it.
struct ScriptDespawnPayload {
    friend constexpr bool operator==(const ScriptDespawnPayload&, const ScriptDespawnPayload&) noexcept = default;
};

// Turn in place towards `target` (#70, docs/behaviour/space-movement.md MV-20).
struct FacePayload {
    math::Vec3 target{};
    friend constexpr bool operator==(const FacePayload&, const FacePayload&) noexcept = default;
};

// Attack-move (#452, OR-10 to OR-13): towards `destination`, or when `target` is nonzero towards
// that unit, which the unit approaches like an attack order without making it its target.
struct AttackMovePayload {
    math::Vec3 destination{};
    EntityId target{};
    friend constexpr bool operator==(const AttackMovePayload&, const AttackMovePayload&) noexcept = default;
};

// Guard (#452, OR-14 to OR-17): follow and escort `target`, or when it is zero hold `destination`.
struct GuardPayload {
    math::Vec3 destination{};
    EntityId target{};
    friend constexpr bool operator==(const GuardPayload&, const GuardPayload&) noexcept = default;
};

// Modelled ordinary abilities, including the behaviour switch HUNT (WAB-50 to WAB-56).
enum class AbilityKind : std::uint8_t {
    none = 0,
    defend = 1,
    turbo = 2,
    power_to_weapons = 3,
    spoiler_lock = 4,
    ion_cannon_shot = 5,
    barrage = 6, // coordinator-reserved, WAD-38
    invulnerability = 7, // coordinator-reserved, WHE-22
    concentrate_fire = 8, // coordinator-reserved, WHE-24/25
    energy_weapon = 9, // coordinator-reserved, WHE-26/57/59
    tractor_beam = 10, // coordinator-reserved, WHE-27/58/60
    harmonic_bomb = 11, // coordinator-reserved, WHE-29/61
    weaken_enemy = 12, // coordinator-reserved, WHE-28/61
    replenish_wingmen = 13, // coordinator-reserved, WHE-30/63
    hunt = 14, // coordinator-reserved, WAB-50/56
    missile_shield = 15, // coordinator-reserved, WPJ-17
    sensor_jamming = 16, // coordinator-reserved, WHE-32
    buzz_droids = 17, // coordinator-reserved, WAB-70 to WAB-79
    laser_defense = 18, // coordinator-reserved, WLD-01 to WLD-12
    full_salvo = 19, // coordinator-reserved, WAB-80 to WAB-83
    stealth = 20, // coordinator-reserved, WST-01..WST-15
    self_destruct = 21, // coordinator-reserved, WSD-01 to WSD-12
    corrupt_systems = 22, // coordinator-reserved, WHE-33/34
    leech_shields = 23, // coordinator-reserved, WLS-01 to WLS-05
    lure = 24, // coordinator-reserved, WLU-01 to WLU-10
    interdict = 25, // coordinator-reserved, WIN-01/02/04
    cluster_bomb = 26, // coordinator-reserved, WCB-01..03
    fow_reveal_ping = 27, // coordinator-reserved, WFP-01/02/03
    blast = 28, // coordinator-reserved, WHE-35/36, FS-34/35
    rocket_attack = 29, // WRA-01: modal weapon selection
    super_laser = 30, // WSPL-01 to WSPL-06
    deploy_squad = 31, // FL-17: manual hangar launch
    lucky_shot = 32, // WLK-01..06: holder-wide shot bonus.
    // WLAB-01 (#3238): the rest of the 75-name vocabulary, in vocabulary order. Appended once; the value is the replay
    // byte, so a value is never reused or renumbered. Space tables never carry these (ability_kind() does not name
    // them); land_ability_kind() (land_abilities.hpp) is their only entry.
    deploy = 33,
    harass = 34,
    spread_out = 35,
    afterburner = 36,
    avoid_danger = 37,
    force_telekinesis = 38,
    force_whirlwind = 39,
    force_lightning = 40,
    flame_thrower = 41,
    jet_pack = 42,
    capture_vehicle = 43,
    targeted_invulnerability = 44,
    targeted_hack = 45,
    targeted_repair = 46,
    sticky_bomb = 47,
    area_effect_stun = 48,
    area_effect_heal = 49,
    area_effect_convert = 50,
    distract = 51,
    cable_attack = 52,
    untargeted_sticky_bomb = 53,
    deploy_troopers = 54,
    sprint = 55,
    maximum_firepower = 56,
    eject_vehicle_thief = 57,
    fire_lobbing_superweapon = 58,
    stun = 59,
    radioactive_contaminate = 60,
    berserker = 61,
    force_sight = 62,
    saber_throw = 63,
    force_cloak = 64,
    force_confuse = 65,
    tactical_bribe = 66,
    swap_weapons = 67,
    place_remote_bomb = 68,
    detonate_remote_bomb = 69,
    infection = 70,
    proximity_mines = 71,
    stim_pack = 72,
    drain_life = 73,
    shield_flare = 74,
    summon = 75,
};

// What an ability command asks (AB-10 to AB-13): switch the ability on or off, or set whether it
// is on autofire (AB-40).
enum class AbilityAction : std::uint8_t {
    activate = 1,
    deactivate = 2,
    autofire_on = 3,
    autofire_off = 4,
};

// A unit ability (#76, docs/behaviour/space-abilities.md AB-10 to AB-15, AB-40): switch `ability`
// on or off, or set its autofire, on each listed unit; a squadron container passes it to its
// craft (AB-15).
// A targeted ability (#561, AB-61) switches on at `target`, and at its hardpoint `target_hardpoint`
// when that is not no_hardpoint; every other ability command has no target.
struct AbilityPayload {
    AbilityKind ability{AbilityKind::none};
    AbilityAction action{AbilityAction::activate};
    EntityId target{};
    std::uint32_t target_hardpoint{0xffffffffU}; // no_hardpoint
    std::optional<math::Vec3> position{}; // WHE-61: WEAKEN_ENEMY; wire extension flag 2
    friend constexpr bool operator==(const AbilityPayload&, const AbilityPayload&) noexcept = default;
};

// WAD-38: activate an ability at a world point. Existing entity-target bytes stay unchanged.
struct AreaAbilityPayload {
    AbilityKind ability{AbilityKind::none};
    math::Vec3 point{};
    friend constexpr bool operator==(const AreaAbilityPayload&, const AreaAbilityPayload&) noexcept = default;
};

// WAD-39: assign an enemy object to one manual weapon hardpoint. The issuer is retained.
struct ManualTargetPayload {
    EntityId target{};
    std::uint32_t hardpoint{};
    friend constexpr bool operator==(const ManualTargetPayload&, const ManualTargetPayload&) noexcept = default;
};

// WSW-15/40/41: preserve entry-route admission in the recorded authoritative
// command. AI routes require an authoritative AI player; they are not a HUD bypass.
enum class SpecialFireAdmission : std::uint8_t { player = 0, ai_object = 1, ai_taskforce = 2 };
struct SpecialFirePayload {
    EntityId target{};
    SpecialFireAdmission admission{SpecialFireAdmission::player};
    friend constexpr bool operator==(const SpecialFirePayload&, const SpecialFirePayload&) noexcept = default;
};

// Buy (#530, PU-10 to PU-15): queue `type` at the command's one listed unit, the station.
struct BuyPayload {
    TypeId type{};
    bool prepaid{}; // WAS-26: funded AI entry uses coordinator-reserved opcode 24
    friend constexpr bool operator==(const BuyPayload&, const BuyPayload&) noexcept = default;
};

// Cancel (#530, PU-17): entry `index` of the issuer's queue `queue` (0 units, 1 upgrades). The
// command lists no unit.
struct CancelPayload {
    std::uint32_t queue{};
    std::uint32_t index{};
    std::uint64_t entry_id{}; // PU-17: nonzero identifies the clicked entry; zero retains legacy index cancellation.
    friend constexpr bool operator==(const CancelPayload&, const CancelPayload&) noexcept = default;
};

// Reinforce (#530, PU-30 to PU-34): bring the issuer's first pooled unit of `type` in at
// `position`. The command lists no unit.
struct ReinforcePayload {
    TypeId type{};
    math::Vec3 position{};
    std::uint64_t pool_token{}; // SAE-11: zero keeps ordinary type-based admission
    std::optional<math::Fixed> facing_yaw{}; // WR-X01: absent preserves the player's FoC facing
    friend constexpr bool operator==(const ReinforcePayload&, const ReinforcePayload&) noexcept = default;
};

struct PadBuildPayload {
    TypeId type{};
    friend constexpr bool operator==(const PadBuildPayload&, const PadBuildPayload&) noexcept = default;
};
struct CreditGrantPayload {
    math::Fixed amount{};
    friend constexpr bool operator==(const CreditGrantPayload&, const CreditGrantPayload&) = default;
};
struct CampaignCreditReleasePayload {
    math::Fixed request{}; // WGA-54: negative requests all remaining strategic credits, within cap headroom
    friend constexpr bool operator==(const CampaignCreditReleasePayload&, const CampaignCreditReleasePayload&) = default;
};
struct AiReservationDebitPayload {
    math::Fixed amount{};
    friend constexpr bool operator==(const AiReservationDebitPayload&, const AiReservationDebitPayload&) = default;
};

struct RetreatPayload {
    RetreatAction action{RetreatAction::start};
    friend bool operator==(const RetreatPayload&, const RetreatPayload&) = default;
};
struct RetreatCinematicPayload {
    bool playing{};
    std::uint32_t frame{}; // reserved wire field: must be zero; the session owns cinematic time
    friend bool operator==(const RetreatCinematicPayload&, const RetreatCinematicPayload&) = default;
};

struct QuitPayload {
    friend constexpr bool operator==(const QuitPayload&, const QuitPayload&) noexcept = default;
};

struct DepartureControl {
    PlayerId player{}, controller{};
    std::uint64_t tick{};
    bool basic_ai{};
    friend bool operator==(const DepartureControl&, const DepartureControl&) = default;
};

struct PlayerQuit {
    PlayerId player{};
    std::uint64_t tick{};
    friend constexpr bool operator==(const PlayerQuit&, const PlayerQuit&) noexcept = default;
};

struct PadSellPayload {
    friend constexpr bool operator==(const PadSellPayload&, const PadSellPayload&) noexcept = default;
};

// V-20: persistent whole-map reveal for this player alone; no unit list.
struct RevealAllPayload {
    PlayerId player{};
    friend constexpr bool operator==(const RevealAllPayload&, const RevealAllPayload&) noexcept = default;
};

// WSL-40: units names exactly one station; the command issuer is the payer.
struct RepairHardpointPayload {
    std::uint32_t hardpoint{};
    friend constexpr bool operator==(const RepairHardpointPayload&, const RepairHardpointPayload&) noexcept = default;
};

struct BeaconPayload {
    math::Vec3 position{};
    friend constexpr bool operator==(const BeaconPayload&, const BeaconPayload&) noexcept = default;
};

// ADR-020: transfer selected mobile roots to an active human on the same battle side.
struct GiftPayload {
    SeatId recipient{};
    friend constexpr bool operator==(const GiftPayload&, const GiftPayload&) noexcept = default;
};

// An agreed engine boundary, never a human-wire request. The complete effective
// roster makes standalone child playback independent of live disconnect events.
struct BattleControlPayload {
    SeatId departed{};
    BattleControllerPolicy policy;
    friend bool operator==(const BattleControlPayload&, const BattleControlPayload&) = default;
};

// HCB-12: bounded per-issuer state; these objects do not join unit services.
struct BeaconState {
    PlayerId player{};
    TypeId type{};
    math::Vec3 position{};
    std::uint64_t placed_tick{}, expires_tick{}, sequence{};
    friend constexpr bool operator==(const BeaconState&, const BeaconState&) noexcept = default;
};

struct LandGarrisonPayload {
    EntityId host{};
    std::uint32_t bone{};
    friend constexpr bool operator==(const LandGarrisonPayload&, const LandGarrisonPayload&) noexcept = default;
};
struct LandUnloadPayload {
    EntityId occupant{}; // zero when all is set
    bool all{};
    friend constexpr bool operator==(const LandUnloadPayload&, const LandUnloadPayload&) noexcept = default;
};

// TSW-10: the Death Star switch (docs/behaviour/space-tactical-superweapon.md). One listed unit, the object with the
// tactical super weapon behaviour; the stage is the transition the click or the script asks for. `ai_object` marks the
// script call (an AI admission, never a human wire command).
enum class TswStage : std::uint8_t { start_approach = 1, start_power_up = 2 };
enum class TswAdmission : std::uint8_t { player = 0, ai_object = 1 };
struct TacticalSuperweaponPayload {
    TswStage stage{TswStage::start_approach};
    TswAdmission admission{TswAdmission::player};
    friend constexpr bool operator==(const TacticalSuperweaponPayload&, const TacticalSuperweaponPayload&) noexcept = default;
};

// SSU-03: direct factory creation, with no purchase or queue transaction.
// The high-bit identity is a Lua reference; the ordered commit allocates the ordinary object ID.
inline constexpr std::uint64_t script_creation_namespace = std::uint64_t{1} << 63;
struct ScriptCreateObjectPayload {
    TypeId type{};
    PlayerId owner{};
    math::Vec3 position{};
    std::uint64_t identity{};
    friend constexpr bool operator==(const ScriptCreateObjectPayload&, const ScriptCreateObjectPayload&) noexcept = default;
};

// Lua `Teleport_And_Face(target)`: each listed unit moves to the target's position and takes its facing (the retail
// wrapper reads both from the target object); the unit stops where it lands. Only the script host issues it.
struct ScriptTeleportPayload {
    EntityId target{};
    friend constexpr bool operator==(const ScriptTeleportPayload&, const ScriptTeleportPayload&) noexcept = default;
};

// Lua `Prevent_All_Fire(enabled)`: sets or clears the per-unit hold-fire flag on each listed unit; while set, none
// of the unit's weapons fire. Idempotent, no expiry; only the script host issues it.
struct ScriptHoldFirePayload {
    bool enabled{};
    friend constexpr bool operator==(const ScriptHoldFirePayload&, const ScriptHoldFirePayload&) noexcept = default;
};

// Lua `Change_Owner(player)`: each listed unit (and the rest of its squadron) changes to the named player, keeping its
// hull and shield percentages; only the script host issues it.
struct ScriptChangeOwnerPayload {
    PlayerId owner{};
    friend constexpr bool operator==(const ScriptChangeOwnerPayload&, const ScriptChangeOwnerPayload&) noexcept = default;
};

using CommandPayload = std::variant<StopPayload, MovePayload, AttackPayload, DamagePayload, FacePayload,
    AttackMovePayload, GuardPayload, AbilityPayload, BuyPayload, CancelPayload, ReinforcePayload, PadBuildPayload,
    CreditGrantPayload, QuitPayload, PadSellPayload, AreaAbilityPayload, ManualTargetPayload, RevealAllPayload,
    RepairHardpointPayload, AiReservationDebitPayload, SpecialFirePayload, CampaignCreditReleasePayload, RetreatPayload, RetreatCinematicPayload, BeaconPayload, GiftPayload, BattleControlPayload, ScriptInvulnerablePayload, LandGarrisonPayload, LandUnloadPayload, TacticalSuperweaponPayload, ScriptCreateObjectPayload, ScriptTeleportPayload, ScriptHoldFirePayload, ScriptChangeOwnerPayload, ScriptDespawnPayload>;

// Whether a command acts on the issuer's economy (#530) rather than on units.
[[nodiscard]] constexpr bool economy_command(const CommandPayload& payload) noexcept {
    return std::holds_alternative<BuyPayload>(payload) || std::holds_alternative<CancelPayload>(payload)
        || std::holds_alternative<ReinforcePayload>(payload) || std::holds_alternative<PadBuildPayload>(payload)
        || std::holds_alternative<CreditGrantPayload>(payload) || std::holds_alternative<PadSellPayload>(payload)
        || std::holds_alternative<AiReservationDebitPayload>(payload) || std::holds_alternative<CampaignCreditReleasePayload>(payload)
        || std::holds_alternative<ScriptCreateObjectPayload>(payload);
}

// key.player_id is the issuer. Units are nonzero and strictly increasing; a buy lists exactly its
// station, a cancel or reinforce none (#530), every other command at least one unit.
struct PlayerCommand {
    CommandKey key;
    std::vector<EntityId> units;
    CommandPayload payload;
    SeatId issuing_seat{};
    CommandOrigin origin{CommandOrigin::ordinary};
    friend bool operator==(const PlayerCommand&, const PlayerCommand&) = default;
};

[[nodiscard]] inline PlayerCommand retreat_command(const CommandKey key, const RetreatAction action) {
    return {key, {}, RetreatPayload{action}};
}
[[nodiscard]] inline PlayerCommand retreat_cinematic_command(const CommandKey key, const bool playing, const std::uint32_t frame) {
    return {key, {}, RetreatCinematicPayload{playing, frame}};
}

[[nodiscard]] constexpr OrderKind order_kind(const CommandPayload& payload) noexcept {
    if (std::holds_alternative<ScriptCreateObjectPayload>(payload)) return OrderKind::script_create_object;
    if (std::holds_alternative<ScriptTeleportPayload>(payload)) return OrderKind::script_teleport;
    if (std::holds_alternative<ScriptHoldFirePayload>(payload)) return OrderKind::script_hold_fire;
    if (std::holds_alternative<ScriptChangeOwnerPayload>(payload)) return OrderKind::script_change_owner;
    if (std::holds_alternative<TacticalSuperweaponPayload>(payload)) return OrderKind::tactical_superweapon;
    if (std::holds_alternative<LandGarrisonPayload>(payload)) return OrderKind::land_garrison;
    if (std::holds_alternative<LandUnloadPayload>(payload)) return OrderKind::land_unload;
    if (std::holds_alternative<ScriptInvulnerablePayload>(payload)) return OrderKind::script_invulnerable;
    if (std::holds_alternative<ScriptDespawnPayload>(payload)) return OrderKind::script_despawn;
    if (std::holds_alternative<GiftPayload>(payload)) return OrderKind::gift;
    if (std::holds_alternative<BattleControlPayload>(payload)) return OrderKind::battle_control;
    if (std::holds_alternative<RetreatPayload>(payload) || std::holds_alternative<RetreatCinematicPayload>(payload)) return OrderKind::retreat;
    if (std::holds_alternative<BeaconPayload>(payload)) return OrderKind::beacon;
    if (std::holds_alternative<SpecialFirePayload>(payload)) return OrderKind::special_fire;
    if (std::holds_alternative<RepairHardpointPayload>(payload)) return OrderKind::repair_hardpoint;
    if (std::holds_alternative<AiReservationDebitPayload>(payload)) return OrderKind::ai_reservation_debit;
    if (std::holds_alternative<CampaignCreditReleasePayload>(payload)) return OrderKind::campaign_credit_release;
    if (std::holds_alternative<RevealAllPayload>(payload)) return OrderKind::reveal_all;
    if (std::holds_alternative<QuitPayload>(payload)) return OrderKind::intentional_quit;
    if (std::holds_alternative<AreaAbilityPayload>(payload)) return OrderKind::area_ability;
    if (std::holds_alternative<ManualTargetPayload>(payload)) return OrderKind::manual_target;
    return static_cast<OrderKind>(payload.index() + 1U);
}

enum class EventKind : std::uint8_t {
    order_accepted = 1,
    order_rejected = 2,
    hardpoint_destroyed = 3, // #72: `hardpoint` of `unit` reached zero health
    unit_destroyed = 4,      // #72: `unit`'s hull reached zero; it leaves the session
    victory = 5,             // #77: `player` won; `unit` is the star base whose destruction decided it
    // #447 (space-fighter-deaths SP-02, SP-08): killed craft `unit` of `player` spins away before
    // it explodes (right after its unit_destroyed event), and the spin ended: it explodes.
    spin_away_started = 6,
    spin_away_ended = 7,
    reinforcement_unloaded = 8, // WR-40: reserved by coordinator; arrival completed for `unit`
    station_replaced = 9, // WPR-52: coordinator-reserved; unit=old station, sequence=new station
    pad_captured = 10, // coordinator-reserved: includes transitions to neutral
    pad_construction_started = 11,
    pad_construction_completed = 12,
    pad_structure_sold = 13, // coordinator-reserved: WBP-32, unit=sold child, sequence=parent
    player_quit = 14, // coordinator-reserved: intentional departure, WBF-48
    manual_target_timeout = 15, // WAD-39: feedback belongs to the requesting player only
    ability_cancelled = 16, // coordinator-reserved, WHZ-23; sequence carries AbilityKind
    ability_ready = 17, // coordinator-reserved, WHZ-24; sequence carries AbilityKind
    special_weapon_fired = 18, // WSW-22/33: every wrapper pulse; sequence carries source index.
    special_weapon_ready = 19, // WSW-33: source recharge completed while enabled; sequence carries index.
    controller_changed = 20, // ADR-020: one event per transferred live root/member, ordered by ID.
    // TSW-05/06: `unit` is the tactical super weapon object, `player` its owner. tsw_approach_started opens the
    // approach, tsw_speech_threshold carries the crossed ETA threshold in whole seconds (60, 30, 15, 5) in `sequence`,
    // tsw_in_range is the arrival at the attack distance. Presentation events: none of them feeds the state hash.
    tsw_approach_started = 21,
    tsw_speech_threshold = 22,
    tsw_in_range = 23,
    // TSW-08..TSW-11 (the fire sequence): `unit` is the super weapon object, `player` its owner; for the target events
    // `sequence` is the 1-based target index (the setup's target list), so the presentation names the prop and its
    // replacement list. tsw_power_up_started opens the sequence (voice, sound, music, cinematic), tsw_attack_started the
    // attack clip, tsw_target_destroyed the destruction (explosion, jitter, prop removal), planet_obliterated the planet
    // commit the galactic side consumes (once per battle), tsw_debris_created the replacement props, tsw_sequence_complete
    // the end of the post wait. Presentation and hand-off events: none of them feeds the state hash.
    tsw_power_up_started = 24,
    tsw_attack_started = 25,
    tsw_target_destroyed = 26,
    planet_obliterated = 27,
    tsw_debris_created = 28,
    tsw_sequence_complete = 29,
    // Lua Change_Owner: `unit` changed owner; player is the new owner and sequence the previous one (like pad_captured, but
    // without the pad capture sounds). The viewer drops the previous owner's selection and control groups on it.
    object_owner_changed = 30,
};

enum class RejectReason : std::uint8_t {
    none = 0,
    unit_not_live = 1,
    unit_not_owned = 2,
    target_not_live = 3,
    target_not_hostile = 4,
    not_damageable = 5,    // #72: the unit's type has no durability profile
    hardpoint_invalid = 6, // #72: no destroyable hardpoint with that index
    target_is_unit = 7,    // #452: a guard or attack-move naming the ordered unit itself
    ability_unavailable = 8, // #76: the unit lacks the ability, or it is not ready or already in that state
    // #530 (docs/behaviour/space-purchasing.md):
    cannot_produce = 9,        // PU-11: not in the station's list, the station not live or allied, or not available
    queue_full = 10,           // PU-14
    insufficient_credits = 11, // PU-15
    no_queue_entry = 12,       // PU-17: no such entry to cancel
    not_in_pool = 13,          // PU-33: no pooled unit of that type
    no_population_room = 14,   // PU-21, PU-33
    invalid_position = 15,     // PU-31, PU-32
    no_economy = 16,           // the session has no economy rules, or the issuer none
    arriving = 17,             // PU-39: the unit is still arriving from hyperspace
    battle_decided = 18,       // WR-19: pending victory forbids reinforcement
    target_hidden = 19,        // WST-11: a stealth-invisible enemy, for a type that cannot target stealth (coordinator-reserved)
    beacon_unavailable = 20, // coordinator-reserved: HCB-12, no agreed faction beacon rule.
    unit_not_controlled = 21, // coordinator-reserved: #4590 P2, mobile unit not controlled by the issuing seat
    gift_recipient_invalid = 22, // ADR-020: recipient must be another active human on the same side.
};

// One event per listed unit of an executed command, in command order then unit order, each
// followed by the destruction events it caused; then the destruction events of the tick's
// durability service in ascending unit ID, then the spin-away events (#447: the spins that ended,
// in ascending unit ID, then those started, in destruction order), then the tick's victory event
// (#77). tick is the frame that produced it; it is published with completed tick + 1. Destruction,
// spin-away and victory events carry sequence zero and no order; a destruction or spin-away event
// carries the unit's owner, a victory the winner.
// WNO-23: pad_captured carries the new owner in player and the previous owner in sequence.
// WBP-18: pad_construction_completed carries the finished child in unit and the
// replaced under-construction child in sequence, preserving its authored completion cue.
struct Event {
    std::uint64_t tick{};
    EventKind kind{EventKind::order_accepted};
    PlayerId player{};
    std::uint64_t sequence{};
    EntityId unit{};
    OrderKind order{OrderKind::none};
    RejectReason reason{RejectReason::none};
    std::uint32_t hardpoint{}; // hardpoint_destroyed only; encoded as one byte below 255 (see append_event)
    std::uint32_t special_pulse{}; // special_weapon_fired: 0 initial, 1..N continuation.
    SeatId previous_controller{}, controller{}; // controller_changed only; faction owner stays in player.
    friend constexpr bool operator==(const Event&, const Event&) noexcept = default;
};

[[nodiscard]] std::string_view to_string(OrderKind kind) noexcept;
[[nodiscard]] std::string_view to_string(EventKind kind) noexcept;
[[nodiscard]] std::string_view to_string(RejectReason reason) noexcept;
[[nodiscard]] std::string_view to_string(AbilityKind kind) noexcept;
[[nodiscard]] std::string_view to_string(AbilityAction action) noexcept;

namespace diagnostic_codes {
inline constexpr std::string_view malformed = "EAWR-SIM-0301";
inline constexpr std::string_view version = "EAWR-SIM-0302";
inline constexpr std::string_view resource_limit = "EAWR-SIM-0303";
inline constexpr std::string_view order = "EAWR-SIM-0304";
inline constexpr std::string_view invalid_setup = "EAWR-SIM-0305";
inline constexpr std::string_view invalid_issuer = "EAWR-SIM-0306";
inline constexpr std::string_view late_command = "EAWR-SIM-0307";
inline constexpr std::string_view invalid_command = "EAWR-SIM-0308";
inline constexpr std::string_view command_rejected = "EAWR-SIM-0309";
inline constexpr std::string_view worker_failure = "EAWR-SIM-0310";
inline constexpr std::string_view land_provider_unavailable = "EAWR-SIM-0311";
inline constexpr std::string_view invalid_land_terrain = "EAWR-SIM-0312";
inline constexpr std::string_view invalid_land_path = "EAWR-SIM-0313";
// A campaign setup record of payload version 5 or 6, written before the location flag existed (not read; see
// docs/replay-format.md). No other failure uses this code.
inline constexpr std::string_view older_campaign_setup = "EAWR-SIM-0314";
} // namespace diagnostic_codes

} // namespace eawr::sim::tactical
