#pragma once

#include "eawr/net/gns_room.hpp"
#include "eawr/net/gns_signalling.hpp"
#include "eawr/net/online_lobby.hpp"

namespace eawr::net {
struct NetworkIntroductionOptions {
    std::string address;
    std::uint16_t port{27020};
    // Without a pin, verify this name and the chain against ca_file, or the
    // operating system CA store when ca_file is empty. System trust requires a
    // DNS name; IP literals are rejected. Explicit CA-file and pin modes are unchanged.
    std::string certificate_name;
    std::string ca_file;
    // 64 hexadecimal digits of SHA-256 over certificate DER. A nonempty pin
    // selects explicit certificate trust instead of CA/hostname verification,
    // taking precedence even when ca_file and certificate_name are supplied.
    std::string certificate_sha256;
    std::string access_token; // publisher-only; join authorization is the single-use code
    std::chrono::milliseconds timeout{10000};
    // Retry delay after credential refresh failure: >0 and <=1 min; at most six attempts per lease.
    std::chrono::milliseconds renewal_retry{5000};
    std::size_t queue_bytes{256 * 1024};
    std::size_t queue_frames{64};
    // Only public/direct publications may have a guessable invitation alias.
    bool public_alias{true};
};

// One persistent TLS connection. Operational calls block their caller for the
// bounded network exchange, including system-store loading and TLS handshake.
// The caller must run them on a background I/O worker, never a frame/UI thread;
// OnlineLobbyGameSource provides that worker. Construction performs no TLS I/O.
// Calls are serialized. A returned principal is a secret resume capability and
// must never be written to profile, list, chat or diagnostic output.
class NetworkOnlineLobby final : public OnlineLobbyService {
public:
    explicit NetworkOnlineLobby(NetworkIntroductionOptions, std::string resume_principal = {});
    ~NetworkOnlineLobby() override;
    std::string principal() const;
    core::Result<OnlineRoomPage> list(std::string after = {}, std::uint32_t limit = 50) override;
    core::Result<OnlineRoomPublication> publish(OnlineRoomMetadata, std::string password = {}) override;
    core::Result<OnlineRoomPublication> publish_bridge(OnlineRoomMetadata, std::string password, std::string invitation, std::string ownership = {}) override;
    core::Result<void> heartbeat(const OnlineRoomPublication&, const OnlineRoomMetadata&) override;
    core::Result<void> remove(const OnlineRoomPublication&) override;
    core::Result<OnlineRoomAdmission> join(std::string room, std::string password, std::string name) override;
    core::Result<void> leave() override;
    core::Result<void> send_chat(std::string text) override;
    core::Result<OnlineChatPage> chat(std::uint32_t after) override;
    core::Result<void> kick(const OnlineRoomPublication&, std::uint32_t seat) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Optional TLS implementation. The constructor performs no I/O; publish/join
// wait for a bounded admission exchange. Each endpoint owns a network worker;
// send/poll only access copied, bounded queues, including on GNS callbacks.
class NetworkGnsIntroduction final : public GnsIntroductionService {
public:
    static bool available();
    explicit NetworkGnsIntroduction(NetworkIntroductionOptions options);
    core::Result<GnsIntroduction> publish(std::string identity, std::chrono::milliseconds lifetime) override;
    core::Result<std::shared_ptr<GnsSignallingEndpoint>> join(std::string join_code, std::string identity) override;
    static core::Result<void> health(NetworkIntroductionOptions options);
private:
    NetworkIntroductionOptions options_;
};

// One member of an N-peer room over the TLS service (slice N3 of docs/net-online-npeer.md): a connection of its own,
// separate from the directory connection. The factories block for the bounded admission exchange (including the
// per-member TURN credential), so call them from a background I/O worker. After that, send/poll/roster only touch
// copied, bounded queues and are safe on GNS callbacks; one worker thread owns all TLS I/O. Host-only operations
// (release, kick, lock) are refused locally from any other slot. The service sets every signal's source slot and
// identity: nothing a member sends can name another source. Never log the member capability.
//
// Behaviour relative to the in-process fixture (InProcessGnsRoom), for N4's contract tests:
//  * Lock is not gated on completeness; the session (N4) refuses Start while a seat is missing.
//  * A signal to a seat that is not attached is accepted and dropped by the service (counted), never an error and
//    never a disconnect; the fixture returns an error to the caller.
//  * A kick whose denylist entry does not fit (64 identities/principals) removes the member without denying it and the
//    acknowledgement carries EAWR-NET-1777 (last_refusal); the fixture closes the room.
//  * A guest keeps its NetworkOnlineLobby (directory connection) alive until attach() returns: the seat reservation ends
//    with the directory admission.
//  * Kick denies the identity and the directory principal. A client that changes both (a fresh principal and another
//    identity string) is a new player; the denial is a courtesy against accidents, not an identity guarantee.
//  * Destroying the last reference joins the worker, which may wait out a pending connect or DNS lookup: do not drop it on
//    a GNS callback thread.
class NetworkGnsRoom final : public GnsRoomEndpoint {
public:
    static bool available();
    // Host: opens the signalling scope of an already published directory room (principal from NetworkOnlineLobby).
    static core::Result<std::shared_ptr<NetworkGnsRoom>> open(NetworkIntroductionOptions, const OnlineRoomPublication& room,
        std::string principal, std::string identity, bool public_alias = true);
    // Guest holding a directory admission (slot and member capability from OnlineRoomAdmission).
    static core::Result<std::shared_ptr<NetworkGnsRoom>> attach(NetworkIntroductionOptions, const OnlineRoomAdmission& admission, std::string identity);
    // Guest with a public four-character alias: reserves the lowest free seat and attaches in one step.
    static core::Result<std::shared_ptr<NetworkGnsRoom>> attach_alias(NetworkIntroductionOptions, std::string alias, std::string identity);
    // After Start: binds a new connection to a held seat for repair and returns fresh TURN credentials.
    static core::Result<std::shared_ptr<NetworkGnsRoom>> reattach(NetworkIntroductionOptions, std::string room, std::string capability, std::string identity);
    ~NetworkGnsRoom() override;
    std::string alias() const;       // host only, empty when the room has none
    std::string capability() const;  // secret: the key for reattach
    // Signals that arrived while the receive queue was full are dropped and counted (ICE retries); the member stays.
    std::uint64_t dropped_signals() const;
    // Code ("EAWR-NET-1775") of the host's last operation the service acknowledged as not (fully) applied: a repeated
    // lock, a release after Start (1775), a kick whose denial did not fit the bounded denylist (1777). Empty if none.
    std::string last_refusal() const;
    PeerIndex local_slot() const override;
    GnsRoomRoster roster() const override;
    core::Result<void> send(PeerIndex slot, std::span<const std::uint8_t> blob) override;
    core::Result<std::optional<GnsRoomSignal>> poll() override;
    std::optional<GnsTurnCredentials> turn_credentials() const override;
    GnsRoomLease lease() const override;
    core::Result<void> status() const override;
    core::Result<void> leave() override;
    core::Result<void> release(PeerIndex slot) override;
    core::Result<void> kick(PeerIndex slot) override;
    core::Result<void> lock() override;
private:
    struct Impl;
    explicit NetworkGnsRoom(std::unique_ptr<Impl> impl);
    static core::Result<std::shared_ptr<NetworkGnsRoom>> connect(NetworkIntroductionOptions, std::vector<std::uint8_t> request,
        std::string identity, std::optional<std::uint32_t> expected_slot);
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::net
