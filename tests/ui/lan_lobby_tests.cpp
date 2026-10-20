// The LAN screens' model (MNU-26 to MNU-29, docs/ui/menu-screens.md): the game list's filter and
// sort, the row texts, the join checks and their order, the chat and edit-box limits, the host
// options, the staging row count read from the dialog's gadgets and the start and accept checks.

#include "eawr/presentation/ui/lan_lobby.hpp"
#include "ui_test_support.hpp"
#include "online_lobby_fixture.hpp"

#include <algorithm>
#include <set>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace ui = eawr::presentation::ui;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

ui::LobbyGame game(std::string name, std::string map, std::string type, std::uint32_t players, std::uint32_t max,
                   std::uint32_t ping) {
    ui::LobbyGame value;
    value.name = std::move(name);
    value.map = std::move(map);
    value.game_type = std::move(type);
    value.address = "10.0.0." + std::to_string(players + max + ping % 7) + ":47624";
    value.players = players;
    value.max_players = max;
    value.ping_ms = ping;
    return value;
}

std::vector<ui::LobbyGame> sample() {
    return {game("Bravo", "Kessel", "TEXT_TYPE_B", 1, 4, 30), game("Alpha", "Endor", "TEXT_TYPE_A", 2, 2, 0),
            game("Charlie", "Corellia", "TEXT_TYPE_A", 1, 8, 120)};
}

std::vector<std::string> names(const std::vector<ui::LobbyGame>& games) {
    std::vector<std::string> out;
    for (const ui::LobbyGame& value : games) out.push_back(value.name);
    return out;
}

void list_and_sort() {
    const std::vector<ui::LobbyGame> games = sample();
    const auto by = [&](const ui::LobbySort sort) { return names(ui::arrange_games(games, {}, sort)); };
    expect((by(ui::LobbySort::game_name) == std::vector<std::string>{"Alpha", "Bravo", "Charlie"}),
           "MNU-26: sorts by game name");
    expect((by(ui::LobbySort::map_name) == std::vector<std::string>{"Charlie", "Alpha", "Bravo"}),
           "MNU-26: sorts by map name");
    expect((by(ui::LobbySort::game_type) == std::vector<std::string>{"Alpha", "Charlie", "Bravo"}),
           "MNU-26: sorts by game type, ties by name");
    expect((by(ui::LobbySort::players) == std::vector<std::string>{"Bravo", "Charlie", "Alpha"}),
           "MNU-26: sorts by player count, ties by name");
    expect((by(ui::LobbySort::ping) == std::vector<std::string>{"Alpha", "Bravo", "Charlie"}),
           "MNU-26: sorts by ping");
}

void filtering() {
    const std::vector<ui::LobbyGame> games = sample();
    ui::LobbyFilter filter;
    expect(!filter.active() && ui::arrange_games(games, filter, ui::LobbySort::game_name).size() == 3,
           "an empty filter shows every game");
    filter.game_type = "TEXT_TYPE_A";
    expect((names(ui::arrange_games(games, filter, ui::LobbySort::game_name)) == std::vector<std::string>{"Alpha", "Charlie"}),
           "the game type filter keeps that type");
    filter.min_players = 4;
    expect((names(ui::arrange_games(games, filter, ui::LobbySort::game_name)) == std::vector<std::string>{"Charlie"}),
           "the minimum players filter keeps games with that many slots or more, then the type filter applies");
    filter.clear();
    filter.min_players = 4;
    expect((names(ui::arrange_games(games, filter, ui::LobbySort::game_name)) == std::vector<std::string>{"Bravo", "Charlie"}),
           "the minimum players filter compares the slot count");
    filter.clear();
    filter.max_players = 4;
    expect((names(ui::arrange_games(games, filter, ui::LobbySort::game_name)) == std::vector<std::string>{"Alpha", "Bravo"}),
           "the maximum players filter keeps games with at most that many slots");
    filter.clear();
    filter.max_ping_ms = 50;
    expect((names(ui::arrange_games(games, filter, ui::LobbySort::game_name)) == std::vector<std::string>{"Bravo"}),
           "the maximum ping filter drops slower games and games with an unknown ping");
    filter.clear();
    expect(!filter.active(), "Clear filters unsets all four");
}

void row_text() {
    ui::LobbyGame value = game("Bravo", "Kessel", "TEXT_TYPE_B", 1, 4, 30);
    ui::LobbyRowText row = ui::lobby_row_text(value);
    expect(row.name == "Bravo" && row.players == "1/4" && row.map == "Kessel" && row.ping == "30ms"
               && row.type == "TEXT_TYPE_B",
           "MNU-26: a row shows name, type, players as n/max, map and ping in ms");
    value.ping_ms = 0;
    expect(ui::lobby_row_text(value).ping == "?", "MNU-26: an unknown ping shows as a question mark");
}

void join_checks() {
    using Check = ui::JoinCheck;
    expect(ui::check_join(nullptr, "Me").check == Check::none_selected
               && ui::check_join(nullptr, "Me").key == "TEXT_LAN_LOBBY_SELECT_GAME_FIRST",
           "MNU-26: join with no game selected says to select one first");
    ui::LobbyGame value = game("Alpha", "Endor", "TEXT_TYPE_A", 1, 4, 10);
    expect(ui::check_join(&value, "Me").ok(), "a joinable game passes");
    value.version_matches = false;
    value.players = value.max_players;
    expect(ui::check_join(&value, "Me").check == Check::version_mismatch
               && ui::check_join(&value, "Me").key == "TEXT_VERSION_MISMATCH",
           "MNU-26: a version mismatch is reported before a full game");
    value.version_matches = true;
    expect(ui::check_join(&value, "Me").check == Check::game_full && ui::check_join(&value, "Me").key == "TEXT_GAME_FULL",
           "MNU-26: a full game is refused");
    value.players = 2;
    value.player_names = {"someone", "ME"};
    expect(ui::check_join(&value, "me").check == Check::name_not_unique
               && ui::check_join(&value, "me").key == "TEXT_PLAYER_NAME_NOT_UNIQUE",
           "MNU-26: the player name must be unique in the game, compared without case");
    value.player_names.clear();
    value.passworded = true;
    const ui::JoinVerdict locked = ui::check_join(&value, "Me");
    expect(locked.check == Check::password_unsupported && locked.key.empty() && !locked.literal.empty(),
           "a passworded game is refused with our own reason (decision D-b)");
}

void chat_and_names() {
    expect(!ui::chat_line_to_send("   ").has_value(), "an empty chat line sends nothing");
    expect(ui::chat_line_to_send("  hello ") == std::optional<std::string>("hello"), "a chat line is trimmed");
    expect(ui::chat_line_to_send(std::string(300, 'x'))->size() == ui::lan_chat_max, "a chat line is cut to 128 characters");
    expect(ui::clamp_player_name(std::string(50, 'n')).size() == ui::lan_name_max, "a player name is cut to 20 characters");
    expect(ui::clamp_player_name("   ").empty(), "an empty name stays empty so the caller keeps the old one");
    // A multi-byte character is never split at the cut.
    std::string wide(19, 'a');
    wide += "\xC3\xA9"; // two bytes straddling the 20 limit
    expect(ui::clamp_player_name(wide).size() == 19, "the cut does not split a UTF-8 sequence");

    ui::ChatLog log(3);
    for (int index = 0; index < 5; ++index) log.add(ui::ChatKind::normal, "line " + std::to_string(index));
    expect(log.lines().size() == 3 && log.lines().front().text == "line 2", "the chat history drops the oldest lines");
    // The total never drops: a full log still tells a view which lines are new.
    expect(log.total() == 5 && log.lines().size() == 3 && log.capacity() == 3, "the chat total counts every line added");
    log.add(ui::ChatKind::error, "red");
    expect(log.total() == 6 && log.lines().size() == 3 && log.lines().front().text == "line 3",
           "a full log keeps counting while it drops its oldest line");
    expect(log.lines().back().kind == ui::ChatKind::error, "a line keeps its kind");
}

void host_options() {
    expect(!ui::clean_game_name("  ").has_value(), "MNU-27: a host game needs a name");
    expect(ui::clean_game_name(std::string(40, 'g'))->size() == ui::lan_game_name_max,
           "MNU-27: the game name is cut to 24 characters");
    expect(ui::clean_game_name(" Fleet night ") == std::optional<std::string>("Fleet night"), "the game name is trimmed");
}

std::vector<ui::StagingRow> rows(std::initializer_list<ui::StagingRow> value) { return value; }

ui::StagingRow human(std::uint32_t team, std::string faction, bool local, bool accepted) {
    ui::StagingRow row;
    row.kind = ui::RowKind::human;
    row.team = team;
    row.faction = std::move(faction);
    row.local = local;
    row.accepted = accepted;
    return row;
}

ui::StagingRow computer(std::uint32_t team, std::string faction) {
    ui::StagingRow row;
    row.kind = ui::RowKind::ai;
    row.team = team;
    row.faction = std::move(faction);
    return row;
}

void human_team_choices() {
    using Split = ui::HumanTeamSplit;
    for (std::uint32_t humans = 2; humans <= 9; ++humans) {
        for (std::uint32_t rows = 0; rows <= 3; ++rows) {
            const auto choices = ui::human_team_splits(humans, rows);
            std::set<Split> expected;
            for (std::uint32_t a = 1; a <= humans; ++a)
                for (std::uint32_t b = 0; b <= a; ++b)
                    for (std::uint32_t c = 0; c <= b; ++c)
                        if (a + b + c == humans && 1U + (b > 0 ? 1U : 0U) + (c > 0 ? 1U : 0U) <= rows)
                            expected.insert({a, b, c});
            expect(std::set<Split>(choices.begin(), choices.end()) == expected,
                "human counts enumerate every permitted descending split");
            expect(choices.size() == expected.size(), "each split appears once");
            if (!choices.empty()) expect(choices.front() == Split{humans, 0, 0}, "co-op is first");
            for (const auto split : choices) {
                expect(split[0] >= split[1] && split[1] >= split[2], "largest side first");
                expect(ui::closest_human_team_split(choices, split) == split, "unchanged split stays selected");
            }
        }
    }
    expect(ui::human_team_splits(1, 3).empty() && ui::human_team_splits(10, 3).empty(), "human count limits");
    expect(ui::human_team_split_text({9, 0, 0}) == "Co-op vs AI"
        && ui::human_team_split_text({3, 2, 0}) == "3 vs 2"
        && ui::human_team_split_text({2, 2, 1}) == "2 vs 2 vs 1", "compact team labels");
    expect(ui::closest_human_team_split(ui::human_team_splits(9, 3), {3, 2, 0}) == Split{5, 4, 0},
        "count changes keep the closest proportions and two sides");
    expect(ui::closest_human_team_split(ui::human_team_splits(4, 3), {2, 2, 1}) == Split{2, 1, 1},
        "count changes retain three sides when possible");
    expect(ui::closest_human_team_split(ui::human_team_splits(2, 3), {3, 3, 3}) == Split{1, 1, 0},
        "a smaller count drops a side only when necessary");
    expect(ui::closest_human_team_split(ui::human_team_splits(5, 1), {3, 2, 0}) == Split{5, 0, 0},
        "map row changes select the closest valid split");
    expect(ui::closest_human_team_split(ui::human_team_splits(9, 3), {2, 0, 0}) == Split{9, 0, 0},
        "co-op remains co-op");
    expect(ui::closest_human_team_split(ui::human_team_splits(5, 3), {2, 3, 0}) == Split{3, 2, 0},
        "a previous owner permutation maps to the canonical choice");
    expect(ui::closest_human_team_split({}, {2, 0, 0}) == Split{}, "no choices has no split");
}

void staging_rows() {
    std::vector<std::string> ids;
    for (int index = 0; index < 9; ++index) ids.push_back("IDC_PLAYER_NAME_" + std::to_string(index));
    ids.push_back("IDC_PLAYER_FACTION_0");
    ids.push_back("IDC_PLAYER_NAME_X");
    expect(ui::staging_row_count(ids) == 9, "MNU-28: nine name gadgets give nine rows");
    ids.erase(ids.begin() + 4);
    expect(ui::staging_row_count(ids) == 4, "the count stops at the first missing row");
    std::vector<std::string> modded;
    for (int index = 0; index < 12; ++index) modded.push_back("IDC_PLAYER_NAME_" + std::to_string(index));
    expect(ui::staging_row_count(modded) == 12, "a mod's dialog with twelve rows gives twelve");
    expect(ui::staging_row_count({}) == 0, "no row gadgets give no rows");
    expect(ui::visible_staging_rows(6, 9) == 6 && ui::visible_staging_rows(12, 9) == 9,
           "a map shows its capacity, at most the dialog's rows");
}

void start_checks() {
    using Check = ui::StartCheck;
    const auto two_teams = rows({human(0, "A", true, true), human(1, "B", false, true)});
    expect(ui::check_host_start(two_teams, true).ok(), "two humans on two teams may start");
    expect(ui::check_host_start(two_teams, false).check == Check::no_map && ui::check_host_start(two_teams, false).key.empty(),
           "MNU-28: no listed map starts nothing and says nothing");

    const auto alone = rows({human(0, "A", true, true), computer(1, "B")});
    const ui::StartVerdict few = ui::check_host_start(alone, true);
    expect(few.check == Check::min_players && few.key == "TEXT_LAN_HOST_MIN_PLAYERS",
           "MNU-28: one human with an AI is below the two-player minimum, which counts humans");
    for (const auto seats : {2U, 3U, 4U, 8U, 9U}) {
        expect(ui::check_host_start(alone, true, seats).ok(), "shared seats may start one coop owner against AI");
        expect(ui::host_start_enabled(alone, seats), "shared coop seats enable Start after the transport barrier");
    }
    expect(!ui::check_host_start(alone, true, 10).ok() && !ui::host_start_enabled(alone, 10),
        "a tenth human seat is refused");
    expect(!ui::check_host_start(rows({human(0, "A", true, true), computer(0, "A")}), true, 8).ok(),
        "shared coop seats still require an opposing AI team");

    const auto one_team = rows({human(0, "A", true, true), human(0, "A", false, true)});
    const ui::StartVerdict teams = ui::check_host_start(one_team, true);
    expect(teams.check == Check::not_enough_teams && teams.key == "TEXT_LAN_HOST_NOT_ENOUGH_TEAMS",
           "MNU-28: one assigned team is not enough");

    ui::StagingRow unassigned = human(0, "B", false, true);
    unassigned.team.reset();
    expect(ui::check_host_start(rows({human(0, "A", true, true), unassigned}), true).check == Check::not_enough_teams,
           "WSS-17: an unassigned team does not count");

    const auto mixed = rows({human(0, "A", true, true), human(0, "B", false, true), computer(1, "A")});
    const ui::StartVerdict factions = ui::check_host_start(mixed, true);
    expect(factions.check == Check::mismatched_factions && factions.key == "TEXT_LAN_HOST_MISMATCHED_FACTIONS",
           "WSS-18: teammates on one team must share a faction");

    const auto with_ai = rows({human(0, "A", true, true), human(1, "B", false, true), computer(1, "B")});
    expect(ui::check_host_start(with_ai, true).ok(), "AI rows on a team with one faction pass");

    const ui::StartVerdict missing = ui::check_client_accept(two_teams, false);
    expect(missing.check == Check::map_not_available && missing.key == "TEXT_MAP_NOT_AVAILABLE_TO_PLAY",
           "MNU-28: a joiner without the map cannot accept");
    expect(ui::check_client_accept(two_teams, true).ok() && ui::check_client_accept(one_team, true).check == Check::not_enough_teams,
           "the joiner's accept runs the same team checks");
}

void start_enabled() {
    expect(!ui::host_start_enabled(rows({human(0, "A", true, true)})), "MNU-28: Start waits for a second human");
    expect(!ui::host_start_enabled(rows({human(0, "A", true, true), human(1, "B", false, false)})),
           "MNU-28: Start waits until the joiner has accepted");
    expect(ui::host_start_enabled(rows({human(0, "A", true, false), human(1, "B", false, true)})),
           "MNU-28: Start enables once every joiner accepted; the host's own row needs no accept");
    const auto three = rows({human(0, "Mod_A", true, true), human(1, "Mod_B", false, true), human(2, "Mod_C", false, true)});
    expect(ui::check_host_start(three, true, 9).ok() && ui::host_start_enabled(three, 9), "nine seats with three loaded owner teams enable Start");
    auto pending = three; pending.back().accepted = false;
    expect(!ui::host_start_enabled(pending, 9), "third human owner's unready controller disables nine-seat Start");
    expect(!ui::host_start_enabled(rows({computer(0, "A"), computer(1, "B")})), "AI rows alone never enable Start");
}

void fixed_source() {
    ui::FixedLobbySource source(sample(), {{7, "Remy", 3, ui::LobbyPlace::joining, "Alpha"},
                                           {8, "Sol", 1, ui::LobbyPlace::lobby, {}}});
    expect(source.games().size() == 3, "MNU-26: a fixed source lists the games it was given");
    expect(source.players().size() == 2, "MNU-26: a fixed source lists its players");
    expect(source.pump(), "MNU-26: the first pump reports the list as new");
    expect(!source.pump(), "MNU-26: a fixed list does not change again");
    const auto joining = source.joining("Alpha");
    expect(joining.size() == 1 && joining.front().name == "Remy", "MNU-28: the players joining a game are found by game");
    expect(source.joining("Bravo").empty(), "MNU-28: nobody joins another game");

    source.queue_chat({ui::LobbyChatScope::lobby, {}, "Sol", "hello"});
    const auto chat = source.take_chat();
    expect(chat.size() == 1 && chat.front().text == "hello", "MNU-26: queued chat is delivered");
    expect(source.take_chat().empty(), "MNU-26: chat is delivered once");

    ui::LobbyPresence presence;
    presence.place = ui::LobbyPlace::hosting;
    presence.game = "Fleet night";
    source.announce(presence);
    source.send_chat(ui::LobbyChatScope::game, "Fleet night", "Me", "gl");
    expect(source.announced().game == "Fleet night", "MNU-27: the announced presence is remembered");
    expect(source.sent().size() == 1 && source.sent().front().scope == ui::LobbyChatScope::game,
           "MNU-28: a sent chat line is remembered with its scope");
}

void invitation_targets() {
    const std::string code = "0123456789abcdef0123456789abcdef";
    expect(ui::join_target_source(ui::LanAddressTarget{"example:47624"}) == ui::LobbySourceKind::lan,
           "a direct address stays a LAN target");
    expect(ui::join_target_source(ui::LanDiscoveryTarget{sample().front()}) == ui::LobbySourceKind::lan,
           "a discovery entry stays a LAN target");
    expect(ui::join_target_source(ui::OnlineInvitationTarget{"service", code}) == ui::LobbySourceKind::online,
           "an invitation cannot become a TCP target");
    expect(ui::normalize_invitation_code(" 0123-4567 89AB-CDEF\t0123-4567-89ab-cdef\n") == code,
           "pasted ASCII formatting canonicalizes without changing invitation identity");
    for (const auto& malformed : {std::string{}, code.substr(1), code + "0", "O" + code.substr(1),
                                  "\xEF\xBC\x90" + code.substr(1), "\xC2\xA0" + code}) {
        expect(!ui::normalize_invitation_code(malformed), "malformed/lookalike codes are rejected locally");
    }
    expect(!ui::private_online_empty_notice().empty(), "private online explains invitation-only entry");
}

void online_source_and_isolation() {
    using Status = ui::OnlineJoinStatus;
    const std::string code = "0123456789abcdef0123456789abcdef";
    const std::string expired = "1123456789abcdef0123456789abcdef";
    const std::string consumed = "2123456789abcdef0123456789abcdef";
    const std::string unknown = "3123456789abcdef0123456789abcdef";
    eawr::test::ui::FakeOnlineLobbySource source("private-service");
    source.add_invitation(code);
    source.add_invitation(expired, eawr::test::ui::FixtureInvitationState::expired);
    source.add_invitation(consumed, eawr::test::ui::FixtureInvitationState::consumed);
    ui::LobbyJoinSelection selection;
    expect(!selection.begin(ui::OnlineInvitationTarget{source.service(), code}), "LAN rejects online targets");
    const auto lan = selection.begin(ui::LanDiscoveryTarget{sample().front()});
    expect(lan.has_value(), "LAN selection still accepts discovery targets");
    selection.select_source(ui::LobbySourceKind::online);
    expect(!selection.target() && !selection.complete(*lan), "switch drops the LAN target and its completion");
    expect(!selection.begin(ui::LanAddressTarget{code}), "Online rejects direct addresses even if they look like a code");
    expect(!selection.begin(ui::OnlineInvitationTarget{source.service(), "short"}), "bad input does not start an attempt");
    expect(!selection.begin(ui::OnlineInvitationTarget{{}, code}), "an invitation needs an explicit service");
    const auto attempt = selection.begin(ui::OnlineInvitationTarget{source.service(), code});
    expect(attempt.has_value(), "an online attempt starts");
    expect(!selection.begin(ui::OnlineInvitationTarget{source.service(), expired}), "only one join may run at a time");
    source.begin_join(*attempt, std::get<ui::OnlineInvitationTarget>(*selection.target()));
    source.cancel(*attempt);
    selection.cancel();
    source.pump();
    expect(source.take_results().empty(), "cancel before pairing releases the attempt without consuming its code");

    const auto join = [&](const std::string& invitation, const Status expected) {
        const auto id = selection.begin(ui::OnlineInvitationTarget{source.service(), invitation});
        expect(id.has_value(), "retry starts with a fresh generation");
        source.begin_join(*id, std::get<ui::OnlineInvitationTarget>(*selection.target()));
        expect(source.take_results().empty(), "begin never completes synchronously");
        source.pump();
        const auto results = source.take_results();
        expect(results.size() == 1 && results.front().status == expected && results.front().attempt == *id,
               "fake source returns a typed result bound to the request");
        expect(selection.complete(results.front().attempt), "current result is accepted");
        expect(!selection.complete(results.front().attempt), "a duplicate result is ignored");
        const auto notice = ui::online_join_notice(expected);
        expect(!notice.empty() && notice.find(invitation) == std::string_view::npos,
               "notice never echoes the invitation");
    };
    source.set_trusted(false);
    source.set_access_allowed(false);
    join(code, Status::trust_failed);
    source.set_trusted(true);
    join(code, Status::access_denied);
    source.set_access_allowed(true);
    join(code, Status::paired);
    join(code, Status::unavailable);
    join(expired, Status::unavailable);
    join(consumed, Status::unavailable);
    join(unknown, Status::unavailable);

    source.add_invitation(code);
    const auto old = selection.begin(ui::OnlineInvitationTarget{source.service(), code});
    source.begin_join(*old, std::get<ui::OnlineInvitationTarget>(*selection.target()));
    source.pump(); // Result was queued just before the source switch/cancel.
    source.cancel(*old);
    selection.select_source(ui::LobbySourceKind::lan);
    selection.select_source(ui::LobbySourceKind::online);
    const auto fresh = selection.begin(ui::OnlineInvitationTarget{source.service(), unknown});
    const auto stale = source.take_results();
    expect(stale.size() == 1 && !selection.complete(stale.front().attempt), "switch-away-and-back rejects queued success");
    expect(selection.attempt() == fresh, "stale completion cannot cancel the replacement attempt");
    selection.cancel();
    expect(!selection.complete(*fresh), "cancel rejects a late result while staying in Online");

    const auto wrong_service = selection.begin(ui::OnlineInvitationTarget{"other-service", code});
    source.begin_join(*wrong_service, std::get<ui::OnlineInvitationTarget>(*selection.target()));
    source.pump();
    expect(source.take_results().front().status == Status::not_configured, "codes cannot cross the selected service");

    source.add_invitation(code);
    const ui::LobbyAttemptId first{ui::LobbySourceKind::online, 1000};
    const ui::LobbyAttemptId second{ui::LobbySourceKind::online, 1001};
    source.begin_join(first, {source.service(), code});
    source.begin_join(second, {source.service(), unknown});
    source.pump();
    const auto overlapping = source.take_results();
    expect(overlapping.size() == 2, "two begins without cancellation each yield exactly one result");
    expect(overlapping[0].attempt == second && overlapping[0].status == Status::busy,
           "overlap returns a typed busy result for the second request");
    expect(overlapping[1].attempt == first && overlapping[1].status == Status::paired,
           "overlap preserves the first request and its result");
    source.pump();
    expect(source.take_results().empty(), "neither overlapping request produces a duplicate completion");
}

ui::LobbyGame room(std::string name, std::string host, std::uint32_t players, std::uint32_t max, bool locked) {
    ui::LobbyGame value = game(std::move(name), "Kessel", "TEXT_TYPE_A", players, max, 0);
    value.address = std::string(32, 'a');
    value.player_names = {std::move(host)};
    value.passworded = locked;
    return value;
}

void online_rows_and_paging() {
    using Failure = ui::OnlineFailure;
    const ui::LobbyGame locked = room("Night", "Rex", 3, 9, true);
    const ui::LobbyRowText row = ui::online_row_text(locked);
    expect(row.name == "Night" && row.type == "Rex" && row.players == "3/9" && row.map == "Kessel" && row.ping == "?",
           "an online row shows name, host, occupancy over capacity, map and an unknown ping");
    const ui::LobbyGame measured = game("A", "M", "T", 1, 2, 40);
    expect(ui::online_row_text(measured).type.empty() && ui::online_row_text(measured).ping == "40ms",
           "a room without a host name leaves the host cell empty and a measured ping still shows");

    std::vector<ui::LobbyGame> rooms{room("Zulu", "bravo", 1, 2, false), room("Alpha", "Charlie", 1, 2, false),
                                     room("Mike", "alpha", 1, 2, false)};
    std::vector<std::string> by_host;
    for (const auto& value : ui::arrange_games(rooms, {}, ui::LobbySort::host)) by_host.push_back(value.name);
    expect((by_host == std::vector<std::string>{"Mike", "Zulu", "Alpha"}), "the host column sorts by host name, ignoring case");

    auto page = ui::online_page(0, 5);
    expect(page.page == 0 && page.pages == 1 && page.first == 0 && page.count == 0, "an empty list is one empty page");
    page = ui::online_page(25, 1);
    expect(page.page == 1 && page.pages == 3 && page.first == 10 && page.count == 10, "the middle page has a full page of rooms");
    page = ui::online_page(25, 99);
    expect(page.page == 2 && page.first == 20 && page.count == 5, "a page past the end clamps to the last page");
    page = ui::online_page(10, 1);
    expect(page.pages == 1 && page.page == 0 && page.count == 10, "exactly one page of rooms is one page");
    page = ui::online_page(7, 0, 0);
    expect(page.pages == 7 && page.count == 1, "a zero page size cannot divide by zero");

    expect(ui::check_join(&locked, "Me").check == ui::JoinCheck::password_unsupported, "LAN joins still refuse a passworded game");
    expect(ui::check_join(&locked, "Me", true).ok(), "the online list asks for the password instead of refusing");
    ui::LobbyGame full = locked;
    full.players = full.max_players;
    expect(ui::check_join(&full, "Me", true).check == ui::JoinCheck::game_full, "a full private room is full before it asks for a password");
    ui::LobbyGame other = locked;
    other.version_matches = false;
    expect(ui::check_join(&other, "Me", true).check == ui::JoinCheck::version_mismatch, "a private room from another build is refused first");
    expect(ui::check_join(&locked, "rex", true).check == ui::JoinCheck::name_not_unique, "the host's name is a taken name online too");

    expect(ui::classify_online_failure("EAWR-NET-1546") == Failure::wrong_password
               && ui::classify_online_failure("EAWR-NET-1547") == Failure::cooldown
               && ui::classify_online_failure("EAWR-NET-1545") == Failure::full
               && ui::classify_online_failure("EAWR-NET-1544") == Failure::unavailable
               && ui::classify_online_failure("EAWR-NET-1553") == Failure::duplicate_name
               && ui::classify_online_failure("EAWR-NET-1548") == Failure::removed
               && ui::classify_online_failure("EAWR-NET-1552") == Failure::timeout
               && ui::classify_online_failure("EAWR-NET-1549") == Failure::busy
               && ui::classify_online_failure("EAWR-NET-1554") == Failure::password_locked,
           "the service diagnostics map to their player-facing meaning");
    for (const char* unknown : {"", "1546", "EAWR-NET-", "EAWR-NET-15460", "EAWR-NET-15x6", "EAWR-NET-1599", "EAWR-LAN-1546"}) {
        expect(ui::classify_online_failure(unknown) == Failure::other, "an unknown or malformed code is not classified");
    }
    expect(ui::online_failure_text(Failure::wrong_password) == "The password was not accepted.", "the wrong-password text is the typed one");
    expect(ui::online_failure_text(Failure::cooldown) == "Too many password attempts. Wait a minute and try again.",
           "the cooldown text is the typed one");
    expect(ui::online_failure_keeps_prompt(Failure::wrong_password) && ui::online_failure_keeps_prompt(Failure::cooldown)
               && !ui::online_failure_keeps_prompt(Failure::full) && !ui::online_failure_keeps_prompt(Failure::unavailable),
           "only a wrong password or the cooldown keeps the prompt open");
    for (int value = 0; value <= static_cast<int>(Failure::password_locked); ++value) {
        const std::string_view text = ui::online_failure_text(static_cast<Failure>(value));
        expect(!text.empty() && text.find("EAWR") == std::string_view::npos && text.find("0x") == std::string_view::npos,
               "every failure text is curated: no diagnostic id, no address");
    }
    expect(ui::online_list_notice(false, 3).empty() && !ui::online_list_notice(false, 0).empty() && !ui::online_list_notice(true, 3).empty(),
           "the list line explains an empty or unavailable list and stays quiet for rooms");
    expect(!ui::online_chat_closed_notice().empty(), "the chat window says why a line was not sent");
    expect(ui::online_menu_chat_notice(ui::OnlineMenuChat::active).empty()
               && ui::online_menu_chat_notice(ui::OnlineMenuChat::idle) == ui::online_chat_closed_notice()
               && ui::online_menu_chat_notice(ui::OnlineMenuChat::unavailable) == "Lobby chat is not available on this server."
               && !ui::online_menu_chat_notice(ui::OnlineMenuChat::connecting).empty(),
           "the browser's chat window says why a line cannot be sent: connecting, an old service, or no channel");
    expect(ui::online_hosting_notice(ui::OnlineHostingPhase::idle).empty()
               && !ui::online_hosting_notice(ui::OnlineHostingPhase::publishing).empty()
               && ui::online_hosting_notice(ui::OnlineHostingPhase::hosted) == "Your room is listed."
               && !ui::online_hosting_notice(ui::OnlineHostingPhase::retrying).empty()
               && ui::online_hosting_name(ui::OnlineHostingPhase::retrying) == "retrying",
           "the hosting states have their own lines and names");
}

void online_password_and_codes() {
    expect(ui::clamp_room_password("0123456789abcdefXYZ") == "0123456789abcdef", "a password is cut to the edit box's limit");
    expect(ui::clamp_room_password("  sp ace ") == "  sp ace ", "spaces are part of a password and are not trimmed");
    ui::OnlinePasswordPrompt prompt;
    prompt.game = "Night";
    expect(ui::can_submit_password(prompt, "x") && !ui::can_submit_password(prompt, ""), "an empty password is not sent");
    prompt.waiting = true;
    expect(!ui::can_submit_password(prompt, "x"), "no second submit while an attempt is in flight");

    expect(ui::normalize_short_code(" ab1c\t") == "AB1C", "a typed room code is trimmed and upper-cased");
    for (const char* bad : {"", "abc", "abcde", "ab-c", "ab c", "ab\xC3\xA9", "\xEF\xBC\x90" "bcd"}) {
        expect(!ui::normalize_short_code(bad), "a room code is exactly four ASCII letters or digits");
    }
    ui::LobbyJoinSelection selection;
    selection.select_source(ui::LobbySourceKind::online);
    expect(!selection.begin(ui::OnlineInvitationTarget{"online", "abcde"}), "a five-character code starts no attempt");
    const auto attempt = selection.begin(ui::OnlineInvitationTarget{"online", "ab1c"});
    expect(attempt.has_value() && std::get<ui::OnlineInvitationTarget>(*selection.target()).code == "AB1C",
           "the four-character code field starts an online attempt with the canonical code");
    expect(!selection.begin(ui::OnlineInvitationTarget{"online", "zz99"}), "one join at a time");
    expect(selection.complete(*attempt) && !selection.attempt(), "the answer clears the attempt");
    expect(selection.begin(ui::OnlineInvitationTarget{"online", std::string(32, 'a')}).has_value(), "a listed room id starts an attempt too");
}

void lan_menu_chat_acceptance() {
    ui::FixedLobbySource source;
    expect(source.try_send_chat(ui::LobbyChatScope::lobby, {}, "Me", "hello LAN"),
           "an existing source accepts menu chat through the optional refusal seam");
    expect(source.sent().size() == 1 && source.sent()[0].scope == ui::LobbyChatScope::lobby
               && source.sent()[0].game.empty() && source.sent()[0].text == "hello LAN",
           "LAN menu sends remain room-less and reach the selected source exactly once");
    source.queue_chat({ui::LobbyChatScope::lobby, {}, "Peer", "welcome"});
    expect(source.take_chat().size() == 1 && source.take_chat().empty(), "LAN menu arrivals drain once");
    expect(source.try_send_chat(ui::LobbyChatScope::game, "Room", "Me", "room only")
               && source.sent().back().scope == ui::LobbyChatScope::game && source.sent().back().game == "Room",
           "the same source still separates staging chat by room");
}

void online_chat_state() {
    ui::ChatLog log(3);
    log.add(ui::ChatKind::notice, std::string(ui::online_chat_closed_notice()));
    log.add(ui::ChatKind::normal, "Rex: gl", "Rex");
    log.add(ui::ChatKind::error, std::string(ui::online_failure_text(ui::OnlineFailure::wrong_password)));
    log.add(ui::ChatKind::notice, "Finding game...");
    expect(log.lines().size() == 3 && log.total() == 4 && log.lines().front().sender == "Rex",
           "the online chat window is the same bounded log: the oldest line drops when it is full");
    expect(ui::chat_line_to_send("  hi  ") == "hi" && !ui::chat_line_to_send("   "),
           "an online chat line is trimmed and an empty one is not sent");
    ui::FixedLobbySource source;
    source.send_chat(ui::LobbyChatScope::game, "Night", "Me", "hello");
    expect(source.sent().size() == 1 && source.sent().front().game == "Night", "a room chat line goes to the source with its room");
}

} // namespace

int main() {
    list_and_sort();
    filtering();
    row_text();
    join_checks();
    chat_and_names();
    host_options();
    human_team_choices();
    staging_rows();
    start_checks();
    start_enabled();
    fixed_source();
    invitation_targets();
    online_source_and_isolation();
    online_rows_and_paging();
    online_password_and_codes();
    online_chat_state();
    lan_menu_chat_acceptance();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " LAN lobby contract(s) failed\n";
        return 1;
    }
    std::cout << "LAN lobby contracts passed\n";
    return 0;
}
