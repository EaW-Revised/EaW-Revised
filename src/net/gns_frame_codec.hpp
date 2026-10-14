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
};
class GnsFrameDecoder final {
public:
    GnsFrameDecoder(TcpOptions options, bool hosting);
    core::Result<GnsEnvelope> receive(std::span<const std::uint8_t> packet,
        GnsDecodeContext context, bool reliable = true);
    void reset();
private:
    TcpOptions options_;
    bool hosting_{}, assembling_{};
    std::uint32_t total_{};
    std::vector<std::uint8_t> assembly_;
};
} // namespace eawr::net
