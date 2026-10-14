#pragma once

// The socket layer under the LAN lobby (docs/ui/menu-screens.md MNU-26): one UDP socket that sends
// the local player's presence and chat to the broadcast address (and the loopback address, for a
// second viewer on this machine) on the lobby port and any extra ports, and feeds what it receives
// to a LanDirectory. Call pump() once per frame from the thread that owns it; it never blocks.

#include "eawr/net/lan_directory.hpp"

#include <memory>
#include <string>
#include <vector>

namespace eawr::net {

struct LanBroadcastOptions {
    // The port this viewer listens on and sends to.
    std::uint16_t listen_port{lan_broadcast_port};
    // More ports to send to: another lobby on the same machine, or a subnet the broadcast does not reach.
    std::vector<std::uint16_t> extra_ports;
    // Numeric IPv4 address to bind; all interfaces by default.
    std::string bind_address{"0.0.0.0"};
    // What a presence is compared with to say a game's version matches.
    std::uint32_t protocol{1};
    std::string build;
};

class LanBroadcaster {
public:
    // Fails when the socket cannot be opened or the port cannot be bound.
    [[nodiscard]] static core::Result<std::unique_ptr<LanBroadcaster>> open(LanBroadcastOptions options);
    ~LanBroadcaster();
    LanBroadcaster(const LanBroadcaster&) = delete;
    LanBroadcaster& operator=(const LanBroadcaster&) = delete;

    [[nodiscard]] std::uint64_t self() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

    // What to announce from now on; the sender id, protocol and build are filled in. The first
    // pump after a change sends it at once.
    void set_presence(LanPresence presence);
    void send_chat(ChatScope scope, std::string game, std::string name, std::string text);

    // Sends the presence when it is due, reads every pending datagram and drops stale players.
    // True when the lists changed.
    bool pump(LanClock::time_point now);

    [[nodiscard]] LanDirectory& directory() noexcept;
    [[nodiscard]] const LanDirectory& directory() const noexcept;
    // Datagrams received that were not LAN lobby traffic, for the report.
    [[nodiscard]] std::uint64_t ignored_datagrams() const noexcept;

private:
    struct Impl;
    explicit LanBroadcaster(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::net
