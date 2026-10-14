#pragma once

// LAN lobby presence and chat (MNU-26, MNU-28, docs/ui/menu-screens.md): the datagrams a viewer on
// the local network sends so the others can list its games and its players and show its chat, and
// the directory that turns received datagrams into those lists. The match itself still runs over
// the TcpTransport; nothing here enters the lockstep, the replay or a world hash.
//
//   - A *presence* datagram says who a player is (name, colour) and where it is: in the lobby,
//     hosting a game (name, map, game type, the TCP port to join, players, slots, password flag)
//     or joining a named game. Players repeat it about once a second; one not heard for a few
//     seconds is gone.
//   - A *chat* datagram carries one line, for the lobby or for one game's staging.
//
// The codec and the directory are engine-free and take the time as an argument, so tests drive
// them with synthetic datagrams and a fake clock. LanBroadcaster (lan_broadcast.hpp) is the thin
// socket layer.

#include "eawr/core/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::net {

using LanClock = std::chrono::steady_clock;

// The UDP port the lobbies listen on and send to unless told otherwise. The match's TCP port is
// separate (TcpOptions::default_port, 47624).
inline constexpr std::uint16_t lan_broadcast_port = 47625;
// Datagrams stay far below an Ethernet MTU.
inline constexpr std::size_t lan_max_datagram = 1200;
// Field limits of the wire format (bytes of UTF-8).
inline constexpr std::size_t lan_max_text = 255;
inline constexpr std::size_t lan_max_chat = 400;
// Players repeat their presence at this interval; one not heard within the timeout is dropped.
inline constexpr std::chrono::milliseconds lan_presence_interval{1000};
inline constexpr std::chrono::milliseconds lan_presence_timeout{5000};

// Bounds on what a LAN peer can make the directory hold. A datagram from a new sender past a bound
// is dropped; entries expire so the room frees up again.
inline constexpr std::size_t lan_max_peers = 256;        // presence entries
inline constexpr std::size_t lan_max_chat_senders = 256; // chat dedupe entries
inline constexpr std::size_t lan_max_games = 64;         // games the lobby lists
inline constexpr std::size_t lan_max_queued_chat = 256;  // chat lines held until taken
inline constexpr std::chrono::milliseconds lan_chat_window{60000};

enum class LanLocation : std::uint8_t { lobby = 0, hosting = 1, joining = 2 };

// What one viewer announces about itself.
struct LanPresence {
    std::uint64_t sender{}; // random per running viewer; its own datagrams are ignored
    std::uint32_t protocol{};
    std::string build; // the build identity; a game from another build is listed but not joinable
    std::string name;
    std::uint32_t colour{};
    LanLocation location{LanLocation::lobby};
    // hosting: the game's name; joining: the name of the game being joined.
    std::string game;
    // hosting only:
    std::string map;
    std::string game_type;
    std::uint16_t tcp_port{};
    std::uint32_t max_players{2};
    bool passworded{};
    // True once the host left the staging for the battle: the game is no longer joinable.
    bool locked{};

    friend bool operator==(const LanPresence&, const LanPresence&) = default;
};

enum class ChatScope : std::uint8_t { lobby = 0, game = 1 };
struct LanChat {
    std::uint64_t sender{};
    std::uint32_t sequence{}; // per sender; a repeat is dropped
    ChatScope scope{ChatScope::lobby};
    std::string game; // the staging the line belongs to when scope is game
    std::string name;
    std::string text;

    friend bool operator==(const LanChat&, const LanChat&) = default;
};

// Text from the network is untrusted: invalid UTF-8 (truncated, overlong, a stray continuation,
// a surrogate, beyond U+10FFFF) and NUL become one U+FFFD per bad sequence, and the result is cut
// at `limit` bytes on a character boundary, never inside a sequence. Used on every text field
// the decoder returns, so nothing the UI receives from a datagram is invalid.
[[nodiscard]] std::string sanitize_lan_text(std::string_view text, std::size_t limit);

[[nodiscard]] std::vector<std::uint8_t> encode_presence(const LanPresence& presence);
[[nodiscard]] std::vector<std::uint8_t> encode_chat(const LanChat& chat);

// One decoded datagram: exactly one of the two is set.
struct LanMessage {
    std::optional<LanPresence> presence;
    std::optional<LanChat> chat;
};
// A datagram that is not ours (wrong magic or version), truncated or oversized is an error; the
// directory drops it.
[[nodiscard]] core::Result<LanMessage> decode_lan_message(std::span<const std::uint8_t> datagram);

// A game some host announces, as the lobby lists it.
struct LanHostedGame {
    std::uint64_t host{};
    std::string name;
    std::string map;
    std::string game_type;
    std::string address; // "<ip>:<tcp port>"
    std::uint32_t players{1}; // the host plus those joining it
    std::uint32_t max_players{2};
    bool passworded{};
    bool version_matches{true};
    bool locked{};
    std::vector<std::string> player_names;
};

// A player on the network, as the lobby's player list shows.
struct LanPlayer {
    std::uint64_t id{};
    std::string name;
    std::uint32_t colour{};
    LanLocation location{LanLocation::lobby};
    std::string game;
    std::string address; // the IPv4 address the datagram came from
};

struct LanDirectoryOptions {
    std::uint64_t self{};
    std::uint32_t protocol{};
    std::string build;
};

class LanDirectory {
public:
    explicit LanDirectory(LanDirectoryOptions options) : options_(std::move(options)) {}

    // Feeds one received datagram. `from` is the sender's dotted IPv4 address. Own datagrams, other
    // builds' malformed ones and repeated chat are dropped; the return is whether it changed the lists.
    bool receive(std::span<const std::uint8_t> datagram, const std::string& from, LanClock::time_point now);
    // Drops players not heard within the timeout and chat dedupe entries older than the window.
    void prune(LanClock::time_point now);

    [[nodiscard]] std::vector<LanPlayer> players() const;
    // At most lan_max_games, oldest first; games_hidden() counts the ones left out.
    [[nodiscard]] std::vector<LanHostedGame> games() const;
    [[nodiscard]] std::size_t games_hidden() const;
    // Chat lines received since the last call, oldest first.
    [[nodiscard]] std::vector<LanChat> take_chat();
    // The remote players currently joining the named game (their names, oldest first).
    [[nodiscard]] std::vector<LanPlayer> joining(const std::string& game) const;

private:
    struct Entry {
        LanPresence presence;
        std::string address;
        LanClock::time_point seen;
        std::uint64_t order{};
    };
    LanDirectoryOptions options_;
    std::map<std::uint64_t, Entry> entries_;
    struct ChatSequence {
        std::uint32_t sequence{};
        LanClock::time_point seen;
    };
    std::map<std::uint64_t, ChatSequence> chat_sequences_;
    std::deque<LanChat> chat_;
    std::uint64_t order_{};
};

// Dotted text of a host-order IPv4 address.
[[nodiscard]] std::string ipv4_text(std::uint32_t address);

} // namespace eawr::net
