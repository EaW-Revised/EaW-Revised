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
    // The room protocol: the host publishes a plain directory room and opens its signalling scope; no bridge invitation.
    // A different tag keeps two-player bridge rooms of older builds out of this build's list.
    tags.build = options.build_identity; tags.protocol = "online-room-v1";
    return std::make_shared<net::OnlineLobbyGameSource>(std::make_unique<net::NetworkOnlineLobby>(options.service), std::move(tags), false);
}
// The host's match opens the signalling scope of the directory room its source published, and fails with the source's
// typed error when the publication cannot succeed. Weak: whoever owns the source (the lobby screen, a tool) keeps it alive.
inline void bind_online_room_source(OnlineMatchOptions& options, const std::shared_ptr<net::OnlineRoomSource>& source) {
    options.hosted_room = [weak = std::weak_ptr<net::OnlineRoomSource>(source)]() -> std::optional<net::OnlineHostedRoom> {
        const auto locked = weak.lock();
        return locked ? locked->hosted_room() : std::nullopt;
    };
    options.hosting_failure = [weak = std::weak_ptr<net::OnlineRoomSource>(source)]() -> std::optional<core::Diagnostic> {
        const auto locked = weak.lock();
        if (!locked || locked->hosting_state() != net::OnlineHostingState::retrying) return std::nullopt;
        // The source retries a busy or cooling-down service on its own; the match gives up only on what a retry cannot fix.
        auto error = locked->last_error();
        if (!error) return std::nullopt;
        const auto failure = presentation::ui::classify_online_failure(error->code);
        if (failure == presentation::ui::OnlineFailure::busy || failure == presentation::ui::OnlineFailure::cooldown
            || failure == presentation::ui::OnlineFailure::timeout) return std::nullopt;
        return error;
    };
}
// A host without the lobby screen (the online test tool, the process tests): a live directory source that publishes a
// plain room for `seats` humans, bound to the match options. The caller keeps the returned source for the match's lifetime.
inline std::shared_ptr<net::OnlineRoomSource> host_standalone_room(OnlineMatchOptions& options, const std::string& game,
        std::uint32_t seats, const std::string& password = {}) {
    auto source = make_live_online_source(options);
    if (!source->set_host_password(password)) return nullptr;
    presentation::ui::LobbyPresence presence;
    presence.place = presentation::ui::LobbyPlace::hosting; presence.name = "Host"; presence.game = game;
    presence.map = "map-from-data"; presence.game_type = "space"; presence.max_players = seats; presence.passworded = !password.empty();
    source->announce(presence);
    bind_online_room_source(options, source);
    return source;
}
} // namespace eawr::presentation::godot_backend
