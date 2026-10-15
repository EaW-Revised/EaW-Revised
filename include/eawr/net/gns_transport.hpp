#pragma once

#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/gns_signalling.hpp"

namespace eawr::net {
// Path of the established GNS connection: kind of ICE route in use. `unknown` until
// the first sample, and whenever the SDK text names neither a relay nor a direct kind.
enum class GnsRoute { unknown, direct, relayed };
struct GnsOptions : TcpOptions {
    // Qualification/deployment policy: require TURN instead of direct ICE.
    bool turn_only{};
};

// Two-peer GNS adapter, with TcpTransport's message/lifecycle contract. All
// transports in a process share one standalone GNS runtime and must be owned
// by the same I/O thread. poll drives callbacks, control traffic and bounded
// message I/O. GNS itself owns its UDP/background threads. Destruction aborts;
// an orderly close requires pumping both sides until closed().
class GnsTransport final : public MessageTransport {
public:
    static bool available();
    // Route of the most recent open GNS connection in this process, sampled by poll at
    // a one-second cadence. Safe to read from any thread; carries no address.
    static GnsRoute observed_route();
    static core::Result<std::unique_ptr<GnsTransport>> host(std::string address = "0.0.0.0",
        std::uint16_t port = TcpOptions::default_port, GnsOptions options = {});
    static core::Result<std::unique_ptr<GnsTransport>> connect(std::string address,
        std::uint16_t port = TcpOptions::default_port, GnsOptions options = {});
    // Custom signalling uses the rendezvous's generic identities. There is one
    // GNS identity per process; changing it while another adapter is alive is
    // rejected. A rendezvous must be paired before either factory is called.
    static core::Result<std::unique_ptr<GnsTransport>> host_introduced(
        std::shared_ptr<GnsSignallingEndpoint> endpoint, GnsOptions options = {});
    static core::Result<std::unique_ptr<GnsTransport>> connect_introduced(
        std::shared_ptr<GnsSignallingEndpoint> endpoint, GnsOptions options = {});
    ~GnsTransport() override;
    core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> frame) override;
    std::optional<Received> poll() override;
    LinkState link(PeerIndex peer) const override;
    PeerIndex self() const override;
    std::uint32_t peer_count() const override { return 2; }
    std::uint16_t port() const;
    const std::optional<core::Diagnostic>& diagnostic() const;
    // Cached native ping observations, sampled by poll at a one-second cadence.
    // Unknown until the first valid estimate; query on the owning I/O thread.
    std::optional<LinkStats> link_stats(PeerIndex peer) const override;
    std::optional<std::chrono::microseconds> connection_rtt(PeerIndex peer) const;
    void close();
    bool closed() const;
    void cancel();
private:
    struct Impl;
    explicit GnsTransport(std::unique_ptr<Impl> impl);
    static core::Result<std::unique_ptr<GnsTransport>> create(bool hosting, std::string address,
        std::uint16_t port, GnsOptions options, std::shared_ptr<GnsSignallingEndpoint> endpoint);
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::net
