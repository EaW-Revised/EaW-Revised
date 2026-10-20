// LAN lobby presence and chat (docs/ui/menu-screens.md MNU-26, MNU-28): the datagram codec, the
// directory that builds the game and player lists (own datagrams, other builds, joiners, expiry,
// repeated chat) with a fake clock, and two LanBroadcasters exchanging presence and chat over real
// loopback UDP sockets.

#include "../support/check.hpp"
#include "eawr/net/lan_broadcast.hpp"
#include "eawr/net/lan_directory.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace net = eawr::net;
using Clock = net::LanClock;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(const bool condition, const std::string& message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}

net::LanPresence hosting(std::uint64_t sender, std::string name, std::string game) {
    net::LanPresence value;
    value.sender = sender;
    value.protocol = 1;
    value.build = "build-a";
    value.name = std::move(name);
    value.colour = 3;
    value.location = net::LanLocation::hosting;
    value.game = std::move(game);
    value.map = "Corellia";
    value.game_type = "TEXT_GAME_TYPE_SKIRMISH";
    value.tcp_port = 47624;
    value.max_players = 2;
    return value;
}

net::LanDirectoryOptions viewer(std::uint64_t self) { return {self, 1, "build-a"}; }

void codec() {
    net::LanPresence presence = hosting(0x0102030405060708ULL, "Alice \xC3\xA9", "Fleet night");
    presence.passworded = true;
    presence.locked = true;
    auto decoded = net::decode_lan_message(net::encode_presence(presence));
    expect(decoded && decoded.value().presence && *decoded.value().presence == presence,
           "a presence survives the round trip, including flags and UTF-8");

    net::LanChat chat;
    chat.sender = 9;
    chat.sequence = 7;
    chat.scope = net::ChatScope::game;
    chat.game = "Fleet night";
    chat.name = "Alice";
    chat.text = std::string(300, 'x');
    auto line = net::decode_lan_message(net::encode_chat(chat));
    expect(line && line.value().chat && *line.value().chat == chat, "a chat line survives the round trip");

    expect(!net::decode_lan_message({}), "an empty datagram is refused");
    std::vector<std::uint8_t> foreign = net::encode_presence(presence);
    foreign[0] = 'X';
    expect(!net::decode_lan_message(foreign), "a datagram with another magic is refused");
    std::vector<std::uint8_t> other_version = net::encode_presence(presence);
    other_version[4] = 9;
    expect(!net::decode_lan_message(other_version), "a datagram from another wire version is refused");
    std::vector<std::uint8_t> truncated = net::encode_presence(presence);
    truncated.resize(truncated.size() - 3);
    expect(!net::decode_lan_message(truncated), "a truncated presence is refused");
    std::vector<std::uint8_t> padded = net::encode_presence(presence);
    padded.push_back(0);
    expect(!net::decode_lan_message(padded), "a presence with trailing bytes is refused");
    std::vector<std::uint8_t> oversize(net::lan_max_datagram + 1, 0);
    expect(!net::decode_lan_message(oversize), "an oversize datagram is refused");

    net::LanPresence long_name = hosting(5, std::string(400, 'n'), "g");
    auto cut = net::decode_lan_message(net::encode_presence(long_name));
    expect(cut && cut.value().presence && cut.value().presence->name.size() == net::lan_max_text,
           "a name over the field limit is cut, not refused");
}

void directory() {
    net::LanDirectory directory(viewer(1));
    const auto t0 = Clock::time_point{} + std::chrono::seconds(100);

    expect(directory.receive(net::encode_presence(hosting(1, "Me", "My own")), "10.0.0.1", t0) == false
               && directory.games().empty(),
           "the viewer's own datagrams are ignored");
    expect(directory.receive(net::encode_presence(hosting(2, "Alice", "Fleet night")), "10.0.0.2", t0),
           "a new presence changes the lists");
    expect(!directory.receive(net::encode_presence(hosting(2, "Alice", "Fleet night")), "10.0.0.2", t0),
           "a repeated presence changes nothing");
    auto games = directory.games();
    expect(games.size() == 1 && games[0].name == "Fleet night" && games[0].address == "10.0.0.2:47624"
               && games[0].players == 1 && games[0].max_players == 2 && games[0].version_matches
               && games[0].map == "Corellia",
           "a hosting presence is a game at the sender's address and its announced port");

    net::LanPresence joiner;
    joiner.sender = 3;
    joiner.protocol = 1;
    joiner.build = "build-a";
    joiner.name = "Bob";
    joiner.location = net::LanLocation::joining;
    joiner.game = "Fleet night";
    expect(directory.receive(net::encode_presence(joiner), "10.0.0.3", t0), "a joiner is a presence");
    games = directory.games();
    expect(games.size() == 1 && games[0].players == 2 && games[0].player_names.size() == 2
               && games[0].player_names[1] == "Bob",
           "a joiner counts toward the game it names and its name is listed");
    expect(directory.joining("Fleet night").size() == 1 && directory.joining("Fleet night")[0].name == "Bob"
               && directory.joining("Other").empty(),
           "the joiners of one game are listed by game name");

    net::LanPresence stranger = hosting(4, "Carol", "Other build");
    stranger.build = "build-b";
    directory.receive(net::encode_presence(stranger), "10.0.0.4", t0);
    bool found = false;
    for (const auto& game : directory.games()) {
        if (game.name == "Other build") found = !game.version_matches;
    }
    expect(found, "a game from another build is listed and marked as not matching");

    net::LanPresence lobby;
    lobby.sender = 5;
    lobby.protocol = 1;
    lobby.build = "build-a";
    lobby.name = "Dave";
    lobby.location = net::LanLocation::lobby;
    directory.receive(net::encode_presence(lobby), "10.0.0.5", t0);
    expect(directory.players().size() == 4, "every player seen is in the player list, the lobby ones too");

    // Only Alice keeps announcing; the others age out.
    const auto later = t0 + net::lan_presence_timeout / 2;
    directory.receive(net::encode_presence(hosting(2, "Alice", "Fleet night")), "10.0.0.2", later);
    directory.prune(t0 + net::lan_presence_timeout + std::chrono::milliseconds(1));
    expect(directory.players().size() == 1 && directory.players()[0].name == "Alice",
           "a player not heard within the timeout is dropped");
    expect(directory.games()[0].players == 1, "a game loses a joiner that stopped announcing");

    net::LanChat first;
    first.sender = 2;
    first.sequence = 1;
    first.name = "Alice";
    first.text = "hello";
    expect(directory.receive(net::encode_chat(first), "10.0.0.2", later), "a chat line from another player is taken");
    expect(!directory.receive(net::encode_chat(first), "10.0.0.2", later), "the same chat line again is dropped");
    net::LanChat own = first;
    own.sender = 1;
    expect(!directory.receive(net::encode_chat(own), "10.0.0.1", later), "the viewer's own chat is ignored");
    net::LanChat second = first;
    second.sequence = 2;
    second.text = "again";
    directory.receive(net::encode_chat(second), "10.0.0.2", later);
    const auto chat = directory.take_chat();
    expect(chat.size() == 2 && chat[0].text == "hello" && chat[1].text == "again" && directory.take_chat().empty(),
           "chat lines come out oldest first, once");
}

void loopback() {
    // Two viewers on one machine: each listens on its own port and sends to the other's.
    std::uint16_t port_a = 0;
    std::uint16_t port_b = 0;
    // Free ports are found by trying a high range; a taken port makes open() fail, so try the next.
    std::unique_ptr<net::LanBroadcaster> a;
    std::unique_ptr<net::LanBroadcaster> b;
    for (std::uint16_t candidate = 47710; candidate < 47790 && (!a || !b); candidate += 2) {
        net::LanBroadcastOptions first;
        first.listen_port = candidate;
        first.menu_chat_limits = true;
        first.extra_ports = {static_cast<std::uint16_t>(candidate + 1)};
        first.build = "build-a";
        net::LanBroadcastOptions second;
        second.listen_port = static_cast<std::uint16_t>(candidate + 1);
        second.menu_chat_limits = true;
        second.extra_ports = {candidate};
        second.build = "build-a";
        auto opened_a = net::LanBroadcaster::open(first);
        auto opened_b = net::LanBroadcaster::open(second);
        if (opened_a && opened_b) {
            a = std::move(opened_a).value();
            b = std::move(opened_b).value();
            port_a = candidate;
            port_b = static_cast<std::uint16_t>(candidate + 1);
        }
    }
    expect(a && b, "two broadcasters open on a free pair of loopback ports");
    if (!a || !b) return;
    expect(a->port() == port_a && b->port() == port_b && a->self() != b->self(), "each has its own port and id");

    net::LanPresence host = hosting(0, "Alice", "Fleet night");
    a->set_presence(host);
    net::LanPresence lobby;
    lobby.name = "Bob";
    lobby.colour = 5;
    b->set_presence(lobby);

    bool seen_game = false;
    bool seen_player = false;
    bool seen_chat = false;
    bool chat_sent = false;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline && !(seen_game && seen_player && seen_chat)) {
        a->pump(Clock::now());
        b->pump(Clock::now());
        for (const auto& game : b->directory().games()) {
            if (game.name == "Fleet night" && game.address == "127.0.0.1:47624" && game.version_matches) seen_game = true;
        }
        for (const auto& player : a->directory().players()) {
            if (player.name == "Bob" && player.colour == 5) seen_player = true;
        }
        if (seen_game && !chat_sent) {
            b->send_chat(net::ChatScope::lobby, {}, "Bob", "anyone there?");
            chat_sent = true;
        }
        for (const auto& line : a->directory().take_chat()) {
            if (line.text == "anyone there?" && line.name == "Bob") seen_chat = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(seen_game, "the joiner's lobby lists the host's game over UDP");
    expect(seen_player, "the host's player list shows the other lobby player with its colour");
    expect(seen_chat, "a lobby chat line arrives once at the other viewer");
}

} // namespace

// A peer that floods distinct sender ids must not grow the directory without bound, and the
// bounds free up again as entries expire.
void flood() {
    const auto start = Clock::now();
    net::LanDirectory presence_flood(viewer(1));
    for (std::uint64_t sender = 2; sender < 2 + 5000; ++sender) {
        presence_flood.receive(net::encode_presence(hosting(sender, "h", "g" + std::to_string(sender))), "10.0.0.2", start);
    }
    expect(presence_flood.players().size() == net::lan_max_peers, "presence flood is capped at the peer bound");
    expect(presence_flood.games().size() == net::lan_max_games, "listed games are capped");
    expect(presence_flood.games_hidden() == net::lan_max_peers - net::lan_max_games, "the games over the cap are counted");
    // A known peer keeps refreshing while the table is full.
    expect(!presence_flood.receive(net::encode_presence(hosting(2, "h", "g2")), "10.0.0.2", start + std::chrono::seconds(1)),
           "a refresh of a known peer is not a change");
    expect(presence_flood.players().size() == net::lan_max_peers, "a refresh does not grow the table");
    // A new peer is dropped while full, accepted once the old ones expire.
    const auto late = start + net::lan_presence_timeout + std::chrono::seconds(1);
    presence_flood.prune(late);
    expect(presence_flood.players().size() == 1, "the refreshed peer survives the expiry");
    expect(presence_flood.receive(net::encode_presence(hosting(9000, "new", "late")), "10.0.0.3", late),
           "a new peer is accepted after expiry");

    net::LanDirectory chat_flood(viewer(1));
    for (std::uint64_t sender = 2; sender < 2 + 5000; ++sender) {
        net::LanChat chat;
        chat.sender = sender;
        chat.sequence = 1;
        chat.name = "x";
        chat.text = "y";
        chat_flood.receive(net::encode_chat(chat), "10.0.0.2", start);
    }
    expect(chat_flood.take_chat().size() == net::lan_max_chat_senders, "chat from unknown senders is capped");
    net::LanChat repeat;
    repeat.sender = 2;
    repeat.sequence = 1;
    repeat.name = "x";
    repeat.text = "y";
    expect(!chat_flood.receive(net::encode_chat(repeat), "10.0.0.2", start), "a repeat inside the window is dropped");
    const auto after_window = start + net::lan_chat_window + std::chrono::seconds(1);
    chat_flood.prune(after_window);
    net::LanChat fresh;
    fresh.sender = 7000;
    fresh.sequence = 1;
    fresh.name = "x";
    fresh.text = "y";
    expect(chat_flood.receive(net::encode_chat(fresh), "10.0.0.2", after_window),
           "the chat dedupe table frees up after the window");

    // Joiners are counted once per game in one pass: many joiners of many games stay correct.
    net::LanDirectory joiners(viewer(1));
    joiners.receive(net::encode_presence(hosting(2, "Host", "Alpha")), "10.0.0.2", start);
    for (std::uint64_t sender = 10; sender < 14; ++sender) {
        net::LanPresence join = hosting(sender, "J" + std::to_string(sender), "Alpha");
        join.location = net::LanLocation::joining;
        joiners.receive(net::encode_presence(join), "10.0.0.5", start);
    }
    const auto games = joiners.games();
    expect(games.size() == 1 && games[0].players == 5 && games[0].player_names.size() == 5,
           "four joiners are counted for the named game");
}

// Text from the network (#3014): whatever bytes a datagram carries, the decoder returns valid
// UTF-8, cut on a character boundary, and the fields after it stay aligned.
std::vector<std::uint8_t> presence_with_name(const std::string& name) {
    // Encode with a placeholder of the same length, then patch the raw bytes in: the encoder
    // itself sanitises, so a hostile sender is simulated by writing the wire form by hand.
    net::LanPresence presence = hosting(0x77, std::string(name.size(), 'n'), "Fleet night");
    std::vector<std::uint8_t> wire = net::encode_presence(presence);
    const std::string marker(name.size(), 'n');
    for (std::size_t at = 0; at + marker.size() <= wire.size(); ++at) {
        if (std::equal(marker.begin(), marker.end(), wire.begin() + static_cast<std::ptrdiff_t>(at))) {
            std::copy(name.begin(), name.end(), wire.begin() + static_cast<std::ptrdiff_t>(at));
            break;
        }
    }
    return wire;
}

std::string decoded_name(const std::string& raw) {
    const auto message = net::decode_lan_message(presence_with_name(raw));
    return message && message.value().presence ? message.value().presence->name : std::string("<refused>");
}

void hostile_text() {
    const std::string replacement = "\xEF\xBF\xBD";
    expect(net::sanitize_lan_text("plain", 255) == "plain", "ASCII passes unchanged");
    expect(net::sanitize_lan_text("J\xC3\xBCrgen \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x9A\x80", 255)
               == "J\xC3\xBCrgen \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x9A\x80",
           "a non-ASCII name survives");
    expect(net::sanitize_lan_text("ab\xE2\x82", 255) == "ab" + replacement, "a truncated 3-byte sequence is one U+FFFD");
    expect(net::sanitize_lan_text("ab\xF0\x9F", 255) == "ab" + replacement, "a truncated 4-byte sequence is one U+FFFD");
    expect(net::sanitize_lan_text("a\xC0\xAF" "b", 255) == "a" + replacement + replacement + "b",
           "an overlong slash (C0 AF) is refused");
    expect(net::sanitize_lan_text("\xE0\x80\xAF", 255) == replacement + replacement + replacement,
           "an overlong 3-byte form is refused");
    expect(net::sanitize_lan_text("\xED\xA0\x80", 255) == replacement + replacement + replacement, "a surrogate is refused");
    expect(net::sanitize_lan_text("\xF4\x90\x80\x80", 255) == replacement + replacement + replacement + replacement,
           "a code point past U+10FFFF is refused");
    expect(net::sanitize_lan_text("a\x80z", 255) == "a" + replacement + "z", "a stray continuation byte is replaced");
    expect(net::sanitize_lan_text("a\xE2(\xA1z", 255) == "a" + replacement + "(" + replacement + "z",
           "an invalid continuation keeps the byte after it");
    expect(net::sanitize_lan_text(std::string("a\0b", 3), 255) == "a" + replacement + "b", "an embedded NUL is replaced");
    expect(net::sanitize_lan_text("\xFF\xFE", 255) == replacement + replacement, "FE and FF never occur");

    // The cut: a sequence that would pass the limit is dropped whole.
    expect(net::sanitize_lan_text("aa\xC3\xA9", 3) == "aa", "a 2-byte character that does not fit is dropped");
    expect(net::sanitize_lan_text("a\xE6\x97\xA5", 3) == "a", "a 3-byte character cut by the limit is dropped");
    expect(net::sanitize_lan_text("\xF0\x9F\x9A\x80", 3).empty(), "a 4-byte character cut by the limit is dropped");
    expect(net::sanitize_lan_text("\xF0\x9F\x9A\x80", 4) == "\xF0\x9F\x9A\x80", "a character that fits whole stays");
    std::string long_text;
    for (int index = 0; index < 300; ++index) long_text += "\xC3\xA9";
    const std::string cut = net::sanitize_lan_text(long_text, 255);
    expect(cut.size() == 254 && net::sanitize_lan_text(cut, 255) == cut, "a long text is cut on a boundary and stays valid");
    expect(net::sanitize_lan_text("\xFF\xFF\xFF", 4) == replacement, "a replacement counts toward the limit");

    // Through the codec: every field is sanitised, the cut applies at the chat limit, and the
    // fields after a bad one stay aligned.
    expect(decoded_name("Zo\xC3\xAB") == "Zo\xC3\xAB", "a presence name with non-ASCII decodes");
    expect(decoded_name("Zo\xE2\x82") == "Zo" + replacement, "a truncated name is repaired");
    expect(decoded_name(std::string("Zo\0e", 4)) == "Zo" + replacement + "e", "a NUL in a name is replaced");
    auto padded = net::decode_lan_message(presence_with_name("\xC0\xAF"));
    expect(padded && padded.value().presence && padded.value().presence->game == "Fleet night"
               && padded.value().presence->map == "Corellia",
           "the fields after a bad name are intact");

    net::LanChat chat;
    chat.sender = 4;
    chat.sequence = 1;
    chat.name = "Alice";
    chat.text = std::string(net::lan_max_chat - 1, 'x') + "\xC3\xA9";
    auto line = net::decode_lan_message(net::encode_chat(chat));
    expect(line && line.value().chat && line.value().chat->text == std::string(net::lan_max_chat - 1, 'x'),
           "a chat line cut at 400 bytes never splits a character");
    // A long field written by hand (the encoder would cut it): the decoder cuts it too.
    std::vector<std::uint8_t> wire = net::encode_chat(chat);
    const std::size_t length_at = wire.size() - (net::lan_max_chat - 1) - 2; // the u16 size before the text
    std::string oversize(net::lan_max_chat + 50, 'y');
    oversize[net::lan_max_chat - 1] = '\xC3';
    oversize[net::lan_max_chat] = '\xA9';
    wire.resize(length_at);
    wire.push_back(static_cast<std::uint8_t>(oversize.size() & 0xFF));
    wire.push_back(static_cast<std::uint8_t>(oversize.size() >> 8));
    wire.insert(wire.end(), oversize.begin(), oversize.end());
    auto wide = net::decode_lan_message(wire);
    expect(wide && wide.value().chat && wide.value().chat->text == oversize.substr(0, net::lan_max_chat - 1),
           "an over-long chat field is cut on a boundary, not refused and not split");

    // The directory hands the lists nothing invalid.
    net::LanDirectory room(viewer(1));
    expect(room.receive(presence_with_name("Bad\xFF" "Name"), "10.0.0.2", Clock::now()), "a repaired presence is accepted");
    expect(room.players().size() == 1 && room.players()[0].name == "Bad" + replacement + "Name",
           "the player list shows the repaired name");
}

void menu_chat_limits() {
    const auto now = Clock::time_point{};
    net::LanDirectory menu({1, 1, "build-a", true});
    net::LanChat line;
    line.sender = 2; line.sequence = 1; line.name = "  Alice\n "; line.text = "  hello\n ";
    expect(menu.receive(net::encode_chat(line), "10.0.0.2", now), "menu accepts the first line");
    auto taken = menu.take_chat();
    expect(taken.size() == 1 && taken[0].name == "Alice" && taken[0].text == "hello",
           "menu text and names remove controls and surrounding spaces");
    line.sequence = 2;
    expect(!menu.receive(net::encode_chat(line), "10.0.0.2", now + std::chrono::milliseconds(500)),
           "menu still rejects a sender flooding at 500 ms");
    expect(!menu.receive(net::encode_chat(line), "10.0.0.2", now + std::chrono::milliseconds(749)),
           "receive spacing remains bounded just below the jitter allowance");
    auto other = line; other.sender = 3;
    expect(menu.receive(net::encode_chat(other), "10.0.0.3", now), "a different menu sender has its own spacing");
    expect(menu.receive(net::encode_chat(line), "10.0.0.2", now + std::chrono::milliseconds(760)),
           "menu accepts legal one-second sends received 760 ms apart due to network/pump jitter");
    expect(!menu.receive(net::encode_chat(line), "10.0.0.2", now + std::chrono::seconds(2)),
           "rate spacing does not defeat sequence dedupe");
    line.sequence = 3; line.text = std::string(net::lan_menu_max_chat + 1, 'x');
    expect(!menu.receive(net::encode_chat(line), "10.0.0.2", now + std::chrono::seconds(2)),
           "menu refuses oversized text without spending the rate allowance");
    line.text = std::string(net::lan_menu_max_chat - 2, 'x') + "\xC3\xA9";
    line.name = std::string(30, 'n');
    expect(menu.receive(net::encode_chat(line), "10.0.0.2", now + std::chrono::seconds(2)),
           "menu accepts an exact 128-byte UTF-8 line");
    taken = menu.take_chat();
    expect(taken.size() == 3 && taken.back().text == line.text && taken.back().name.size() == net::lan_menu_max_name,
           "menu keeps the exact UTF-8 boundary and clamps a remote name");
    line.sequence = 4; line.text = "\n\t\xC2\x85\xE2\x80\xAE\xE2\x81\xA6";
    expect(!menu.receive(net::encode_chat(line), "10.0.0.2", now + std::chrono::seconds(3)),
           "menu drops empty control and directional-only lines");
    line.scope = net::ChatScope::game; line.text = std::string(300, 'x');
    expect(menu.receive(net::encode_chat(line), "10.0.0.2", now + std::chrono::seconds(2)),
           "game scope retains its existing 400-byte limit and independent timing");
    line.sequence = 5; line.scope = net::ChatScope::lobby; line.text = "hello";
    expect(!menu.receive(net::encode_chat(line), "10.0.0.2", now + std::chrono::seconds(2)),
           "a game line does not reset the menu rate limiter");
    menu.prune(now + net::lan_chat_window + std::chrono::seconds(3));
    expect(menu.receive(net::encode_chat(line), "10.0.0.2", now + net::lan_chat_window + std::chrono::seconds(3)),
           "sender accounting expires and menu chat returns after inactivity");

    net::LanPresence presence = hosting(8, std::string(30, 'p'), "Room");
    menu.receive(net::encode_presence(presence), "10.0.0.8", now);
    expect(menu.players().size() == 1 && menu.players()[0].name.size() == net::lan_menu_max_name,
           "production menu roster names use the same bound as chat names");

    net::LanBroadcastOptions options; options.listen_port = 0; options.menu_chat_limits = true;
    auto opened = net::LanBroadcaster::open(options);
    expect(static_cast<bool>(opened), "menu broadcaster opens using the existing UDP transport");
    if (!opened) return;
    auto sender = std::move(opened).value();
    expect(sender->try_send_chat(net::ChatScope::lobby, {}, "Alice", "first", now), "first outgoing menu line sends");
    expect(!sender->try_send_chat(net::ChatScope::lobby, {}, "Alice", "fast", now), "outgoing menu flood is refused");
    expect(!sender->try_send_chat(net::ChatScope::lobby, {}, "Alice", std::string(129, 'x'), now + std::chrono::seconds(1)),
           "outgoing menu oversized text is refused");
    expect(sender->try_send_chat(net::ChatScope::lobby, {}, "Alice", "next", now + std::chrono::seconds(1)),
           "outgoing menu accepts exactly one second later");
    expect(sender->try_send_chat(net::ChatScope::game, "Room", "Alice", std::string(300, 'x'), now),
           "outgoing room chat keeps its original behavior");
}

int main() {
    codec();
    menu_chat_limits();
    hostile_text();
    directory();
    flood();
    loopback();
    if (failures != 0) {
        std::cerr << failures << " LAN directory contract(s) failed\n";
        return 1;
    }
    std::cout << "LAN directory contracts passed\n";
    return 0;
}
