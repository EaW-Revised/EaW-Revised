#pragma once

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
} // namespace eawr::net
