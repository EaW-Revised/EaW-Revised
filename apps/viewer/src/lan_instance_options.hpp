#pragma once

// Real-play LAN flags of one game instance. `--eawr-lan-game-port <port>` is the TCP port this instance hosts on
// (the launcher gives instance n 47624 + 2*(n-1), so two instances on one PC can both host); 0 lets the system pick.
// A missing or malformed value keeps the default port.

#include "viewer_profile_arguments.hpp"

#include "eawr/net/tcp_transport.hpp"

#include <charconv>
#include <cstdint>
#include <string>

namespace eawr::presentation::godot_backend {

[[nodiscard]] inline std::uint16_t lan_game_port_argument() {
    const godot::PackedStringArray arguments = viewer_profile_arguments();
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] != godot::String("--eawr-lan-game-port")) continue;
        const std::string value = arguments[index + 1].utf8().get_data();
        unsigned parsed{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed <= 65535U)
            return static_cast<std::uint16_t>(parsed);
    }
    return net::TcpOptions::default_port;
}

} // namespace eawr::presentation::godot_backend
