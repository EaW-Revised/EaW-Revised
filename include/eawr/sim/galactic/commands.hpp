#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

// The command envelope of a game session (ADR-015, ADR-020; docs/galactic-architecture.md
// section 4). Every player, AI and script action is one typed GameCommand issued by a seat, in
// canonical (tick, seat, sequence) order; presentation state is never a command.
namespace eawr::sim::galactic {

enum class CommandLayer : std::uint8_t {
    session = 1,  // pause, speed, save, resign, battle choice, dialogs, seats, debug
    galactic = 2, // galaxy actions, executed by GalacticSession
    tactical = 3, // the battle's tactical opcodes, executed by the child battle
};

// Session-layer opcodes, coordinator-reserved as one block: 1..14 below, 15..61 kept for the
// wave-3 walks' additions, 62..63 the debug commands. Its own number space.
namespace session_opcodes {
inline constexpr std::uint16_t pause = 1;
inline constexpr std::uint16_t resume = 2;
inline constexpr std::uint16_t set_strategic_speed = 3;
inline constexpr std::uint16_t set_tactical_speed = 4;
inline constexpr std::uint16_t save_request = 5;
inline constexpr std::uint16_t resign = 6;
inline constexpr std::uint16_t battle_choice = 7;
inline constexpr std::uint16_t battle_vote = 8;
inline constexpr std::uint16_t dialog_acknowledge = 9;
inline constexpr std::uint16_t victory_continue = 10;
inline constexpr std::uint16_t seat_leave = 11;
inline constexpr std::uint16_t seat_rejoin = 12;
inline constexpr std::uint16_t post_load_fixup = 13; // coordinator-reserved
inline constexpr std::uint16_t battle_team_role = 14; // coordinator-reserved
inline constexpr std::uint16_t auto_resolve_withdraw = 15; // WGM-182, conflict owner
inline constexpr std::uint16_t first_reserved = 16;
inline constexpr std::uint16_t last_reserved = 61;
inline constexpr std::uint16_t debug_set_planet_owner = 62;
inline constexpr std::uint16_t debug_set_fleet_orbit = 63;
} // namespace session_opcodes

// Galactic-layer opcodes, coordinator-reserved as one block: additions below, remaining IDs kept for the
// wave-3 walks' additions. A number space separate from the tactical opcodes.
namespace galactic_opcodes {
inline constexpr std::uint16_t create_fleet = 1;        // WGM-08
inline constexpr std::uint16_t add_members = 2;         // WGM-09
inline constexpr std::uint16_t remove_members = 3;      // WGM-10/12
inline constexpr std::uint16_t merge_fleets = 4;        // WGM-11
inline constexpr std::uint16_t move_fleet = 5;          // WGM-23, WGM-84/85, WGM-37
inline constexpr std::uint16_t set_orbit_slot = 6;      // WGM-15
inline constexpr std::uint16_t production = 7;          // coordinator-reserved, WGEP-18..46
inline constexpr std::uint16_t observer_intelligence = 11; // coordinator-reserved, WGUI-01..24
inline constexpr std::uint16_t land_transports = 8;     // WGM-13, WGM-39/40
inline constexpr std::uint16_t buy = 9;                 // WGEP-18..30
// WGEP-45/56: index/type cancellation uses the production opcode, without separate aliases.
inline constexpr std::uint16_t black_market_prepare = 12; // WGEP-55/58, WGCH-37/38
inline constexpr std::uint16_t black_market_buy = 13;
inline constexpr std::uint16_t special_prepare = 14;    // WGCH-01..10, 20..42, 57..59
inline constexpr std::uint16_t special_execute = 15;
inline constexpr std::uint16_t special_cancel = 16;
inline constexpr std::uint16_t ability = 17;            // WGM-14/83, WGCH
inline constexpr std::uint16_t launch_from_surface = 18; // coordinator-reserved (#2880), WGM-13, WGUI-55
inline constexpr std::uint16_t sell_object = 19; // GHUD-S84: coordinator-reserved identity sale
inline constexpr std::uint16_t transfer_fleet_members = 20; // GHUD-S86
inline constexpr std::uint16_t recheck_orbit = 21; // WGM-154, script request at an existing orbit
inline constexpr std::uint16_t first_reserved = 22; // coordinator-held #2263 launch follow-up
inline constexpr std::uint16_t last_reserved = 127;
} // namespace galactic_opcodes

// The tactical retreat (WGM-64, WR-42..48): tactical replay opcode 28, CommandKind 23,
// coordinator-reserved; the tactical return and retreat work implements it.
inline constexpr std::uint8_t tactical_retreat_opcode = 28;

// Pause and resume the galaxy (WGM-05/06): a nested counter. Any active human seat.
struct PauseGalaxy {
    friend constexpr bool operator==(const PauseGalaxy&, const PauseGalaxy&) noexcept = default;
};
struct ResumeGalaxy {
    friend constexpr bool operator==(const ResumeGalaxy&, const ResumeGalaxy&) noexcept = default;
};
// WGS-P03: the trusted session service records departures/reconnects at an agreed tick.
// The envelope identifies the affected seat; no player may remove another seat.
struct SeatLeave { friend bool operator==(const SeatLeave&, const SeatLeave&) = default; };
struct SeatRejoin { friend bool operator==(const SeatRejoin&, const SeatRejoin&) = default; };
// WGS-P05: one lead decision for the whole side, never a per-seat subset.
struct AssignBattleTeamRole {
    EntityId conflict{};
    BattleTeamRole role{BattleTeamRole::pending};
    friend bool operator==(const AssignBattleTeamRole&, const AssignBattleTeamRole&) = default;
};

// WGS-33..35: quick/autosave admission is separate from the ordinary save-event route.
// A prebattle callback is issued before the conflict service changes its invasion context.
struct SaveRequest {
    std::int64_t slot{};
    bool quick{}, autosave{}, prebattle{};
    friend constexpr bool operator==(const SaveRequest&, const SaveRequest&) noexcept = default;
};
// Internal restore prologue, never admitted as a player command. Runtime checkpoints preserve
// pause exactly; player loads explicitly request WGS-43/61 normalization and WGS-63/64 context.
struct PostLoadFixup {
    std::uint32_t pause_depth{};
    bool resume{}, pending_battle{}, autosave{};
    friend constexpr bool operator==(const PostLoadFixup&, const PostLoadFixup&) noexcept = default;
};

// The strategic or tactical speed setting (WGS-04/41): a Q24 factor over 30 ticks a second. Pacing
// only until the speed capture (WGM-U02) shows a per-tick effect: it changes no rule and no state
// hash. Any active human seat. The factor is in (0, 16]; the original's settings are unverified.
inline constexpr std::int64_t max_speed_factor_raw = std::int64_t{16} << 24;
struct SetSpeed {
    bool tactical{};
    math::Fixed factor{};
    friend constexpr bool operator==(const SetSpeed&, const SetSpeed&) noexcept = default;
};

// Debug: sets a planet's owner directly. Any active seat; setup flag only.
struct DebugSetPlanetOwner {
    PlanetId planet{};
    PlayerId owner{};
    friend constexpr bool operator==(const DebugSetPlanetOwner&, const DebugSetPlanetOwner&) noexcept = default;
};

// Debug: moves a fleet to another planet's orbit at once. Authorized by the faction: the seat's
// player must own the fleet. Setup flag only.
struct DebugSetFleetOrbit {
    FleetId fleet{};
    PlanetId planet{};
    friend constexpr bool operator==(const DebugSetFleetOrbit&, const DebugSetFleetOrbit&) noexcept = default;
};

// A tactical command for one battle: the conflict it belongs to, the tactical opcode and the
// command's body bytes exactly as the tactical replay encodes them. A command for an ended or
// another battle is rejected, so a late click never reaches the next battle.
struct TacticalLayerCommand {
    EntityId conflict{};
    std::uint8_t opcode{};
    std::vector<std::uint8_t> body;
    friend bool operator==(const TacticalLayerCommand&, const TacticalLayerCommand&) = default;
};

// Existing identities, never spawn requests. Ordered member lists are not re-sorted.
struct CreateFleet {
    PlanetId planet{};
    std::vector<EntityId> members;
    std::uint8_t slot{};
    friend bool operator==(const CreateFleet&, const CreateFleet&) = default;
};
struct AddFleetMembers {
    FleetId fleet{};
    std::vector<EntityId> members;
    friend bool operator==(const AddFleetMembers&, const AddFleetMembers&) = default;
};
struct RemoveFleetMembers {
    FleetId fleet{};
    std::vector<EntityId> members;
    friend bool operator==(const RemoveFleetMembers&, const RemoveFleetMembers&) = default;
};
struct MergeFleets {
    FleetId source{}, target{};
    friend bool operator==(const MergeFleets&, const MergeFleets&) = default;
};
struct SetFleetOrbitSlot {
    FleetId fleet{};
    std::uint8_t slot{};
    friend bool operator==(const SetFleetOrbitSlot&, const SetFleetOrbitSlot&) = default;
};

// GHUD-S86: stable direct membership, including a type stack spread across
// several live fleets displayed together in one orbit card.
struct FleetMemberRef {
    FleetId fleet{};
    EntityId member{};
    friend bool operator==(const FleetMemberRef&, const FleetMemberRef&) = default;
};
struct TransferFleetMembers {
    std::vector<FleetMemberRef> members;
    FleetId target{}; // zero creates one fleet at the source planet in `slot`
    std::uint8_t slot{}; // canonically zero when target is nonzero
    friend bool operator==(const TransferFleetMembers&, const TransferFleetMembers&) = default;
};

enum class ProductionAction : std::uint8_t { buy = 0, cancel_index = 1, cancel_type = 2, stop = 3 };
struct SellObject {
    PlanetId planet{};
    EntityId object{};
    friend bool operator==(const SellObject&, const SellObject&) = default;
};
struct ProductionCommand {
    PlanetId planet{};
    TypeId type{};
    ProductionAction action{};
    ProductionDomain domain{};
    std::uint32_t index{};
    // WGEP-45: optional identity guard for a displayed indexed cancel. Absent
    // preserves the original replay's type/index command semantics.
    std::optional<EntityId> target_build_id{};
    friend bool operator==(const ProductionCommand&, const ProductionCommand&) = default;
};
// WGM-154: script-only planet/fleet dispatch; never supplies a lane meeting point.
struct RecheckOrbit {
    EntityId target{};
    friend bool operator==(const RecheckOrbit&, const RecheckOrbit&) = default;
};
struct MoveFleet {
    FleetId fleet{};
    std::vector<PlanetId> path; // WGM-88: accepted graph path, source first
    std::optional<std::uint8_t> destination_slot;
    friend bool operator==(const MoveFleet&, const MoveFleet&) = default;
};

struct LandFleet {
    FleetId fleet{};
    PlanetId planet{};
    friend bool operator==(const LandFleet&, const LandFleet&) = default;
};

// WGM-182: sending seat identifies the player; no presentation destination.
struct AutoResolveWithdraw {
    EntityId conflict{};
    friend bool operator==(const AutoResolveWithdraw&, const AutoResolveWithdraw&) = default;
};
struct BattleChoice {
    EntityId conflict{};
    bool tactical{};
    friend bool operator==(const BattleChoice&, const BattleChoice&) = default;
};

struct PrepareBlackMarket {
    EntityId buyer{};
    PlanetId planet{};
    friend bool operator==(const PrepareBlackMarket&, const PrepareBlackMarket&) = default;
};
struct BuyBlackMarket {
    EntityId buyer{};
    std::uint32_t index{};
    friend bool operator==(const BuyBlackMarket&, const BuyBlackMarket&) = default;
};
struct PrepareGalacticAbility {
    EntityId actor{};
    PlanetId planet{};
    AbilityKind kind{AbilityKind::black_market};
    friend bool operator==(const PrepareGalacticAbility&, const PrepareGalacticAbility&) = default;
};
struct ExecutePreparedAbility {
    PreparedAbility prepared;
    friend bool operator==(const ExecutePreparedAbility&, const ExecutePreparedAbility&) = default;
};
struct CancelGalacticAbility {
    EntityId actor{};
    std::uint64_t preparation{};
    friend bool operator==(const CancelGalacticAbility&, const CancelGalacticAbility&) = default;
};
// WGM-13/WGUI-55 (#2880): the seat's ordinary surface-to-orbit launch. It moves the issuer's landed
// transports, in the listed order, into one orbital fleet through the same admission as the native
// launch: `target` adds them to that orbit fleet, zero creates a fleet at their planet in `slot`.
// All-or-nothing; a refusal leaves every unit on the surface. That atomicity is our choice, not a sourced
// retail rule: WGUI-54 sends one callback per multiselected widget and WGA-227's AI launch is explicitly
// non-atomic, and retail's outcome for a refused member of a group is unverified. Never an invasion or a movement.
struct LaunchFromSurface {
    std::vector<EntityId> units;
    FleetId target{};
    std::uint8_t slot{}; // creation slot (0..2); canonically zero when a target fleet is named
    friend bool operator==(const LaunchFromSurface&, const LaunchFromSurface&) = default;
};
using CommandPayload =
    std::variant<PauseGalaxy, ResumeGalaxy, SetSpeed, DebugSetPlanetOwner, DebugSetFleetOrbit, TacticalLayerCommand,
        SaveRequest, PostLoadFixup, CreateFleet, AddFleetMembers, RemoveFleetMembers, MergeFleets, SetFleetOrbitSlot,
        ProductionCommand, SellObject, SeatLeave, SeatRejoin, AssignBattleTeamRole, RecheckOrbit, MoveFleet, LandFleet, BattleChoice, AutoResolveWithdraw, ObserverCommand,
        PrepareBlackMarket, BuyBlackMarket, PrepareGalacticAbility, ExecutePreparedAbility, CancelGalacticAbility,
        LaunchFromSurface, TransferFleetMembers>;

struct CommandOrderKey {
    std::uint64_t tick{};
    SeatId seat{};
    std::uint64_t sequence{};
    friend constexpr auto operator<=>(const CommandOrderKey&, const CommandOrderKey&) noexcept = default;
};

struct GameCommand {
    CommandOrderKey key;
    CommandPayload payload;
    friend bool operator==(const GameCommand&, const GameCommand&) = default;
};

[[nodiscard]] CommandLayer layer_of(const CommandPayload& payload) noexcept;
[[nodiscard]] std::uint16_t opcode_of(const CommandPayload& payload) noexcept;

// The payload's canonical bytes (docs/galactic-replay-format.md), and the payload of a layer,
// opcode and bytes: a reserved number without an implementation, an unknown number or a body of
// the wrong length gives nothing.
[[nodiscard]] std::vector<std::uint8_t> encode_payload(const CommandPayload& payload);
[[nodiscard]] std::optional<CommandPayload> decode_payload(
    CommandLayer layer, std::uint16_t opcode, const std::vector<std::uint8_t>& bytes);

} // namespace eawr::sim::galactic
