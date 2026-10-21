#pragma once

#include "eawr/net/gns_signalling.hpp"
#include "eawr/net/transport.hpp"

#include <array>

namespace eawr::net {
enum class GnsRoomState : std::uint8_t { open, locked, closed };
enum class GnsRoomSlotState : std::uint8_t { vacant, reserved, attached, held, left };
struct GnsRoomSlot {
    GnsRoomSlotState state{GnsRoomSlotState::vacant};
    std::string identity;
};
struct GnsRoomRoster {
    std::uint64_t revision{};
    std::uint32_t capacity{};
    GnsRoomState state{GnsRoomState::open};
    std::array<GnsRoomSlot, max_match_seats> slots{};
};
struct GnsRoomSignal {
    PeerIndex from_slot{};
    std::string from_identity;
    std::vector<std::uint8_t> bytes;
};
struct GnsRoomLease {
    std::chrono::steady_clock::time_point expires{};
};
// send can run on a GNS thread: copy the blob, bound the queue, and never
// call GNS while holding a room lock. Roster/source identities are authoritative.
class GnsRoomEndpoint {
public:
    virtual ~GnsRoomEndpoint() = default;
    virtual PeerIndex local_slot() const = 0;
    virtual GnsRoomRoster roster() const = 0;
    virtual core::Result<void> send(PeerIndex slot, std::span<const std::uint8_t> blob) = 0;
    virtual core::Result<std::optional<GnsRoomSignal>> poll() = 0;
    virtual std::optional<GnsTurnCredentials> turn_credentials() const = 0;
    virtual GnsRoomLease lease() const = 0;
    virtual core::Result<void> status() const = 0;
    virtual core::Result<void> leave() = 0;
    virtual core::Result<void> release(PeerIndex slot) = 0;
    virtual core::Result<void> kick(PeerIndex slot) = 0;
    virtual core::Result<void> lock() = 0;
    // Roster-lock protocol 1: atomic compare of revision and attached-seat mask.
    // False is pending; true means the service confirmed exactly that roster.
    virtual core::Result<bool> lock_roster(std::uint32_t, std::uint64_t) {
        core::Diagnostic error; error.code = "EAWR-NET-1764";
        error.message = "The room endpoint does not support roster-lock protocol 1.";
        return core::Result<bool>::failure(std::move(error));
    }
    // Nonblocking repair attachment: call with a new nonzero epoch to request
    // Reattach, then the same epoch to poll. True means that attachment's fresh
    // TURN response is installed (including an explicit no-TURN response).
    // Existing data connections are unaffected. Unsupported endpoints fail closed.
    virtual core::Result<bool> repair_attachment(std::uint64_t) {
        core::Diagnostic error;
        error.code = "EAWR-NET-1791"; error.message = "Room attachment repair is unavailable";
        return core::Result<bool>::failure(std::move(error));
    }
};

// One room, no sockets/authentication/TURN issuance. Explicit slot attachment
// models an admission reservation already granted by the service. Endpoints
// retain the room independently of the fixture lifetime; old attachments cannot
// send after refill. Injectable credentials let adapter tests exercise TURN.
class InProcessGnsRoom final {
public:
    using Clock = std::chrono::steady_clock;
    using Now = Clock::time_point (*)();
    explicit InProcessGnsRoom(std::uint32_t capacity, Now now = Clock::now,
        std::size_t queue_bytes = 256 * 1024, std::size_t queue_signals = 64,
        std::chrono::seconds lifetime = std::chrono::hours(24));
    ~InProcessGnsRoom();
    core::Result<std::shared_ptr<GnsRoomEndpoint>> attach(PeerIndex slot, std::string identity,
        std::optional<GnsTurnCredentials> turn = {});
    // Model a lost member connection. Before Lock, losing the host closes the
    // room; after Lock a member is held and can reattach with the same identity.
    core::Result<void> disconnect(PeerIndex slot);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::net
