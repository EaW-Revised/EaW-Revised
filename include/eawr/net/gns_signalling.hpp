#pragma once

#include "eawr/core/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::net {
// Pinned GNS generic string identity capacity is 32 bytes including the NUL.
inline constexpr std::size_t gns_identity_max_bytes = 31;

// Service protocol v1: publish a join code, resolve it to this rendezvous,
// then forward opaque GNS signals within this session. ICE candidates live
// inside GNS blobs; these are service operations, not new game message kinds.
struct GnsRendezvous {
    std::string session, local_identity, remote_identity;
};
struct GnsSignal {
    std::string from;
    std::vector<std::uint8_t> bytes;
};

struct GnsTurnCredentials {
    std::string uri;
    std::string username;
    std::string password;
    std::chrono::seconds lifetime{};
};

// Service lease of a paired session (ticket E), in the local monotonic clock. The service renews it before it ends
// until hard_ends; after that the session closes with a typed reason (EAWR-NET-1730).
struct GnsSessionLease {
    std::chrono::steady_clock::time_point ends{};
    std::chrono::steady_clock::time_point hard_ends{};
    bool renewable{true};
    std::uint32_t renewals{};
    // Set when the last renewal attempt was refused (EAWR-NET-1732/1733); the lease keeps its current end.
    std::string last_refusal;
};

// send may run on a GNS background thread. Implementations must copy payloads,
// bound queues and avoid calling GNS under their own locks. poll is game-thread
// only. Each endpoint authorizes exactly the remote identity in its rendezvous.
class GnsSignallingEndpoint {
public:
    virtual ~GnsSignallingEndpoint() = default;
    virtual GnsRendezvous rendezvous() const = 0;
    virtual std::optional<GnsTurnCredentials> turn_credentials() const { return {}; }
    // Lease of the paired session; empty before pairing or when the service does not report one.
    virtual std::optional<GnsSessionLease> lease() const { return {}; }
    // Credentials delivered by the latest renewal, once. The transport cannot apply them to a live connection
    // (docs/gns-transport.md, "Long sessions"); they are delivered so a reconnect or a later transport can.
    virtual std::optional<GnsTurnCredentials> take_refreshed_turn_credentials() { return {}; }
    // Observe service health without consuming GNS signals during admission.
    virtual core::Result<void> status() const { return core::Result<void>::success(); }
    virtual core::Result<void> close_publication() {
        core::Diagnostic error; error.code = "EAWR-NET-1534"; error.message = "Publication close is unavailable.";
        return core::Result<void>::failure(std::move(error));
    }
    virtual core::Result<void> send(std::span<const std::uint8_t> signal) = 0;
    virtual core::Result<std::optional<GnsSignal>> poll() = 0;
};
struct GnsIntroduction {
    std::string join_code;
    std::shared_ptr<GnsSignallingEndpoint> endpoint;
    std::string room_alias{};
    std::chrono::steady_clock::time_point expires{};
    std::string publisher_ownership{}; // host-only binding proof, never given to a guest
};
class GnsIntroductionService {
public:
    virtual ~GnsIntroductionService() = default;
    virtual core::Result<GnsIntroduction> publish(std::string identity, std::chrono::milliseconds lifetime) = 0;
    virtual core::Result<std::shared_ptr<GnsSignallingEndpoint>> join(std::string join_code, std::string identity) = 0;
};

// Test implementation only: predictable join codes, no authentication/network
// service. Expiry uses an injected monotonic clock; both sides retain their
// paired endpoints independently of the service object's lifetime.
class InProcessGnsIntroduction final : public GnsIntroductionService {
public:
    using Clock = std::chrono::steady_clock;
    using Now = Clock::time_point (*)();
    explicit InProcessGnsIntroduction(Now now = Clock::now, std::size_t queue_bytes = 256 * 1024,
        std::size_t queue_signals = 64);
    ~InProcessGnsIntroduction() override;
    core::Result<GnsIntroduction> publish(std::string identity, std::chrono::milliseconds lifetime) override;
    core::Result<std::shared_ptr<GnsSignallingEndpoint>> join(std::string join_code, std::string identity) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::net
