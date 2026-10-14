#pragma once

#include "eawr/net/transport.hpp"
#include "eawr/net/reconnect.hpp"
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace eawr::net {
// Coordinator-reserved transport message kinds. Application frames are wrapped so an
// empty frame and arbitrary first bytes cannot collide with transport control messages.
enum class TcpMessage : std::uint8_t {
    hello = 1, accept = 2, refuse = 3, keepalive = 4, close = 5, data = 6, ping = 7, pong = 8,
    mesh_join = 9, mesh_assign = 10, mesh_roster = 11, mesh_pair = 12,
    mesh_ready = 13, mesh_open = 14
};

struct TcpOptions {
    static constexpr std::uint16_t default_port = 47624;
    std::uint32_t protocol_version{1};
    std::string build_identity; // required; supplied by the caller's build identity provider
    std::uint32_t max_frame_size{1024 * 1024}; // application bytes; wire adds one kind byte
    std::size_t queue_bytes{8 * 1024 * 1024};
    std::size_t queue_frames{4096};
    std::chrono::milliseconds keepalive{1000};
    std::chrono::milliseconds timeout{10000};
    std::chrono::milliseconds handshake_timeout{10000};
    // LAN-8B: peers correspond to stable seat positions, separately from faction owners.
    std::uint32_t peers{2}; // 2..max_match_seats; the default retains the direct two-seat path.
    PeerIndex seat{}; // Joining mesh seat, 1..peers-1; zero requests the first available seat.
    std::string mesh_listen{"0.0.0.0"};
    std::uint16_t mesh_port{}; // zero chooses an ephemeral per-client listener.
    std::string mesh_advertise; // optional numeric address override for routed networks.
};

// Confidential local credential material, never part of the TCP roster/setup.
struct TcpRejoinAuth {
    std::array<std::uint8_t, 32> match_identity{};
    ReconnectTokens seat_tokens;
    std::uint64_t epoch{};
};
struct JoinEndpoint { std::string host; std::uint16_t port{TcpOptions::default_port}; };
// Typed join syntax: host, host:port, [IPv6], [IPv6]:port. Bare IPv6 is also accepted.
core::Result<JoinEndpoint> parse_join_endpoint(std::string_view text);

// Host is peer 0. For N>2 it assigns seat indices and distributes client listener
// endpoints; each pair then opens one reliable stream (full mesh, never a relay).
// Every participant must select the same TcpOptions::peers before joining. Client
// listener ports must be reachable by the other clients; NAT traversal is external.
// Call send/poll/link/close on the same game thread. poll drives non-blocking I/O;
// callers must pump regularly, including while idle and while closing. Work and
// queues are bounded; send reports backpressure without waiting for the network.
// Hostnames and IPv4/IPv6 literals; DNS runs on a resolver thread. Default port 47624; host port 0 chooses an ephemeral port.
// A host listens until a valid join or cancel; failed attempts return it to connecting,
// retaining the attempt diagnostic until another connection arrives. Handshake timeouts
// apply to accepted/connecting sockets; idle timeout counts received bytes on open links.
class TcpTransport final : public MessageTransport {
public:
    static core::Result<std::unique_ptr<TcpTransport>> host(
        std::string address = "0.0.0.0", std::uint16_t port = TcpOptions::default_port, TcpOptions options = {});
    static core::Result<std::unique_ptr<TcpTransport>> connect(
        std::string address, std::uint16_t port = TcpOptions::default_port, TcpOptions options = {});
    ~TcpTransport() override;
    core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> frame) override;
    std::optional<Received> poll() override;
    LinkState link(PeerIndex peer) const override;
    PeerIndex self() const override;
    std::uint32_t peer_count() const override;
    std::optional<LinkStats> link_stats(PeerIndex peer) const override;
    std::uint16_t port() const;
    const std::optional<core::Diagnostic>& diagnostic() const;
    // Ordered close of one admitted edge; discard its application input. Vacant/closed is a no-op success.
    [[nodiscard]] core::Result<void> close_peer(PeerIndex peer);
    [[nodiscard]] std::uint64_t edge_identity(PeerIndex peer) const; // Local generation, never a display name or credential.
    [[nodiscard]] core::Result<void> reopen_staging_listener(); // Two-seat unauthenticated LAN host only; same port.
    void close(); // ordered clean-close frame, flushed by poll; destructor aborts immediately
    // Like poll/link, query only on the current I/O owner's thread. Ownership may
    // transfer between polls after the previous owner has stopped; no internal lock.
    bool listening() const; // I/O-owner observation of a bounded admission listener.
    bool closed() const; // both socket handles released, including an orderly close's drain
    // Explicit recovery owner only: reopen closed edges with the original peer IDs
    // and locally retained topology. Confidential local credentials are required;
    // candidates prove their seat with an OS nonce and HMAC before link/poll/send
    // expose them to the application. Failed candidates keep bounded admission open.
    // Repair errors are retryable by the caller within its agreed hold deadline.
    core::Result<void> repair_closed_links(std::optional<TcpRejoinAuth> auth = {});
    core::Result<void> restart_links(std::optional<TcpRejoinAuth> auth = {});
    void cancel(); // abort a pending host/join; host otherwise listens through failed attempts
    void erase_reconnect_credentials() noexcept; // Terminal match owner only.
private:
    struct Impl;
    struct Mesh;
    explicit TcpTransport(std::unique_ptr<Impl> impl);
    explicit TcpTransport(std::unique_ptr<Mesh> mesh);
    JoinEndpoint reconnect_endpoint_;
    std::optional<TcpRejoinAuth> repair_auth_;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<Impl> staging_retiring_;
    std::unique_ptr<Mesh> mesh_;
};
} // namespace eawr::net
