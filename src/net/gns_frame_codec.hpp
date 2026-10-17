#pragma once

#include "eawr/net/tcp_transport.hpp"

namespace eawr::net {
// Transport state is supplied by the I/O owner. This parser has no GNS or
// socket dependency, so hostile envelopes are checked in default builds too.
struct GnsDecodeContext {
    bool hello_seen{}, accepted{}, peer_close{};
    std::size_t queued_bytes{}, queued_frames{};
};
struct GnsEnvelope {
    TcpMessage kind{TcpMessage::data};
    std::optional<std::vector<std::uint8_t>> frame;
    // Mesh controls (pair handshake and host-stream controls 15..18): the validated body, without the kind byte.
    std::optional<std::vector<std::uint8_t>> control;
};
// N-peer mesh IDs (EAWR-NET-1766..1771, N2 of docs/net-online-npeer.md). 1760..1765 belong to the room fixture and policy.
inline constexpr unsigned gns_error_slot_mismatch = 1766;  // hello/accept roster size or slot differs from the roster
inline constexpr unsigned gns_error_pair_identity = 1767;  // pair secret/identity or connect request does not match the grant
inline constexpr unsigned gns_error_pair_timeout = 1768;   // connected natively but the pair handshake did not finish in time
inline constexpr unsigned gns_error_pair_lost = 1769;      // an open pair closed or timed out
inline constexpr unsigned gns_error_mesh_control = 1770;   // malformed, unexpected or over-quota mesh control
inline constexpr unsigned gns_error_grant_missing = 1771;  // no host grant before the pair deadline, or grant queue overflow
// Reasons a pair can fail with, as members report them and the host forwards them to a refused newcomer: the adapter's
// 1500..1529, 1762 (unreachable) and the mesh pair's 1766..1771.
constexpr bool gns_pair_reason(std::uint32_t code) {
    return (code >= 1500 && code <= 1529) || code == 1762 || (code >= gns_error_slot_mismatch && code <= gns_error_grant_missing);
}
inline constexpr std::size_t gns_pair_secret_size = 16;
inline constexpr std::size_t gns_pending_pair_limit = 8;
class GnsFrameDecoder final {
public:
    // hosting is the listening (accepting) side of this one connection. The two-peer defaults are
    // slot 0 (listener) and slot 1 (dialer); bind() replaces them for a mesh pair and enables mesh controls.
    GnsFrameDecoder(TcpOptions options, bool hosting);
    void bind(std::uint32_t local, std::uint32_t remote);
    core::Result<GnsEnvelope> receive(std::span<const std::uint8_t> packet,
        GnsDecodeContext context, bool reliable = true);
    void reset();
private:
    TcpOptions options_;
    bool hosting_{}, assembling_{}, mesh_{};
    std::uint32_t total_{}, local_{}, remote_{};
    std::vector<std::uint8_t> assembly_;
};
} // namespace eawr::net
