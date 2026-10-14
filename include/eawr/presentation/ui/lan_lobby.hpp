#pragma once

// The LAN multiplayer screens' model (MNU-26 to MNU-29, docs/ui/menu-screens.md): the lobby's game
// list with its filter and sort, the chat lines, the local name and colour, the join checks, the
// host options and the staging rows with the start and accept checks. Engine-free: the viewer's
// EawrLanLobby draws it, a LobbyGameSource feeds its game list and the lockstep transport runs the match.
//
// Texts are text-database keys; the viewer resolves each through the text lookup, which shows a
// missing key as written (UI-T3). The sound names are not here: the screen resolves them from the
// effective sound data.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace eawr::presentation::ui {

// Field limits the retail edit boxes set (debug build, MNU-26/27). The chat history length is ours.
inline constexpr std::size_t lan_name_max = 20;
inline constexpr std::size_t lan_chat_max = 128;
inline constexpr std::size_t lan_game_name_max = 24;
inline constexpr std::size_t lan_password_max = 16;
inline constexpr std::size_t lan_chat_history = 200;
// Our command-seat capacity; the viewer checks this against the match domain.
inline constexpr std::size_t lan_command_seat_limit = 9;
// The lobby's game list rebuilds when this much time passed since the last build (milliseconds).
inline constexpr std::uint32_t lan_game_list_refresh_ms = 1000;

// ---- Lobby (MNU-26) ----

// One game a host announces. Ping 0 means unknown and shows as "?".
struct LobbyGame {
    std::string name;
    std::string map;
    std::string game_type; // the host's game type key (TEXT_...), shown through the text lookup
    std::string address;   // where to join: "<ip>:<port>"; opaque to the model
    std::uint32_t players{1};
    std::uint32_t max_players{2};
    std::uint32_t ping_ms{};
    bool passworded{};
    bool version_matches{true};
    // Names of the players in the game, for the unique-name check.
    std::vector<std::string> player_names;
    // The source's id of the hosting player (0 when it has none), to find the host in players().
    std::uint64_t host{};

    friend bool operator==(const LobbyGame&, const LobbyGame&) = default;
};

// ADR-023: an invitation is resolved only at its selected service, never as a TCP address.
enum class LobbySourceKind : std::uint8_t { lan, online };
struct LanAddressTarget { std::string address; };
struct LanDiscoveryTarget { LobbyGame game; };
struct OnlineInvitationTarget { std::string service; std::string code; };
using LobbyJoinTarget = std::variant<LanAddressTarget, LanDiscoveryTarget, OnlineInvitationTarget>;
[[nodiscard]] LobbySourceKind join_target_source(const LobbyJoinTarget& target) noexcept;
// Slice-2a recommendation: 128-bit hex, ASCII whitespace/hyphens allowed when pasted.
[[nodiscard]] std::optional<std::string> normalize_invitation_code(std::string_view typed);

struct LobbyAttemptId {
    LobbySourceKind source{LobbySourceKind::lan};
    std::uint64_t generation{};
    friend bool operator==(const LobbyAttemptId&, const LobbyAttemptId&) = default;
};
enum class OnlineJoinStatus : std::uint8_t {
    paired, unavailable, access_denied, trust_failed, not_configured, busy,
};
struct OnlineJoinResult {
    LobbyAttemptId attempt;
    OnlineJoinStatus status{OnlineJoinStatus::not_configured};
};
// Safe player-facing text only; no backend diagnostics or invitation material.
[[nodiscard]] std::string_view online_join_notice(OnlineJoinStatus status) noexcept;
[[nodiscard]] std::string_view private_online_empty_notice() noexcept;
// The optional four-letter-or-digit room code: trimmed, upper-cased, nothing else accepted.
[[nodiscard]] std::optional<std::string> normalize_short_code(std::string_view typed);

// ---- Online rooms (docs/net-online-lobby.md "One screen, two sources") ----

// How many rooms one page of the online list shows; refresh and paging step through the rest.
inline constexpr std::size_t online_rooms_per_page = 10;
struct OnlinePageView {
    std::size_t page{};  // the clamped page shown, from zero
    std::size_t pages{1};
    std::size_t first{}; // index of the page's first room
    std::size_t count{}; // rooms on this page
};
[[nodiscard]] OnlinePageView online_page(std::size_t total, std::size_t requested,
                                         std::size_t per_page = online_rooms_per_page) noexcept;

// The published state of the room the local player hosts.
enum class OnlineHostingPhase : std::uint8_t { idle, publishing, hosted, retrying };
[[nodiscard]] std::string_view online_hosting_notice(OnlineHostingPhase phase) noexcept;
[[nodiscard]] std::string_view online_hosting_name(OnlineHostingPhase phase) noexcept;

// What a failed service call means to the player. Classified from the diagnostic code only, so the
// text can never carry a password, a code or an address.
enum class OnlineFailure : std::uint8_t {
    other,
    invalid,
    access,
    busy,
    already_in_room,
    unavailable,
    full,
    wrong_password,
    cooldown,
    removed,
    timeout,
    duplicate_name,
    password_locked,
};
[[nodiscard]] OnlineFailure classify_online_failure(std::string_view diagnostic_code) noexcept;
[[nodiscard]] std::string_view online_failure_text(OnlineFailure failure) noexcept;
// A wrong password and a cooldown leave the prompt open so the player can type again.
[[nodiscard]] bool online_failure_keeps_prompt(OnlineFailure failure) noexcept;
[[nodiscard]] std::string_view online_list_notice(bool service_failed, std::size_t rooms) noexcept;
[[nodiscard]] std::string_view online_chat_closed_notice() noexcept;

// The password prompt of a join: the room it is for and the last refusal it shows.
struct OnlinePasswordPrompt {
    std::string game;
    std::string room;
    std::string error;
    bool waiting{}; // an attempt is in flight: no second submit
    friend bool operator==(const OnlinePasswordPrompt&, const OnlinePasswordPrompt&) = default;
};
// What a password edit sends: cut to the edit box's limit, never trimmed (spaces are part of it).
[[nodiscard]] std::string clamp_room_password(std::string_view typed);
[[nodiscard]] bool can_submit_password(const OnlinePasswordPrompt& prompt, std::string_view typed) noexcept;

// One join at a time. Cancellation and switches invalidate every result from the old generation.
class LobbyJoinSelection {
public:
    [[nodiscard]] LobbySourceKind source() const noexcept { return source_; }
    [[nodiscard]] const std::optional<LobbyJoinTarget>& target() const noexcept { return target_; }
    [[nodiscard]] const std::optional<LobbyAttemptId>& attempt() const noexcept { return attempt_; }
    void select_source(LobbySourceKind source);
    void cancel();
    [[nodiscard]] std::optional<LobbyAttemptId> begin(LobbyJoinTarget target);
    [[nodiscard]] bool complete(LobbyAttemptId attempt);
private:
    LobbySourceKind source_{LobbySourceKind::lan};
    std::uint64_t generation_{};
    std::optional<LobbyJoinTarget> target_;
    std::optional<LobbyAttemptId> attempt_;
};

// Private online has no public discovery or global chat. Each operation is nonblocking and
// completions retain the caller's attempt ID, including when cancellation raced with delivery.
// Every begin yields one result unless cancelled before completion; overlapping requests yield busy.
class OnlineLobbySource {
public:
    virtual ~OnlineLobbySource() = default;
    [[nodiscard]] virtual std::string service() const = 0;
    virtual void begin_join(LobbyAttemptId attempt, const OnlineInvitationTarget& target) = 0;
    virtual void cancel(LobbyAttemptId attempt) = 0;
    virtual void pump() = 0;
    [[nodiscard]] virtual std::vector<OnlineJoinResult> take_results() = 0;
};

// The filter dialog (IDD_LOBBY_GAME_LIST_FILTER_DIALOG): game type, minimum and maximum players,
// maximum ping. An unset field does not filter; "Clear filters" unsets all four. The player limits
// compare a game's slot count; a ping limit hides a game whose ping is unknown.
struct LobbyFilter {
    std::optional<std::string> game_type;
    std::optional<std::uint32_t> min_players;
    std::optional<std::uint32_t> max_players;
    std::optional<std::uint32_t> max_ping_ms;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool matches(const LobbyGame& game) const noexcept;
    void clear() noexcept { *this = LobbyFilter{}; }
};

// The sort buttons above the list. The direction a repeated press gives is not traced: every press
// sorts ascending (unverified, docs/ui/menu-screens.md).
enum class LobbySort : std::uint8_t { game_name, game_type, players, map_name, ping, host };

// The games the list shows: filtered, then sorted by the key with ties by name then address.
[[nodiscard]] std::vector<LobbyGame> arrange_games(std::span<const LobbyGame> games, const LobbyFilter& filter,
                                                   LobbySort sort);

// The cell texts of one list row, in the retail column order: name, type, players, map, ping.
struct LobbyRowText {
    std::string name;
    std::string type; // the key; the viewer resolves it
    std::string players;
    std::string map;
    std::string ping;
};
[[nodiscard]] LobbyRowText lobby_row_text(const LobbyGame& game);
// The online list's cells: the host's name takes the type column; the view draws the lock for a
// passworded room.
[[nodiscard]] LobbyRowText online_row_text(const LobbyGame& game);

// The checks Join runs on the selected game, in retail's order. `key` is the text key the chat
// list shows in red; `literal` is our own English text for a reason retail does not have.
enum class JoinCheck : std::uint8_t {
    ok,
    none_selected,
    version_mismatch,
    game_full,
    name_not_unique,
    password_unsupported,
};
struct JoinVerdict {
    JoinCheck check{JoinCheck::ok};
    std::string key;
    std::string literal;
    [[nodiscard]] bool ok() const noexcept { return check == JoinCheck::ok; }
};
// `password_supported` is true for the online list, where a passworded game asks for its password.
[[nodiscard]] JoinVerdict check_join(const LobbyGame* selected, std::string_view local_name,
                                     bool password_supported = false);

// ---- Chat (lobby and staging) ----

enum class ChatKind : std::uint8_t { normal, error, notice };
struct ChatLine {
    ChatKind kind{ChatKind::normal};
    std::string text;
    std::string sender;
};

// A bounded chat list: the oldest line drops when the history is full.
class ChatLog {
public:
    explicit ChatLog(std::size_t capacity = lan_chat_history) : capacity_(capacity == 0 ? 1 : capacity) {}
    void add(ChatKind kind, std::string text, std::string sender = {});
    [[nodiscard]] const std::vector<ChatLine>& lines() const noexcept { return lines_; }
    // Lines ever added, never reduced: line i of lines() is number total() - lines().size() + i, so a
    // view that has drawn up to a total knows which lines are new even once the log is full.
    [[nodiscard]] std::size_t total() const noexcept { return total_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    void clear() { lines_.clear(); }

private:
    std::size_t capacity_;
    std::size_t total_{};
    std::vector<ChatLine> lines_;
};

// A line the player sends: trimmed and cut to the edit box's limit; empty sends nothing.
[[nodiscard]] std::optional<std::string> chat_line_to_send(std::string_view typed);
// The local name the player committed: trimmed and cut to the edit box's limit; empty keeps the old one.
[[nodiscard]] std::string clamp_player_name(std::string_view typed);

// ---- Game source (the seam the lobby's discovery plugs into) ----

// Where a player is, as a source announces and lists it.
enum class LobbyPlace : std::uint8_t { lobby, hosting, joining };
enum class LobbyChatScope : std::uint8_t { lobby, game };

// What the local player announces about itself: the lobby sends it whenever it changes.
struct LobbyPresence {
    LobbyPlace place{LobbyPlace::lobby};
    std::string name;
    std::uint32_t colour{};
    // hosting: the game's name; joining: the name of the game being joined.
    std::string game;
    // hosting only:
    std::string map;
    std::string game_type;
    std::uint16_t tcp_port{};
    std::uint32_t max_players{2};
    bool passworded{};
    // The host left the staging for the battle: the game is no longer joinable.
    bool locked{};

    friend bool operator==(const LobbyPresence&, const LobbyPresence&) = default;
};

// A remote player the source knows.
struct LobbyPlayer {
    std::uint64_t id{};
    std::string name;
    std::uint32_t colour{};
    LobbyPlace place{LobbyPlace::lobby};
    std::string game;
};

// A chat line that arrived from another player.
struct LobbyChatMessage {
    LobbyChatScope scope{LobbyChatScope::lobby};
    std::string game; // the staging the line belongs to when the scope is game
    std::string name;
    std::string text;
};

// What the lobby reads games, players and chat from, and tells its own presence and chat to. The
// game list is its only discovery: the lobby also joins a typed address without one (direct
// connect). `pump` runs once per frame on the screen's thread and never blocks.
class LobbyGameSource {
public:
    virtual ~LobbyGameSource() = default;

    virtual void announce(const LobbyPresence& presence) = 0;
    virtual void send_chat(LobbyChatScope scope, const std::string& game, const std::string& name,
                           const std::string& text) = 0;
    // Reads what arrived and drops the stale; true when the games or players changed.
    [[nodiscard]] virtual bool pump() = 0;
    // The joinable games (a game whose host locked it for the battle is not listed).
    [[nodiscard]] virtual std::vector<LobbyGame> games() const = 0;
    [[nodiscard]] virtual std::vector<LobbyPlayer> players() const = 0;
    // The remote players currently joining the named game, oldest first.
    [[nodiscard]] virtual std::vector<LobbyPlayer> joining(const std::string& game) const = 0;
    // Chat lines received since the last call, oldest first.
    [[nodiscard]] virtual std::vector<LobbyChatMessage> take_chat() = 0;
};

// A source with a fixed list, for presentation fixtures and tests: it lists what it was given,
// delivers queued chat once, and remembers what the lobby announced and sent.
class FixedLobbySource final : public LobbyGameSource {
public:
    FixedLobbySource() = default;
    FixedLobbySource(std::vector<LobbyGame> games, std::vector<LobbyPlayer> players)
        : games_(std::move(games)), players_(std::move(players)) {}

    void queue_chat(LobbyChatMessage message) { incoming_.push_back(std::move(message)); }
    [[nodiscard]] const LobbyPresence& announced() const noexcept { return announced_; }
    [[nodiscard]] const std::vector<LobbyChatMessage>& sent() const noexcept { return sent_; }

    void announce(const LobbyPresence& presence) override { announced_ = presence; }
    void send_chat(LobbyChatScope scope, const std::string& game, const std::string& name,
                   const std::string& text) override {
        sent_.push_back({scope, game, name, text});
    }
    [[nodiscard]] bool pump() override;
    [[nodiscard]] std::vector<LobbyGame> games() const override { return games_; }
    [[nodiscard]] std::vector<LobbyPlayer> players() const override { return players_; }
    [[nodiscard]] std::vector<LobbyPlayer> joining(const std::string& game) const override;
    [[nodiscard]] std::vector<LobbyChatMessage> take_chat() override;

private:
    std::vector<LobbyGame> games_;
    std::vector<LobbyPlayer> players_;
    std::vector<LobbyChatMessage> incoming_;
    std::vector<LobbyChatMessage> sent_;
    LobbyPresence announced_;
    bool first_pump_{true};
};

// ---- Host options (MNU-27) ----

// Counts on each shared human side, largest first; zero means that side is unused.
using HumanTeamSplit = std::array<std::uint32_t, 3>;
[[nodiscard]] std::vector<HumanTeamSplit> human_team_splits(std::uint32_t humans, std::uint32_t owner_rows);
// Keep the number of sides when possible, then the closest proportions. Co-op stays co-op.
[[nodiscard]] HumanTeamSplit closest_human_team_split(std::span<const HumanTeamSplit> choices,
    HumanTeamSplit previous);
[[nodiscard]] std::string human_team_split_text(HumanTeamSplit split);

struct HostOptions {
    std::string game_name;
    std::string game_type; // the chosen game type key
    std::string password;  // disabled in this build: always empty
};
// A host game needs a name (the dialog refocuses the name edit when it is empty); the name is cut
// to the edit box's limit.
[[nodiscard]] std::optional<std::string> clean_game_name(std::string_view typed);

// ---- Staging (MNU-28) ----

// The staging rows are the dialog's player gadgets: IDC_PLAYER_NAME_0, _1, ... counted from zero
// without a gap. The count comes from the dialog the data supplies, never from a constant here.
[[nodiscard]] std::size_t staging_row_count(std::span<const std::string> gadget_ids);

enum class RowKind : std::uint8_t { none, open, human, ai };

// One row as the staging shows it. `faction` is a faction id of the loaded data; `team` and
// `colour` are the row's picks (team absent: unassigned).
struct StagingRow {
    RowKind kind{RowKind::none};
    std::string name;
    std::string faction;
    std::optional<std::uint32_t> team;
    std::uint32_t colour{};
    bool accepted{};
    bool local{};

    [[nodiscard]] bool occupied() const noexcept { return kind == RowKind::human || kind == RowKind::ai; }
};

enum class StartCheck : std::uint8_t {
    ok,
    no_map,
    min_players,
    not_enough_teams,
    mismatched_factions,
    map_not_available,
};
struct StartVerdict {
    StartCheck check{StartCheck::ok};
    // The key the red chat line shows (empty for ok and no_map: no line).
    std::string key;
    [[nodiscard]] bool ok() const noexcept { return check == StartCheck::ok; }
};

// The host's Start checks, in retail's order: a map is listed, at least two humans are in the
// game, then the teams (two or more assigned teams, one faction per team). AI rows count as
// players for the teams, not for the human minimum.
[[nodiscard]] StartVerdict check_host_start(std::span<const StagingRow> rows, bool map_listed,
    std::size_t human_seats = 0); // zero counts one-seat-per-owner rows
// The joiner's Accept checks: the map is available here, then the same team checks.
[[nodiscard]] StartVerdict check_client_accept(std::span<const StagingRow> rows, bool map_available);

// The host's Start button is enabled while at least two humans are in the game and every human
// has accepted. The host's own row counts as accepted.
[[nodiscard]] bool host_start_enabled(std::span<const StagingRow> rows, std::size_t human_seats = 0);

// The number of rows a map of this capacity shows; rows past it are hidden.
[[nodiscard]] std::size_t visible_staging_rows(std::size_t map_capacity, std::size_t dialog_rows) noexcept;

} // namespace eawr::presentation::ui
