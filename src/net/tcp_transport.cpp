#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/lan_directory.hpp"
#include "eawr/net/lan_credentials.hpp"
#include "platform_socket.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <limits>
#include <utility>
#include <charconv>
#include <mutex>
#include <thread>
#include <random>
#include <set>

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
struct EraseBytes {
    Bytes& bytes;
    ~EraseBytes() { erase_reconnect_secret(bytes); }
};
core::Diagnostic error(std::string code, std::string message) {
    core::Diagnostic result;
    result.code = std::move(code);
    result.message = std::move(message);
    return result;
}
void u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::uint32_t u32(std::span<const std::uint8_t> bytes) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[i]) << (i * 8);
    return value;
}
void string(Bytes& bytes, std::string_view text, std::size_t limit) {
    const auto value = sanitize_lan_text(text, limit);
    u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
bool string(std::span<const std::uint8_t>& bytes, std::string& value, std::size_t limit) {
    if (bytes.size() < 4) return false;
    const auto length = u32(bytes);
    bytes = bytes.subspan(4);
    if (length > limit || length > bytes.size()) return false;
    value = sanitize_lan_text(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), length), limit);
    bytes = bytes.subspan(length);
    return true;
}
bool valid(const TcpOptions& options) {
    return options.peers >= 2 && options.peers <= max_match_seats && options.seat < options.peers
        && options.max_frame_size <= 16 * 1024 * 1024
        && options.queue_bytes >= static_cast<std::size_t>(options.max_frame_size) + 5
        && options.queue_frames > 0 && !options.build_identity.empty() && options.build_identity.size() <= 1024
        && options.keepalive.count() > 0 && options.timeout > options.keepalive && options.handshake_timeout.count() > 0;
}
}

struct TcpTransport::Impl {
    struct Resolution { std::mutex mutex; bool done{}; std::vector<socket::Address> addresses; };
    std::shared_ptr<Resolution> resolution;
    std::vector<socket::Address> addresses;
    std::size_t next_address{};
    Clock::time_point attempt_started{};
    TcpOptions options;
    bool hosting{}, socket_ready{}, hello_seen{}, accepted{}, closing{};
    bool send_shutdown{}, peer_close{}, peer_eof{}, cancelled{}, discard_application{};
    std::uint64_t edge_identity{};
    PeerIndex index{};
    LinkState state{LinkState::connecting};
    socket::Handle listener{socket::invalid}, stream{socket::invalid};
    std::uint16_t bound_port{};
    std::optional<core::Diagnostic> failure;
    std::deque<Bytes> outbound;
    std::deque<Received> inbound;
    bool mesh_controls{};
    std::deque<Bytes> controls;
    std::size_t outbound_bytes{}, inbound_bytes{}, written{};
    std::array<std::uint8_t, 4> prefix{};
    std::size_t prefix_size{};
    Bytes payload;
    std::size_t payload_size{};
    Clock::time_point started{Clock::now()}, last_receive{started}, last_write{started};
    Clock::time_point last_probe{started};
    std::optional<Clock::time_point> probe_sent;
    std::optional<std::uint32_t> pending_probe;
    std::uint32_t probe_sequence{};
    LinkStats stats;
    std::optional<TcpRejoinAuth> auth;
    PeerIndex auth_self{}, auth_remote{};
    std::array<std::uint8_t, 32> auth_identity{};
    std::optional<RejoinNonce> auth_nonce;
    bool auth_sent{}, auth_proven{}, auth_verified{};
    std::optional<RejoinNonce> auth_peer_nonce;
    bool authenticated() const { return !auth || auth_proven; }
    void set_auth(const TcpRejoinAuth& credentials, PeerIndex self, PeerIndex remote_peer) {
        auth = credentials; auth_self = self; auth_remote = remote_peer;
        const std::string_view domain = "EAWR-TCP-rejoin-v1";
        Bytes bytes(domain.begin(), domain.end()); bytes.insert(bytes.end(), credentials.match_identity.begin(), credentials.match_identity.end());
        auth_identity = core::sha256(bytes); auth_nonce.reset(); auth_peer_nonce.reset(); auth_sent = auth_proven = auth_verified = false;
    }
    void finish_auth() {
        if (auth_verified && auth_peer_nonce) {
            auth_proven = true; auth_nonce.reset();
            if (hosting) { socket::close(listener); listener = socket::invalid; }
        }
    }
    Bytes auth_frame(std::uint8_t phase, PeerIndex seat, const RejoinNonce& nonce) const {
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::data), rejoin_hello_kind};
        for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(auth->epoch >> (i * 8U)));
        bytes.push_back(phase); u32(bytes, seat + 1);
        bytes.insert(bytes.end(), auth_identity.begin(), auth_identity.end());
        bytes.insert(bytes.end(), nonce.begin(), nonce.end()); return bytes;
    }
    bool auth_message(std::span<const std::uint8_t> bytes) {
        if (!auth) return false;
        if (bytes.size() < 46 || bytes[0] != rejoin_hello_kind
            || !std::equal(auth_identity.begin(), auth_identity.end(), bytes.begin() + 14)) {
            if (!auth_proven) { refuse("EAWR-NET-1016", "Replacement stream has not proved its seat credential"); return true; }
            return false;
        }
        std::uint64_t epoch{};
        for (unsigned i = 0; i < 8; ++i) epoch |= static_cast<std::uint64_t>(bytes[1 + i]) << (i * 8U);
        const auto phase = bytes[9]; const auto seat = u32(bytes.subspan(10));
        if (epoch != auth->epoch || bytes.size() < 78) {
            refuse("EAWR-NET-1016", "Stale or malformed replacement proof"); return true;
        }
        if (auth_proven) return true; // Reliable retransmissions are idempotent after proof.
        RejoinNonce nonce{}; std::copy_n(bytes.begin() + 46, nonce.size(), nonce.begin());
        if (phase == rejoin_link_challenge && seat == auth_self + 1 && bytes.size() == 78) {
            if (auth_peer_nonce) {
                if (nonce != *auth_peer_nonce) refuse("EAWR-NET-1016", "Replacement challenger changed its nonce");
                return true;
            }
            auth_peer_nonce = nonce;
            auto reply = auth_frame(rejoin_link_response, auth_self, nonce);
            const auto tag = rejoin_authenticator(auth->seat_tokens[auth_remote], auth_identity, auth_self + 1, epoch, nonce, false);
            reply.insert(reply.end(), tag.begin(), tag.end()); queue(std::move(reply)); finish_auth(); return true;
        }
        if (phase == rejoin_link_response && seat == auth_remote + 1 && bytes.size() == 110 && auth_nonce && nonce == *auth_nonce) {
            const auto tag = rejoin_authenticator(auth->seat_tokens[auth_remote], auth_identity, seat, epoch, nonce, false);
            std::uint8_t difference{};
            for (std::size_t i = 0; i < tag.size(); ++i) difference |= tag[i] ^ bytes[78 + i];
            if (difference == 0) {
                auth_verified = true; finish_auth();
                return true;
            }
        }
        refuse("EAWR-NET-1016", "Replacement stream seat proof differs"); return true;
    }
    void pump_auth() {
        if (!auth || auth_sent || state != LinkState::open) return;
        auto nonce = make_rejoin_nonce();
        if (!nonce) { refuse("EAWR-NET-1016", nonce.error().message); return; }
        auth_nonce = std::move(nonce).value(); queue(auth_frame(rejoin_link_challenge, auth_remote, *auth_nonce)); auth_sent = true;
    }

    Impl(bool host, TcpOptions config) : options(std::move(config)), hosting(host), index(host ? 0U : 1U) {
        options.build_identity = sanitize_lan_text(options.build_identity, 1024);
    }
    ~Impl() {
        socket::close(stream); socket::close(listener);
        erase_buffers();
    }
    void erase_buffers() noexcept {
        for (auto& frame : outbound) erase_reconnect_secret(frame);
        for (auto& received : inbound) erase_reconnect_secret(received.frame);
        erase_reconnect_secret(payload);
    }
    void erase_credentials() noexcept {
        auth.reset();
        for (auto& frame : outbound)
            if (frame.size() > 5 && frame[4] == static_cast<std::uint8_t>(TcpMessage::data) && frame[5] == lan_credentials_kind)
                erase_reconnect_secret(frame);
        for (auto& received : inbound)
            if (!received.frame.empty() && received.frame[0] == lan_credentials_kind) erase_reconnect_secret(received.frame);
        if (payload.size() > 1 && payload[0] == static_cast<std::uint8_t>(TcpMessage::data) && payload[1] == lan_credentials_kind)
            erase_reconnect_secret(payload);
    }
    PeerIndex remote() const { return hosting ? 1U : 0U; }
    void stop(std::optional<core::Diagnostic> reason = {}) {
        resolution.reset();
        if (reason && !failure) failure = std::move(reason);
        const bool retry = hosting && (!accepted || (auth && !auth_proven)) && !cancelled && listener != socket::invalid;
        state = retry ? LinkState::connecting : LinkState::closed;
        socket::close(stream); stream = socket::invalid;
        if (!retry) { socket::close(listener); listener = socket::invalid; }
        for (auto& frame : outbound) erase_reconnect_secret(frame);
        erase_reconnect_secret(payload);
        outbound.clear(); outbound_bytes = 0; written = 0;
        payload.clear(); payload_size = 0; prefix_size = 0;
        pending_probe.reset(); probe_sent.reset(); stats = {};
        if (retry) {
            for (auto& received : inbound) erase_reconnect_secret(received.frame);
            socket_ready = hello_seen = accepted = closing = send_shutdown = peer_close = peer_eof = false;
            auth_nonce.reset(); auth_peer_nonce.reset(); auth_sent = auth_proven = auth_verified = false; inbound.clear(); inbound_bytes = 0;
            started = Clock::now();
        }
    }
    void queue(Bytes bytes) {
        EraseBytes eraser{bytes};
        Bytes frame(bytes.size() + 4);
        const auto length = static_cast<std::uint32_t>(bytes.size());
        for (unsigned i = 0; i < 4; ++i) frame[i] = static_cast<std::uint8_t>(length >> (i * 8));
        std::copy(bytes.begin(), bytes.end(), frame.begin() + 4);
        outbound_bytes += frame.size();
        outbound.push_back(std::move(frame));
    }
    void refuse(std::string code, std::string reason) {
        if (failure) return;
        failure = error(std::move(code), std::move(reason));
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::refuse)};
        string(bytes, failure->code, 13); string(bytes, failure->message, 1024);
        if (!send_shutdown) queue(std::move(bytes));
        closing = true;
        started = Clock::now();
        state = LinkState::closed;
    }
    void hello() {
        static std::atomic<std::uint64_t> next_edge{1};
        edge_identity = next_edge.fetch_add(1, std::memory_order_relaxed);
        socket_ready = true;
        started = last_receive = last_write = Clock::now();
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::hello), 'E', 'A', 'W', 'R'};
        u32(bytes, options.protocol_version);
        string(bytes, options.build_identity, 1024);
        u32(bytes, options.max_frame_size);
        bytes.push_back(hosting ? 0U : 1U);
        queue(std::move(bytes));
    }
    void message(Bytes bytes) {
        EraseBytes eraser{bytes};
        if (bytes.empty()) { refuse("EAWR-NET-0203", "Empty transport envelope"); return; }
        const auto kind = static_cast<TcpMessage>(bytes.front());
        auto body = std::span<const std::uint8_t>(bytes).subspan(1);
        if (kind == TcpMessage::refuse) {
            std::string code, reason;
            if (!string(body, code, 13) || !string(body, reason, 1024) || !body.empty()
                || (!code.starts_with("EAWR-NET-02") && !code.starts_with("EAWR-NET-080")
                    && !code.starts_with("EAWR-NET-13") && !code.starts_with("EAWR-NET-101")) || code.size() != 13) {
                refuse("EAWR-NET-0203", "Malformed transport refusal"); return;
            }
            if (!failure) failure = error(std::move(code), std::move(reason));
            for (auto& frame : outbound) erase_reconnect_secret(frame);
            outbound.clear(); outbound_bytes = 0; written = 0;
            closing = true; state = LinkState::closed; started = Clock::now(); return;
        }
        if (kind == TcpMessage::hello) {
            constexpr std::array<std::uint8_t, 4> magic{'E', 'A', 'W', 'R'};
            if (hello_seen || body.size() < 8 || !std::equal(magic.begin(), magic.end(), body.begin())) {
                refuse("EAWR-NET-0203", "Malformed or duplicate transport hello"); return;
            }
            const auto version = u32(body.subspan(4));
            body = body.subspan(8);
            std::string identity;
            if (!string(body, identity, 1024) || body.size() != 5
                || body[4] != (hosting ? 1U : 0U)) {
                refuse("EAWR-NET-0203", "Malformed transport identity or peer role"); return;
            }
            if (version != options.protocol_version) {
                refuse("EAWR-NET-0201", "Network protocol versions differ"); return;
            }
            if (identity != options.build_identity) {
                refuse("EAWR-NET-0202", "Game build identities differ"); return;
            }
            if (u32(body) != options.max_frame_size) {
                refuse("EAWR-NET-0204", "Transport frame limits differ"); return;
            }
            hello_seen = true;
            if (hosting) {
                Bytes reply{static_cast<std::uint8_t>(TcpMessage::accept)};
                u32(reply, 1); // host assigns peer 1
                queue(std::move(reply));
            }
        } else if (kind == TcpMessage::accept) {
            if (!hello_seen || accepted || body.size() != 4 || u32(body) != 1) {
                refuse("EAWR-NET-0203", "Invalid transport peer assignment"); return;
            }
            accepted = true;
            if (!hosting) {
                index = u32(body);
                Bytes reply{static_cast<std::uint8_t>(TcpMessage::accept)};
                u32(reply, index); queue(std::move(reply));
            }
            state = LinkState::open;
            if (hosting && authenticated()) { socket::close(listener); listener = socket::invalid; }
        } else if (kind == TcpMessage::data) {
            if (discard_application) return; // Locally removed edge cannot re-enter application polling.
            if (state != LinkState::open && (!closing || failure || peer_close)) {
                refuse("EAWR-NET-0203", "Data outside the open stream"); return;
            }
            if (auth_message(body)) return;
            if (body.size() > options.max_frame_size) { refuse("EAWR-NET-0204", "Application frame exceeds limit"); return; }
            if (inbound.size() >= options.queue_frames || body.size() > options.queue_bytes - inbound_bytes) {
                refuse("EAWR-NET-0205", "Receive queue capacity exceeded"); return;
            }
            inbound_bytes += body.size();
            inbound.push_back({remote(), Bytes(body.begin(), body.end())});
        } else if (kind >= TcpMessage::mesh_join && kind <= TcpMessage::mesh_open && mesh_controls) {
            if (state != LinkState::open || bytes.size() > 2048 || controls.size() >= 16) {
                refuse("EAWR-NET-1301", "Invalid mesh control phase or capacity"); return;
            }
            controls.push_back(std::move(bytes));
        } else if (kind == TcpMessage::ping || kind == TcpMessage::pong) {
            if (body.size() != 4 || (state != LinkState::open && (!closing || failure || peer_close))) {
                refuse("EAWR-NET-0800", "Invalid RTT probe frame"); return;
            }
            if (kind == TcpMessage::ping) {
                // Our ordered close may already be on the wire. Never append a
                // pong after it; the peer will retire its outstanding probe on close.
                if (closing) return;
                // Reserve two nine-byte controls beyond application capacity for
                // crossing probes/replies, including tiny application limits. A valid
                // probe must not close a healthy stream under send backpressure.
                if ((outbound.size() >= options.queue_frames && outbound.size() - options.queue_frames >= 2)
                    || (outbound_bytes > options.queue_bytes && outbound_bytes - options.queue_bytes > 9)) {
                    refuse("EAWR-NET-0801", "RTT reply queue capacity exceeded"); return;
                }
                bytes.front() = static_cast<std::uint8_t>(TcpMessage::pong);
                queue(std::move(bytes));
            } else if (pending_probe && probe_sent && u32(body) == *pending_probe) {
                const auto sample = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - *probe_sent);
                if (stats.sample_count == 0) stats.smoothed_rtt = sample;
                else {
                    const auto delta = sample > stats.last_rtt ? sample - stats.last_rtt : stats.last_rtt - sample;
                    stats.jitter += (delta - stats.jitter) / 4;
                    stats.smoothed_rtt += (sample - stats.smoothed_rtt) / 8;
                }
                stats.last_rtt = sample;
                if (stats.sample_count != std::numeric_limits<std::uint64_t>::max()) ++stats.sample_count;
                pending_probe.reset(); probe_sent.reset();
            } // Unsolicited/stale echoes cannot create samples.
        } else if (kind == TcpMessage::keepalive || kind == TcpMessage::close) {
            if (!body.empty() || (state != LinkState::open && (!closing || failure || peer_close))) {
                refuse("EAWR-NET-0203", "Invalid transport control frame"); return;
            }
            if (kind == TcpMessage::close) {
                peer_close = true;
                if (!closing) {
                    queue({static_cast<std::uint8_t>(TcpMessage::close)});
                    closing = true; state = LinkState::closed; started = Clock::now();
                }
            }
        } else { refuse("EAWR-NET-0203", "Unknown transport message kind"); return; }
        last_receive = Clock::now();
    }
    void flush() {
        std::size_t budget = 256 * 1024;
        for (unsigned calls = 0; calls < 64 && budget > 0 && !outbound.empty(); ++calls) {
            const auto bytes = std::span<const std::uint8_t>(outbound.front()).subspan(written);
            const auto sent = socket::write(stream, bytes.first(std::min(bytes.size(), budget)));
            if (sent.status == socket::Status::pending) break;
            if (sent.status != socket::Status::ready) { stop(error("EAWR-NET-0206", "TCP send failed or peer disconnected")); return; }
            budget -= sent.count;
            written += sent.count;
            last_write = Clock::now();
            if (written == outbound.front().size()) {
                if (outbound.front()[4] == static_cast<std::uint8_t>(TcpMessage::ping)) probe_sent = last_write;
                outbound_bytes -= outbound.front().size(); erase_reconnect_secret(outbound.front()); outbound.pop_front(); written = 0;
            }
        }
        if (closing && outbound.empty() && (failure || peer_close)) {
            // Closing a Winsock socket with unread incoming data can reset the link,
            // discarding an accepted large outgoing frame. Half-close only after all
            // queued bytes enter TCP and the peer acknowledges close. The acknowledgement
            // follows its application tail, allowing receive failures to send a refusal
            // before shutdown. Drain the receive half through EOF before release.
            if (!send_shutdown) {
                if (!socket::shutdown_write(stream)) {
                    stop(error("EAWR-NET-0206", "TCP shutdown failed")); return;
                }
                send_shutdown = true;
            }
            if (peer_eof) stop();
        }
    }
    void receive() {
        std::size_t budget = 256 * 1024;
        for (unsigned calls = 0; calls < 64 && budget > 0 && !peer_eof && stream != socket::invalid; ++calls) {
            if (closing && failure) {
                // After refusing a malformed stream, drain without interpreting further
                // application bytes so the refusal itself is delivered before teardown.
                std::array<std::uint8_t, 4096> discard{};
                const auto read = socket::read(stream, std::span(discard).first(std::min(discard.size(), budget)));
                if (read.status == socket::Status::pending) break;
                if (read.status == socket::Status::closed) { peer_eof = true; if (send_shutdown) stop(); return; }
                if (read.status != socket::Status::ready) { stop(); return; }
                last_receive = Clock::now();
                budget -= read.count; continue;
            }
            auto target = payload.empty() ? std::span<std::uint8_t>(prefix).subspan(prefix_size)
                : std::span<std::uint8_t>(payload).subspan(payload_size);
            const auto read = socket::read(stream, target.first(std::min(target.size(), budget)));
            if (read.status == socket::Status::pending) break;
            if (read.status == socket::Status::closed && closing && peer_close) {
                peer_eof = true; if (send_shutdown) stop(); return;
            }
            if (read.status != socket::Status::ready) { stop(error("EAWR-NET-0206", "TCP peer disconnected")); return; }
            last_receive = Clock::now();
            budget -= read.count;
            if (payload.empty()) {
                prefix_size += read.count;
                if (prefix_size == prefix.size()) {
                    const auto length = u32(prefix);
                    // Control frames have a separate small cap; reject the prefix before allocation.
                    const auto limit = std::max(options.max_frame_size + 1, 2048U);
                    if (length == 0 || length > limit) { refuse("EAWR-NET-0204", "Wire frame length exceeds limit or is zero"); return; }
                    payload.resize(length); payload_size = 0;
                }
            } else {
                payload_size += read.count;
                if (payload_size == payload.size()) {
                    auto bytes = std::move(payload); payload = {}; prefix_size = 0; payload_size = 0;
                    message(std::move(bytes));
                }
            }
        }
    }
    bool next_attempt() {
        socket::close(stream); stream = socket::invalid;
        while (next_address < addresses.size()) {
            stream = socket::connect(addresses[next_address++]);
            if (stream != socket::invalid) { attempt_started = Clock::now(); return true; }
        }
        stop(error("EAWR-NET-0209", "Could not connect to any resolved address. Check the host and port."));
        return false;
    }
    void pump() {
        if (resolution) {
            if (Clock::now() - started >= options.handshake_timeout) {
                resolution.reset(); stop(error("EAWR-NET-0210", "Hostname resolution timed out.")); return;
            }
            {
                const std::lock_guard lock(resolution->mutex);
                if (!resolution->done) return;
                addresses = std::move(resolution->addresses);
            }
            resolution.reset();
            if (addresses.empty()) { stop(error("EAWR-NET-0208", "Could not resolve the hostname or IP address.")); return; }
            started = Clock::now();
            if (!next_attempt()) return;
        }
        if (stream == socket::invalid && listener == socket::invalid) return;
        if (hosting && stream == socket::invalid) {
            stream = socket::accept(listener);
            if (stream == socket::invalid) return; // listening has no keepalive/join timeout
            failure.reset();
            hello();
        }
        const auto now = Clock::now();
        const auto phase_timeout = closing ? (discard_application ? std::min(options.timeout, std::chrono::milliseconds(1000)) : options.timeout) : options.handshake_timeout;
        if ((state != LinkState::open && now - started >= phase_timeout)
            || (state == LinkState::open && ((!authenticated() && now - started >= options.handshake_timeout)
                || (authenticated() && now - last_receive >= options.timeout)))) {
            if (!hosting && !socket_ready && !closing)
                stop(error("EAWR-NET-0209", "Could not connect to any resolved address before the connection deadline."));
            else stop(error("EAWR-NET-0207", "TCP link timed out waiting for the peer"));
            return;
        }
        if (!socket_ready) {
            const auto status = socket::connected(stream);
            const auto attempt_timeout = options.handshake_timeout / static_cast<std::int64_t>(addresses.size());
            if (status == socket::Status::pending && now - attempt_started < attempt_timeout) return;
            if (status != socket::Status::ready) { (void)next_attempt(); return; }
            hello();
        }
        flush();
        if (stream == socket::invalid) return;
        receive();
        if (stream == socket::invalid) return;
        pump_auth();
        // Independent of application writes: busy links need measurements too.
        // One outstanding probe, timed only after its full envelope enters TCP.
        if (state == LinkState::open && !pending_probe && now - last_probe >= options.keepalive
            && outbound.size() < options.queue_frames && outbound_bytes <= options.queue_bytes) {
            pending_probe = ++probe_sequence;
            Bytes bytes{static_cast<std::uint8_t>(TcpMessage::ping)};
            u32(bytes, *pending_probe); queue(std::move(bytes)); last_probe = now;
        }
        if (state == LinkState::open && outbound.empty() && now - last_write >= options.keepalive)
            queue({static_cast<std::uint8_t>(TcpMessage::keepalive)});
        flush();
    }
};

// Reuse the bounded framing, handshake, DNS mailbox and RTT service for every edge.
// Bootstrap controls are never wrapped as application data and never relay inputs.
struct TcpTransport::Mesh {
    TcpOptions options;
    bool hosting{}, assigned{}, roster_seen{}, ready_sent{}, opened{}, stopping{}, repairing{};
    std::string listen_address;
    std::optional<TcpRejoinAuth> auth;
    PeerIndex index{};
    socket::Handle listener{socket::invalid};
    std::uint16_t bound_port{};
    std::optional<core::Diagnostic> failure;
    std::vector<std::unique_ptr<Impl>> links;
    std::vector<std::unique_ptr<Impl>> pending;
    std::vector<bool> confirmed, pair_sent, ready;
    std::vector<JoinEndpoint> endpoints;
    using PairSecret = std::array<std::uint8_t, 16>;
    std::vector<PairSecret> secrets;
    Clock::time_point roster_started{};
    std::size_t cursor{};

    Mesh(bool host, TcpOptions config) : options(std::move(config)), hosting(host), assigned(host),
        index(host ? 0U : options.peers), links(options.peers), confirmed(options.peers),
        pair_sent(options.peers), ready(options.peers), endpoints(options.peers), secrets(options.peers) {}
    ~Mesh() { socket::close(listener); }
    static core::Result<std::unique_ptr<Impl>> dial(std::string address, std::uint16_t port, TcpOptions config) {
        config.peers = 2; config.seat = 0;
        auto connection = TcpTransport::connect(std::move(address), port, std::move(config));
        if (!connection) return core::Result<std::unique_ptr<Impl>>::failure(connection.error());
        auto transport = std::move(connection).value();
        auto result = std::move(transport->impl_);
        result->mesh_controls = true;
        return core::Result<std::unique_ptr<Impl>>::success(std::move(result));
    }
    void fail(std::string message) {
        if (!failure) failure = error("EAWR-NET-1301", std::move(message));
        stopping = true;
        socket::close(listener); listener = socket::invalid;
        for (auto& link : links) if (link) { link->cancelled = true; link->stop(); }
        pending.clear();
    }
    bool queue(Impl& link, Bytes bytes, bool admission = false) {
        if (bytes.size() > 2048 || link.outbound.size() >= options.queue_frames
            || link.outbound_bytes > options.queue_bytes || bytes.size() + 4 > options.queue_bytes - link.outbound_bytes) {
            if (admission) link.refuse("EAWR-NET-1301", "Mesh candidate control queue capacity exceeded");
            else fail("Mesh control queue capacity exceeded");
            return false;
        }
        link.queue(std::move(bytes)); return true;
    }
    static bool read_word(std::span<const std::uint8_t>& bytes, std::uint32_t& value) {
        if (bytes.size() < 4) return false;
        value = u32(bytes); bytes = bytes.subspan(4); return true;
    }
    void assignment(Impl& candidate, std::span<const std::uint8_t> body) {
        std::uint32_t count{}, seat{}, port{};
        std::string address;
        if (!read_word(body, count) || !read_word(body, seat) || !read_word(body, port)
            || !string(body, address, 128) || !body.empty() || count != options.peers
            || seat >= count || port == 0 || port > 65535) {
            candidate.refuse("EAWR-NET-1300", "Invalid mesh roster size, seat or endpoint"); return;
        }
        if (seat == 0) for (PeerIndex peer = 1; peer < options.peers; ++peer)
            if (!links[peer]) { seat = peer; break; }
        if (seat == 0 || links[seat]) { candidate.refuse("EAWR-NET-1302", "Mesh seat is already occupied"); return; }
        if (address.empty()) address = socket::peer_address(candidate.stream);
        if (address.empty() || address.size() > 128 || address.find_first_of(" \t\r\n[]") != std::string::npos) {
            candidate.refuse("EAWR-NET-1300", "Cannot determine the mesh endpoint"); return;
        }
        Bytes reply{static_cast<std::uint8_t>(TcpMessage::mesh_assign)};
        u32(reply, options.peers); u32(reply, seat);
        if (!queue(candidate, std::move(reply), true)) return;
        endpoints[seat] = {std::move(address), static_cast<std::uint16_t>(port)};
        candidate.index = seat; // Bootstrap stream's external seat; inner peer roles remain 0/1.
        confirmed[seat] = true;
    }
    Bytes pair(PeerIndex to) const {
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_pair)};
        bytes.insert(bytes.end(), secrets[to].begin(), secrets[to].end()); u32(bytes, index); u32(bytes, to); return bytes;
    }
    bool pair_identity(std::span<const std::uint8_t> body, PeerIndex& from) const {
        constexpr auto size = PairSecret{}.size();
        return body.size() == size + 8 && (from = u32(body.subspan(size))) > index && from < options.peers
            && u32(body.subspan(size + 4)) == index
            && std::equal(secrets[from].begin(), secrets[from].end(), body.begin());
    }
    void roster(std::span<const std::uint8_t> body) {
        std::uint32_t count{};
        if (!assigned || roster_seen || !read_word(body, count) || count != options.peers) {
            fail("Invalid or duplicate mesh roster"); return;
        }
        for (PeerIndex peer = 1; peer < count; ++peer) {
            std::uint32_t port{};
            if (!string(body, endpoints[peer].host, 128) || endpoints[peer].host.empty()
                || endpoints[peer].host.find_first_of(" \t\r\n[]") != std::string::npos
                || !read_word(body, port) || port == 0 || port > 65535) {
                fail("Invalid distributed mesh endpoint"); return;
            }
            endpoints[peer].port = static_cast<std::uint16_t>(port);
        }
        constexpr auto secret_size = PairSecret{}.size();
        if (body.size() != count * secret_size) { fail("Invalid mesh pair secrets"); return; }
        for (auto& secret : secrets) {
            std::copy_n(body.begin(), secret.size(), secret.begin()); body = body.subspan(secret.size());
        }
        roster_seen = true; roster_started = Clock::now(); confirmed[0] = true;
        for (PeerIndex peer = 1; peer < index; ++peer) {
            auto connected = dial(endpoints[peer].host, endpoints[peer].port, options);
            if (!connected) { fail(connected.error().message); return; }
            links[peer] = std::move(connected).value();
        }
    }
    void control(PeerIndex from, Bytes bytes) {
        const auto kind = static_cast<TcpMessage>(bytes.front());
        auto body = std::span<const std::uint8_t>(bytes).subspan(1);
        if (!hosting && from == 0 && kind == TcpMessage::mesh_assign) {
            std::uint32_t count{}, seat{};
            if (assigned || !read_word(body, count) || !read_word(body, seat) || !body.empty()
                || count != options.peers || seat == 0 || seat >= count || (options.seat != 0 && seat != options.seat)) {
                fail("Invalid mesh seat assignment"); return;
            }
            index = seat; assigned = true;
        } else if (!hosting && from == 0 && kind == TcpMessage::mesh_roster) roster(body);
        else if ((!hosting || repairing) && (from != 0 || repairing) && kind == TcpMessage::mesh_pair) {
            constexpr auto size = PairSecret{}.size();
            if (!roster_seen || confirmed[from] || body.size() != size + 8
                || !std::equal(secrets[from].begin(), secrets[from].end(), body.begin())
                || u32(body.subspan(size)) != from || u32(body.subspan(size + 4)) != index) {
                fail("Mesh pair identity does not match the assigned seats"); return;
            }
            confirmed[from] = true;
        } else if (hosting && kind == TcpMessage::mesh_ready && roster_seen && !opened && !ready[from] && body.empty()) ready[from] = true;
        else if (!hosting && from == 0 && kind == TcpMessage::mesh_open && ready_sent && !opened && body.empty()) {
            opened = true; socket::close(listener); listener = socket::invalid;
        } else fail("Unexpected mesh control, sender or phase");
    }
    void pump() {
        if (stopping) {
            for (auto& link : links) if (link) link->pump();
            return;
        }
        if (listener != socket::invalid && pending.size() < options.peers) {
            const auto stream = socket::accept(listener);
            if (stream != socket::invalid) {
                auto candidate = std::make_unique<Impl>(true, options);
                candidate->stream = stream; candidate->mesh_controls = true; candidate->hello();
                pending.push_back(std::move(candidate));
            }
        }
        for (auto& candidate : pending) {
            if (!candidate) continue;
            candidate->pump();
            // TCP keepalives do not extend the deadline to supply mesh identity.
            // Unbound refusals use that same short drain bound, rather than the
            // much longer idle timeout a viewer allows for scene loading.
            if (candidate->stream != socket::invalid && Clock::now() - candidate->started >= options.handshake_timeout) {
                if (candidate->closing) { candidate->cancelled = true; candidate->stop(); }
                else candidate->refuse("EAWR-NET-1301", "Mesh admission timed out; use the hosted human seat count");
            }
            if (candidate->stream == socket::invalid) continue;
            if (!repairing && !candidate->inbound.empty()) { candidate->refuse("EAWR-NET-1301", "Application data before mesh identity"); continue; }
            if (candidate->closing) continue;
            if (repairing && candidate->auth) {
                // Impl::message consumes proof frames before queuing application
                // data. Keep post-proof data, including the tail of the same read.
                if (candidate->auth_proven && !candidate->closing) {
                    const auto seat = candidate->auth_remote;
                    if (links[seat] && links[seat]->state != LinkState::closed) {
                        candidate->refuse("EAWR-NET-1016", "Replacement seat already has a proved stream"); continue;
                    }
                    confirmed[seat] = true; links[seat] = std::move(candidate);
                }
                continue;
            }
            if (candidate->controls.empty()) continue;
            // A higher seat can learn the roster and dial us before our host
            // stream delivers it. Keep the bounded candidate behind its admission
            // deadline until we have the secret needed to verify that pair.
            if (!hosting && !roster_seen
                && static_cast<TcpMessage>(candidate->controls.front().front()) == TcpMessage::mesh_pair) continue;
            auto bytes = std::move(candidate->controls.front()); candidate->controls.pop_front();
            if (!candidate->controls.empty()) { candidate->refuse("EAWR-NET-1301", "Duplicate mesh admission controls"); continue; }
            const auto kind = static_cast<TcpMessage>(bytes.front());
            auto body = std::span<const std::uint8_t>(bytes).subspan(1);
            if (hosting && !roster_seen && kind == TcpMessage::mesh_join) {
                assignment(*candidate, body);
                if (!stopping && !candidate->closing) {
                    const auto seat = candidate->index; links[seat] = std::move(candidate);
                }
            } else if (roster_seen && (!hosting || repairing) && kind == TcpMessage::mesh_pair) {
                PeerIndex from{};
                if (!pair_identity(body, from) || (links[from] && links[from]->state != LinkState::closed)) { candidate->refuse("EAWR-NET-1302", "Invalid or occupied mesh pair"); continue; }
                if (repairing) {
                    if (!auth) { candidate->refuse("EAWR-NET-1016", "Confidential repair credentials are unavailable"); continue; }
                    candidate->set_auth(*auth, index, from);
                    // A dialer may coalesce its first challenge with mesh_pair.
                    // Classify this pre-binding queue once under the new auth
                    // owner; ordinary application data still fails before proof.
                    while (!candidate->inbound.empty() && !candidate->closing) {
                        auto frame = std::move(candidate->inbound.front()); candidate->inbound.pop_front();
                        candidate->inbound_bytes -= frame.frame.size(); (void)candidate->auth_message(frame.frame);
                    }
                    if (candidate->closing) continue;
                    if (!queue(*candidate, pair(from), true)) continue;
                    candidate->pump_auth();
                } else if (queue(*candidate, pair(from), true)) { confirmed[from] = true; links[from] = std::move(candidate); }
            } else candidate->refuse("EAWR-NET-1301", "Unexpected mesh admission");
            if (stopping) return;
        }
        std::erase_if(pending, [](const auto& candidate) { return !candidate || candidate->stream == socket::invalid; });
        for (PeerIndex peer = 0; peer < options.peers; ++peer) if (links[peer]) {
            auto& link = *links[peer]; link.pump();
            if (link.auth) {
                // Replacement edges already retain their seat/roster. Bootstrap
                // controls cannot alter or fail that mesh, even before proof.
                link.controls.clear();
                if (link.authenticated() && link.state == LinkState::open) confirmed[peer] = true;
            } else {
                while (!link.controls.empty()) {
                    auto bytes = std::move(link.controls.front()); link.controls.pop_front();
                    control(peer, std::move(bytes)); if (stopping) return;
                }
            }
            // An opened peer may send before this client's host stream delivers Open.
            // Keep the edge's already bounded queue behind the local readiness gate.
            if (!opened && !link.inbound.empty() && (!roster_seen || !confirmed[peer])) {
                fail("Application data before mesh identity"); return;
            }
            if (link.state == LinkState::closed && !opened) {
                if (link.failure && !failure) failure = link.failure;
                fail("Peer disconnected while building the mesh"); return;
            }
        }
        if (!hosting && !repairing && links[0] && links[0]->state == LinkState::open && !pair_sent[0]) {
            Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_join)};
            u32(bytes, options.peers); u32(bytes, options.seat); u32(bytes, bound_port);
            string(bytes, options.mesh_advertise, 128);
            if (!queue(*links[0], std::move(bytes))) return;
            pair_sent[0] = true;
        }
        if (hosting && !roster_seen && std::all_of(links.begin() + 1, links.end(), [](const auto& link) { return !!link; })) {
            std::random_device random;
            std::vector<PairSecret> pairs(options.peers * options.peers);
            for (PeerIndex first = 1; first < options.peers; ++first)
                for (PeerIndex second = first + 1; second < options.peers; ++second) {
                    auto& secret = pairs[first * options.peers + second];
                    for (auto& byte : secret) byte = static_cast<std::uint8_t>(random());
                    pairs[second * options.peers + first] = secret;
                }
            Bytes roster{static_cast<std::uint8_t>(TcpMessage::mesh_roster)}; u32(roster, options.peers);
            for (PeerIndex peer = 1; peer < options.peers; ++peer) { string(roster, endpoints[peer].host, 128); u32(roster, endpoints[peer].port); }
            for (PeerIndex peer = 1; peer < options.peers; ++peer) {
                auto bytes = roster;
                for (PeerIndex remote = 0; remote < options.peers; ++remote) {
                    const auto& secret = pairs[peer * options.peers + remote];
                    bytes.insert(bytes.end(), secret.begin(), secret.end());
                }
                if (!queue(*links[peer], std::move(bytes))) return;
            }
            roster_seen = true; roster_started = Clock::now(); ready[0] = true;
            socket::close(listener); listener = socket::invalid;
        }
        if ((!hosting || repairing) && roster_seen) {
            for (PeerIndex peer = repairing ? 0U : 1U; peer < index; ++peer)
                if (links[peer] && links[peer]->state == LinkState::open && !pair_sent[peer]) {
                    if (!queue(*links[peer], pair(peer))) return;
                    pair_sent[peer] = true;
                }
            bool complete = true;
            for (PeerIndex peer = 0; peer < options.peers; ++peer)
                if (peer != index && (!links[peer] || !confirmed[peer] || links[peer]->state != LinkState::open)) complete = false;
            // Repair reuses the roster, not the initial host readiness barrier.
            // In particular, seat zero has no links[0] to send mesh_ready on.
            if (!repairing && complete && !ready_sent) {
                if (!queue(*links[0], {static_cast<std::uint8_t>(TcpMessage::mesh_ready)})) return;
                ready_sent = true;
            }
        }
        if (hosting && roster_seen && !opened && std::all_of(ready.begin(), ready.end(), [](bool value) { return value; })) {
            for (PeerIndex peer = 1; peer < options.peers; ++peer)
                if (!queue(*links[peer], {static_cast<std::uint8_t>(TcpMessage::mesh_open)})) return;
            opened = true;
        }
        if (repairing) {
            bool complete = true;
            for (PeerIndex peer = 0; peer < options.peers; ++peer) if (peer != index)
                complete = complete && links[peer] && confirmed[peer] && links[peer]->authenticated() && links[peer]->state == LinkState::open;
            if (complete) { socket::close(listener); listener = socket::invalid; repairing = false; }
        }
        if (roster_seen && !opened && Clock::now() - roster_started >= options.handshake_timeout) fail("Whole-mesh readiness timed out");
    }
};

core::Result<JoinEndpoint> parse_join_endpoint(std::string_view text) {
    const auto fail = [] { return core::Result<JoinEndpoint>::failure(error("EAWR-NET-0211",
        "Enter a hostname, host:port, [IPv6], or [IPv6]:port (port 1 to 65535).")); };
    if (text.empty() || text.find_first_of(" \t\r\n") != std::string_view::npos || text.find('\0') != std::string_view::npos) return fail();
    JoinEndpoint result;
    std::string_view host = text, port;
    if (text.front() == '[') {
        const auto end = text.find(']');
        if (end == std::string_view::npos) return fail();
        host = text.substr(1, end - 1);
        if (host.find(':') == std::string_view::npos) return fail();
        const auto tail = text.substr(end + 1);
        if (!tail.empty()) { if (tail.front() != ':' || tail.size() == 1) return fail(); port = tail.substr(1); }
    } else if (const auto colon = text.find(':'); colon != std::string_view::npos && colon == text.rfind(':')) {
        host = text.substr(0, colon); port = text.substr(colon + 1);
        if (port.empty()) return fail();
    }
    if (host.empty() || host.find_first_of("[]") != std::string_view::npos) return fail();
    if (!port.empty()) {
        unsigned value{}; const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || value == 0 || value > 65535) return fail();
        result.port = static_cast<std::uint16_t>(value);
    }
    result.host = host;
    return core::Result<JoinEndpoint>::success(std::move(result));
}

TcpTransport::TcpTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TcpTransport::TcpTransport(std::unique_ptr<Mesh> mesh) : mesh_(std::move(mesh)) {}
TcpTransport::~TcpTransport() = default;
void TcpTransport::erase_reconnect_credentials() noexcept {
    repair_auth_.reset();
    if (impl_) impl_->erase_credentials();
    if (mesh_) {
        mesh_->auth.reset();
        for (auto& edge : mesh_->links) if (edge) edge->erase_credentials();
        for (auto& candidate : mesh_->pending) if (candidate) candidate->erase_credentials();
    }
}
core::Result<std::unique_ptr<TcpTransport>> TcpTransport::host(std::string address, std::uint16_t port, TcpOptions options) {
    if (!valid(options)) return core::Result<std::unique_ptr<TcpTransport>>::failure(error("EAWR-NET-0200", "Invalid TCP transport options"));
    if (options.peers > 2) {
        if (options.seat != 0) return core::Result<std::unique_ptr<TcpTransport>>::failure(error("EAWR-NET-1300", "Host must occupy mesh seat zero"));
        auto mesh = std::make_unique<Mesh>(true, std::move(options));
        const auto listener = socket::listen(address, port);
        if (listener.handle == socket::invalid) return core::Result<std::unique_ptr<TcpTransport>>::failure(error("EAWR-NET-0206", "Cannot listen on the mesh host address and port"));
        mesh->listener = listener.handle; mesh->bound_port = listener.port; mesh->listen_address = address;
        return core::Result<std::unique_ptr<TcpTransport>>::success(std::unique_ptr<TcpTransport>(new TcpTransport(std::move(mesh))));
    }
    auto impl = std::make_unique<Impl>(true, std::move(options));
    const auto listener = socket::listen(address, port);
    if (listener.handle == socket::invalid) return core::Result<std::unique_ptr<TcpTransport>>::failure(error("EAWR-NET-0206", "Cannot listen on the IP address and port"));
    impl->listener = listener.handle; impl->bound_port = listener.port;
    auto result = std::unique_ptr<TcpTransport>(new TcpTransport(std::move(impl)));
    result->reconnect_endpoint_ = {std::move(address), result->port()};
    return core::Result<std::unique_ptr<TcpTransport>>::success(std::move(result));
}
core::Result<std::unique_ptr<TcpTransport>> TcpTransport::connect(std::string address, std::uint16_t port, TcpOptions options) {
    if (!valid(options) || port == 0) return core::Result<std::unique_ptr<TcpTransport>>::failure(error("EAWR-NET-0200", "Invalid TCP transport options or port"));
    if (options.peers > 2) {
        if (options.mesh_advertise.size() > 128 || sanitize_lan_text(options.mesh_advertise, 128) != options.mesh_advertise)
            return core::Result<std::unique_ptr<TcpTransport>>::failure(error("EAWR-NET-1300", "Invalid advertised mesh address"));
        auto mesh = std::make_unique<Mesh>(false, options);
        const auto listener = socket::listen(options.mesh_listen, options.mesh_port);
        if (listener.handle == socket::invalid) return core::Result<std::unique_ptr<TcpTransport>>::failure(error("EAWR-NET-1303", "Cannot listen for mesh peer pairs"));
        mesh->listener = listener.handle; mesh->bound_port = listener.port;
        mesh->listen_address = options.mesh_listen; mesh->endpoints[0] = {address, port};
        auto bootstrap = Mesh::dial(std::move(address), port, std::move(options));
        if (!bootstrap) return core::Result<std::unique_ptr<TcpTransport>>::failure(bootstrap.error());
        mesh->links[0] = std::move(bootstrap).value();
        return core::Result<std::unique_ptr<TcpTransport>>::success(std::unique_ptr<TcpTransport>(new TcpTransport(std::move(mesh))));
    }
    auto impl = std::make_unique<Impl>(false, std::move(options));
    if (address.empty() || address.find_first_of(" \t\r\n[]") != std::string::npos || address.find('\0') != std::string::npos)
        return core::Result<std::unique_ptr<TcpTransport>>::failure(error("EAWR-NET-0211", "Enter a hostname or IP address."));
    impl->bound_port = port;
    impl->resolution = std::make_shared<Impl::Resolution>();
    const auto reconnect_address = address;
    // The resolver owns only its result mailbox: cancellation and destruction never wait for DNS.
    try {
        std::thread([mailbox = impl->resolution, address = std::move(address), port] {
            std::vector<socket::Address> resolved;
            try { resolved = socket::resolve(address, port); } catch (...) { /* Report as resolution failure. */ }
            const std::lock_guard lock(mailbox->mutex);
            mailbox->addresses = std::move(resolved); mailbox->done = true;
        }).detach();
    } catch (const std::system_error&) {
        return core::Result<std::unique_ptr<TcpTransport>>::failure(error("EAWR-NET-0212", "Could not start hostname resolution."));
    }
    auto result = std::unique_ptr<TcpTransport>(new TcpTransport(std::move(impl)));
    result->reconnect_endpoint_ = {reconnect_address, port};
    return core::Result<std::unique_ptr<TcpTransport>>::success(std::move(result));
}
core::Result<void> TcpTransport::send(PeerIndex to, std::span<const std::uint8_t> frame) {
    if (mesh_) {
        if (to >= mesh_->options.peers || to == mesh_->index)
            return core::Result<void>::failure(error("EAWR-NET-1300", "Invalid mesh destination peer"));
        if (!mesh_->opened || mesh_->stopping || !mesh_->links[to] || !mesh_->links[to]->authenticated() || mesh_->links[to]->state != LinkState::open)
            return core::Result<void>::failure(error("EAWR-NET-1304", "Mesh peer stream is not open"));
        auto& edge = *mesh_->links[to];
        if (frame.size() > edge.options.max_frame_size)
            return core::Result<void>::failure(error("EAWR-NET-0204", "Application frame exceeds limit"));
        if (edge.outbound.size() >= edge.options.queue_frames || edge.outbound_bytes > edge.options.queue_bytes
            || frame.size() + 5 > edge.options.queue_bytes - edge.outbound_bytes)
            return core::Result<void>::failure(error("EAWR-NET-0205", "Send queue capacity exceeded; pump and retry"));
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::data)};
        bytes.insert(bytes.end(), frame.begin(), frame.end()); edge.queue(std::move(bytes));
        return core::Result<void>::success();
    }
    if (to != impl_->remote()) return core::Result<void>::failure(error("EAWR-NET-0200", "Invalid destination peer"));
    if (impl_->state != LinkState::open || !impl_->authenticated()) return core::Result<void>::failure(impl_->failure.value_or(error("EAWR-NET-0206", "TCP link is not open")));
    if (frame.size() > impl_->options.max_frame_size) return core::Result<void>::failure(error("EAWR-NET-0204", "Application frame exceeds limit"));
    if (impl_->outbound.size() >= impl_->options.queue_frames || impl_->outbound_bytes >= impl_->options.queue_bytes
        || frame.size() + 5 > impl_->options.queue_bytes - impl_->outbound_bytes)
        return core::Result<void>::failure(error("EAWR-NET-0205", "Send queue capacity exceeded; pump and retry"));
    Bytes bytes{static_cast<std::uint8_t>(TcpMessage::data)};
    bytes.insert(bytes.end(), frame.begin(), frame.end()); impl_->queue(std::move(bytes));
    return core::Result<void>::success();
}
std::optional<Received> TcpTransport::poll() {
    if (staging_retiring_) {
        staging_retiring_->pump();
        if (staging_retiring_->stream == socket::invalid || Clock::now() - staging_retiring_->started >= std::chrono::seconds(1)) {
            staging_retiring_->stop(); staging_retiring_.reset();
        }
    }
    if (mesh_) {
        mesh_->pump();
        if (!mesh_->opened || mesh_->stopping) return {};
        for (std::uint32_t count = 0; count < mesh_->options.peers; ++count) {
            const auto peer = static_cast<PeerIndex>(mesh_->cursor++ % mesh_->options.peers);
            if (!mesh_->links[peer] || !mesh_->links[peer]->authenticated() || mesh_->links[peer]->discard_application || mesh_->links[peer]->inbound.empty()) continue;
            auto& edge = *mesh_->links[peer];
            auto result = std::move(edge.inbound.front()); edge.inbound.pop_front();
            edge.inbound_bytes -= result.frame.size(); result.from = peer; return result;
        }
        return {};
    }
    impl_->pump();
    if (impl_->discard_application || impl_->inbound.empty()) return {};
    auto result = std::move(impl_->inbound.front()); impl_->inbound.pop_front();
    impl_->inbound_bytes -= result.frame.size(); return result;
}
LinkState TcpTransport::link(PeerIndex peer) const {
    if (mesh_) {
        if (peer >= mesh_->options.peers || mesh_->stopping) return LinkState::closed;
        if (mesh_->assigned && peer == mesh_->index) return LinkState::open;
        if (!mesh_->opened) return LinkState::connecting;
        return mesh_->links[peer] ? (mesh_->links[peer]->state == LinkState::open && !mesh_->links[peer]->authenticated()
            ? LinkState::connecting : mesh_->links[peer]->state) : LinkState::closed;
    }
    return peer < 2 ? (peer == self() ? LinkState::open : impl_->state == LinkState::open && !impl_->authenticated() ? LinkState::connecting : impl_->state) : LinkState::closed;
}
PeerIndex TcpTransport::self() const { return mesh_ ? mesh_->index : impl_->index; }
std::uint32_t TcpTransport::peer_count() const { return mesh_ ? mesh_->options.peers : 2U; }
std::optional<LinkStats> TcpTransport::link_stats(PeerIndex peer) const {
    if (mesh_) {
        if (link(peer) != LinkState::open || peer == self() || !mesh_->links[peer]
            || mesh_->links[peer]->stats.sample_count == 0) return {};
        return mesh_->links[peer]->stats;
    }
    if (peer != impl_->remote() || impl_->state != LinkState::open || !impl_->authenticated() || impl_->stats.sample_count == 0) return {};
    return impl_->stats;
}
std::uint16_t TcpTransport::port() const { return mesh_ ? mesh_->bound_port : impl_->bound_port; }
const std::optional<core::Diagnostic>& TcpTransport::diagnostic() const {
    if (mesh_) {
        if (mesh_->failure) return mesh_->failure;
        for (const auto& edge : mesh_->links) if (edge && edge->failure) return edge->failure;
        return mesh_->failure;
    }
    return impl_->failure;
}
core::Result<void> TcpTransport::close_peer(PeerIndex peer) {
    if (peer >= peer_count() || peer == self()) return core::Result<void>::failure(error("EAWR-NET-0200", "Invalid remote peer."));
    auto* edge = mesh_ ? mesh_->links[peer].get() : impl_.get();
    if (!edge || edge->state != LinkState::open || !edge->authenticated()) return core::Result<void>::success();
    edge->cancelled = edge->discard_application = true;
    edge->inbound.clear(); edge->inbound_bytes = 0;
    socket::close(edge->listener); edge->listener = socket::invalid;
    edge->queue({static_cast<std::uint8_t>(TcpMessage::close)});
    edge->closing = true; edge->state = LinkState::closed; edge->started = Clock::now();
    return core::Result<void>::success();
}
std::uint64_t TcpTransport::edge_identity(PeerIndex peer) const {
    if (peer >= peer_count() || peer == self() || link(peer) != LinkState::open) return 0;
    const auto* edge = mesh_ ? mesh_->links[peer].get() : impl_.get();
    return edge ? edge->edge_identity : 0;
}
core::Result<void> TcpTransport::reopen_staging_listener() {
    if (mesh_ || !impl_->hosting || repair_auth_) return core::Result<void>::failure(error("EAWR-NET-0200", "Staging refill requires an unauthenticated two-seat LAN host."));
    if (impl_->listener != socket::invalid || impl_->state == LinkState::open) return core::Result<void>::success();
    impl_->pump(); // Flush the typed application notice and ordered close before replacing the edge.
    if (!impl_->outbound.empty()) return core::Result<void>::success();
    auto replacement = host(reconnect_endpoint_.host, reconnect_endpoint_.port, impl_->options);
    if (!replacement) return core::Result<void>::failure(replacement.error());
    if (staging_retiring_) staging_retiring_->stop();
    staging_retiring_ = std::move(impl_);
    impl_ = std::move(std::move(replacement).value()->impl_);
    return core::Result<void>::success();
}
void TcpTransport::close() {
    if (mesh_) {
        socket::close(mesh_->listener); mesh_->listener = socket::invalid;
        mesh_->pending.clear(); mesh_->stopping = true;
        for (auto& edge : mesh_->links) if (edge) {
            edge->cancelled = true;
            if (edge->state == LinkState::open) {
                edge->queue({static_cast<std::uint8_t>(TcpMessage::close)});
                edge->closing = true; edge->state = LinkState::closed; edge->started = Clock::now();
            } else if (!edge->closing) edge->stop();
        }
        return;
    }
    impl_->cancelled = true;
    socket::close(impl_->listener); impl_->listener = socket::invalid;
    if (impl_->auth && !impl_->auth_proven) { impl_->stop(); return; }
    if (impl_->state == LinkState::open) {
        impl_->queue({static_cast<std::uint8_t>(TcpMessage::close)}); impl_->closing = true; impl_->state = LinkState::closed;
        impl_->started = Clock::now();
    } else {
        if (!impl_->closing) impl_->stop();
    }
}
core::Result<void> TcpTransport::repair_closed_links(std::optional<TcpRejoinAuth> credentials) {
    if (credentials) {
        if (repair_auth_ && credentials->epoch < repair_auth_->epoch)
            return core::Result<void>::failure(error("EAWR-NET-1016", "Stale repair epoch"));
        repair_auth_ = std::move(credentials);
    }
    const auto& settings = mesh_ ? mesh_->options : impl_->options;
    const auto nonzero = [](const auto& bytes) { return std::any_of(bytes.begin(), bytes.end(), [](auto byte) { return byte != 0; }); };
    if (!repair_auth_ || !nonzero(repair_auth_->match_identity) || repair_auth_->epoch == 0
        || settings.max_frame_size < 110 || settings.queue_bytes < 512 || settings.queue_frames < 4
        || !valid_reconnect_tokens(repair_auth_->seat_tokens, self(), peer_count()))
        return core::Result<void>::failure(error("EAWR-NET-1016", "Confidential repair credentials are unavailable"));
    if (mesh_) {
        auto& mesh = *mesh_;
        if (!mesh.opened || mesh.stopping) return core::Result<void>::failure(error("EAWR-NET-1014", "No retained mesh is available for rejoin"));
        mesh.repairing = true; mesh.auth = repair_auth_;
        bool needs_listener{};
        for (PeerIndex peer = 0; peer < mesh.options.peers; ++peer) {
            if (peer == mesh.index || (mesh.links[peer] && mesh.links[peer]->state != LinkState::closed)) continue;
            mesh.confirmed[peer] = false; mesh.pair_sent[peer] = false;
            if (peer < mesh.index) {
                auto connected = Mesh::dial(mesh.endpoints[peer].host, mesh.endpoints[peer].port, mesh.options);
                if (!connected) return core::Result<void>::failure(connected.error());
                mesh.links[peer] = std::move(connected).value(); mesh.links[peer]->set_auth(*repair_auth_, mesh.index, peer);
            } else needs_listener = true;
        }
        if (needs_listener && mesh.listener == socket::invalid) {
            const auto listener = socket::listen(mesh.listen_address, mesh.bound_port);
            if (listener.handle == socket::invalid) return core::Result<void>::failure(error("EAWR-NET-1014", "Cannot reopen the retained mesh listener"));
            mesh.listener = listener.handle;
        }
        return core::Result<void>::success();
    }
    if (impl_->state != LinkState::closed) return core::Result<void>::success();
    const auto config = impl_->options; const bool hosting = impl_->hosting;
    impl_->cancelled = true; impl_->stop();
    auto replacement = hosting ? host(reconnect_endpoint_.host, reconnect_endpoint_.port, config)
        : connect(reconnect_endpoint_.host, reconnect_endpoint_.port, config);
    if (!replacement) return core::Result<void>::failure(replacement.error());
    impl_ = std::move(std::move(replacement).value()->impl_);
    impl_->set_auth(*repair_auth_, self(), impl_->remote());
    return core::Result<void>::success();
}
core::Result<void> TcpTransport::restart_links(std::optional<TcpRejoinAuth> credentials) {
    auto configured = repair_closed_links(std::move(credentials));
    if (!configured) return configured;
    if (mesh_) {
        if (!mesh_->opened || mesh_->stopping) return core::Result<void>::failure(error("EAWR-NET-1014", "No retained mesh is available for rejoin"));
        for (auto& edge : mesh_->links) edge.reset();
        mesh_->pending.clear();
    } else { impl_->cancelled = true; impl_->stop(); }
    return repair_closed_links();
}
void TcpTransport::cancel() {
    if (mesh_) { mesh_->fail("Mesh connection cancelled"); return; }
    impl_->cancelled = true; impl_->stop();
}
bool TcpTransport::listening() const { return mesh_ ? mesh_->listener != socket::invalid : impl_->listener != socket::invalid; }
bool TcpTransport::closed() const {
    if (mesh_) return mesh_->listener == socket::invalid && mesh_->pending.empty()
        && std::all_of(mesh_->links.begin(), mesh_->links.end(), [](const auto& edge) {
            return !edge || (!edge->resolution && edge->stream == socket::invalid && edge->listener == socket::invalid);
        });
    return !impl_->resolution && impl_->stream == socket::invalid && impl_->listener == socket::invalid;
}
} // namespace eawr::net
