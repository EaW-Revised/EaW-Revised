#include "gns_transport_signalling.hpp"
#include "gns_log_redact.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>

#if EAWR_HAS_GNS
namespace eawr::net::gns_detail {
constexpr std::size_t gns_log_max_pending = 256;
} // namespace eawr::net::gns_detail
#endif

namespace eawr::net::gns_detail {
core::Diagnostic error(unsigned number, std::string message) {
    core::Diagnostic result;
    result.code = "EAWR-NET-" + std::to_string(number); result.message = std::move(message);
    return result;
}

GnsLogState& gns_log() {
    static GnsLogState state;
    return state;
}

std::string gns_log_line(int seat, int peer, std::string_view body, std::string_view event) {
    const auto since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto day_ms = since_epoch % 86400000;
    char stamp[48];
    std::snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d", static_cast<int>(day_ms / 3600000), static_cast<int>(day_ms / 60000 % 60),
        static_cast<int>(day_ms / 1000 % 60), static_cast<int>(day_ms % 1000));
    std::string line = "EAWR net event=";
    line += event;
    line += " utc=";
    line += stamp;
    if (seat >= 0) line += " seat=" + std::to_string(seat);
    if (peer >= 0) line += " peer=" + std::to_string(peer);
    line += ' ';
    line += body;
    return line;
}

#if EAWR_HAS_GNS
void gns_log_push(int seat, int peer, std::string_view text, bool limited, std::string_view event) {
    auto& log = gns_log();
    if (!log.enabled.load(std::memory_order_relaxed)) return;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
    if (text.empty()) return;
    auto body = redact_log_text(text);
    const std::lock_guard lock(log.mutex);
    if (limited) {
        const auto now = Clock::now();
        if (log.limited.size() >= 32 && !log.limited.contains(body)) log.limited.clear();
        auto& entry = log.limited[body];
        if (entry.first != Clock::time_point{} && now - entry.first < std::chrono::seconds(10)) { ++entry.second; return; }
        if (entry.second) body += " (" + std::to_string(entry.second) + " more since the last one)";
        entry = {now, 0};
    }
    if (log.pending.size() >= gns_log_max_pending) { ++log.dropped; return; }
    log.pending.push_back(gns_log_line(seat, peer, body, event));
    log.has_pending.store(true, std::memory_order_release);
}

#endif
void gns_log_flush() {
    auto& log = gns_log();
    if (!log.has_pending.load(std::memory_order_acquire)) return;
    std::deque<std::string> lines;
    GnsLogSink sink;
    std::size_t dropped = 0;
    {
        const std::lock_guard lock(log.mutex);
        lines.swap(log.pending);
        sink = log.sink;
        dropped = std::exchange(log.dropped, std::size_t{});
        log.has_pending.store(false, std::memory_order_release);
    }
    if (!sink) return;
    // A diagnostic consumer cannot take the transport down.
    try {
        for (const auto& line : lines) sink(line);
        if (dropped) sink(gns_log_line(-1, -1, "lines-dropped=" + std::to_string(dropped)));
    } catch (...) {}
}

#if EAWR_HAS_GNS
void gns_spew(ESteamNetworkingSocketsDebugOutputType type, const char* text) {
    if (!text) return;
    const std::string_view line{text};
    if (type > k_ESteamNetworkingSocketsDebugOutputType_Warning && line.find("TURN") == std::string_view::npos
        && line.find("ICE") == std::string_view::npos) return;
    gns_log_push(gns_log().seat.load(), -1, line, type <= k_ESteamNetworkingSocketsDebugOutputType_Warning);
}

void u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool turn_uri_list(const std::string& list) {
    if (list.empty() || list.size() > 512) return false;
    std::size_t servers = 0, begin = 0;
    for (;;) {
        const auto end = list.find(',', begin);
        const auto item = list.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (item.size() <= 5 || !item.starts_with("turn:") || ++servers > 8
            || std::any_of(item.begin(), item.end(), [](unsigned char ch) { return ch < 33 || ch > 126; })) return false;
        if (end == std::string::npos) return true;
        begin = end + 1;
    }
}

bool stun_uri_list(const std::string& list) {
    if (list.empty()) return true;
    if (list.size() > 512) return false;
    std::size_t servers = 0, begin = 0;
    for (;;) {
        const auto end = list.find(',', begin);
        const auto item = list.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (item.size() <= 5 || !item.starts_with("stun:") || ++servers > 8
            || std::any_of(item.begin(), item.end(), [](unsigned char ch) { return ch < 33 || ch > 126; })) return false;
        if (end == std::string::npos) return true;
        begin = end + 1;
    }
}

bool valid(const GnsOptions& options, bool room) {
    constexpr auto max_ms = std::numeric_limits<std::int32_t>::max();
    return (room ? options.peers >= 2 && options.peers <= max_match_seats && options.pair_timeout.count() > 0
            && options.pair_timeout.count() <= max_ms && options.repair_grace.count() > 0
            && options.repair_grace.count() <= max_ms : options.peers == 2)
        && options.max_frame_size <= 16 * 1024 * 1024
        && options.queue_bytes >= static_cast<std::size_t>(options.max_frame_size) + 9
        && options.queue_bytes <= 128 * 1024 * 1024 && options.queue_frames > 0
        && stun_uri_list(options.stun_servers)
        && !options.build_identity.empty() && options.build_identity.size() <= 1024
        && options.keepalive.count() > 0 && options.timeout > options.keepalive && options.ack_silence_timeout.count() > 0
        && options.timeout.count() <= max_ms && options.handshake_timeout.count() > 0
        && options.handshake_timeout.count() <= max_ms;
}

std::string_view turn_invalid_check(const GnsTurnCredentials& turn) {
    if (turn.uri.empty()) return "uri_empty";
    if (turn.username.empty()) return "user_empty";
    if (turn.password.empty()) return "cred_empty";
    if (!turn_uri_list(turn.uri)) return "uri_list";
    if (turn.username.find(',') != std::string::npos) return "user_comma";
    if (turn.password.find(',') != std::string::npos) return "cred_comma";
    if (turn.lifetime.count() <= 0) return "lifetime_nonpositive";
    return {};
}

bool turn_valid(const GnsTurnCredentials& turn) {
    return turn_invalid_check(turn).empty();
}

void log_turn_refused(int seat, const std::optional<GnsTurnCredentials>& turn) {
    const auto uris = !turn || turn->uri.empty() ? std::size_t{}
        : static_cast<std::size_t>(std::count(turn->uri.begin(), turn->uri.end(), ',')) + 1;
    std::string body = turn ? "reason=invalid check=" : "reason=missing check=";
    body += turn ? turn_invalid_check(*turn) : "credentials_absent";
    body += " uris=" + std::to_string(uris);
    body += !turn || turn->username.empty() ? " user_empty=1" : " user_empty=0";
    body += !turn || turn->password.empty() ? " cred_empty=1" : " cred_empty=0";
    body += " expires_in_s=" + (turn ? std::to_string(turn->lifetime.count()) : "unknown");
    gns_log_push(seat, -1, body, false, "turn-refused");
    // No transport is returned on refusal, so there may never be a poll to deliver this line.
    gns_log_flush();
}

Signal::Signal(std::shared_ptr<SignallingState> state) : state_(std::move(state)) {}

bool Signal::SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* data, int size) {
    if (size <= 0 || size > 64 * 1024) return false;
    if (state_->unavailable.load()) return state_->admitted.load() || state_->refusing.load();
    const std::span<const std::uint8_t> blob{static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size)};
    Bytes wrapped;
    auto wire = blob;
    if (state_->formation_grant) {
        wrapped = encode_room_signal(blob, *state_->formation_grant);
        if (wrapped.empty()) return false;
        wire = wrapped;
    }
    const auto sent = state_->room ? state_->room->send(state_->destination, wire) : state_->endpoint->send(wire);
    if (sent) {
        state_->sent_blobs.fetch_add(1); state_->sent_bytes.fetch_add(static_cast<std::uint64_t>(wire.size()));
    }
    // A signalling outage cannot abort an established data path or its tail.
    if (!sent && (state_->admitted.load() || state_->refusing.load())) { state_->unavailable.store(true); return true; }
    return static_cast<bool>(sent);
}

void Signal::Release() { delete this; }

#endif
} // namespace eawr::net::gns_detail
