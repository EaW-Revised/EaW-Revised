#pragma once

#include "eawr/sim/commands.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <cstdint>
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
    friend constexpr bool operator==(const Order&, const Order&) noexcept = default;
};

struct UnitState {
    EntityId entity_id{};
    TypeId type_id{};
    PlayerId owner{};
    math::Vec3 position{};
    math::Quat rotation{math::identity_quat()};
    Order order{};
    friend constexpr bool operator==(const UnitState&, const UnitState&) noexcept = default;
};

struct StopPayload {
    friend constexpr bool operator==(const StopPayload&, const StopPayload&) noexcept = default;
};

struct MovePayload {
    math::Vec3 destination{};
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

// The modelled abilities. HUNT, which the fleet also authors, is cut (AB-03); ION_CANNON_SHOT is
// the Y-wing squadron's targeted attack (#561, AB-60 to AB-69).
enum class AbilityKind : std::uint8_t {
    none = 0,
    defend = 1,
    turbo = 2,
    power_to_weapons = 3,
    spoiler_lock = 4,
    ion_cannon_shot = 5,
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
    friend constexpr bool operator==(const AbilityPayload&, const AbilityPayload&) noexcept = default;
};

// Buy (#530, PU-10 to PU-15): queue `type` at the command's one listed unit, the station.
struct BuyPayload {
    TypeId type{};
    friend constexpr bool operator==(const BuyPayload&, const BuyPayload&) noexcept = default;
};

// Cancel (#530, PU-17): entry `index` of the issuer's queue `queue` (0 units, 1 upgrades). The
// command lists no unit.
struct CancelPayload {
    std::uint32_t queue{};
    std::uint32_t index{};
    friend constexpr bool operator==(const CancelPayload&, const CancelPayload&) noexcept = default;
};

// Reinforce (#530, PU-30 to PU-34): bring the issuer's first pooled unit of `type` in at
// `position`. The command lists no unit.
struct ReinforcePayload {
    TypeId type{};
    math::Vec3 position{};
    friend constexpr bool operator==(const ReinforcePayload&, const ReinforcePayload&) noexcept = default;
};

using CommandPayload = std::variant<StopPayload, MovePayload, AttackPayload, DamagePayload, FacePayload,
    AttackMovePayload, GuardPayload, AbilityPayload, BuyPayload, CancelPayload, ReinforcePayload>;

// Whether a command acts on the issuer's economy (#530) rather than on units.
[[nodiscard]] constexpr bool economy_command(const CommandPayload& payload) noexcept {
    return std::holds_alternative<BuyPayload>(payload) || std::holds_alternative<CancelPayload>(payload)
        || std::holds_alternative<ReinforcePayload>(payload);
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
