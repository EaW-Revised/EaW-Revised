#pragma once

#include "eawr/net/galactic_setup.hpp"
#include "eawr/net/lobby_session.hpp"
#include "eawr/sim/galactic/mp_lobby_catalog.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// GCMP-07 (#4363, M-GC1): the lobby's Galactic Conquest mode. The lobby core is mode-agnostic (LobbyGame is a
// map string and opaque options); this layer gives those bytes a second meaning. The host owns the mode: a
// room's options either start with the skirmish text v1 (a digit, unchanged since the LAN wave) or with the tag
// below, so every peer reads the same mode from the same revision and a skirmish room's bytes never move.
//
// A GC room names the campaign in LobbyGame::map and the agreed choices in the options. A seat's selection means:
// `slot` = the faction row it plays (the row's player ID), `faction` = that row's authored name, `team` = the
// human side (1 or 2, the row's rank among the host's human rows), `colour` unused (zero). Several seats may share
// one row (up to eight humans on one side against AI); the host plans at most two human rows (a third is refused), and
// a planned row nobody takes at Start is an AI faction, so both players on one row is a co-op game.
namespace eawr::net {

inline constexpr std::string_view galactic_lobby_options_tag = "gc";
inline constexpr unsigned galactic_lobby_options_version = 1;
// Stock MP_Default_Allow_Auto_Resolve (WAR-01): 0 always, 1 never, 2 ask, 3 overwhelming odds.
inline constexpr std::uint32_t galactic_lobby_default_auto_resolve = 2;
inline constexpr std::uint32_t galactic_lobby_max_auto_resolve = 3;
// Owner decision D5 (#4358) is open: what a human faction becomes when its side empties. Plan recommendation: AI.
inline constexpr sim::galactic::FactionControl galactic_lobby_default_dropout = sim::galactic::FactionControl::ai;
// Lobby rule: at most two human sides (WGS), at most eight humans in total (galactic_max_human_seats).
inline constexpr std::size_t galactic_lobby_max_human_sides = 2;

enum class LobbyGameMode : std::uint8_t { skirmish, galactic };
// Which text the room's options carry. An empty or unrecognised option blob is a skirmish room (its own codec refuses it).
[[nodiscard]] LobbyGameMode lobby_game_mode(const LobbyGame& game) noexcept;

// One faction row of the room: the campaign's faction, whether humans may hold it and what it becomes when its
// last human leaves (WGS-P04).
struct GalacticLobbyRow {
    sim::galactic::PlayerId player{};
    sim::galactic::FactionId faction_id{};
    std::uint32_t flags{};
    std::string faction;
    bool human{}; // a human side holds this faction
    sim::galactic::FactionControl dropout{galactic_lobby_default_dropout};
    friend bool operator==(const GalacticLobbyRow&, const GalacticLobbyRow&) = default;
};

struct GalacticLobbyOptions {
    std::string campaign;       // LobbyGame::map
    std::string difficulty;
    std::uint64_t input_delay{3};
    std::uint64_t seed{};       // the session seed every peer derives the galaxy and the AI draws from
    // WGS-13/14 custom settings; without `custom` every value stays zero and heroes stay on (the roster rule).
    bool custom{};
    std::int64_t credits{}, starting_tech{}, maximum_tech{};
    bool allow_heroes{true};
    // MP_Default_Allow_Auto_Resolve seed (WAR-01). The setup offer does not carry it yet: it is agreed here and
    // applied by the arbitration slice (GCMP-06).
    std::uint32_t auto_resolve{galactic_lobby_default_auto_resolve};
    std::vector<GalacticLobbyRow> rows;
    friend bool operator==(const GalacticLobbyOptions&, const GalacticLobbyOptions&) = default;
};

// Canonical text, same style as the skirmish v1 codec: `gc1 <delay> <seed> <auto resolve> <custom> <credits>
// <start tech> <max tech> <heroes> "<difficulty>" <row count>` then per row `<player> <faction id> <flags>
// "<faction>" <human> <dropout>`. Decode accepts only bytes that re-encode to themselves; an unknown version is
// refused by name.
[[nodiscard]] core::Result<LobbyGame> encode_galactic_lobby_game(const GalacticLobbyOptions& options);
[[nodiscard]] core::Result<GalacticLobbyOptions> decode_galactic_lobby_game(const LobbyGame& game);

// The one place a host draws a session seed: random, and 32-bit because the AI perception seed is (the world-generation
// seed rule). Every host path (lobby screen, acceptance run) uses it, so none can hand out a seed the AI setup refuses.
[[nodiscard]] std::uint64_t draw_galactic_session_seed();

// The options a host starts a room with: the campaign's factions as rows (the first two playable ones human
// when `two_sides`, else the first), stock custom settings off. Fails when the campaign has no playable faction.
[[nodiscard]] core::Result<GalacticLobbyOptions> default_galactic_lobby_options(
    const sim::galactic::MpLobbyCampaign& campaign, std::string_view difficulty, std::uint64_t seed,
    std::uint64_t input_delay = 3, bool two_sides = true);

// The human side (1 or 2) a faction row forms: the row's rank among the host's human rows. Nothing for an AI row
// or a player ID the room does not list. A seat's `team` is this value; a seat's `faction` is the row's name.
[[nodiscard]] std::optional<sim::galactic::HumanSideId> galactic_lobby_side(const GalacticLobbyOptions& options,
    sim::galactic::PlayerId player) noexcept;

// Staging validation. Run on the lobby's validate hook for a GC room: refuses a campaign that is not in the
// local multiplayer list, rows that differ from local content, a human row on an unplayable or neutral faction,
// a third human side, seats outside the planned rows, two sides on one faction and, when `starting`, everything
// the setup offer would refuse (galactic_roster_draft + validate_mp_roster: at least two humans, at most eight,
// a lead per side). Editable staging may be incomplete.
[[nodiscard]] core::Result<void> validate_galactic_lobby(const LobbySnapshot& snapshot, bool starting,
    std::span<const sim::galactic::MpLobbyCampaign> catalog);

// The roster the agreed choices describe: every campaign faction as a player, a human seat per lobby seat on its
// row and side, an AI seat per remaining non-neutral faction, the lowest seat of each side as its lead. The host
// seals it (AI configurations and the tick-zero claim) with campaign::seal_mp_roster before it becomes an offer.
[[nodiscard]] core::Result<sim::galactic::MpRoster> galactic_roster_draft(const LobbySnapshot& snapshot,
    std::span<const sim::galactic::MpLobbyCampaign> catalog);

// The offer a host publishes at Start: the sealed roster, the host's own locally derived content identity, the
// lobby's dense setup seats, the room's seed and delay. The result passes validate_galactic_offer, so a lobby
// seat set that does not match the roster is refused here, before any peer sees the offer.
[[nodiscard]] core::Result<SetupOffer> make_galactic_lobby_offer(const GalacticLobbyOptions& options,
    const sim::galactic::MpRoster& sealed, const sim::MatchAgreement& identity, const std::vector<SetupSeat>& seats);

} // namespace eawr::net
