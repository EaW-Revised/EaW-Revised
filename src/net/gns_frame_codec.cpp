#include "gns_frame_codec.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
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
    : options_(std::move(options)), hosting_(hosting), local_(hosting ? 0U : 1U), remote_(hosting ? 1U : 0U) {}
void GnsFrameDecoder::bind(std::uint32_t local, std::uint32_t remote) { local_ = local; remote_ = remote; mesh_ = true; }
void GnsFrameDecoder::reset() { assembly_ = {}; total_ = 0; assembling_ = false; }
core::Result<GnsEnvelope> GnsFrameDecoder::receive(std::span<const std::uint8_t> packet,
    GnsDecodeContext context, bool reliable) {
    if (!reliable) {
        // A one-byte keepalive is an out-of-band native ACK flush during
        // teardown. It carries no game data and may overtake reliable fragments.
        if (context.accepted && packet.size() == 1 && packet.front() == static_cast<std::uint8_t>(TcpMessage::keepalive))
            return core::Result<GnsEnvelope>::success({TcpMessage::keepalive, {}, {}});
        return reject(1518, "Invalid GNS message delivery mode");
    }
    if (packet.empty()) return reject(1518, "Empty GNS transport envelope");
    const auto kind = static_cast<TcpMessage>(packet.front());
    auto body = packet.subspan(1);
    GnsEnvelope decoded{kind, {}, {}};
    if (kind == TcpMessage::refuse) {
        // Reuse TCP's length-prefixed diagnostic envelope. Refusal is valid
        // before hello, but only the host may refuse a pending admission.
        if (hosting_ || context.accepted || context.peer_close || body.size() < 21 || u32(body) != 13)
            return reject(1518, "Invalid GNS admission refusal");
        const std::string code(reinterpret_cast<const char*>(body.data() + 4), 13);
        const auto length = u32(body.subspan(17));
        // Reasons the listener may report before admission: the GNS adapter's 1500..1529 and the mesh pair's 1766..1771.
        unsigned number = 0;
        bool digits = code.starts_with("EAWR-NET-");
        for (std::size_t i = 9; digits && i < 13; ++i) {
            digits = code[i] >= '0' && code[i] <= '9'; number = number * 10U + static_cast<unsigned>(code[i] - '0');
        }
        if (length > 1024 || body.size() != 21 + static_cast<std::size_t>(length) || !digits
            || !((number >= 1500 && number <= 1529) || (number >= gns_error_slot_mismatch && number <= gns_error_grant_missing)))
            return reject(1518, "Malformed GNS admission refusal");
        return reject(number, std::string(reinterpret_cast<const char*>(body.data() + 21), length));
    } else if (kind == TcpMessage::hello) {
        constexpr std::array<std::uint8_t, 4> magic{'E', 'A', 'W', 'R'};
        // Layout: magic, version, build length, build, then (revision 2) frame limit, sender slot, roster size, role.
        // Magic, version and build identity come first and are checked before the revision-specific tail, so a peer on
        // another hello revision or build gets the typed mismatch (1502/1503), never a malformed-hello error.
        if (context.hello_seen || body.size() < 12 || !std::equal(magic.begin(), magic.end(), body.begin()))
            return reject(1518, "Malformed or duplicate GNS hello");
        const auto version = u32(body.subspan(4)), length = u32(body.subspan(8));
        if (length > 1024 || body.size() < static_cast<std::size_t>(length) + 12)
            return reject(1518, "Malformed GNS identity or role");
        const std::string identity(reinterpret_cast<const char*>(body.data() + 12), length);
        if (version != options_.protocol_version) return reject(1502, "Network protocol versions differ");
        if (identity != options_.build_identity) return reject(1503, "Game build identities differ");
        if (body.size() != static_cast<std::size_t>(length) + 25 || body.back() != (hosting_ ? 1U : 0U))
            return reject(1518, "Malformed GNS identity or role");
        if (u32(body.subspan(12 + length)) != options_.max_frame_size) return reject(1504, "Transport frame limits differ");
        if (u32(body.subspan(16 + length)) != remote_ || u32(body.subspan(20 + length)) != options_.peers)
            return reject(gns_error_slot_mismatch, "Roster size or seat differs from the room roster");
    } else if (kind == TcpMessage::accept) {
        if (!context.hello_seen || context.accepted || body.size() != 4)
            return reject(1518, "Invalid GNS peer assignment");
        // Both directions carry the dialer's roster position (1 for the two-peer adapter).
        if (u32(body) != (hosting_ ? remote_ : local_))
            return reject(gns_error_slot_mismatch, "Accepted seat differs from the room roster");
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
    } else if (kind == TcpMessage::mesh_pair || (kind >= TcpMessage::mesh_grant && kind <= TcpMessage::mesh_ready_mask)) {
        // Pair handshake and host-stream controls. Only after admission, never inside a fragmented frame.
        if (!mesh_ || !context.accepted || context.peer_close || assembling_)
            return reject(1518, "Mesh control outside a mesh pair");
        const auto bad = [] { return reject(gns_error_mesh_control, "Malformed or misdirected mesh control"); };
        const bool host_remote = remote_ == 0, host_local = local_ == 0;
        const auto seat = [&](std::size_t at) { return u32(body.subspan(at)); };
        if (kind == TcpMessage::mesh_pair) {
            constexpr auto size = gns_pair_secret_size;
            if (host_remote || host_local || body.size() != size + 8 || seat(size) != remote_ || seat(size + 4) != local_) return bad();
        } else if (kind == TcpMessage::mesh_grant) {
            // remote seat, its occupancy epoch, dial flag, pair secret
            if (!host_remote || body.size() != 9 + gns_pair_secret_size || seat(0) == 0 || seat(0) == local_
                || seat(0) >= options_.peers || body[8] > 1) return bad();
        } else if (kind == TcpMessage::mesh_vacated) {
            // seat, its epoch, reason (0 left, 1 could not connect to `other`, 2 removed), other, the failing pair's
            // reason code and whether a relay was offered (both zero unless reason is 1)
            if (!host_remote || body.size() != 18 || seat(0) == 0 || seat(0) >= options_.peers || body[8] > 2
                || (body[8] == 1 && (seat(9) >= options_.peers || seat(9) == seat(0) || !gns_pair_reason(seat(13)) || body[17] > 1))
                || (body[8] != 1 && (seat(9) != 0 || seat(13) != 0 || body[17] != 0))) return bad();
        } else if (kind == TcpMessage::mesh_report) {
            // remote seat, state (0 open, 1 failed, 2 lost), relay offered, reason code, grant secret
            if (!host_local || body.size() != 10 + gns_pair_secret_size || seat(0) == 0 || seat(0) == remote_ || seat(0) >= options_.peers
                || body[4] > 2 || body[5] > 1 || !gns_pair_reason(seat(6))) return bad();
        } else {
            const auto mask = body.size() == 4 ? seat(0) : 0xffffffffU;
            if (!host_local || body.size() != 4 || (mask >> options_.peers) != 0 || (mask & (1U << remote_)) != 0) return bad();
        }
        decoded.control = Bytes(body.begin(), body.end());
    } else if (kind == TcpMessage::keepalive || kind == TcpMessage::close) {
        if (!context.accepted || context.peer_close || assembling_ || !body.empty())
            return reject(1518, "Invalid GNS control frame");
    } else return reject(1518, "Unknown GNS transport envelope");
    return core::Result<GnsEnvelope>::success(std::move(decoded));
}
} // namespace eawr::net
