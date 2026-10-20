#pragma once

// The LAN lobby's discovery over UDP (docs/ui/lan-lobby.md LLB-10 to LLB-12): a LobbyGameSource that
// announces the local player's presence and chat to the broadcast address (and the loopback address,
// for a second viewer on this machine) and lists the games and players it hears, through
// net::LanBroadcaster. It is the only place the lobby's model meets the network discovery types.

#include "eawr/net/lan_broadcast.hpp"
#include "eawr/presentation/ui/lan_lobby.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

class BroadcastLobbySource final : public presentation::ui::LobbyGameSource {
public:
    struct Options final {
        // The UDP port this viewer listens on and sends to.
        std::uint16_t port{net::lan_broadcast_port};
        // More ports to send to: another lobby on the same machine, or a subnet the broadcast does not reach.
        std::vector<std::uint16_t> extra_ports;
        bool menu_chat_limits{};
    };

    // Null when the socket cannot be opened; `failure` then says why.
    [[nodiscard]] static std::shared_ptr<BroadcastLobbySource> open(Options options, std::string& failure);

    void announce(const presentation::ui::LobbyPresence& presence) override;
    void send_chat(presentation::ui::LobbyChatScope scope, const std::string& game, const std::string& name,
                   const std::string& text) override;
    [[nodiscard]] bool try_send_chat(presentation::ui::LobbyChatScope scope, const std::string& game,
                                     const std::string& name, const std::string& text) override;
    [[nodiscard]] bool pump() override;
    [[nodiscard]] std::vector<presentation::ui::LobbyGame> games() const override;
    [[nodiscard]] std::vector<presentation::ui::LobbyPlayer> players() const override;
    [[nodiscard]] std::vector<presentation::ui::LobbyPlayer> joining(const std::string& game) const override;
    [[nodiscard]] std::vector<presentation::ui::LobbyChatMessage> take_chat() override;

private:
    explicit BroadcastLobbySource(std::unique_ptr<net::LanBroadcaster> broadcaster)
        : broadcaster_(std::move(broadcaster)) {}

    std::unique_ptr<net::LanBroadcaster> broadcaster_;
};

} // namespace eawr::presentation::godot_backend
