#pragma once
#include "lan_match.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/net/network_introduction.hpp"
#include <atomic>
#include <mutex>

namespace eawr::presentation::godot_backend {
struct OnlineMatchOptions {
    net::NetworkIntroductionOptions service;
    std::string identity;
    std::string build_identity{core::build_identity()};
    std::chrono::milliseconds invitation_lifetime{std::chrono::minutes(15)};
    bool short_invitation{};
    bool turn_only{};
    std::uint64_t hash_exchange_every{30};
    // Called by the setup I/O owner after pairing, before creating GNS. Never logged here.
    std::function<void(const net::GnsTurnCredentials&)> turn_credentials_observer{};
};
// Factory performs no I/O. Host/join use a bounded worker, pump_setup creates GNS
// on the setup I/O owner and then uses the shared staging/setup implementation.
std::shared_ptr<LanMatch> make_online_match(OnlineMatchOptions options,
    std::filesystem::path diagnostic_root = {}, std::uint64_t input_delay = 3);

// Private bridge state: cancellation detaches one generation from the UI; the
// registered drain worker owns it until the bounded service call returns.
struct OnlineAdmission {
    std::atomic_bool cancelled{}, finished{}, close_confirmed{};
    mutable std::mutex mutex;
    std::string code;
    std::string bridge_owner;
    std::string bridge_code; // full private capability; never a display field
    std::chrono::steady_clock::time_point expires{};
    std::shared_ptr<net::GnsSignallingEndpoint> paired;
    std::optional<core::Diagnostic> failure;
    void run(OnlineMatchOptions options, bool host, std::string code_to_join);
};
}
