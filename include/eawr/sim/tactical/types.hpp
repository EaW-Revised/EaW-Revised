#pragma once

#include "eawr/sim/commands.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace eawr::sim::tactical {

// Logical frame rate pinned by docs/behaviour/tactical-tick-rate.md: one tick is 1/30 s.
inline constexpr std::uint32_t logical_frames_per_second = 30;
inline constexpr std::uint32_t tick_numerator = 1;
inline constexpr std::uint32_t tick_denominator = logical_frames_per_second;
inline constexpr std::uint32_t tactical_rules_version = 1;
inline constexpr std::uint32_t tactical_state_encoding_version = 1;
inline constexpr std::uint32_t tactical_snapshot_encoding_version = 3;

inline constexpr std::uint32_t max_players = 64;
inline constexpr std::uint32_t max_units_per_command = 1024;
inline constexpr std::uint64_t max_units = 1'000'000;
inline constexpr std::uint64_t max_commands = 1'000'000;
inline constexpr std::uint64_t max_ticks = 1'000'000;

using PlayerId = std::uint32_t;
using TeamId = std::uint32_t;
using FactionId = std::uint64_t;
using TypeId = std::uint64_t;

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
struct AiReservationDebitPayload {
    math::Fixed amount{};
    friend constexpr bool operator==(const AiReservationDebitPayload&, const AiReservationDebitPayload&) = default;
};

struct QuitPayload {
    friend constexpr bool operator==(const QuitPayload&, const QuitPayload&) noexcept = default;
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

using CommandPayload = std::variant<StopPayload, MovePayload, AttackPayload, DamagePayload, FacePayload,
    AttackMovePayload, GuardPayload, AbilityPayload, BuyPayload, CancelPayload, ReinforcePayload, PadBuildPayload,
    CreditGrantPayload, QuitPayload, PadSellPayload, AreaAbilityPayload, ManualTargetPayload, RevealAllPayload,
    RepairHardpointPayload, AiReservationDebitPayload>;

// Whether a command acts on the issuer's economy (#530) rather than on units.
[[nodiscard]] constexpr bool economy_command(const CommandPayload& payload) noexcept {
    return std::holds_alternative<BuyPayload>(payload) || std::holds_alternative<CancelPayload>(payload)
        || std::holds_alternative<ReinforcePayload>(payload) || std::holds_alternative<PadBuildPayload>(payload)
        || std::holds_alternative<CreditGrantPayload>(payload) || std::holds_alternative<PadSellPayload>(payload)
        || std::holds_alternative<AiReservationDebitPayload>(payload);
}

// key.player_id is the issuer. Units are nonzero and strictly increasing; a buy lists exactly its
// station, a cancel or reinforce none (#530), every other command at least one unit.
struct PlayerCommand {
    CommandKey key;
    std::vector<EntityId> units;
    CommandPayload payload;
    friend bool operator==(const PlayerCommand&, const PlayerCommand&) = default;
};

[[nodiscard]] constexpr OrderKind order_kind(const CommandPayload& payload) noexcept {
    if (std::holds_alternative<RepairHardpointPayload>(payload)) return OrderKind::repair_hardpoint;
    if (std::holds_alternative<AiReservationDebitPayload>(payload)) return OrderKind::ai_reservation_debit;
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
};

// One event per listed unit of an executed command, in command order then unit order, each
// followed by the destruction events it caused; then the destruction events of the tick's
// durability service in ascending unit ID, then the spin-away events (#447: the spins that ended,
// in ascending unit ID, then those started, in destruction order), then the tick's victory event
// (#77). tick is the frame that produced it; it is published with completed tick + 1. Destruction,
// spin-away and victory events carry sequence zero and no order; a destruction or spin-away event
// carries the unit's owner, a victory the winner.
// WNO-23: pad_captured carries the new owner in player and the previous owner in sequence.
struct Event {
    std::uint64_t tick{};
    EventKind kind{EventKind::order_accepted};
    PlayerId player{};
    std::uint64_t sequence{};
    EntityId unit{};
    OrderKind order{OrderKind::none};
    RejectReason reason{RejectReason::none};
    std::uint8_t hardpoint{}; // hardpoint_destroyed only
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
} // namespace diagnostic_codes

} // namespace eawr::sim::tactical
