#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/match_identity.hpp"
#include "eawr/sim/tactical/ai_level.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/object_turn.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::sim::tactical {

inline constexpr std::uint16_t replay_format_version = 2;
// Version 3 (#271) is version 2 plus a squadron table. A replay without squadrons is written
// as version 2, so every replay has exactly one encoding; the reader accepts both.
inline constexpr std::uint16_t replay_format_version_squadrons = 3;
// Coordinator-reserved versions: tagged header records extend v2/v3 without changing them.
inline constexpr std::uint16_t replay_format_version_extensions = 4;
inline constexpr std::uint16_t replay_format_version_squadron_extensions = 5;
// Native metadata uses wide header/body lengths; ordinary replays retain formats 2..5.
inline constexpr std::uint16_t replay_format_version_native = 6;
inline constexpr std::uint16_t replay_format_version_squadron_native = 7;
inline constexpr std::size_t replay_header_size = 104;
inline constexpr std::size_t replay_policy_header_size = 116;
inline constexpr std::size_t replay_object_turn_record_size = 64; // #3644 OBJT record, after the last command
inline constexpr std::uint16_t replay_extension_match_policy = 1;
inline constexpr std::uint16_t replay_extension_skirmish_setup = 2; // coordinator-reserved SKSU
inline constexpr std::uint16_t replay_extension_garrison_disabled = 3; // coordinator-reserved GSPN
inline constexpr std::uint16_t replay_extension_queue_identities = 4; // coordinator-reserved QIDS
inline constexpr std::uint16_t replay_extension_multiplayer_departure = 8; // coordinator-reserved MPDP
inline constexpr std::uint16_t replay_extension_free_garrison = 5; // coordinator-reserved GARR
inline constexpr std::uint16_t replay_extension_first_free_id = 6; // coordinator-reserved, GC shared allocator
inline constexpr std::uint16_t replay_extension_campaign = 10; // WGT-26..29
inline constexpr std::uint16_t replay_extension_carriers = 11; // WGT-21/22
inline constexpr std::uint16_t replay_extension_cinematic_seats = 12; // coordinator-reserved, FC-R02
inline constexpr std::uint16_t replay_extension_match_identity = 7; // coordinator-reserved MTID
inline constexpr std::uint16_t replay_extension_beacon_rules = 9; // HCB-12f, coordinator-reserved
inline constexpr std::uint16_t replay_extension_attract_pool = 13; // MAT-15 ATRC, coordinator-reserved
inline constexpr std::uint16_t replay_extension_native_scene = 15; // optional native campaign content selection
inline constexpr std::uint16_t replay_extension_object_turn = 16; // #3644: object-turn uses follow the commands
inline constexpr std::uint16_t replay_extension_battle_domain = 17; // BF-1: absent is space
inline constexpr std::size_t replay_max_bytes = 256U * 1024U * 1024U;

// Recorded lobby settings are load-time metadata, never part of canonical simulation state.
struct ReplayMatchOptions final {
    bool allow_heroes{true}, allow_superweapons{true}, free_starting_units{true}, pre_built_base{true};
    bool allow_random_events{};
    math::Fixed credits{};
    std::int32_t start_tech{}, max_tech{}, game_timer{}, win_integer{}, auto_resolve{};
    math::Fixed win_float{};
    // The tactical condition keeps the existing second SKSU text field in either domain.
    std::string win_condition, tactical_win_condition;
    bool operator==(const ReplayMatchOptions&) const = default;
};

struct ReplayLobbySlot final {
    PlayerId player{};
    bool human{};
    std::optional<std::uint32_t> colour_index;
    std::vector<std::string> fleet;
    AiLevel difficulty{}; // WTA-47 (#735): recorded in the human word's bits 1-2, so a Normal slot's bytes are unchanged
    bool operator==(const ReplayLobbySlot&) const = default;
};

struct ReplaySkirmishSetup final {
    std::string map, map_sha256;
    ReplayMatchOptions match;
    std::uint32_t victory_condition{};
    std::vector<ReplayLobbySlot> slots;
    bool operator==(const ReplaySkirmishSetup&) const = default;
};

// Optional player-owned free starting force and its full-depletion replacement delay.
struct FreeGarrisonSetup {
    PlayerId player{};
    std::uint32_t delay_frames{};
    std::vector<TypeId> templates; // faction order, including repeated companies
    std::vector<EntityId> registered; // actual free craft/objects, excluding squadron containers
    friend bool operator==(const FreeGarrisonSetup&, const FreeGarrisonSetup&) = default;
};

// The tick-zero session: players and units in strictly increasing ID order. Setup units
// carry no order. Squadrons are in strictly increasing container ID; each container and
// craft is a setup unit of one owner, and no unit belongs to two squadrons.
struct CampaignReserve {
    EntityId id{};
    TypeId type{};
    PlayerId owner{};
    bool restricted{}; // WR-09: campaign location restriction
    TypeId purchase_type{}; // original tactical type; zero = unchanged
    std::vector<CarriedObject> contained;
    friend bool operator==(const CampaignReserve&, const CampaignReserve&) = default;
};
struct CampaignUpgrade {
    TypeId type{};
    EntityId holder{};
    PlayerId owner{};
    friend bool operator==(const CampaignUpgrade&, const CampaignUpgrade&) = default;
};
struct CampaignBattleSetup {
    math::Fixed corruption_price_multiplier{math::Fixed::from_raw(math::Fixed::scale)};
    std::vector<EconomyPlayer> players;
    std::vector<CampaignReserve> reserves; // ascending persistent ID
    std::vector<CampaignUpgrade> upgrades; // ascending (owner, holder, type)
    std::vector<EntityId> population_registered; // WGT-19: non-garrison initial force roots
    std::vector<EntityId> victory_leaders; // ascending original identities, including riders
    EntityId victory_station{}; // optional authored station-destruction condition
    std::vector<EntityId> reserve_order{}; // WGT-07: complete source order; empty uses ascending reserve ID
    friend bool operator==(const CampaignBattleSetup&, const CampaignBattleSetup&) = default;
};
struct CinematicSeat {
    PlayerId player{};
    bool cinematics_enabled{true}; // immutable battle-creation setting, never a playback report
    friend bool operator==(const CinematicSeat&, const CinematicSeat&) = default;
};
struct MultiplayerDeparturePolicy {
    std::vector<PlayerId> humans; // ascending active human faction IDs, including shared seats once
    friend bool operator==(const MultiplayerDeparturePolicy&, const MultiplayerDeparturePolicy&) = default;
};

// MAT-15 (docs/behaviour/main-menu-attract.md): the demo battle's pool return. Present only in an
// attract session; a session without it returns nothing and keeps every legacy replay byte.
struct AttractPoolReturn {
    // The types a destroyed object returns to its owner's reinforcement pool: the loaded content's
    // types with Build_Tab_Space_Units, ascending and unique. The setup carries them, so the session
    // never reads content tags and a replay binds the exact set.
    std::vector<TypeId> types;
    friend bool operator==(const AttractPoolReturn&, const AttractPoolReturn&) = default;
};

// Native campaign content selection. SKSU binds the map, players and victory;
// these loader roots reproduce its authored profiles without a skirmish lobby.
struct ReplayNativeScene {
    bool multiplayer{};
    std::vector<std::string> source_types{}, source_obstacles{}, battle_types{};
    friend bool operator==(const ReplayNativeScene&, const ReplayNativeScene&) = default;
};

struct TacticalSetup {
    std::uint64_t seed{};
    std::array<std::uint8_t, 32> content_identity{};
    std::vector<Player> players;
    std::vector<UnitState> units;
    std::vector<Squadron> squadrons;
    // Absent is the legacy all-enabled policy and retains v2/v3 bytes.
    std::optional<SkirmishMatchPolicy> match_policy;
    // Optional SKSU is attached by live recording callers; pinned builders retain old bytes.
    std::optional<ReplaySkirmishSetup> skirmish{};
    bool queue_identities{}; // PU-17: opt in to canonical queue identities; legacy recordings retain their hashes.
    std::optional<MultiplayerDeparturePolicy> departure_policy; // MP-36..38, replayable opt-in
    std::vector<FreeGarrisonSetup> free_garrisons; // FL-14: optional, strictly increasing player IDs
    // Opt-in shared allocator boundary. Absent retains every legacy replay byte and hash.
    std::optional<EntityId> first_free_entity_id;
    std::optional<CampaignBattleSetup> campaign;
    std::vector<CinematicSeat> cinematic_seats; // ascending tactical seat perspectives (player IDs)
    std::optional<sim::MatchIdentity> match_identity{};
    struct BeaconRule {
        PlayerId player{};
        TypeId type{};
        std::uint32_t lifetime_frames{};
        friend bool operator==(const BeaconRule&, const BeaconRule&) = default;
    };
    std::vector<BeaconRule> beacon_rules; // HCB-12: resolved faction type/lifetime, ascending issuer.
    std::optional<ReplayNativeScene> native_scene{};
    std::optional<AttractPoolReturn> attract_pool; // MAT-15/16: the demo attract's pool return, opt-in
    BattleDomain domain{BattleDomain::space};
    friend bool operator==(const TacticalSetup&, const TacticalSetup&) = default;
};

// Replay v2 (docs/replay-format.md; v3 with squadrons): a setup plus every recorded player
// command in strictly increasing (tick, player, sequence) order, each tick below
// final_tick_count.
struct TacticalReplay {
    TacticalSetup setup;
    std::uint64_t final_tick_count{};
    std::vector<PlayerCommand> commands;
    // #3644: every ability use the object-turn provider issued, applied or refused, in strictly increasing
    // (tick, unit, sequence) order, each tick below final_tick_count. Written as the OBJT header extension followed by the
    // fixed-size records after the last command; empty keeps every other replay byte.
    std::vector<ObjectTurnRecord> object_turns{};
    friend bool operator==(const TacticalReplay&, const TacticalReplay&) = default;
};

// Reads the shared EAWRPLY magic and format version without validating the rest, so a
// caller can route v1 and v2 files. Returns no value for short input or another magic.
[[nodiscard]] std::optional<std::uint16_t> peek_replay_format_version(
    std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] core::Result<void> validate_setup(const TacticalSetup& setup);
// Fill omitted battle-creation settings from trusted roles: AI false, human true.
// Call after validate_setup; explicit profile settings retain their value.
void resolve_cinematic_seats(TacticalSetup&, std::span<const EconomyPlayer>);
[[nodiscard]] core::Result<void> validate_replay(const TacticalReplay& replay);
[[nodiscard]] core::Result<TacticalReplay> parse_replay(
    std::span<const std::uint8_t> bytes,
    std::string_view logical_path = {});
[[nodiscard]] core::Result<std::vector<std::uint8_t>> write_replay(const TacticalReplay& replay);

} // namespace eawr::sim::tactical
