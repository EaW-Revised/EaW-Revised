#pragma once
#include "lan_match.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/net/network_introduction.hpp"
#include "eawr/net/online_lobby_source.hpp"
#include <atomic>
#include <mutex>

namespace eawr::presentation::godot_backend {
struct OnlineMatchOptions {
    net::NetworkIntroductionOptions service;
    std::string identity;
    std::string build_identity{core::build_identity()};
    // How long a host waits for the directory room it will open the signalling scope on, and the bound on its lease display.
    std::chrono::milliseconds invitation_lifetime{std::chrono::minutes(15)};
    bool short_invitation{};
    // The host's only way to be joined is the four-character room code (the online test kit): a service without room aliases
    // (room_aliases=0) then fails the host at once with EAWR-NET-1787 instead of leaving it waiting for a code that never
    // comes. The lobby screen joins through the room list and leaves this off.
    bool require_short_code{};
    bool turn_only{};
    // Test harness only, empty in production and never set by UI/settings.
    // Called on the I/O owner; retained transports and fault calls stay on that owner.
    std::function<void(net::GnsTransport&)> transport_observer{};
    std::uint64_t hash_exchange_every{30};
    // Host: the directory room the room source published and its owner principal (docs/net-online-npeer.md, section 2),
    // polled on the bridge worker until it exists. hosting_failure reports a publication that cannot succeed (the service
    // refused or is unreachable) so the host fails with that typed diagnostic instead of waiting for the lifetime to pass.
    std::function<std::optional<net::OnlineHostedRoom>()> hosted_room{};
    std::function<std::optional<core::Diagnostic>()> hosting_failure{};
    // Called by the setup I/O owner after the room is attached, before creating GNS. Never logged here.
    std::function<void(const net::GnsTurnCredentials&)> turn_credentials_observer{};
};
namespace online_match_errors {
inline constexpr std::string_view no_room_code = "EAWR-NET-1787"; // the service issues no room codes
}
// Factory performs no I/O. Host/join use a bounded worker, pump_setup creates GNS
// on the setup I/O owner and then uses the shared staging/setup implementation.
std::shared_ptr<LanMatch> make_online_match(OnlineMatchOptions options,
    std::filesystem::path diagnostic_root = {}, std::uint64_t input_delay = 3);

// Private bridge state: cancellation detaches one generation from the UI; the
// registered drain worker owns it until the bounded service call returns.
struct OnlineAdmission {
    std::atomic_bool cancelled{}, finished{}, close_confirmed{};
    mutable std::mutex mutex;
    std::string code;  // the public four-character alias of a host's room, for display
    std::chrono::steady_clock::time_point expires{};
    // The attached room member (docs/net-online-npeer.md, section 3), handed to the setup owner exactly once. A host gets
    // it as soon as its room scope is open: guests then attach while the mesh forms, and the lobby admits them one by one.
    std::shared_ptr<net::NetworkGnsRoom> room;
    std::optional<core::Diagnostic> failure;
    // Host: open the room on the published directory room. Guest: attach with the directory admission (slot and member
    // capability; the room source's directory connection must stay alive until this returns) or, without one, with `alias`.
    void run(OnlineMatchOptions options, bool host, std::string alias, std::optional<net::OnlineRoomAdmission> admission);
};
}
