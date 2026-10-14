#include "gns_frame_codec.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace eawr::net {
namespace {
core::Result<GnsEnvelope> reject(unsigned code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = "EAWR-NET-" + std::to_string(code); diagnostic.message = std::move(message);
    return core::Result<GnsEnvelope>::failure(std::move(diagnostic));
}
std::uint32_t u32(std::span<const std::uint8_t> bytes) {
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[i]) << (i * 8);
    return value;
}
}
GnsFrameDecoder::GnsFrameDecoder(TcpOptions options, bool hosting)
    : options_(std::move(options)), hosting_(hosting) {}
void GnsFrameDecoder::reset() { assembly_ = {}; total_ = 0; assembling_ = false; }
core::Result<GnsEnvelope> GnsFrameDecoder::receive(std::span<const std::uint8_t> packet,
    GnsDecodeContext context, bool reliable) {
    if (!reliable) {
        // A one-byte keepalive is an out-of-band native ACK flush during
        // teardown. It carries no game data and may overtake reliable fragments.
        if (context.accepted && packet.size() == 1 && packet.front() == static_cast<std::uint8_t>(TcpMessage::keepalive))
            return core::Result<GnsEnvelope>::success({TcpMessage::keepalive, {}});
        return reject(1518, "Invalid GNS message delivery mode");
    }
    if (packet.empty()) return reject(1518, "Empty GNS transport envelope");
    const auto kind = static_cast<TcpMessage>(packet.front());
    auto body = packet.subspan(1);
    GnsEnvelope decoded{kind, {}};
    if (kind == TcpMessage::refuse) {
        // Reuse TCP's length-prefixed diagnostic envelope. Refusal is valid
        // before hello, but only the host may refuse a pending admission.
        if (hosting_ || context.accepted || context.peer_close || body.size() < 21 || u32(body) != 13)
            return reject(1518, "Invalid GNS admission refusal");
        const std::string code(reinterpret_cast<const char*>(body.data() + 4), 13);
        const auto length = u32(body.subspan(17));
        if (length > 1024 || body.size() != 21 + static_cast<std::size_t>(length)
            || !code.starts_with("EAWR-NET-15") || code[11] < '0' || code[11] > '2'
            || code[12] < '0' || code[12] > '9') return reject(1518, "Malformed GNS admission refusal");
        return reject(1500U + static_cast<unsigned>(code[11] - '0') * 10U + static_cast<unsigned>(code[12] - '0'),
            std::string(reinterpret_cast<const char*>(body.data() + 21), length));
    } else if (kind == TcpMessage::hello) {
        constexpr std::array<std::uint8_t, 4> magic{'E', 'A', 'W', 'R'};
        if (context.hello_seen || body.size() < 17 || !std::equal(magic.begin(), magic.end(), body.begin()))
            return reject(1518, "Malformed or duplicate GNS hello");
        const auto version = u32(body.subspan(4)), length = u32(body.subspan(8));
        if (length > 1024 || body.size() != static_cast<std::size_t>(length) + 17
            || body.back() != (hosting_ ? 1U : 0U)) return reject(1518, "Malformed GNS identity or role");
        const std::string identity(reinterpret_cast<const char*>(body.data() + 12), length);
        if (version != options_.protocol_version) return reject(1502, "Network protocol versions differ");
        if (identity != options_.build_identity) return reject(1503, "Game build identities differ");
        if (u32(body.subspan(12 + length)) != options_.max_frame_size) return reject(1504, "Transport frame limits differ");
    } else if (kind == TcpMessage::accept) {
        if (!context.hello_seen || context.accepted || body.size() != 4 || u32(body) != 1)
            return reject(1518, "Invalid GNS peer assignment");
    } else if (kind == TcpMessage::data) {
        if (!context.accepted || context.peer_close || body.size() < 8)
            return reject(1518, "Data outside the GNS stream");
        const auto total = u32(body), offset = u32(body.subspan(4));
        body = body.subspan(8);
        if (total > options_.max_frame_size || total > 16 * 1024 * 1024)
            return reject(1504, "GNS application frame exceeds limit");
        if ((!assembling_ && offset != 0) || (assembling_ && (total != total_ || offset != assembly_.size()))
            || offset > total || body.size() > total - offset || (total != 0 && body.empty()))
            return reject(1518, "Invalid GNS frame fragment");
        if (!assembling_) {
            if (context.queued_frames >= options_.queue_frames || context.queued_bytes > options_.queue_bytes
                || total > options_.queue_bytes - context.queued_bytes)
                return reject(1505, "GNS receive queue capacity exceeded");
            total_ = total; assembly_.reserve(total); assembling_ = true;
        }
        assembly_.insert(assembly_.end(), body.begin(), body.end());
        if (assembly_.size() == total_) {
            decoded.frame = std::move(assembly_); assembly_ = {}; assembling_ = false;
        }
    } else if (kind == TcpMessage::keepalive || kind == TcpMessage::close) {
        if (!context.accepted || context.peer_close || assembling_ || !body.empty())
            return reject(1518, "Invalid GNS control frame");
    } else return reject(1518, "Unknown GNS transport envelope");
    return core::Result<GnsEnvelope>::success(std::move(decoded));
}
} // namespace eawr::net
