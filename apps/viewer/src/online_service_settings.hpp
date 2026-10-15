#pragma once

#include "online_match.hpp"
#include "eawr/net/online_lobby_source.hpp"
#include "eawr/presentation/ui/settings.hpp"
#include <random>

namespace eawr::presentation::godot_backend {
// Constructors perform no service I/O. The source and match own their bounded
// workers; the frame owner only selects them and copies settings once.
inline core::Result<OnlineMatchOptions> online_match_options(const presentation::ui::NetworkSettings& settings) {
    if (const auto error = presentation::ui::validate_online_settings(settings, true)) return core::Result<OnlineMatchOptions>::failure(*error);
    OnlineMatchOptions options;
    options.service.address = settings.online_host;
    options.service.port = static_cast<std::uint16_t>(settings.online_port);
    options.service.certificate_name = settings.online_host;
    if (settings.online_trust == "pin") options.service.certificate_sha256 = settings.online_pin;
    else if (settings.online_trust == "ca-file") options.service.ca_file = settings.online_ca_file;
    // System trust leaves both overrides empty, even if the saved form has inactive values.
    // Display names can be equal, contain spaces or use non-ASCII text. Peer
    // identities are separate opaque identifiers satisfying the wire grammar.
    try {
        std::random_device random;
        // Four independent 32-bit words retain 128 bits of entropy. Seven
        // base32 digits per word fit the transport's 31-byte identity limit.
        constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        static_assert(28 <= net::gns_identity_max_bytes);
        for (unsigned i = 0; i < 4; ++i) {
            const auto value = static_cast<std::uint32_t>(random());
            for (unsigned digit = 0; digit < 7; ++digit) options.identity += alphabet[(value >> (digit * 5)) & 31U];
        }
    } catch (...) {
        core::Diagnostic error; error.code = "EAWR-NET-1753"; error.message = "Could not initialize the online connection. Try again.";
        return core::Result<OnlineMatchOptions>::failure(std::move(error));
    }
    options.short_invitation = true; // private rooms hide the alias and bind only the full capability
    options.turn_only = settings.online_relay_only;
    return core::Result<OnlineMatchOptions>::success(std::move(options));
}
inline std::shared_ptr<net::OnlineRoomSource> make_live_online_source(const OnlineMatchOptions& options) {
    net::OnlineRoomMetadata tags;
    tags.build = options.build_identity; tags.protocol = "online-two-player-v1";
    return std::make_shared<net::OnlineLobbyGameSource>(std::make_unique<net::NetworkOnlineLobby>(options.service), std::move(tags), true);
}
} // namespace eawr::presentation::godot_backend
