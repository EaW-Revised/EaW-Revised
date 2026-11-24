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
// human side (1..galactic_lobby_max_human_sides, the row's rank among the host's human rows), `colour` unused (zero).
// Several seats may share one row (a co-op side; eight humans in all, galactic_max_human_seats). The room shows a row
// for every playable faction of the campaign (D8, #8071): a row nobody holds is an AI faction, and a player takes a row
// (claims it) to make it a human side; the number of human sides is bounded by the campaign's playable rows and by
// galactic_lobby_max_human_sides. A planned row nobody takes at Start is an AI faction too.
namespace eawr::net {

inline constexpr std::string_view galactic_lobby_options_tag = "gc";
// The line a room shows while a campaign is being prepared (#7198): the bound it names is preparation_timeout_minutes, the same
// value the deadline uses. The elapsed time is shown in whole seconds. The lines stay short: the lobby shows the counter in place
// and puts it in chat only when the line is new (presentation::ui::StatusChatThrottle).
[[nodiscard]] std::string galactic_prepare_wait_text(bool host_waits, std::uint64_t elapsed_seconds);
// Added once, to the host's first preparation line in chat: the room refuses changes until preparation ends.
inline constexpr std::string_view galactic_prepare_locked_note = "Room locked while preparing.";
inline constexpr unsigned galactic_lobby_options_version = 2;
// Stock MP_Default_Allow_Auto_Resolve (WAR-01): 0 always, 1 never, 2 ask, 3 overwhelming odds.
inline constexpr std::uint32_t galactic_lobby_default_auto_resolve = 2;
inline constexpr std::uint32_t galactic_lobby_max_auto_resolve = 3;
// Battle-vote timeout, a host setting (#6156, owner D9): seconds a battle vote waits between different human sides. A room's
// default is 30 whatever pending-battle timeout constant the data authors (owner decision D11, 2026-11-23, a deliberate
// deviation from retail's constant 15). The host may choose 5 to 120. Players on one side never vote against a clock
// (GameSession::untimed_votes).
inline constexpr std::uint32_t galactic_lobby_min_vote_timeout = 5;
inline constexpr std::uint32_t galactic_lobby_max_vote_timeout = 120;
inline constexpr std::uint32_t galactic_lobby_default_vote_timeout = 30;
[[nodiscard]] constexpr std::uint32_t galactic_lobby_clamp_vote_timeout(const std::int64_t seconds) noexcept {
    return seconds < galactic_lobby_min_vote_timeout ? galactic_lobby_min_vote_timeout
        : seconds > galactic_lobby_max_vote_timeout ? galactic_lobby_max_vote_timeout : static_cast<std::uint32_t>(seconds);
}
// Owner decision D5 (#4358) is open: what a human faction becomes when its side empties. Plan recommendation: AI.
inline constexpr sim::galactic::FactionControl galactic_lobby_default_dropout = sim::galactic::FactionControl::ai;
// Lobby rule (D1, #8071): at most galactic_max_human_sides human sides, at most galactic_max_human_seats humans in total.
// The real bound of a room is the campaign's playable rows; this is the room-independent ceiling and never a count of its own.
inline constexpr std::size_t galactic_lobby_max_human_sides = sim::galactic::galactic_max_human_sides;

// Whether the staging dialog draws a row for this faction. A row no player can take whose AI seat runs no controller (Neutral,
// or an ambient faction the entry assigns nothing) is not drawn: it has neither a player nor an AI to name.
[[nodiscard]] bool galactic_lobby_row_drawn(const sim::galactic::MpLobbyFaction& faction) noexcept;

// The most human rows a room of `campaign` may plan when the staging dialog has `dialog_rows` player rows (0: no dialog, no
// limit but the ceiling). Every planned row is drawn: when the campaign has more drawn factions than the dialog has rows, the
// last row is kept for the "+N AI factions" line, so one row fewer can be a player. Never above galactic_lobby_max_human_sides.
// A claim that would plan more rows is refused with a message, so no seat is ever on a row the dialog does not draw.
[[nodiscard]] std::size_t galactic_lobby_human_row_limit(const sim::galactic::MpLobbyCampaign& campaign, std::size_t dialog_rows) noexcept;

// The most humans a campaign room is offered for (the host's "Human players" dropdown): a multiplayer campaign's playable factions
// (every player takes a faction row), a co-op campaign (one side, co-op seats) the seat ceiling; at least two, at most `seat_limit`
// (the transport's seats) and the human seat ceiling (eight), so a room is never opened for a number the campaign cannot start with.
[[nodiscard]] std::uint32_t galactic_lobby_max_humans(const sim::galactic::MpLobbyCampaign& campaign, std::uint32_t seat_limit) noexcept;

// The number of humans a new campaign room opens for when the host has not chosen one: two. A room waits for every seat it opens
// with (its mesh is fixed), so a larger default left two players unable to start a multi-faction campaign; this holds until rooms can
// start with fewer humans than seats (#8130). The host's "Human players" choice raises it up to galactic_lobby_max_humans.
[[nodiscard]] std::uint32_t galactic_lobby_default_humans(const sim::galactic::MpLobbyCampaign& campaign, std::uint32_t seat_limit) noexcept;

// What the host reads while a campaign room that opened for `capacity` players has only `seated` of them: Start stays off until the
// room is full (its mesh is fixed; #8130), so the text names the missing players and how to start sooner. Empty when the room is full.
[[nodiscard]] std::string galactic_room_waiting_text(std::size_t seated, std::uint32_t capacity);

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
    // Seconds a battle vote waits between different human sides (#6156). The host sets it, every peer arms the same tick
    // count from it. A new room starts at the campaign's own constant when it states one (clamped to the host range), else
    // the retail 15. A resumed save shows the value it recorded, which can lie outside the host range.
    std::uint32_t vote_timeout{galactic_lobby_default_vote_timeout};
    std::vector<GalacticLobbyRow> rows;
    friend bool operator==(const GalacticLobbyOptions&, const GalacticLobbyOptions&) = default;
};

// Canonical text, same style as the skirmish v1 codec: `gc2 <delay> <seed> <auto resolve> <vote timeout> <custom> <credits>
// <start tech> <max tech> <heroes> "<difficulty>" <row count>` then per row `<player> <faction id> <flags>
// "<faction>" <human> <dropout>`. Decode accepts only bytes that re-encode to themselves; an unknown version is
// refused by name.
[[nodiscard]] core::Result<LobbyGame> encode_galactic_lobby_game(const GalacticLobbyOptions& options);
[[nodiscard]] core::Result<GalacticLobbyOptions> decode_galactic_lobby_game(const LobbyGame& game);

// The one place a host draws a session seed: random, and 32-bit because the AI perception seed is (the world-generation
// seed rule). Every host path (lobby screen, acceptance run) uses it, so none can hand out a seed the AI setup refuses.
[[nodiscard]] std::uint64_t draw_galactic_session_seed();

// The options a host starts a room with: the campaign's factions as rows, the first `planned_humans` playable ones planned
// for a player (default one: the host's own row; every other playable row starts as an AI faction and a player takes it,
// D8), stock custom settings off. `planned_humans` is clamped to 1..galactic_lobby_max_human_sides and to the campaign's
// playable rows; a co-op campaign (single-player rules) always plans exactly one. Fails when the campaign has no playable faction.
[[nodiscard]] core::Result<GalacticLobbyOptions> default_galactic_lobby_options(
    const sim::galactic::MpLobbyCampaign& campaign, std::string_view difficulty, std::uint64_t seed,
    std::uint64_t input_delay = 3, std::size_t planned_humans = 1);

// The stock AI caption level a room difficulty name stands for ("Easy_Default" -> easy, "Normal_Default" -> normal,
// "Hard_Default" -> hard; case-insensitive prefix). Nothing for a mod's own name: the lobby shows that name as authored.
// The room has one difficulty, so every AI seat caption comes from this, never from a fixed "Medium".
enum class GalacticAiCaption : std::uint8_t { easy, normal, hard };
[[nodiscard]] std::optional<GalacticAiCaption> galactic_ai_caption(std::string_view difficulty) noexcept;
// The level a lobby SHOWS for a room difficulty whose campaign entry list is not at hand (a room of a campaign this install lacks): the stock
// caption level when the name has one, else the level named by the name's first word (Medium_Skirmish -> normal, Hard_Story -> hard), else
// normal. Never a raw entry name: the lobby words a difficulty as Easy, Normal or Hard (or the stack's own text for that level).
[[nodiscard]] GalacticAiCaption galactic_ai_display_level(std::string_view difficulty) noexcept;
// The text keys the AI captions are worded under (the stock Easy / Medium / Hard AI player names, which a mod may reword), and the plain
// words a lobby shows for a key when its text stack words none: Easy, Normal or Hard (anything else reads Normal).
[[nodiscard]] std::string_view galactic_ai_caption_key(GalacticAiCaption level) noexcept;
[[nodiscard]] std::string_view galactic_ai_caption_words(std::string_view key) noexcept;

// The human side (1..galactic_lobby_max_human_sides) a faction row forms: the row's rank among the host's human rows. Nothing for an AI row
// or a player ID the room does not list. A seat's `team` is this value; a seat's `faction` is the row's name.
[[nodiscard]] std::optional<sim::galactic::HumanSideId> galactic_lobby_side(const GalacticLobbyOptions& options,
    sim::galactic::PlayerId player) noexcept;

// Staging validation. Run on the lobby's validate hook for a GC room: refuses a campaign that is not in the
// local multiplayer list, rows that differ from local content, a human row on an unplayable or neutral faction,
// more human sides than the ceiling, more seats than the human seat limit, seats outside the planned rows, two sides on one faction and, when `starting`, everything
// the setup offer would refuse (galactic_roster_draft + validate_mp_roster: at least two humans, at most eight,
// a lead per side). Editable staging may be incomplete.
// `dialog_rows` is the host's staging dialog (galactic_lobby_human_row_limit); 0 when there is none.
[[nodiscard]] core::Result<void> validate_galactic_lobby(const LobbySnapshot& snapshot, bool starting,
    std::span<const sim::galactic::MpLobbyCampaign> catalog, std::size_t dialog_rows = 0);

// #6134: the options after the seat of `peer` sits on faction row `slot`, for a row that is not planned for a player yet.
// The row becomes a planned (human) row; the row the seat leaves is closed when no other seat holds it, so the number of
// human sides does not grow when a guest moves to a free faction. Nothing when the row cannot be held (unplayable, neutral,
// unknown) or the move would plan more human sides than a campaign game has (galactic_lobby_max_human_sides). A row that
// is already planned is unchanged.
// `before` is the room as it was, so the seat's old row and the other holders are known.
[[nodiscard]] std::optional<GalacticLobbyOptions> galactic_lobby_options_with_seat_on(const GalacticLobbyOptions& options,
    const sim::galactic::MpLobbyCampaign& campaign, const LobbySnapshot& before, PeerIndex peer, sim::galactic::PlayerId slot,
    std::size_t dialog_rows = 0);

// The lobby's choice hook for a Galactic Conquest room (host side, while a seat edits its own choice): when the choice
// names a faction row that is not planned for a player yet, the room's rows are reshaped with galactic_lobby_options_with_seat_on
// and every seat's side and faction follow its row again, so a guest can pick any free playable faction without the host
// closing another row first. A choice that cannot be reshaped is refused with the reason (the human side ceiling). The same
// hook runs when a seat is admitted (the row a newcomer was seated on becomes a human side when it was an AI row).
[[nodiscard]] core::Result<void> follow_galactic_choice(const LobbySnapshot& before, LobbySnapshot& candidate, PeerIndex peer,
    std::span<const sim::galactic::MpLobbyCampaign> catalog, std::size_t dialog_rows = 0);

// D8 (#8071): the faction row a newcomer is seated on. In order: a row the host planned for a player that nobody holds, else
// the first playable row that is still an AI faction (the newcomer claims it, follow_galactic_choice plans it) while the human
// side ceiling allows it (never in a co-op campaign), else the planned row with the fewest seats (the lowest faction on a tie:
// a co-op seat). Nothing when the room has no planned row.
[[nodiscard]] std::optional<sim::galactic::PlayerId> galactic_lobby_newcomer_row(const GalacticLobbyOptions& options,
    const sim::galactic::MpLobbyCampaign& campaign, const LobbySnapshot& room, std::size_t dialog_rows = 0);

// A campaign switch in staging (D8): the rows of the room's seats after the host changed the campaign to `fresh` (the new campaign's
// default options, host row planned only). The host takes the first planned row; every guest, in seat order, keeps a row of its own:
// a planned row nobody holds, else the next playable AI faction (planned with the seat while the human rows allow, see
// galactic_lobby_human_row_limit), else the emptiest planned row (a co-op seat; a co-op campaign always shares the host's row).
// `options` are `fresh` with the claimed rows planned and `slots` the faction row of each seat in `room.seats` order. Nothing when the
// campaign has no row for a seat.
struct GalacticLobbyReseat final {
    GalacticLobbyOptions options;
    std::vector<sim::galactic::PlayerId> slots;
};
[[nodiscard]] std::optional<GalacticLobbyReseat> galactic_lobby_reseat(GalacticLobbyOptions fresh,
    const sim::galactic::MpLobbyCampaign& campaign, const LobbySnapshot& room, std::size_t dialog_rows);

// Why the host may not switch a room with guests seated to `campaign` (D8): its connections are fixed when the room opens, so the room
// waits for `room.capacity` players, and a multiplayer campaign with fewer playable factions than that could never start. Nothing for
// a room with only the host in (it is opened again for the campaign), for a co-op campaign (one side, co-op seats) and for a campaign
// with at least as many playable factions as the room has seats.
[[nodiscard]] std::optional<std::string> galactic_lobby_switch_refusal(const sim::galactic::MpLobbyCampaign& campaign, const LobbySnapshot& room);

// The room's departure hook (host side), after `peer`'s seat was removed (left, kicked, link lost): the row it held, when no
// other seat holds it any more, goes back to an AI faction (unclaimed), every seat's side follows the rows and a seat whose
// side moved is unreadied by the room's ready rule. The room is left as it is when nothing changes.
void follow_galactic_departure(const LobbySnapshot& before, LobbySnapshot& after, PeerIndex peer,
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
