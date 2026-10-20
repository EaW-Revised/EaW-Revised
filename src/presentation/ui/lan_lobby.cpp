#include "eawr/presentation/ui/lan_lobby.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <functional>
#include <set>
#include <tuple>

namespace eawr::presentation::ui {

std::vector<HumanTeamSplit> human_team_splits(const std::uint32_t humans, const std::uint32_t owner_rows) {
    if (humans < 2 || humans > lan_command_seat_limit || owner_rows == 0) return {};
    std::vector<HumanTeamSplit> result{{humans, 0, 0}};
    for (std::uint32_t first = humans - 1; first > 0; --first) {
        const auto second = humans - first;
        if (owner_rows >= 2 && first >= second) result.push_back({first, second, 0});
    }
    if (owner_rows >= 3) {
        for (std::uint32_t first = humans - 2; first > 0; --first) {
            for (std::uint32_t second = humans - first - 1; second > 0; --second) {
                const auto third = humans - first - second;
                if (first >= second && second >= third) result.push_back({first, second, third});
            }
        }
    }
    return result;
}

HumanTeamSplit closest_human_team_split(const std::span<const HumanTeamSplit> choices, HumanTeamSplit previous) {
    if (choices.empty()) return {};
    std::sort(previous.begin(), previous.end(), std::greater<>());
    const auto sides = [](const HumanTeamSplit& value) { return std::count_if(value.begin(), value.end(),
        [](const auto count) { return count != 0; }); };
    const auto total = [](const HumanTeamSplit& value) { return value[0] + value[1] + value[2]; };
    const auto distance = [](const auto left, const auto right) { return left > right ? left - right : right - left; };
    const auto score = [&](const HumanTeamSplit& value) {
        std::uint64_t difference{};
        for (std::size_t index = 0; index < value.size(); ++index)
            difference += distance(static_cast<std::uint64_t>(value[index]) * total(previous),
                static_cast<std::uint64_t>(previous[index]) * total(value));
        return std::pair{distance(sides(value), sides(previous)), difference};
    };
    return *std::min_element(choices.begin(), choices.end(), [&](const auto& left, const auto& right) {
        return score(left) < score(right);
    });
}

std::string human_team_split_text(const HumanTeamSplit split) {
    if (split[1] == 0) return "Co-op vs AI";
    return std::to_string(split[0]) + " vs " + std::to_string(split[1])
        + (split[2] == 0 ? std::string{} : " vs " + std::to_string(split[2]));
}

namespace {

[[nodiscard]] std::string trimmed(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return std::string(text.substr(begin, end - begin));
}

// Cuts at the limit without splitting a UTF-8 sequence.
[[nodiscard]] std::string cut(std::string text, const std::size_t limit) {
    if (text.size() <= limit) return text;
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
    text.resize(end);
    return text;
}

[[nodiscard]] bool same_name(const std::string& a, const std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(a[index])) != std::tolower(static_cast<unsigned char>(b[index]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace

LobbySourceKind join_target_source(const LobbyJoinTarget& target) noexcept {
    return std::holds_alternative<OnlineInvitationTarget>(target) ? LobbySourceKind::online : LobbySourceKind::lan;
}

std::optional<std::string> normalize_invitation_code(const std::string_view typed) {
    std::string code;
    for (const char value : typed) {
        if (value == '-' || value == ' ' || value == '\t' || value == '\r' || value == '\n'
            || value == '\f' || value == '\v') continue;
        if (code.size() == 32) return std::nullopt;
        if (value >= '0' && value <= '9') code.push_back(value);
        else if (value >= 'a' && value <= 'f') code.push_back(value);
        else if (value >= 'A' && value <= 'F') code.push_back(static_cast<char>(value - 'A' + 'a'));
        else return std::nullopt;
    }
    if (code.size() != 32) return std::nullopt;
    return code;
}

std::string_view private_online_empty_notice() noexcept {
    return "Online games are private. Ask the host for an invitation code.";
}

std::optional<std::string> normalize_short_code(const std::string_view typed) {
    std::size_t begin = 0, end = typed.size();
    const auto space = [](const char c) { return c == ' ' || (c >= 9 && c <= 13); };
    while (begin < end && space(typed[begin])) ++begin;
    while (end > begin && space(typed[end - 1])) --end;
    if (end - begin != 4) return std::nullopt;
    std::string code;
    for (std::size_t i = begin; i < end; ++i) {
        const char c = typed[i];
        if (c >= '0' && c <= '9') code.push_back(c);
        else if (c >= 'A' && c <= 'Z') code.push_back(c);
        else if (c >= 'a' && c <= 'z') code.push_back(static_cast<char>(c - 'a' + 'A'));
        else return std::nullopt;
    }
    return code;
}

OnlinePageView online_page(const std::size_t total, const std::size_t requested, const std::size_t per_page) noexcept {
    OnlinePageView view;
    const std::size_t size = per_page == 0 ? 1 : per_page;
    view.pages = total == 0 ? std::size_t{1} : (total + size - 1) / size;
    view.page = requested >= view.pages ? view.pages - 1 : requested;
    view.first = view.page * size;
    view.count = view.first >= total ? 0 : std::min(size, total - view.first);
    return view;
}

std::string_view online_hosting_notice(const OnlineHostingPhase phase) noexcept {
    switch (phase) {
    case OnlineHostingPhase::idle: return {};
    case OnlineHostingPhase::publishing: return "Listing your room…";
    case OnlineHostingPhase::hosted: return "Your room is listed.";
    case OnlineHostingPhase::retrying: return "Could not list your room. Trying again…";
    }
    return {};
}

std::string_view online_hosting_name(const OnlineHostingPhase phase) noexcept {
    switch (phase) {
    case OnlineHostingPhase::idle: return "idle";
    case OnlineHostingPhase::publishing: return "publishing";
    case OnlineHostingPhase::hosted: return "hosted";
    case OnlineHostingPhase::retrying: return "retrying";
    }
    return "idle";
}

OnlineFailure classify_online_failure(const std::string_view code) noexcept {
    constexpr std::string_view prefix = "EAWR-NET-";
    if (code.substr(0, prefix.size()) != prefix) return OnlineFailure::other;
    const std::string_view digits = code.substr(prefix.size());
    int number{};
    if (digits.size() != 4 || std::from_chars(digits.data(), digits.data() + digits.size(), number).ec != std::errc{})
        return OnlineFailure::other;
    switch (number) {
    case 1540: return OnlineFailure::invalid;
    case 1541: return OnlineFailure::access;
    case 1542: case 1549: case 1550: case 1551: return OnlineFailure::busy;
    case 1543: return OnlineFailure::already_in_room;
    case 1544: return OnlineFailure::unavailable;
    case 1545: return OnlineFailure::full;
    case 1546: return OnlineFailure::wrong_password;
    case 1547: return OnlineFailure::cooldown;
    case 1548: return OnlineFailure::removed;
    case 1552: return OnlineFailure::timeout;
    case 1553: return OnlineFailure::duplicate_name;
    case 1554: return OnlineFailure::password_locked;
    default: return OnlineFailure::other;
    }
}

std::string_view online_failure_text(const OnlineFailure failure) noexcept {
    switch (failure) {
    case OnlineFailure::invalid: return "That room could not be used. Check the room and the name, then try again.";
    case OnlineFailure::access: return "The online service did not accept your request.";
    case OnlineFailure::busy: return "The online service is busy. Try again shortly.";
    case OnlineFailure::already_in_room: return "You already have a room open. Leave it first.";
    case OnlineFailure::unavailable: return "This game is no longer available.";
    case OnlineFailure::full: return "This game is full.";
    case OnlineFailure::wrong_password: return "The password was not accepted.";
    case OnlineFailure::cooldown: return "Too many password attempts. Wait a minute and try again.";
    case OnlineFailure::removed: return "The host removed you from this game.";
    case OnlineFailure::timeout: return "Could not connect to this game. Check your connection and try again.";
    case OnlineFailure::duplicate_name: return "That player name is already in this game. Choose another name.";
    case OnlineFailure::password_locked: return "The room password cannot change while the room is listed.";
    case OnlineFailure::other: break;
    }
    return "Online play is not available right now. Try again shortly.";
}

bool online_failure_keeps_prompt(const OnlineFailure failure) noexcept {
    return failure == OnlineFailure::wrong_password || failure == OnlineFailure::cooldown;
}

std::string_view online_list_notice(const bool service_failed, const std::size_t rooms) noexcept {
    if (service_failed) return "The online room list is unavailable. Try Refresh in a moment.";
    if (rooms == 0) return "No rooms are listed. Host one, or press Refresh.";
    return {};
}

std::string_view online_chat_closed_notice() noexcept { return "Join or host a room to chat."; }

std::string_view online_menu_chat_notice(const OnlineMenuChat state) noexcept {
    switch (state) {
        case OnlineMenuChat::active: return {};
        case OnlineMenuChat::connecting: return "Connecting to lobby chat...";
        case OnlineMenuChat::unavailable: return "Lobby chat is not available on this server.";
        case OnlineMenuChat::idle: break;
    }
    return online_chat_closed_notice();
}

std::string clamp_room_password(const std::string_view typed) {
    return cut(std::string(typed), lan_password_max);
}

bool can_submit_password(const OnlinePasswordPrompt& prompt, const std::string_view typed) noexcept {
    return !prompt.waiting && !typed.empty();
}

std::string_view online_join_notice(const OnlineJoinStatus status) noexcept {
    switch (status) {
    case OnlineJoinStatus::paired: return "Player connecting…";
    case OnlineJoinStatus::unavailable:
        return "This invitation is no longer available. Ask the host for a new one.";
    case OnlineJoinStatus::access_denied: return "The online service did not accept your access credentials.";
    case OnlineJoinStatus::trust_failed: return "Could not verify the online service.";
    case OnlineJoinStatus::not_configured:
        return "Online play is not available in this build or service configuration.";
    case OnlineJoinStatus::busy: return "A join is already in progress.";
    }
    return {};
}

void LobbyJoinSelection::cancel() {
    ++generation_;
    target_.reset();
    attempt_.reset();
}

void LobbyJoinSelection::select_source(const LobbySourceKind source) {
    if (source == source_) return;
    cancel();
    source_ = source;
}

std::optional<LobbyAttemptId> LobbyJoinSelection::begin(LobbyJoinTarget target) {
    if (attempt_ || join_target_source(target) != source_) return std::nullopt;
    if (auto* invitation = std::get_if<OnlineInvitationTarget>(&target)) {
        // The 128-bit invitation or the optional four-character room code.
        auto code = normalize_invitation_code(invitation->code);
        if (!code) code = normalize_short_code(invitation->code);
        if (!code || invitation->service.empty()) return std::nullopt;
        invitation->code = *code;
    }
    target_ = std::move(target);
    attempt_ = LobbyAttemptId{source_, ++generation_};
    return attempt_;
}

bool LobbyJoinSelection::complete(const LobbyAttemptId attempt) {
    if (!attempt_ || attempt != *attempt_) return false;
    attempt_.reset();
    target_.reset();
    return true;
}

bool LobbyFilter::active() const noexcept {
    return game_type.has_value() || min_players.has_value() || max_players.has_value() || max_ping_ms.has_value();
}

bool LobbyFilter::matches(const LobbyGame& game) const noexcept {
    if (game_type && game.game_type != *game_type) return false;
    // Both player filters compare the game's slot count, not who is in it yet (debug build).
    if (min_players && game.max_players < *min_players) return false;
    if (max_players && game.max_players > *max_players) return false;
    // With a ping limit set, a game whose ping is unknown is hidden as well (debug build).
    if (max_ping_ms && (game.ping_ms == 0 || game.ping_ms > *max_ping_ms)) return false;
    return true;
}

std::vector<LobbyGame> arrange_games(const std::span<const LobbyGame> games, const LobbyFilter& filter,
                                     const LobbySort sort) {
    std::vector<LobbyGame> shown;
    for (const LobbyGame& game : games) {
        if (filter.matches(game)) shown.push_back(game);
    }
    const auto key = [sort](const LobbyGame& game) {
        switch (sort) {
        case LobbySort::game_type: return std::make_tuple(lower(game.game_type), std::uint32_t{});
        case LobbySort::map_name: return std::make_tuple(lower(game.map), std::uint32_t{});
        case LobbySort::host:
            return std::make_tuple(game.player_names.empty() ? std::string{} : lower(game.player_names.front()), std::uint32_t{});
        case LobbySort::players: return std::make_tuple(std::string{}, game.players);
        case LobbySort::ping: return std::make_tuple(std::string{}, game.ping_ms);
        case LobbySort::game_name: break;
        }
        return std::make_tuple(lower(game.name), std::uint32_t{});
    };
    std::stable_sort(shown.begin(), shown.end(), [&](const LobbyGame& a, const LobbyGame& b) {
        const auto ka = key(a);
        const auto kb = key(b);
        if (ka != kb) return ka < kb;
        return std::tie(a.name, a.address) < std::tie(b.name, b.address);
    });
    return shown;
}

LobbyRowText lobby_row_text(const LobbyGame& game) {
    LobbyRowText row;
    row.name = game.name;
    row.type = game.game_type;
    row.players = std::to_string(game.players) + "/" + std::to_string(game.max_players);
    row.map = game.map;
    row.ping = game.ping_ms == 0 ? std::string("?") : std::to_string(game.ping_ms) + "ms";
    return row;
}

LobbyRowText online_row_text(const LobbyGame& game) {
    LobbyRowText row = lobby_row_text(game);
    row.type = game.player_names.empty() ? std::string{} : game.player_names.front();
    return row;
}

JoinVerdict check_join(const LobbyGame* selected, const std::string_view local_name, const bool password_supported) {
    JoinVerdict verdict;
    if (selected == nullptr) {
        verdict.check = JoinCheck::none_selected;
        verdict.key = "TEXT_LAN_LOBBY_SELECT_GAME_FIRST";
        return verdict;
    }
    if (!selected->version_matches) {
        verdict.check = JoinCheck::version_mismatch;
        verdict.key = "TEXT_VERSION_MISMATCH";
        return verdict;
    }
    if (selected->max_players <= selected->players) {
        verdict.check = JoinCheck::game_full;
        verdict.key = "TEXT_GAME_FULL";
        return verdict;
    }
    for (const std::string& name : selected->player_names) {
        if (same_name(name, local_name)) {
            verdict.check = JoinCheck::name_not_unique;
            verdict.key = "TEXT_PLAYER_NAME_NOT_UNIQUE";
            return verdict;
        }
    }
    if (selected->passworded && !password_supported) {
        // Retail asks for the password; this build has no password entry (decision D-b).
        verdict.check = JoinCheck::password_unsupported;
        verdict.literal = "This game needs a password, which this build does not support yet.";
        return verdict;
    }
    return verdict;
}

void ChatLog::add(const ChatKind kind, std::string text, std::string sender) {
    lines_.push_back({kind, std::move(text), std::move(sender)});
    ++total_;
    if (lines_.size() > capacity_) lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - capacity_));
}

std::optional<std::string> chat_line_to_send(const std::string_view typed) {
    std::string line = cut(trimmed(typed), lan_chat_max);
    if (line.empty()) return std::nullopt;
    return line;
}

std::string clamp_player_name(const std::string_view typed) { return cut(trimmed(typed), lan_name_max); }

std::optional<std::string> clean_game_name(const std::string_view typed) {
    std::string name = cut(trimmed(typed), lan_game_name_max);
    if (name.empty()) return std::nullopt;
    return name;
}

std::size_t staging_row_count(const std::span<const std::string> gadget_ids) {
    constexpr std::string_view prefix = "IDC_PLAYER_NAME_";
    std::set<std::size_t> found;
    for (const std::string& id : gadget_ids) {
        if (id.size() <= prefix.size() || id.compare(0, prefix.size(), prefix) != 0) continue;
        std::size_t index{};
        const char* begin = id.data() + prefix.size();
        const char* end = id.data() + id.size();
        const auto [stop, error] = std::from_chars(begin, end, index);
        if (error == std::errc{} && stop == end) found.insert(index);
    }
    std::size_t count = 0;
    while (found.contains(count)) ++count;
    return count;
}

namespace {

// Teams across the occupied rows: two or more assigned teams, one faction per team (WSS-17, WSS-18).
[[nodiscard]] StartVerdict check_teams(const std::span<const StagingRow> rows) {
    std::map<std::uint32_t, std::set<std::string>> factions_by_team;
    for (const StagingRow& row : rows) {
        if (!row.occupied() || !row.team) continue;
        factions_by_team[*row.team].insert(row.faction);
    }
    StartVerdict verdict;
    if (factions_by_team.size() < 2) {
        verdict.check = StartCheck::not_enough_teams;
        verdict.key = "TEXT_LAN_HOST_NOT_ENOUGH_TEAMS";
        return verdict;
    }
    for (const auto& [team, factions] : factions_by_team) {
        if (factions.size() > 1) {
            verdict.check = StartCheck::mismatched_factions;
            verdict.key = "TEXT_LAN_HOST_MISMATCHED_FACTIONS";
            return verdict;
        }
    }
    return verdict;
}

[[nodiscard]] std::size_t humans_in(const std::span<const StagingRow> rows) {
    return static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(),
                                                  [](const StagingRow& row) { return row.kind == RowKind::human; }));
}

} // namespace

StartVerdict check_host_start(const std::span<const StagingRow> rows, const bool map_listed,
    const std::size_t human_seats) {
    StartVerdict verdict;
    if (!map_listed) {
        verdict.check = StartCheck::no_map;
        return verdict;
    }
    const auto count = human_seats == 0 ? humans_in(rows) : human_seats;
    if (count < 2 || count > lan_command_seat_limit || humans_in(rows) == 0) {
        verdict.check = StartCheck::min_players;
        verdict.key = "TEXT_LAN_HOST_MIN_PLAYERS";
        return verdict;
    }
    return check_teams(rows);
}

StartVerdict check_client_accept(const std::span<const StagingRow> rows, const bool map_available) {
    if (!map_available) {
        StartVerdict verdict;
        verdict.check = StartCheck::map_not_available;
        verdict.key = "TEXT_MAP_NOT_AVAILABLE_TO_PLAY";
        return verdict;
    }
    return check_teams(rows);
}

bool host_start_enabled(const std::span<const StagingRow> rows, const std::size_t human_seats) {
    const auto count = human_seats == 0 ? humans_in(rows) : human_seats;
    if (count < 2 || count > lan_command_seat_limit || humans_in(rows) == 0) return false;
    for (const StagingRow& row : rows) {
        if (row.kind == RowKind::human && !row.local && !row.accepted) return false;
    }
    return true;
}

std::size_t visible_staging_rows(const std::size_t map_capacity, const std::size_t dialog_rows) noexcept {
    return std::min(map_capacity, dialog_rows);
}

bool FixedLobbySource::pump() {
    // The list is new to the screen on the first pump; a fixed list never changes afterwards.
    const bool changed = first_pump_;
    first_pump_ = false;
    return changed;
}

std::vector<LobbyPlayer> FixedLobbySource::joining(const std::string& game) const {
    std::vector<LobbyPlayer> joined;
    for (const LobbyPlayer& player : players_) {
        if (player.place == LobbyPlace::joining && player.game == game) joined.push_back(player);
    }
    return joined;
}

std::vector<LobbyChatMessage> FixedLobbySource::take_chat() {
    std::vector<LobbyChatMessage> taken;
    taken.swap(incoming_);
    return taken;
}

} // namespace eawr::presentation::ui
