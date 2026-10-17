#include "eawr/net/gns_transport.hpp"
#include "gns_frame_codec.hpp"
#include "gns_deferred_signals.hpp"
#include "eawr/net/mesh_policy.hpp"
#include "gns_link_stats.hpp"
#include "gns_log_redact.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <string_view>
#include <utility>

#if EAWR_HAS_GNS
#include "platform_socket.hpp"
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#endif

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
// Repair dials per pair and epoch, and the pause before the second (it doubles each time): 0.5 + 1 + 2 + 4 s.
#if EAWR_HAS_GNS
constexpr unsigned gns_repair_dial_limit = 5;
constexpr std::chrono::milliseconds gns_repair_redial_pause{500};
constexpr std::size_t gns_log_max_pending = 256;
#endif
core::Diagnostic error(unsigned number, std::string message) {
    core::Diagnostic result;
    result.code = "EAWR-NET-" + std::to_string(number); result.message = std::move(message);
    return result;
}
std::atomic<int>& observed_route_cell() {
    static std::atomic<int> cell{static_cast<int>(GnsRoute::unknown)};
    return cell;
}
// Diagnostics for godot.log (#4021). GNS threads and the I/O owner queue lines; the owner's poll hands them to the sink.
struct GnsLogState {
    std::mutex mutex;
    GnsLogSink sink;
    std::deque<std::string> pending;
    std::size_t dropped{};
    std::map<std::string, std::pair<Clock::time_point, std::size_t>> limited;
    std::atomic<bool> enabled{}, has_pending{};
    std::atomic<int> seat{-1};
};
GnsLogState& gns_log() {
    static GnsLogState state;
    return state;
}
// seat/peer < 0 are left out. The time is UTC of day, so lines from several machines can be lined up.
std::string gns_log_line(int seat, int peer, std::string_view body) {
    const auto since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto day_ms = since_epoch % 86400000;
    char stamp[48];
    std::snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d", static_cast<int>(day_ms / 3600000), static_cast<int>(day_ms / 60000 % 60),
        static_cast<int>(day_ms / 1000 % 60), static_cast<int>(day_ms % 1000));
    std::string line = "EAWR net event=gns utc=";
    line += stamp;
    if (seat >= 0) line += " seat=" + std::to_string(seat);
    if (peer >= 0) line += " peer=" + std::to_string(peer);
    line += ' ';
    line += body;
    return line;
}
#if EAWR_HAS_GNS
// `limited` lines (GNS warnings, which can repeat once per failed send during an outage) pass once per ten seconds per
// text, the next one carrying the count that was held back.
void gns_log_push(int seat, int peer, std::string_view text, bool limited = false) {
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
    log.pending.push_back(gns_log_line(seat, peer, body));
    log.has_pending.store(true, std::memory_order_release);
}
#endif
// I/O owner only: deliver what the GNS threads and the transports queued since the last poll.
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
static_assert(gns_identity_max_bytes < SteamNetworkingIdentity::k_cchMaxGenericString);
// GNS spew: every warning or worse, and the ICE/TURN messages (the carried patch adds one per TURN reply).
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
// One numeric `turn:` URI (coturn) or up to eight comma-separated ones (a provider with several endpoints).
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
// Up to eight comma-separated printable "stun:" URIs, or none.
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
// The direct and paired adapters are exactly two peers; a room carries the agreed roster size (2..max_match_seats).
bool valid(const GnsOptions& options, bool room = false) {
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
struct SignallingState {
    std::shared_ptr<GnsSignallingEndpoint> endpoint;
    // A room pair sends through the room to one destination slot instead of a paired endpoint.
    std::shared_ptr<GnsRoomEndpoint> room;
    PeerIndex destination{};
    std::atomic<bool> admitted{}, unavailable{}, refusing{};
    // Signals carried for this pair, for measuring ICE exchange cost (GNS thread writes the sent side).
    std::atomic<std::uint64_t> sent_blobs{}, sent_bytes{}, received_blobs{}, received_bytes{};
};
// One numeric turn: URI list plus single-credential checks shared by the paired and room factories.
bool turn_valid(const GnsTurnCredentials& turn) {
    return !(turn.uri.empty() || turn.username.empty() || turn.password.empty() || !turn_uri_list(turn.uri)
        || turn.username.find(',') != std::string::npos || turn.password.find(',') != std::string::npos
        || turn.lifetime.count() <= 0);
}
class Signal final : public ISteamNetworkingConnectionSignaling {
public:
    explicit Signal(std::shared_ptr<SignallingState> state) : state_(std::move(state)) {}
    bool SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* data, int size) override {
        if (size <= 0 || size > 64 * 1024) return false;
        if (state_->unavailable.load()) return state_->admitted.load() || state_->refusing.load();
        const std::span<const std::uint8_t> blob{static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size)};
        const auto sent = state_->room ? state_->room->send(state_->destination, blob) : state_->endpoint->send(blob);
        if (sent) {
            state_->sent_blobs.fetch_add(1); state_->sent_bytes.fetch_add(static_cast<std::uint64_t>(size));
        }
        // A signalling outage cannot abort an established data path or its tail.
        if (!sent && (state_->admitted.load() || state_->refusing.load())) { state_->unavailable.store(true); return true; }
        return static_cast<bool>(sent);
    }
    void Release() override { delete this; }
private:
    std::shared_ptr<SignallingState> state_;
};
#endif
}

// One connection between two peers. The direct and paired adapters own exactly one; a room mesh owns one per
// remote slot (pair link). `hosting` is the listening side of this connection, not the room host.
struct GnsTransport::Impl {
    GnsOptions options;
    bool hosting{}, accepted{}, closing{}, cancelled{};
    LinkState state{LinkState::connecting};
    std::uint16_t bound_port{};
    std::optional<core::Diagnostic> failure;
    std::deque<Received> inbound;
    std::size_t inbound_bytes{};
    GnsLinkStatsSampler stats;
    Impl(bool host, GnsOptions config) : options(std::move(config)), hosting(host), decoder(options, host) {}
    GnsFrameDecoder decoder;
    // Roster positions of this connection's two ends. The two-peer defaults are listener 0 and dialer 1.
    PeerIndex local_slot{hosting ? 0U : 1U}, peer_slot{hosting ? 1U : 0U};
    PeerIndex remote() const { return peer_slot; }
    // Room pair link. A non-host pair also proves its host-issued secret (mesh_pair) before it opens; a link whose
    // grant has not arrived yet stays parked: no hello, no reads, bounded by the pair deadline.
    bool pair_link{}, needs_secret{}, granted{true}, pair_ok{true}, pair_sent{};
    std::array<std::uint8_t, gns_pair_secret_size> secret{};
    std::deque<Bytes> controls;
    std::unique_ptr<RejoinLinkProof> proof;
    bool proof_started{}, published{};
    bool authenticated() const { return !proof || proof->proven(); }
    unsigned recv_budget{64}, send_budget{64};
    // Route of this connection, sampled by pump. The process-wide cell below follows the two-peer adapter only.
    GnsRoute route{GnsRoute::unknown};
#if EAWR_HAS_GNS
    struct Runtime {
        ISteamNetworkingSockets* sockets{};
        std::optional<core::Diagnostic> failure;
        std::map<HSteamListenSocket, Impl*> listeners;
        std::map<HSteamNetConnection, Impl*> connections;
        Runtime() {
            SteamNetworkingErrMsg reason{};
            gns_log(); // built first, so it outlives the GNS threads that post to it
            if (GameNetworkingSockets_Init(nullptr, reason)) {
                sockets = SteamNetworkingSockets();
                SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, gns_spew);
            }
            else failure = error(1506, "GNS initialization failed");
        }
        ~Runtime() { if (sockets) GameNetworkingSockets_Kill(); }
        static Runtime& get() { static Runtime runtime; return runtime; }
        static void changed(SteamNetConnectionStatusChangedCallback_t* event) {
            // GNS queues these callbacks for RunCallbacks. The owner's factory
            // registers handles before its next poll; GNS threads only post
            // status events and may independently invoke Signal::SendSignal.
            auto& runtime = get();
            const auto connection = runtime.connections.find(event->m_hConn);
            if (connection != runtime.connections.end()) {
                auto& owner = *connection->second;
                if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected) owner.ready = true;
                if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer
                    || event->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                    owner.ended = true;
                    const auto reason = event->m_info.m_eEndReason;
                    // Never show GNS's raw address-bearing debug strings to players.
                    if ((reason >= 1500 && reason <= 1529) || (reason >= static_cast<int>(gns_error_slot_mismatch) && reason <= static_cast<int>(gns_error_grant_missing))
                        || (reason >= 1790 && reason <= 1794))
                        owner.end_error = static_cast<unsigned>(reason);
                    else if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
                        owner.end_error = reason == k_ESteamNetConnectionEnd_Misc_Timeout ? 1507U : 1506U;
                }
                return;
            }
            if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting) {
                const auto listener = runtime.listeners.find(event->m_info.m_hListenSocket);
                if (listener != runtime.listeners.end()) listener->second->adopt(event->m_hConn);
                else runtime.sockets->CloseConnection(event->m_hConn, 1508, "EAWR-NET-1508", false);
            }
        }
    };
    struct ReceiveSignal final : ISteamNetworkingSignalingRecvContext {
        Impl& owner;
        explicit ReceiveSignal(Impl& impl) : owner(impl) {}
        ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection connection,
            const SteamNetworkingIdentity& peer, int virtual_port) override {
            const auto* identity = peer.GetGenericString();
            if (!owner.hosting || virtual_port != 0 || !identity
                || identity != owner.signalling->rendezvous().remote_identity
                || owner.stream != k_HSteamNetConnection_Invalid || owner.cancelled) return nullptr;
            if (!owner.adopt(connection)) return nullptr;
            return new Signal(owner.signalling_state);
        }
        void SendRejectionSignal(const SteamNetworkingIdentity&, const void*, int) override {}
    };
    struct Outbound { Bytes bytes; bool data{}; std::size_t offset{}; };
    HSteamListenSocket listener{k_HSteamListenSocket_Invalid};
    HSteamNetConnection stream{k_HSteamNetConnection_Invalid};
    bool ready{}, hello_sent{}, hello_seen{}, peer_close{}, ended{}, close_ack_flushed{};
    unsigned end_error{};
    bool refusing{};
    unsigned refusal_reason{};
    Clock::time_point refusal_deadline{};
    std::optional<Clock::time_point> service_closed_deadline;
    std::optional<std::size_t> closing_pending;
    std::deque<Outbound> outbound;
    std::size_t outbound_bytes{};
    std::shared_ptr<GnsSignallingEndpoint> signalling;
    std::optional<GnsTurnCredentials> turn;
    // GNS wants one user and password per listed TURN server, comma separated.
    std::string turn_users, turn_passwords;
    std::shared_ptr<SignallingState> signalling_state;
    Clock::time_point started{Clock::now()}, last_receive{started}, last_write{started};
    // Reliable bytes outstanding since this point without one acknowledgement in between (GnsOptions::ack_silence_timeout).
    std::optional<Clock::time_point> unacked_since;
    std::int32_t last_unacked{};
    Clock::time_point next_ack_sample{started};
    ISteamNetworkingSockets& sockets() const { return *Runtime::get().sockets; }
    ~Impl() {
        if (stream != k_HSteamNetConnection_Invalid) {
            Runtime::get().connections.erase(stream);
            sockets().CloseConnection(stream, 0, nullptr, false);
        }
        release_listener();
    }
    void release_listener() {
        if (listener == k_HSteamListenSocket_Invalid) return;
        Runtime::get().listeners.erase(listener);
        sockets().CloseListenSocket(listener); listener = k_HSteamListenSocket_Invalid;
    }
    std::vector<SteamNetworkingConfigValue_t> config() const {
        std::vector<SteamNetworkingConfigValue_t> values(4 + (turn ? 4 : 0) + (options.stun_servers.empty() ? 0 : 1));
        values[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
            reinterpret_cast<void*>(Runtime::changed));
        values[1].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, static_cast<std::int32_t>(options.handshake_timeout.count()));
        values[2].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, static_cast<std::int32_t>(options.timeout.count()));
        // A room mesh holds up to 8 connections: 2 MiB each (above the 1 MiB default frame) keeps the worst case at 16 MiB.
        const auto send_buffer = pair_link ? std::min<std::size_t>(options.queue_bytes, 2 * 1024 * 1024) : options.queue_bytes;
        values[3].SetInt32(k_ESteamNetworkingConfig_SendBufferSize,
            static_cast<std::int32_t>(std::max<std::size_t>(send_buffer, 4096)));
        if (turn) {
            values[4].SetString(k_ESteamNetworkingConfig_P2P_TURN_ServerList, turn->uri.c_str());
            values[5].SetString(k_ESteamNetworkingConfig_P2P_TURN_UserList, turn_users.c_str());
            values[6].SetString(k_ESteamNetworkingConfig_P2P_TURN_PassList, turn_passwords.c_str());
            values[7].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, options.turn_only
                ? k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay : k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All);
        }
        if (!options.stun_servers.empty())
            values.back().SetString(k_ESteamNetworkingConfig_P2P_STUN_ServerList, options.stun_servers.c_str());
        return values;
    }
    bool adopt(HSteamNetConnection connection) {
        if (cancelled || accepted || stream != k_HSteamNetConnection_Invalid) {
            sockets().CloseConnection(connection, 1508, "EAWR-NET-1508", false); return false;
        }
        stream = connection;
        Runtime::get().connections.emplace(stream, this);
        failure.reset(); started = Clock::now();
        if (sockets().AcceptConnection(stream) != k_EResultOK) { stop(1506, "GNS accept failed"); return false; }
        return true;
    }
    void stop(unsigned reason = 0, std::string message = {}) {
        stats.reset();
        forget_route();
        if (reason != 0 && !failure) {
            gns_log_push(static_cast<int>(local_slot), static_cast<int>(peer_slot), "link stops: EAWR-NET-" + std::to_string(reason) + " " + message);
            failure = error(reason, std::move(message));
        }
        if (reason != 0 && hosting && !accepted && !ended && !refusing
            && stream != k_HSteamNetConnection_Invalid) {
            SteamNetConnectionInfo_t info{};
            if (sockets().GetConnectionInfo(stream, &info)
                && (info.m_eState == k_ESteamNetworkingConnectionState_Connected
                    || info.m_eState == k_ESteamNetworkingConnectionState_Connecting
                    || info.m_eState == k_ESteamNetworkingConnectionState_FindingRoute)) {
                // Discard any acceptance queued for this attempt: exactly one
                // bounded refusal replaces it and cannot publish admission.
                outbound.clear(); outbound_bytes = 0;
                Bytes bytes{static_cast<std::uint8_t>(TcpMessage::refuse)};
                u32(bytes, static_cast<std::uint32_t>(failure->code.size()));
                bytes.insert(bytes.end(), failure->code.begin(), failure->code.end());
                const auto length = std::min<std::size_t>(failure->message.size(), 1024);
                u32(bytes, static_cast<std::uint32_t>(length));
                bytes.insert(bytes.end(), failure->message.begin(), failure->message.begin() + static_cast<std::ptrdiff_t>(length));
                control(std::move(bytes)); refusing = closing = true;
                refusal_reason = reason;
                refusal_deadline = Clock::now() + std::chrono::milliseconds(250);
                if (signalling_state) signalling_state->refusing.store(true);
                return;
            }
        }
        if (refusing) reason = refusal_reason;
        if (stream != k_HSteamNetConnection_Invalid) {
            Runtime::get().connections.erase(stream);
            const auto code = reason ? "EAWR-NET-" + std::to_string(reason) : std::string{};
            sockets().CloseConnection(stream, static_cast<int>(reason), code.c_str(), false);
            stream = k_HSteamNetConnection_Invalid;
        }
        const bool retry = hosting && !accepted && !cancelled && listener != k_HSteamListenSocket_Invalid;
        if (retry && signalling_state) {
            // Admission belongs to one native connection, not its reusable
            // listener. A later attempt must verify a fresh hello. Resume
            // endpoint polling so a sticky outage fails closed before admission.
            signalling_state->admitted.store(false);
            signalling_state->unavailable.store(false);
            signalling_state->refusing.store(false);
        }
        state = retry ? LinkState::connecting : LinkState::closed;
        if (!retry) release_listener(); else { inbound.clear(); inbound_bytes = 0; }
        ready = hello_sent = hello_seen = closing = peer_close = ended = close_ack_flushed = false;
        end_error = 0; decoder.reset();
        closing_pending.reset();
        refusing = false;
        refusal_reason = 0;
        outbound.clear(); outbound_bytes = 0;
    }
    void forget_route() {
        route = GnsRoute::unknown;
        if (!pair_link) observed_route_cell().store(static_cast<int>(GnsRoute::unknown));
    }
    void control(Bytes bytes) { outbound_bytes += bytes.size(); outbound.push_back({std::move(bytes), false, 0}); }
    // Pair identity proof: the shared secret plus both roster positions, as in the TCP mesh's mesh_pair.
    Bytes pair_message() const {
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_pair)};
        bytes.insert(bytes.end(), secret.begin(), secret.end()); u32(bytes, local_slot); u32(bytes, peer_slot); return bytes;
    }
    void queue_proof(Bytes bytes) {
        outbound_bytes += bytes.size() + 9;
        outbound.push_back({std::move(bytes), true, 0});
    }
    void hello() {
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::hello), 'E', 'A', 'W', 'R'};
        u32(bytes, options.protocol_version); u32(bytes, static_cast<std::uint32_t>(options.build_identity.size()));
        bytes.insert(bytes.end(), options.build_identity.begin(), options.build_identity.end());
        u32(bytes, options.max_frame_size); u32(bytes, local_slot); u32(bytes, options.peers);
        bytes.push_back(hosting ? 0U : 1U);
        control(std::move(bytes)); hello_sent = true;
        // A pair link keeps the clock it started with (first signal or grant): ICE, any TURN allocation and the
        // application handshake share one pair deadline.
        const auto now = Clock::now();
        if (!pair_link) started = now;
        last_receive = last_write = now;
    }
    void message(std::span<const std::uint8_t> bytes, bool reliable) {
        if (refusing) return;
        auto result = decoder.receive(bytes, {hello_seen, accepted, peer_close, inbound_bytes, inbound.size()}, reliable);
        if (!result) {
            const auto& diagnostic = result.error();
            unsigned number = 0;
            for (const char digit : diagnostic.code.substr(diagnostic.code.size() - 4))
                number = number * 10 + static_cast<unsigned>(digit - '0');
            stop(number, diagnostic.message); return;
        }
        auto& decoded = result.value();
        if (decoded.kind == TcpMessage::hello) {
            hello_seen = true;
            if (hosting) {
                // The accepted seat is the dialer's roster position: 1 for the two-peer adapter.
                Bytes reply{static_cast<std::uint8_t>(TcpMessage::accept)}; u32(reply, peer_slot); control(std::move(reply));
                // The verified hello admits this peer before its acknowledgement
                // arrives. Signalling can disappear anywhere in that interval.
                if (signalling_state) signalling_state->admitted.store(true);
            }
        } else if (decoded.kind == TcpMessage::accept) {
            accepted = true;
            if (signalling_state) signalling_state->admitted.store(true);
            if (!hosting) { Bytes reply{static_cast<std::uint8_t>(TcpMessage::accept)}; u32(reply, local_slot); control(std::move(reply)); }
            // The dialer of a non-host pair proves the host's secret first; the pair opens when both ends have.
            if (needs_secret && !hosting && !pair_sent) { control(pair_message()); pair_sent = true; }
            if (pair_ok && authenticated()) state = LinkState::open;
            if (proof && !proof_started) {
                auto challenge = proof->start();
                if (!challenge) { stop(1792, "Could not generate pair nonce"); return; }
                queue_proof(std::move(challenge).value()); proof_started = true;
            }
            // GNS closes accepted connections with their listener, so keep it
            // until teardown and reject additional arrivals instead.
        } else if (decoded.kind == TcpMessage::data && decoded.frame) {
            if (proof && (!authenticated() || proof->recognizes(*decoded.frame))) {
                auto response = proof->receive(*decoded.frame);
                if (!response) { stop(1792, response.error().message); return; }
                if (response.value()) queue_proof(std::move(*response.value()));
                if (authenticated()) state = LinkState::open;
                return;
            }
            if (!pair_ok) { stop(gns_error_pair_identity, "Application data before the pair identity"); return; }
            inbound_bytes += decoded.frame->size(); inbound.push_back({remote(), std::move(*decoded.frame)});
        } else if (decoded.kind == TcpMessage::mesh_pair && decoded.control) {
            // Same constant-size secret as the pair grant; the decoder already checked the seats.
            if (!needs_secret || pair_ok || !std::equal(secret.begin(), secret.end(), decoded.control->begin())) {
                stop(gns_error_pair_identity, "Pair secret does not match the host's grant"); return;
            }
            if (hosting) control(pair_message());
            pair_ok = true; state = LinkState::open;
        } else if (decoded.control) {
            if (!authenticated()) { stop(1792, "Mesh control before repaired pair proof"); return; }
            if (!pair_link || controls.size() >= 16) { stop(gns_error_mesh_control, "Unexpected mesh control"); return; }
            Bytes control_bytes{static_cast<std::uint8_t>(decoded.kind)};
            control_bytes.insert(control_bytes.end(), decoded.control->begin(), decoded.control->end());
            controls.push_back(std::move(control_bytes));
        } else if (decoded.kind == TcpMessage::close) {
            stats.reset();
            peer_close = true;
            if (!closing) { control({static_cast<std::uint8_t>(TcpMessage::close)}); closing = true; started = Clock::now(); }
            state = LinkState::closed;
        }
        last_receive = Clock::now();
    }
    bool receive() {
        for (; recv_budget > 0 && stream != k_HSteamNetConnection_Invalid; --recv_budget) {
            SteamNetworkingMessage_t* incoming{};
            const auto count = sockets().ReceiveMessagesOnConnection(stream, &incoming, 1);
            if (count <= 0) return true;
            const auto old_stream = stream;
            if (incoming->m_cbSize < 0) stop(1518, "Invalid GNS message size");
            else message({static_cast<const std::uint8_t*>(incoming->m_pData), static_cast<std::size_t>(incoming->m_cbSize)},
                (incoming->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0);
            incoming->Release();
            if (old_stream != stream) return true;
        }
        return false;
    }
    void flush() {
        const auto chunk_size = std::min<std::size_t>(64 * 1024, options.queue_bytes - 9);
        for (; send_budget > 0 && !outbound.empty() && stream != k_HSteamNetConnection_Invalid; --send_budget) {
            auto& next = outbound.front();
            Bytes packet;
            if (next.data) {
                const auto count = std::min(chunk_size, next.bytes.size() - next.offset);
                packet.push_back(static_cast<std::uint8_t>(TcpMessage::data));
                u32(packet, static_cast<std::uint32_t>(next.bytes.size())); u32(packet, static_cast<std::uint32_t>(next.offset));
                packet.insert(packet.end(), next.bytes.begin() + static_cast<std::ptrdiff_t>(next.offset),
                    next.bytes.begin() + static_cast<std::ptrdiff_t>(next.offset + count));
            } else packet = next.bytes;
            const auto sent = sockets().SendMessageToConnection(stream, packet.data(), static_cast<uint32>(packet.size()),
                refusing ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_Reliable, nullptr);
            if (sent == k_EResultLimitExceeded) break;
            // Native state can end between RunCallbacks and this send. Let
            // the queued callback deliver its typed peer reason on the next
            // poll instead of replacing it with a generic send failure.
            if (sent == k_EResultNoConnection || sent == k_EResultInvalidState) return;
            if (sent != k_EResultOK) { stop(1506, "GNS reliable send failed"); return; }
            last_write = Clock::now();
            if (closing) started = last_write;
            next.offset += next.data ? packet.size() - 9 : packet.size();
            if (next.offset == next.bytes.size()) {
                outbound_bytes -= next.bytes.size() + (next.data ? 9 : 0); outbound.pop_front();
            }
        }
    }
    void pump_signals() {
        if (!signalling || (closing && !refusing) || (signalling_state->unavailable.load() && !refusing)) return;
        ReceiveSignal context(*this);
        for (unsigned i = 0; i < 32; ++i) {
            auto next = signalling->poll();
            if (!next) {
                signalling_state->unavailable.store(true);
                if (refusing) return; // keep the original reason and fixed deadline
                // A peer's typed refusal can already be queued on the ICE path
                // when its service session closes. Give callbacks/receive one
                // fixed, nonblocking drain window before the generic closure.
                if (!signalling_state->admitted.load() && !service_closed_deadline)
                    service_closed_deadline = Clock::now() + std::chrono::milliseconds(250);
                return;
            }
            if (!next.value()) return;
            const auto& signal = *next.value();
            if (signal.from != signalling->rendezvous().remote_identity || signal.bytes.empty()
                || signal.bytes.size() > 64 * 1024 || !sockets().ReceivedP2PCustomSignal(signal.bytes.data(),
                    static_cast<int>(signal.bytes.size()), &context)) {
                if (!refusing) stop(1517, "Invalid or unauthorized GNS signal");
                return;
            }
        }
    }
    void settle_service_closure() {
        if (service_closed_deadline && Clock::now() >= *service_closed_deadline
            && !signalling_state->admitted.load() && !refusing) {
            cancelled = true; stop(1517, "GNS introduction service failed");
        }
    }
    void pump() {
        if (state == LinkState::closed && stream == k_HSteamNetConnection_Invalid
            && listener == k_HSteamListenSocket_Invalid) return;
        pump_signals();
        sockets().RunCallbacks();
        if (stream == k_HSteamNetConnection_Invalid) { settle_service_closure(); return; }
        // A parked pair link (grant not yet delivered) neither answers nor reads; GNS buffers its peer's hello.
        if (ready && !hello_sent && !refusing && granted) hello();
        // Drain queued messages before acting on a closed callback, which would
        // otherwise discard the ordered tail at CloseConnection.
        // Custom-signalling negotiation can make SDK messages readable before
        // its queued Connected callback reaches this owner. Wait for readiness
        // so our hello is queued before any reply to the peer's hello.
        const auto drained = ready && granted ? receive() : true;
        if (stream == k_HSteamNetConnection_Invalid) return;
        if (refusing) {
            // This deadline never extends on retries or ACK progress. Polling
            // drives delivery; no signalling or application ACK is required.
            if (ended) { stop(); return; }
            flush();
            if (refusing && Clock::now() >= refusal_deadline) stop();
            return;
        }
        if (ended) {
            if (!drained) return;
            auto reason = end_error ? end_error : (peer_close ? 0U : 1506U);
            // GNS's own initial timeout races the pair deadline: either way a pair that never connected is unreachable.
            if (pair_link && !ready && (reason == 1506 || reason == 1507)) reason = 1762;
            stop(reason, reason == 1762 ? "No direct or relayed path to this peer" : "GNS peer disconnected"); return;
        }
        settle_service_closure();
        if (stream == k_HSteamNetConnection_Invalid) return;
        const auto now = Clock::now();
        if (!(accepted && pair_ok && authenticated()) && now - started >= options.handshake_timeout) {
            if (!pair_link) { stop(1507, "GNS handshake timed out"); return; }
            // Pair deadline, which includes ICE and any TURN allocation: say how far this pair got.
            if (!granted) stop(gns_error_grant_missing, "No host grant arrived for this pair");
            else if (!ready) stop(1762, "No direct or relayed path to this peer");
            else stop(proof ? 1792U : gns_error_pair_timeout, "Pair handshake timed out");
            return;
        }
        if (accepted && !closing && now - last_receive >= options.timeout) { stop(1507, "GNS application peer timed out"); return; }
        if (state == LinkState::open && !closing && now >= next_ack_sample) {
            next_ack_sample = now + std::chrono::milliseconds(250);
            SteamNetConnectionRealTimeStatus_t status{};
            if (sockets().GetConnectionRealTimeStatus(stream, &status, 0, nullptr) == k_EResultOK
                && status.m_eState == k_ESteamNetworkingConnectionState_Connected) {
                // Any decrease or an empty window is an acknowledgement; steady traffic produces one every few samples.
                if (status.m_cbSentUnackedReliable <= 0 || status.m_cbSentUnackedReliable < last_unacked) unacked_since.reset();
                else if (!unacked_since) unacked_since = now;
                last_unacked = status.m_cbSentUnackedReliable;
                if (unacked_since && now - *unacked_since >= options.ack_silence_timeout) {
                    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - *unacked_since).count();
                    stop(1507, "GNS link silent " + std::to_string(seconds) + " s while Connected ("
                        + (route == GnsRoute::relayed ? "relay" : route == GnsRoute::direct ? "direct" : "route unknown")
                        + "): reliable data is not being acknowledged");
                    return;
                }
            }
        }
        if (state == LinkState::open && !closing && stats.due(now)) {
            SteamNetConnectionRealTimeStatus_t status{};
            const auto valid_ping = sockets().GetConnectionRealTimeStatus(stream, &status, 0, nullptr) == k_EResultOK
                && status.m_eState == k_ESteamNetworkingConnectionState_Connected;
            stats.observe(now, valid_ping ? std::optional<int>(status.m_nPing) : std::nullopt);
            SteamNetConnectionInfo_t info{};
            const bool info_ok = sockets().GetConnectionInfo(stream, &info);
            const bool connected = info_ok && info.m_eState == k_ESteamNetworkingConnectionState_Connected;
            route = classify_gns_route(connected, (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0,
                !info.m_addrRemote.IsIPv6AllZeros(), info.m_idPOPRelay != 0 || info.m_idPOPRemote != 0);
            // A room pair reports its own route through pair(); only the two-peer adapter feeds the process-wide cell.
            if (!pair_link) observed_route_cell().store(static_cast<int>(route));
        }
        if (accepted && !closing && outbound.empty() && now - last_write >= options.keepalive)
            control({static_cast<std::uint8_t>(TcpMessage::keepalive)});
        if (ready) flush();
        if (closing && outbound.empty() && stream != k_HSteamNetConnection_Invalid) {
            if (peer_close && !close_ack_flushed) {
                // GNS schedules receiver ACKs separately from ReceiveMessages.
                // Its P2P FinWait notification uses signalling, which may be
                // unavailable. Emit a tiny native packet carrying the ACKs
                // before tearing down this ICE route; no game frame is added.
                const auto probe = static_cast<std::uint8_t>(TcpMessage::keepalive);
                const auto sent = sockets().SendMessageToConnection(stream, &probe, 1,
                    k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_UseCurrentThread, nullptr);
                if (sent == k_EResultOK) close_ack_flushed = true;
                else if (sent == k_EResultNoConnection || sent == k_EResultInvalidState) return;
                else if (sent != k_EResultLimitExceeded) { stop(1506, "GNS close acknowledgement failed"); return; }
            }
            SteamNetConnectionRealTimeStatus_t status{};
            if (sockets().GetConnectionRealTimeStatus(stream, &status, 0, nullptr) == k_EResultOK) {
                const auto pending = static_cast<std::size_t>(std::max(0, status.m_cbPendingReliable))
                    + static_cast<std::size_t>(std::max(0, status.m_cbSentUnackedReliable))
                    + static_cast<std::size_t>(std::max(0, status.m_cbPendingUnreliable));
                // A stalled close times out, but an accepted large tail may
                // take longer than timeout at GNS's bandwidth cap. Only a new
                // low-water mark extends the drain; retransmit fluctuations
                // cannot keep an otherwise stalled connection alive forever.
                if (!closing_pending || pending < *closing_pending) {
                    closing_pending = pending; started = Clock::now();
                }
                if (peer_close && close_ack_flushed && pending == 0) { stop(); return; }
            }
        }
        if (closing && Clock::now() - started >= options.timeout) stop(1507, "GNS close stalled");
    }
    core::Result<void> send_frame(PeerIndex to, std::span<const std::uint8_t> frame) {
        if (to != remote() || state != LinkState::open || closing)
            return core::Result<void>::failure(error(1506, "GNS link is not open for this peer"));
        if (frame.size() > options.max_frame_size)
            return core::Result<void>::failure(error(1504, "GNS application frame exceeds limit"));
        if (outbound.size() >= options.queue_frames
            || frame.size() + 9 > options.queue_bytes - std::min(outbound_bytes, options.queue_bytes))
            return core::Result<void>::failure(error(1505, "GNS send queue is full; pump and retry"));
        outbound.push_back({Bytes(frame.begin(), frame.end()), true, 0}); outbound_bytes += frame.size() + 9;
        return core::Result<void>::success();
    }
    void close_link() {
        if (closing) return;
        if (state != LinkState::open) { cancel_link(); return; }
        control({static_cast<std::uint8_t>(TcpMessage::close)});
        closing = true; state = LinkState::closed; started = Clock::now();
        stats.reset();
        forget_route();
    }
    void cancel_link() { cancelled = true; stop(); }
    bool dead() const { return stream == k_HSteamNetConnection_Invalid && state == LinkState::closed; }
#endif
};

#if EAWR_HAS_GNS

// Room mesh: one pair link per remote roster slot (docs/net-online-npeer.md, sections 2-3). The introduction service
// only signals; the host introduces pairs by granting each a fresh secret over its host streams, and runs the pure
// MeshFormation policy (N1) over the pair reports the members send back. Everything here runs on the owner's I/O thread.
struct GnsTransport::Mesh {
    GnsOptions options; // handshake_timeout carries pair_timeout for the pair links
    std::shared_ptr<GnsRoomEndpoint> room;
    PeerIndex self{};
    std::uint32_t peers{};
    HSteamListenSocket listener{k_HSteamListenSocket_Invalid};
    std::optional<GnsTurnCredentials> turn;
    std::string turn_users, turn_passwords;
    std::optional<core::Diagnostic> failure;
    std::array<std::unique_ptr<Impl>, max_match_seats> links;
    struct Slot {
        enum class Role : std::uint8_t { none, dial, accept };
        GnsRoomSlotState state{GnsRoomSlotState::vacant};
        std::string identity, ignored; // ignored: identity the host released, until the roster stops showing it
        std::uint64_t admitted{};      // roster revision at which this occupant attached
        std::uint32_t epoch{};         // occupancy count: host side counts attaches, member side learns it from grants
        Role role{Role::none};
        bool have_grant{}, reported{}, was_open{}, gone{}, refused{};
        bool repair_attempted{}, excluded{};
        // This epoch's repair of the pair was given up (deadline, refused attachment, signal budget): a request that
        // arrives afterwards must not revive it. Cleared when the next epoch arms.
        bool gave_up{};
        // Dialer side: dials made this epoch, and when the next one may start after an attempt died before it opened.
        unsigned repair_dials{};
        std::optional<Clock::time_point> repair_retry_after;
        std::uint64_t generation{};
        std::array<std::uint8_t, gns_pair_secret_size> secret{};
        std::optional<Clock::time_point> expect_until;
        // Signals of this slot's current link, of the links it replaced (a regrant) and the totals already folded in.
        std::shared_ptr<SignallingState> signals;
        std::deque<std::shared_ptr<SignallingState>> retired;
        GnsSignalCounts folded;
        GnsPairDiagnostic diag;
    };
    std::array<Slot, max_match_seats> slot;
    bool locked{}, closing{};
    std::optional<TcpRejoinAuth> repair_auth;
    std::uint64_t attachment_epoch{};
    bool attachment_pending{}, attachment_ready{};
    std::optional<Clock::time_point> repair_deadline;
    std::deque<GnsRoomSignal> repair_signals;
    bool refresh_failed{};
    std::uint64_t revision{};
    std::size_t rotate{}, served{};
    std::deque<GnsMeshEvent> events;
    // Host side
    std::optional<MeshFormation> formation;
    MeshSeats present{}; // host pair open: the seats that may be introduced to each other
    std::array<MeshSeats, max_match_seats> claimed{}, failed{}, introduced{};
    std::array<std::array<std::optional<Clock::time_point>, max_match_seats>, max_match_seats> failed_since{};
    // Host: what `a` reported about its pair with `b` (reason code and whether a relay was offered), for the refusal.
    struct Reported { unsigned code{}; bool relay{}; };
    std::array<std::array<Reported, max_match_seats>, max_match_seats> reported_by{};
    // Member side
    MeshSeats sent_mask{};
    bool mask_sent{};

    ISteamNetworkingSockets& sockets() const { return *Impl::Runtime::get().sockets; }
    bool host() const { return self == 0; }
    static constexpr MeshSeats bit(PeerIndex seat) { return mesh_seat(seat); }
    static GnsPairDiagnostic diagnostic(GnsPairPhase phase, unsigned code, bool relay_offered, bool relay_failed,
        GnsRoute route = GnsRoute::unknown) {
        GnsPairDiagnostic value;
        value.phase = phase; value.code = code; value.relay_offered = relay_offered; value.relay_failed = relay_failed; value.route = route;
        return value;
    }
    static void add(MeshSeats& seats, PeerIndex seat) { seats = static_cast<MeshSeats>(seats | mesh_seat(seat)); }

    void emit(GnsMeshEventKind kind, PeerIndex seat, PeerIndex other = 0, unsigned code = 0, bool relay = false) {
        if (events.size() >= 64) events.pop_front();
        events.push_back({kind, seat, other, code, relay});
    }
    void fatal(unsigned code, std::string message, PeerIndex other = 0, bool relay = false) {
        if (failure) return;
        failure = error(code, std::move(message));
        for (PeerIndex p = 0; p < peers; ++p) if (links[p]) { links[p]->cancel_link(); }
        emit(GnsMeshEventKind::left_room, self, other, code, relay);
    }

    std::unique_ptr<Impl> make_link(PeerIndex remote, bool acceptor) {
        auto link = std::make_unique<Impl>(acceptor, options);
        link->local_slot = self; link->peer_slot = remote; link->pair_link = true;
        link->decoder.bind(self, remote);
        link->needs_secret = self != 0 && remote != 0;
        if (locked && repair_auth) {
            link->needs_secret = false;
            link->proof = std::make_unique<RejoinLinkProof>(repair_auth->match_identity,
                repair_auth->seat_tokens[remote], self, remote, repair_auth->epoch, "EAWR-GNS-rejoin-v1");
        }
        link->pair_ok = !link->needs_secret;
        link->granted = !link->needs_secret;
        link->turn = turn; link->turn_users = turn_users; link->turn_passwords = turn_passwords;
        link->signalling_state = std::make_shared<SignallingState>();
        link->signalling_state->room = room; link->signalling_state->destination = remote;
        retire_signals(remote);
        slot[remote].signals = link->signalling_state;
        return link;
    }
    // A regrant replaces the pair's link: its signal counts stay with the slot. A few replaced states stay live because
    // GNS threads may still send through them; older ones are folded into plain totals.
    void retire_signals(PeerIndex remote) {
        auto& s = slot[remote];
        if (!s.signals) return;
        s.retired.push_back(std::move(s.signals));
        while (s.retired.size() > 4) {
            const auto& old = *s.retired.front();
            s.folded.sent_blobs += old.sent_blobs.load(); s.folded.sent_bytes += old.sent_bytes.load();
            s.folded.received_blobs += old.received_blobs.load(); s.folded.received_bytes += old.received_bytes.load();
            s.retired.pop_front();
        }
    }
    // Dial the remote slot with the grant (or, for the host pair, the roster identity) already known.
    void dial(PeerIndex remote) {
        auto link = make_link(remote, false);
        if (link->needs_secret) { link->secret = slot[remote].secret; link->granted = true; }
        const auto config = link->config();
        SteamNetworkingIdentity peer{};
        if (!peer.SetGenericString(slot[remote].identity.c_str())) { fail_pair(remote, 1509, "Invalid peer identity"); return; }
        // GNS owns and eventually releases this object, even on failure.
        link->stream = sockets().ConnectP2PCustomSignaling(new Signal(link->signalling_state), &peer, 0,
            static_cast<int>(config.size()), config.data());
        if (link->stream == k_HSteamNetConnection_Invalid) { fail_pair(remote, 1506, "GNS connection creation failed"); return; }
        Impl::Runtime::get().connections.emplace(link->stream, link.get());
        links[remote] = std::move(link);
        slot[remote].expect_until.reset();
    }
    // Repair trace for godot.log (#4021): which link a seat re-dialled, replaced or gave up, with this seat and the peer.
    void note(int peer, const std::string& text) const { gns_log_push(static_cast<int>(self), peer, text); }
    void drop_link(PeerIndex remote) {
        if (!links[remote]) return;
        links[remote]->cancel_link(); links[remote].reset();
    }
    bool parked(PeerIndex remote) const { return links[remote] && !links[remote]->granted; }
    std::size_t parked_count() const {
        std::size_t count = 0;
        for (PeerIndex p = 0; p < peers; ++p) if (parked(p)) ++count;
        return count;
    }

    // ---- control messages ----
    static void put(Bytes& bytes, std::uint32_t value) { u32(bytes, value); }
    bool queue_control(PeerIndex to, Bytes bytes) {
        auto* link = links[to].get();
        if (!link || link->state != LinkState::open || link->closing) return false;
        if (link->outbound.size() >= options.queue_frames || bytes.size() + 4 > options.queue_bytes
            || link->outbound_bytes > options.queue_bytes - std::min(options.queue_bytes, bytes.size() + 4)) {
            // One slow pair ends alone and follows the normal pair-failure path; the room goes on.
            link->stop(gns_error_mesh_control, "Mesh control queue capacity exceeded"); return false;
        }
        link->control(std::move(bytes)); return true;
    }
    static std::array<std::uint8_t, gns_pair_secret_size> random_secret() {
        std::random_device random;
        std::array<std::uint8_t, gns_pair_secret_size> value{};
        for (auto& byte : value) byte = static_cast<std::uint8_t>(random());
        return value;
    }
    void send_grant(PeerIndex to, PeerIndex remote, bool dial_flag, const std::array<std::uint8_t, gns_pair_secret_size>& secret) {
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_grant)};
        put(bytes, remote); put(bytes, slot[remote].epoch); bytes.push_back(dial_flag ? 1U : 0U);
        bytes.insert(bytes.end(), secret.begin(), secret.end());
        queue_control(to, std::move(bytes));
    }
    // reason 1 (could not connect to `other`) carries the failing pair's own reason code and relay flag.
    void send_vacated(PeerIndex to, PeerIndex seat, std::uint8_t reason, PeerIndex other, unsigned code = 0, bool relay = false) {
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_vacated)};
        put(bytes, seat); put(bytes, slot[seat].epoch); bytes.push_back(reason); put(bytes, other);
        put(bytes, code); bytes.push_back(relay ? 1U : 0U);
        queue_control(to, std::move(bytes));
    }
    // Later admission dials, as in the formation policy (admission revision, then seat).
    bool later(PeerIndex a, PeerIndex b) const {
        return slot[a].admitted > slot[b].admitted || (slot[a].admitted == slot[b].admitted && a > b);
    }
    void grant_pair(PeerIndex a, PeerIndex b) {
        const auto secret = random_secret();
        const auto dialer = later(a, b) ? a : b, other = dialer == a ? b : a;
        introduced[a] = static_cast<MeshSeats>(introduced[a] | bit(b)); introduced[b] = static_cast<MeshSeats>(introduced[b] | bit(a));
        failed[a] &= static_cast<MeshSeats>(~bit(b)); failed[b] &= static_cast<MeshSeats>(~bit(a));
        failed_since[a][b].reset(); failed_since[b][a].reset();
        reported_by[a][b] = {}; reported_by[b][a] = {};
        claimed[a] &= static_cast<MeshSeats>(~bit(b)); claimed[b] &= static_cast<MeshSeats>(~bit(a));
        send_grant(dialer, other, true, secret);
        send_grant(other, dialer, false, secret);
    }

    // ---- pair failure bookkeeping ----
    // A pair that never opened ("failed") or one that closed after opening ("lost"). Reports the cause to the host.
    void fail_pair(PeerIndex remote, unsigned code, std::string message, bool was_open_link = false) {
        auto& s = slot[remote];
        if (s.reported) return;
        s.reported = true;
        s.diag = diagnostic(was_open_link ? GnsPairPhase::lost : GnsPairPhase::failed, code, turn.has_value(), true);
        emit(was_open_link ? GnsMeshEventKind::pair_lost : GnsMeshEventKind::pair_failed, remote, 0, code, turn.has_value());
        static_cast<void>(message);
        if (host()) return; // the host reads its own pairs directly
        if (remote == 0) {
            if (!locked) fatal(code, "Could not connect to the room host");
            return;
        }
        if (locked) return; // after Lock the session owns pair loss, not the formation
        mask_sent = false; // the open set changed
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_report)};
        put(bytes, remote); bytes.push_back(was_open_link ? 2U : 1U); bytes.push_back(turn ? 1U : 0U);
        put(bytes, gns_pair_reason(code) ? code : 1762U); // the host only accepts the pair reasons
        queue_control(0, std::move(bytes));
    }

    // ---- roster ----
    void on_attached(PeerIndex seat, std::uint64_t at) {
        auto& s = slot[seat];
        s.admitted = at; s.gone = false; s.refused = false;
        s.reported = false; s.was_open = false; s.diag = {}; s.expect_until.reset();
        s.signals.reset(); s.retired.clear(); s.folded = {}; // a different occupant is a different pair
        if (host()) {
            ++s.epoch;
            formation->attach(seat, at);
        }
    }
    void vacated(PeerIndex seat, std::uint8_t reason) {
        if (seat == self || seat >= max_match_seats || locked) return;
        auto& s = slot[seat];
        const bool announce = host() && (formation->occupied() & bit(seat));
        s.gone = true;
        if (host()) {
            formation->vacate(seat);
            present &= static_cast<MeshSeats>(~bit(seat)); claimed[seat] = 0; failed[seat] = 0; introduced[seat] = 0;
            for (PeerIndex p = 0; p < peers; ++p) {
                claimed[p] &= static_cast<MeshSeats>(~bit(seat)); failed[p] &= static_cast<MeshSeats>(~bit(seat));
                introduced[p] &= static_cast<MeshSeats>(~bit(seat));
                failed_since[p][seat].reset(); failed_since[seat][p].reset();
                reported_by[p][seat] = {}; reported_by[seat][p] = {};
            }
            if (announce) for (PeerIndex p = 1; p < peers; ++p) if (p != seat && (present & bit(p))) send_vacated(p, seat, reason, 0);
            if (links[seat]) { links[seat]->cancel_link(); links[seat].reset(); }
            s.role = Slot::Role::none; s.have_grant = false; s.reported = false; s.was_open = false; s.diag = {};
            if (announce) emit(GnsMeshEventKind::seat_vacated, seat);
        } else {
            const bool was_host = seat == 0;
            drop_link(seat);
            s.role = Slot::Role::none; s.have_grant = false; s.reported = false; s.was_open = false; s.diag = {};
            if (seat != 0) emit(GnsMeshEventKind::seat_vacated, seat);
            if (was_host) fatal(1760, "The room host left before Start");
        }
    }
    void sync_roster() {
        const auto roster = room->roster();
        revision = roster.revision;
        if (roster.capacity != peers) { fatal(1765, "Room size changed"); return; }
        locked = locked || roster.state == GnsRoomState::locked;
        if (!locked && roster.state == GnsRoomState::closed) { fatal(1760, "The room closed before Start"); return; }
        for (PeerIndex seat = 0; seat < peers; ++seat) {
            if (seat == self) continue;
            auto& s = slot[seat];
            const auto& now = roster.slots[seat];
            const bool attached = now.state == GnsRoomSlotState::attached;
            if (locked) {
                if (!now.identity.empty() && !s.identity.empty() && now.identity != s.identity) {
                    fatal(1790, "Repair roster identity changed"); return;
                }
                s.state = now.state;
                continue;
            }
            if (!attached) s.ignored.clear();
            else if (!s.ignored.empty() && now.identity == s.ignored) continue; // released by the host; the service lags
            if (attached && (s.state != GnsRoomSlotState::attached || s.identity != now.identity)) {
                if (s.state == GnsRoomSlotState::attached) vacated(seat, 0);
                s.state = GnsRoomSlotState::attached; s.identity = now.identity;
                on_attached(seat, roster.revision);
            } else if (!attached && s.state == GnsRoomSlotState::attached) {
                vacated(seat, 0);
                s.state = now.state; s.identity.clear();
            } else if (!attached) s.state = now.state;
        }
    }

    // ---- signalling ----
    struct Receive final : ISteamNetworkingSignalingRecvContext {
        Mesh& mesh; PeerIndex from{}; const std::string* identity{}; bool deferred{};
        explicit Receive(Mesh& owner) : mesh(owner) {}
        ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection connection,
            const SteamNetworkingIdentity& peer, int virtual_port) override {
            return mesh.accept_request(connection, peer, virtual_port, from, *identity, deferred);
        }
        void SendRejectionSignal(const SteamNetworkingIdentity&, const void*, int) override {}
    };
    ISteamNetworkingConnectionSignaling* accept_request(HSteamNetConnection connection,
        const SteamNetworkingIdentity& peer, int virtual_port, PeerIndex from, const std::string& identity, bool& deferred) {
        const auto* name = peer.GetGenericString();
        auto& s = slot[from];
        // One repair request is accepted per pair and epoch while the pair is open. A pair that is not open takes the newest
        // request: the dialer may have dialled once with the previous epoch's material (its link closed before its own
        // incident armed) and again after arming, and refusing the second left the pair unrepairable (#4021). A pair this
        // seat gave up on stays failed for the epoch: the request is late, and the mesh has already been told.
        if (virtual_port != 0 || !name || identity != name || from >= peers || from == self || closing || failure
            || (locked && (s.excluded || s.gave_up || (s.repair_attempted && link(from) == LinkState::open)))
            || s.state != GnsRoomSlotState::attached || s.identity != identity || (!locked && !host() && from == 0)
            || (!locked && !host() && s.role == Slot::Role::dial) || (locked && from < self)) return nullptr;
        // The SDK dispatches maintenance for existing connections without this
        // callback. Defer only a genuinely new repair request until its keys and
        // fresh attachment are ready; do not gate the live connection's signals.
        if (locked && (!repair_auth || !attachment_ready)) { deferred = true; return nullptr; }
        if (links[from]) {
            // A regrant can reach the dialer before it reaches us: replace a pair link that is already ending.
            const auto& old = *links[from];
            if (old.state == LinkState::open && !old.closing && !locked) return nullptr;
            if (locked) note(from, std::string("repair request replaces a link that reads ") + (old.state == LinkState::open && !old.closing ? "open" : "not open"));
            if (!locked && !host() && s.role != Slot::Role::accept) return nullptr;
            drop_link(from);
        } else if (!host() && parked_count() >= gns_pending_pair_limit) return nullptr;
        if (locked) { s.repair_attempted = true; s.reported = false; }
        auto link = make_link(from, true);
        if (!host() && s.have_grant && s.role == Slot::Role::accept) { link->secret = s.secret; link->granted = true; }
        auto* raw = link.get();
        links[from] = std::move(link);
        s.expect_until.reset();
        if (locked) {
            if (!raw->turn) {
                // Incoming streams inherit their listener's original config.
                // An explicit no-TURN refresh must clear that old credential.
                for (const auto option : {k_ESteamNetworkingConfig_P2P_TURN_ServerList,
                    k_ESteamNetworkingConfig_P2P_TURN_UserList, k_ESteamNetworkingConfig_P2P_TURN_PassList}) {
                    if (!SteamNetworkingUtils()->SetConnectionConfigValueString(connection, option, "")) {
                        s.diag = diagnostic(GnsPairPhase::lost, 1791, false, true);
                        s.reported = true; s.gave_up = true; emit(GnsMeshEventKind::pair_failed, from, 0, 1791);
                        drop_link(from); return nullptr;
                    }
                }
            }
            const auto settings = raw->config();
            // The first four settings (callback, timeouts, queue bound) are
            // unchanged and already inherited from the listener.
            for (std::size_t i = 4; i < settings.size(); ++i) {
                if (!SteamNetworkingUtils()->SetConfigValueStruct(settings[i], k_ESteamNetworkingConfig_Connection, connection)) {
                    s.diag = diagnostic(GnsPairPhase::lost, 1791, raw->turn.has_value(), true);
                    s.reported = true; s.gave_up = true; emit(GnsMeshEventKind::pair_failed, from, 0, 1791, raw->turn.has_value());
                    drop_link(from); return nullptr;
                }
            }
        }
        if (!raw->adopt(connection)) { drop_link(from); return nullptr; }
        return new Signal(raw->signalling_state);
    }
    void pump_room() {
        std::vector<GnsRoomSignal> incoming;
        for (unsigned i = 0; i < 32; ++i) {
            auto next = room->poll();
            if (!next) {
                if (!locked) fatal(1760, "The room signalling connection failed");
                break;
            }
            if (!next.value()) break;
            incoming.push_back(std::move(*next.value()));
        }
        sync_roster();
        if (failure) return;
        // Only a deferred new connection needs fresh pairing. Ongoing ICE
        // maintenance must keep reaching the SDK even before repair is armed,
        // and must not trigger an attachment refresh of its own.
        if (locked && repair_auth && !repair_signals.empty() && !attachment_ready && !refresh_failed)
            static_cast<void>(refresh_attachment());
        if (attachment_ready) while (!repair_signals.empty()) {
            incoming.push_back(std::move(repair_signals.front())); repair_signals.pop_front();
        }
        Receive context(*this);
        for (const auto& signal : incoming) {
            if (signal.from_slot >= peers || signal.from_slot == self || signal.bytes.empty() || signal.bytes.size() > 64 * 1024
                || signal.from_identity.empty() || slot[signal.from_slot].identity != signal.from_identity) continue;
            auto& signals = slot[signal.from_slot].signals;
            if (signals) {
                signals->received_blobs.fetch_add(1); signals->received_bytes.fetch_add(signal.bytes.size());
            }
            context.from = signal.from_slot; context.identity = &signal.from_identity;
            context.deferred = false;
            // Late signals for a closed connection are normal in a mesh; GNS ignores what it cannot place.
            static_cast<void>(sockets().ReceivedP2PCustomSignal(signal.bytes.data(), static_cast<int>(signal.bytes.size()), &context));
            if (context.deferred) {
                if (!retain_deferred_signal(repair_signals, signal)) {
                    drop_link(signal.from_slot);
                    slot[signal.from_slot].repair_attempted = slot[signal.from_slot].gave_up = true;
                    fail_pair(signal.from_slot, 1794, "Pending repair signal budget exceeded", slot[signal.from_slot].was_open);
                    std::erase_if(repair_signals, [&](const auto& pending) { return pending.from_slot == signal.from_slot; });
                    continue;
                }
            }
        }
    }

    // ---- control consumption ----
    void member_control(Bytes bytes) {
        const auto kind = static_cast<TcpMessage>(bytes.front());
        const std::span<const std::uint8_t> body = std::span<const std::uint8_t>(bytes).subspan(1);
        const auto word = [&](std::size_t at) { std::uint32_t v{}; for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(body[at + i]) << (i * 8); return v; };
        if (kind == TcpMessage::mesh_grant) {
            const PeerIndex remote = word(0);
            if (locked || remote >= peers || remote == self) { fatal(gns_error_mesh_control, "Invalid pair grant"); return; }
            auto& s = slot[remote];
            const bool parked_here = parked(remote) && body[8] == 0;
            s.epoch = word(4); s.role = body[8] ? Slot::Role::dial : Slot::Role::accept; s.have_grant = true;
            std::copy_n(body.begin() + 9, gns_pair_secret_size, s.secret.begin());
            s.reported = false; s.was_open = false; s.diag = {}; s.gone = false;
            s.expect_until = Clock::now() + options.pair_timeout;
            if (parked_here) {
                links[remote]->secret = s.secret; links[remote]->granted = true; s.expect_until.reset();
            } else drop_link(remote); // a regrant replaces whatever the pair had
            sent_mask &= static_cast<MeshSeats>(~bit(remote)); mask_sent = false;
        } else if (kind == TcpMessage::mesh_vacated) {
            const PeerIndex seat = word(0);
            const auto epoch = word(4);
            const auto reason = body[8];
            const PeerIndex other = word(9);
            const unsigned code = word(13);
            const bool relay = body[17] != 0;
            if (seat == self) {
                if (reason == 1) fatal(code, "Could not connect to another player in the room", other, relay);
                else fatal(1760, "You were removed from the room");
                return;
            }
            if (seat >= peers) { fatal(gns_error_mesh_control, "Invalid vacated seat"); return; }
            auto& s = slot[seat];
            // A vacancy report older than this occupant's grant does not end the new pair.
            const bool stale = s.have_grant ? s.epoch > epoch : (links[seat] && s.state == GnsRoomSlotState::attached);
            if (!stale) { vacated(seat, reason); s.state = GnsRoomSlotState::vacant; s.identity.clear(); }
        } else fatal(gns_error_mesh_control, "Unexpected mesh control from the host");
    }
    void evaluate_pair(PeerIndex a, PeerIndex b) {
        const auto result = formation->pair_failed(a, b, {(failed[a] & bit(b)) != 0, (failed[b] & bit(a)) != 0});
        if (result.action == MeshFormationAction::regrant) {
            grant_pair(a, b);
            emit(GnsMeshEventKind::pair_failed, result.seat, result.unreachable, 0, turn.has_value());
        } else if (result.action == MeshFormationAction::refuse) {
            const auto guest = result.seat, unreachable = result.unreachable;
            // The failing pair's own reason, from whichever end reported one (1762 when neither did: both stayed silent).
            auto why = reported_by[guest][unreachable];
            if (why.code == 0) why = reported_by[unreachable][guest];
            if (why.code == 0) why = {1762, turn.has_value()};
            for (PeerIndex p = 1; p < peers; ++p) if (present & bit(p)) send_vacated(p, guest, 1, unreachable, why.code, why.relay);
            if (links[guest]) links[guest]->close_link(); // flushes the notice before the host pair ends
            slot[guest].refused = true; slot[guest].gone = true; slot[guest].ignored = slot[guest].identity;
            slot[guest].state = GnsRoomSlotState::vacant; slot[guest].identity.clear();
            present &= static_cast<MeshSeats>(~bit(guest));
            claimed[guest] = 0; failed[guest] = 0; introduced[guest] = 0;
            for (PeerIndex p = 0; p < peers; ++p) {
                claimed[p] &= static_cast<MeshSeats>(~bit(guest)); failed[p] &= static_cast<MeshSeats>(~bit(guest));
                introduced[p] &= static_cast<MeshSeats>(~bit(guest));
                failed_since[p][guest].reset(); failed_since[guest][p].reset();
                reported_by[p][guest] = {}; reported_by[guest][p] = {};
            }
            static_cast<void>(room->release(guest));
            emit(GnsMeshEventKind::seat_refused, guest, unreachable, why.code, why.relay);
        }
    }
    void host_control(PeerIndex from, Bytes bytes) {
        const auto kind = static_cast<TcpMessage>(bytes.front());
        const std::span<const std::uint8_t> body = std::span<const std::uint8_t>(bytes).subspan(1);
        const auto word = [&](std::size_t at) { std::uint32_t v{}; for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(body[at + i]) << (i * 8); return v; };
        if (locked) return;
        // A seat the host has just refused or released can still deliver controls on its closing stream: ignore them.
        if (!(present & bit(from))) return;
        if (kind == TcpMessage::mesh_ready_mask) {
            claimed[from] = static_cast<MeshSeats>(word(0));
        } else if (kind == TcpMessage::mesh_report) {
            const PeerIndex remote = word(0);
            // Only pairs the host introduced can fail: a report about anything else is not trusted.
            if (!(introduced[from] & bit(remote))) return;
            reported_by[from][remote] = {word(6), body[5] != 0}; // validated by the decoder
            claimed[from] &= static_cast<MeshSeats>(~bit(remote));
            add(failed[from], remote);
            if (!failed_since[from][remote]) failed_since[from][remote] = Clock::now();
            evaluate_pair(from, remote);
        } else fatal(gns_error_mesh_control, "Unexpected mesh control from a member");
    }

    // ---- per-poll state machine ----
    // Openings first, then the controls a pair delivered, then deaths: a closing host pair can carry its last control
    // (the vacancy notice) in the same pump as its close, and that notice must be read before the close is acted on.
    void harvest(bool deaths) {
        for (PeerIndex remote = 0; remote < peers; ++remote) {
            if (remote == self || !links[remote]) continue;
            auto& link = *links[remote];
            auto& s = slot[remote];
            if (!deaths && link.state == LinkState::open && !link.published) {
                link.published = true; ++s.generation;
                s.was_open = true; s.reported = false; s.diag = diagnostic(GnsPairPhase::open, 0, turn.has_value(), false, link.route);
                emit(GnsMeshEventKind::pair_open, remote);
                if (host() && !locked) {
                    add(present, remote);
                    for (PeerIndex other = 1; other < peers; ++other)
                        if (other != remote && (present & bit(other)) && !(introduced[remote] & bit(other))) grant_pair(remote, other);
                }
            }
            if (!deaths) continue;
            if (link.dead() && s.refused) { links[remote].reset(); continue; }
            if (link.dead()) {
                const unsigned code = link.failure ? static_cast<unsigned>(std::stoul(link.failure->code.substr(link.failure->code.size() - 4)))
                    : gns_error_pair_lost;
                if (host()) {
                    if (!locked && !s.gone) {
                        // A guest's host pair ended before Start: the slot is free for another newcomer.
                        s.gone = true;
                        const auto seat = remote;
                        vacated(seat, 0);
                        static_cast<void>(room->release(seat));
                        s.ignored = s.identity; s.state = GnsRoomSlotState::vacant; s.identity.clear();
                        emit(GnsMeshEventKind::pair_failed, seat, 0, code, turn.has_value());
                    } else if (!s.reported) {
                        s.reported = true; s.diag = diagnostic(s.was_open ? GnsPairPhase::lost : GnsPairPhase::failed, code, turn.has_value(), true);
                        emit(s.was_open ? GnsMeshEventKind::pair_lost : GnsMeshEventKind::pair_failed, remote, 0, code, turn.has_value());
                    }
                } else if (!redial_repair(remote, link, code)) fail_pair(remote, code, "Pair closed", s.was_open);
                if (links[remote]) { links[remote].reset(); }
            }
        }
    }
    // A repair dial that died before it opened is made again, after a growing pause, until the dial budget or the repair
    // deadline is spent. The acceptor may still hold the previous epoch's material when the first request arrives (its
    // incident arms later than ours), and that attempt is lost however the two arms are ordered (#4021).
    bool redial_repair(PeerIndex remote, const Impl& link, unsigned code) {
        auto& s = slot[remote];
        if (!locked || !repair_auth || remote >= self || link.published || s.gave_up || s.excluded || !s.repair_attempted
            || s.repair_dials >= gns_repair_dial_limit || !repair_deadline || Clock::now() >= *repair_deadline) return false;
        const auto pause = gns_repair_redial_pause * (1U << (s.repair_dials - 1U));
        s.repair_retry_after = Clock::now() + pause;
        s.repair_attempted = false; s.reported = false;
        note(remote, "repair dial " + std::to_string(s.repair_dials) + " ended before it opened (EAWR-NET-" + std::to_string(code)
            + "); dials again in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(pause).count()) + " ms");
        return true;
    }
    void member_tick() {
        if (self == 0 || failure || locked) return;
        const auto now = Clock::now();
        if (!links[0] && !slot[0].reported && slot[0].state == GnsRoomSlotState::attached && !slot[0].gone) {
            slot[0].role = Slot::Role::dial;
            dial(0);
        }
        for (PeerIndex remote = 1; remote < peers; ++remote) {
            auto& s = slot[remote];
            if (remote == self || s.reported || !s.have_grant) continue;
            if (s.state != GnsRoomSlotState::attached) continue;
            if (s.role == Slot::Role::dial && !links[remote]) dial(remote);
            else if (s.role == Slot::Role::accept && !links[remote] && s.expect_until && now >= *s.expect_until)
                fail_pair(remote, 1762, "No direct or relayed path from this peer");
        }
        // Report the set of verified pairs so the host can tell when this seat is fully connected.
        MeshSeats mask = 0;
        for (PeerIndex remote = 0; remote < peers; ++remote)
            if (links[remote] && links[remote]->state == LinkState::open && !links[remote]->closing) add(mask, remote);
        if ((mask != sent_mask || !mask_sent) && links[0] && links[0]->state == LinkState::open && !locked) {
            Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_ready_mask)};
            put(bytes, mask);
            if (queue_control(0, std::move(bytes))) { sent_mask = mask; mask_sent = true; }
        }
    }
    void host_tick() {
        if (!host() || failure) return;
        const auto now = Clock::now();
        MeshSeats own = 0;
        for (PeerIndex remote = 1; remote < peers; ++remote)
            if (links[remote] && links[remote]->state == LinkState::open && !links[remote]->closing) add(own, remote);
        claimed[0] = own;
        if (!locked) {
            for (PeerIndex a = 0; a < peers; ++a) for (PeerIndex b = a + 1; b < peers; ++b) {
                if (!(present & bit(b)) || !(a == 0 || (present & bit(a)))) continue;
                if ((claimed[a] & bit(b)) && (claimed[b] & bit(a))) formation->pair_open(a, b);
            }
            // One end of a failed pair may never report (it may be the unreachable one): the policy decides when
            // its silence counts as a failure (mesh_silent_end_failed).
            for (PeerIndex a = 1; a < peers; ++a) for (PeerIndex b = 1; b < peers; ++b) {
                if (a == b || !failed_since[a][b] || (failed[b] & bit(a))) continue;
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - *failed_since[a][b]);
                if (!mesh_silent_end_failed({true, false}, waited, options.pair_timeout).b_failed) continue;
                add(failed[b], a);
                evaluate_pair(a, b);
            }
        }
        for (PeerIndex remote = 1; remote < peers; ++remote) {
            auto& s = slot[remote];
            if (s.state == GnsRoomSlotState::attached && !links[remote] && !s.reported && !s.gone)
                s.expect_until = s.expect_until ? s.expect_until : std::optional<Clock::time_point>(now + options.pair_timeout);
            if (s.expect_until && !links[remote] && now >= *s.expect_until && !locked) {
                // The newcomer never reached the host within the pair deadline: free the slot.
                s.expect_until.reset();
                vacated(remote, 0);
                static_cast<void>(room->release(remote));
                s.ignored = s.identity; s.state = GnsRoomSlotState::vacant; s.identity.clear();
                emit(GnsMeshEventKind::pair_failed, remote, 0, 1762, turn.has_value());
            }
        }
    }
    void poll() {
        sockets().RunCallbacks();
        if (!failure && !closing) {
            if (!locked) {
                if (const auto status = room->status(); !status) fatal(1760, "The room attachment closed before Start");
            }
            if (!failure) pump_room();
        }
        unsigned recv = 64, send = 64;
        for (std::size_t i = 0; i < peers; ++i) {
            const auto remote = static_cast<PeerIndex>((rotate + i) % peers);
            if (!links[remote]) continue;
            links[remote]->recv_budget = recv; links[remote]->send_budget = send;
            links[remote]->pump();
            recv = links[remote]->recv_budget; send = links[remote]->send_budget;
        }
        rotate = (rotate + 1) % peers;
        if (failure || closing) {
            if (std::none_of(links.begin(), links.end(), [](const auto& l) { return l && !l->dead(); }) && listener != k_HSteamListenSocket_Invalid) release_listener();
            return;
        }
        harvest(false);
        for (PeerIndex remote = 0; remote < peers && !failure; ++remote) {
            if (!links[remote]) continue;
            auto& link = *links[remote];
            while (!link.controls.empty() && !failure) {
                auto bytes = std::move(link.controls.front()); link.controls.pop_front();
                if (host()) host_control(remote, std::move(bytes)); else if (remote == 0) member_control(std::move(bytes));
                else fatal(gns_error_mesh_control, "Mesh control from a non-host pair");
            }
        }
        harvest(true);
        if (locked) repair_tick();
        else { member_tick(); host_tick(); }
    }
    core::Result<bool> refresh_attachment() {
        if (!repair_deadline) repair_deadline = Clock::now() + options.repair_grace;
        if (refresh_failed) return core::Result<bool>::failure(error(1791, "Fresh room pairing failed"));
        auto refreshed = room->repair_attachment(repair_auth->epoch);
        attachment_pending = true;
        if (!refreshed) {
            refresh_failed = true; repair_signals.clear();
            note(-1, "repair attachment refused: " + refreshed.error().code);
            return refreshed;
        }
        if (!refreshed.value()) return refreshed;
        turn = room->turn_credentials(); turn_users.clear(); turn_passwords.clear();
        if ((turn && !turn_valid(*turn)) || (options.turn_only && !turn)) {
            refresh_failed = true; return core::Result<bool>::failure(error(1791, "Invalid fresh TURN credentials"));
        }
        if (turn) {
            const auto count = static_cast<std::size_t>(std::count(turn->uri.begin(), turn->uri.end(), ',')) + 1;
            for (std::size_t i = 0; i < count; ++i) {
                turn_users += (i ? "," : "") + turn->username;
                turn_passwords += (i ? "," : "") + turn->password;
            }
        }
        attachment_ready = true; attachment_pending = false; attachment_epoch = repair_auth->epoch;
        note(-1, "repair attachment ready for epoch " + std::to_string(attachment_epoch) + (turn ? " with fresh relay credentials" : ""));
        return core::Result<bool>::success(true);
    }
    void repair_tick() {
        if (!repair_auth) return;
        bool needed = false;
        for (PeerIndex p = 0; p < peers; ++p)
            if (p != self && slot[p].was_open && !slot[p].excluded && link(p) != LinkState::open) needed = true;
        if (!needed) return;
        if (!repair_deadline) repair_deadline = Clock::now() + options.repair_grace;
        const auto failed_repair = [&](unsigned code) {
            for (PeerIndex p = 0; p < peers; ++p) {
                auto& s = slot[p];
                if (p == self || s.excluded || !s.was_open || link(p) == LinkState::open
                    || (s.repair_attempted && s.reported && s.diag.relay_failed)) continue;
                note(p, "repair gave up: EAWR-NET-" + std::to_string(code));
                drop_link(p); s.repair_attempted = s.gave_up = true; s.reported = true;
                s.diag = diagnostic(GnsPairPhase::lost, code, turn.has_value(), true);
                emit(GnsMeshEventKind::pair_failed, p, 0, code, turn.has_value());
            }
        };
        if (Clock::now() >= *repair_deadline) { failed_repair(1793); return; }
        if (!attachment_ready) {
            auto fresh = refresh_attachment();
            if (!fresh) { failed_repair(1791); return; }
            if (!fresh.value()) return;
        }
        for (PeerIndex p = 0; p < peers; ++p) {
            auto& s = slot[p];
            if (p == self || s.excluded || !s.was_open || s.repair_attempted || link(p) == LinkState::open) continue;
            if (s.state != GnsRoomSlotState::attached) continue;
            if (s.repair_retry_after && Clock::now() < *s.repair_retry_after) continue;
            if (p < self) {
                note(p, "repair dials");
                drop_link(p); s.repair_attempted = true; s.reported = false;
                ++s.repair_dials; s.repair_retry_after.reset();
                dial(p);
            }
        }
    }
    void release_listener() {
        if (listener == k_HSteamListenSocket_Invalid) return;
        sockets().CloseListenSocket(listener); listener = k_HSteamListenSocket_Invalid;
    }
    void close() {
        if (closing) return;
        closing = true;
        for (PeerIndex p = 0; p < peers; ++p) if (links[p]) links[p]->close_link();
    }
    void cancel() {
        closing = true;
        for (PeerIndex p = 0; p < peers; ++p) if (links[p]) links[p]->cancel_link();
        release_listener();
    }
    bool closed() const {
        if (listener != k_HSteamListenSocket_Invalid) return false;
        return std::none_of(links.begin(), links.end(), [](const auto& l) { return l && l->stream != k_HSteamNetConnection_Invalid; });
    }
    ~Mesh() {
        for (auto& link : links) link.reset();
        release_listener();
    }

    // ---- observation ----
    LinkState link(PeerIndex remote) const {
        if (remote >= peers || remote == self) return LinkState::closed;
        if (slot[remote].excluded) return LinkState::closed;
        if (!links[remote]) return slot[remote].gone || slot[remote].diag.phase == GnsPairPhase::failed
            || slot[remote].diag.phase == GnsPairPhase::lost ? LinkState::closed : LinkState::connecting;
        const auto& state = links[remote]->state;
        return links[remote]->closing ? LinkState::closed : state;
    }
    GnsPairDiagnostic pair(PeerIndex remote) const {
        if (remote >= peers || remote == self) return diagnostic(GnsPairPhase::closed, 0, false, false);
        const auto& s = slot[remote];
        if (s.excluded) return diagnostic(GnsPairPhase::closed, 0, false, false);
        if (locked && repair_auth && s.was_open && link(remote) != LinkState::open && !s.repair_attempted)
            return diagnostic(attachment_ready ? GnsPairPhase::repairing : GnsPairPhase::refreshing, 0, turn.has_value(), false);
        if (!links[remote]) return s.diag.phase == GnsPairPhase::absent && s.gone ? diagnostic(GnsPairPhase::closed, 0, false, false) : s.diag;
        const auto& l = *links[remote];
        if (l.state == LinkState::open) return diagnostic(GnsPairPhase::open, 0, turn.has_value(), false, l.route);
        if (l.dead()) return s.diag;
        if (l.proof && !l.authenticated()) return diagnostic(l.accepted ? GnsPairPhase::authenticating : GnsPairPhase::repairing,
            0, l.turn.has_value(), false);
        return diagnostic(l.ready ? GnsPairPhase::handshaking : GnsPairPhase::signalling, 0, turn.has_value(), false);
    }
    // Per pair, across every link the slot's current occupant has had (a regrant adds to the totals, never resets them).
    GnsSignalCounts counts(PeerIndex remote) const {
        if (remote >= peers) return {};
        GnsSignalCounts total = slot[remote].folded;
        const auto add = [&](const SignallingState& state) {
            total.sent_blobs += state.sent_blobs.load(); total.sent_bytes += state.sent_bytes.load();
            total.received_blobs += state.received_blobs.load(); total.received_bytes += state.received_bytes.load();
        };
        for (const auto& old : slot[remote].retired) add(*old);
        if (slot[remote].signals) add(*slot[remote].signals);
        return total;
    }
    MeshSeats connected_seats() const {
        if (host()) {
            MeshSeats result = 0;
            for (PeerIndex p = 0; p < peers; ++p) if (formation->connected(p)) add(result, p);
            return result;
        }
        MeshSeats result = bit(self);
        for (PeerIndex p = 0; p < peers; ++p) if (links[p] && links[p]->state == LinkState::open && !links[p]->closing) add(result, p);
        return result;
    }
    bool complete() const {
        if (host()) return formation->complete();
        std::uint32_t occupied = 1;
        for (PeerIndex p = 0; p < peers; ++p) if (p != self && slot[p].state == GnsRoomSlotState::attached) ++occupied;
        return occupied == peers && (connected_seats() == static_cast<MeshSeats>((1U << peers) - 1U));
    }
};
#else
struct GnsTransport::Mesh {};
#endif

GnsRoute GnsTransport::observed_route() { return static_cast<GnsRoute>(observed_route_cell().load()); }
bool GnsTransport::available() { return EAWR_HAS_GNS != 0; }
void GnsTransport::set_log_sink(GnsLogSink sink) {
    auto& log = gns_log();
    const std::lock_guard lock(log.mutex);
    log.enabled.store(static_cast<bool>(sink));
    log.sink = std::move(sink);
    if (!log.enabled.load()) { log.pending.clear(); log.limited.clear(); log.dropped = 0; log.has_pending.store(false); }
}
GnsTransport::GnsTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GnsTransport::GnsTransport(std::unique_ptr<Mesh> mesh) : mesh_(std::move(mesh)) {}
GnsTransport::~GnsTransport() = default;
core::Result<std::unique_ptr<GnsTransport>> GnsTransport::create(bool hosting, std::string address,
    std::uint16_t port, GnsOptions options, std::shared_ptr<GnsSignallingEndpoint> endpoint) {
#if EAWR_HAS_GNS
    if (!valid(options) || (!hosting && !endpoint && port == 0))
        return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1501, "Invalid GNS transport options"));
    auto& runtime = Impl::Runtime::get();
    if (runtime.failure) return core::Result<std::unique_ptr<GnsTransport>>::failure(*runtime.failure);
    if (endpoint) {
        const auto& rendezvous = endpoint->rendezvous();
        SteamNetworkingIdentity local{}, remote{}, current{};
        if (rendezvous.session.empty() || rendezvous.local_identity == rendezvous.remote_identity
            || rendezvous.local_identity.empty() || rendezvous.local_identity.size() > gns_identity_max_bytes
            || rendezvous.remote_identity.empty() || rendezvous.remote_identity.size() > gns_identity_max_bytes
            || !local.SetGenericString(rendezvous.local_identity.c_str())
            || !remote.SetGenericString(rendezvous.remote_identity.c_str()))
            return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1509, "Invalid paired GNS rendezvous"));
        runtime.sockets->GetIdentity(&current);
        if (!(current == local)) {
            if (!runtime.connections.empty() || !runtime.listeners.empty())
                return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1509, "Another GNS process identity is active"));
            runtime.sockets->ResetIdentity(&local);
        }
    }
    auto impl = std::make_unique<Impl>(hosting, std::move(options));
    impl->signalling = std::move(endpoint);
    if (impl->signalling) {
        impl->turn = impl->signalling->turn_credentials();
        if (impl->turn && !turn_valid(*impl->turn))
            return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1519, "Invalid TURN rendezvous credentials"));
        if (impl->turn) {
            const auto servers = static_cast<std::size_t>(std::count(impl->turn->uri.begin(), impl->turn->uri.end(), ',')) + 1;
            for (std::size_t i = 0; i < servers; ++i) {
                impl->turn_users += (i ? "," : "") + impl->turn->username;
                impl->turn_passwords += (i ? "," : "") + impl->turn->password;
            }
        }
        impl->signalling_state = std::make_shared<SignallingState>();
        impl->signalling_state->endpoint = impl->signalling;
    }
    if (impl->options.turn_only && !impl->turn)
        return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1519, "TURN-only connection requires relay credentials"));
    const auto config = impl->config();
    if (impl->signalling) {
        if (hosting) impl->listener = runtime.sockets->CreateListenSocketP2P(0, static_cast<int>(config.size()), config.data());
        else {
            SteamNetworkingIdentity peer{};
            peer.SetGenericString(impl->signalling->rendezvous().remote_identity.c_str());
            // GNS owns and eventually releases this object, even on failure.
            impl->stream = runtime.sockets->ConnectP2PCustomSignaling(new Signal(impl->signalling_state), &peer, 0,
                static_cast<int>(config.size()), config.data());
        }
    } else {
        SteamNetworkingIPAddr ip{};
        if (!ip.ParseString(address.c_str()))
            return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1501, "GNS requires a numeric IP address"));
        ip.m_port = port;
        if (hosting) {
            // GNS requires an explicit nonzero listen port. Preserve our host
            // port-0 API by asking the OS UDP allocator and retrying a raced
            // bind. The IPv4 probe can also select a candidate for an IPv6 bind.
            const auto attempts = port == 0 ? 8U : 1U;
            for (unsigned attempt = 0; attempt < attempts && impl->listener == k_HSteamListenSocket_Invalid; ++attempt) {
                if (port == 0) {
                    const auto probe = socket::udp_open("0.0.0.0", 0);
                    if (probe.handle == socket::invalid || probe.port == 0) { socket::close(probe.handle); break; }
                    ip.m_port = probe.port; socket::close(probe.handle);
                }
                impl->listener = runtime.sockets->CreateListenSocketIP(ip, static_cast<int>(config.size()), config.data());
            }
            if (impl->listener != k_HSteamListenSocket_Invalid && runtime.sockets->GetListenSocketAddress(impl->listener, &ip))
                impl->bound_port = ip.m_port;
        } else impl->stream = runtime.sockets->ConnectByIPAddress(ip, static_cast<int>(config.size()), config.data());
    }
    if ((hosting && impl->listener == k_HSteamListenSocket_Invalid)
        || (!hosting && impl->stream == k_HSteamNetConnection_Invalid))
        return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1506, "GNS socket creation failed"));
    if (hosting) runtime.listeners.emplace(impl->listener, impl.get()); else runtime.connections.emplace(impl->stream, impl.get());
    return core::Result<std::unique_ptr<GnsTransport>>::success(std::unique_ptr<GnsTransport>(new GnsTransport(std::move(impl))));
#else
    static_cast<void>(hosting); static_cast<void>(address); static_cast<void>(port);
    static_cast<void>(options); static_cast<void>(endpoint);
    return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1500, "This build has GNS disabled"));
#endif
}
core::Result<std::unique_ptr<GnsTransport>> GnsTransport::host(std::string address, std::uint16_t port, GnsOptions options) {
    return create(true, std::move(address), port, std::move(options), {});
}
core::Result<std::unique_ptr<GnsTransport>> GnsTransport::connect(std::string address, std::uint16_t port, GnsOptions options) {
    return create(false, std::move(address), port, std::move(options), {});
}
core::Result<std::unique_ptr<GnsTransport>> GnsTransport::host_introduced(std::shared_ptr<GnsSignallingEndpoint> endpoint, GnsOptions options) {
    if (!endpoint) return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1509, "Missing GNS signalling endpoint"));
    return create(true, {}, 0, std::move(options), std::move(endpoint));
}
core::Result<std::unique_ptr<GnsTransport>> GnsTransport::connect_introduced(std::shared_ptr<GnsSignallingEndpoint> endpoint, GnsOptions options) {
    if (!endpoint) return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1509, "Missing GNS signalling endpoint"));
    return create(false, {}, 0, std::move(options), std::move(endpoint));
}
core::Result<std::unique_ptr<GnsTransport>> GnsTransport::host_room(std::shared_ptr<GnsRoomEndpoint> room, GnsOptions options) {
    return create_room(true, std::move(room), std::move(options));
}
core::Result<std::unique_ptr<GnsTransport>> GnsTransport::join_room(std::shared_ptr<GnsRoomEndpoint> room, GnsOptions options) {
    return create_room(false, std::move(room), std::move(options));
}
core::Result<std::unique_ptr<GnsTransport>> GnsTransport::create_room(bool hosting,
    std::shared_ptr<GnsRoomEndpoint> room, GnsOptions options) {
#if EAWR_HAS_GNS
    using Failure = core::Result<std::unique_ptr<GnsTransport>>;
    if (!room) return Failure::failure(error(1509, "Missing GNS room endpoint"));
    if (!valid(options, true)) return Failure::failure(error(1501, "Invalid GNS transport options"));
    auto& runtime = Impl::Runtime::get();
    if (runtime.failure) return Failure::failure(*runtime.failure);
    const auto roster = room->roster();
    const auto me = room->local_slot();
    gns_log().seat.store(static_cast<int>(me));
    if (roster.capacity != options.peers || me >= options.peers || (me == 0) != hosting)
        return Failure::failure(error(1765, "Room size or seat differs from the agreed roster"));
    if (roster.state != GnsRoomState::open || roster.slots[me].state != GnsRoomSlotState::attached)
        return Failure::failure(error(1760, "The room is closed or this seat is not attached"));
    const auto& name = roster.slots[me].identity;
    SteamNetworkingIdentity local{}, current{};
    if (name.empty() || name.size() > gns_identity_max_bytes || !local.SetGenericString(name.c_str()))
        return Failure::failure(error(1509, "Invalid room member identity"));
    runtime.sockets->GetIdentity(&current);
    if (!(current == local)) {
        if (!runtime.connections.empty() || !runtime.listeners.empty())
            return Failure::failure(error(1509, "Another GNS process identity is active"));
        runtime.sockets->ResetIdentity(&local);
    }
    auto mesh = std::make_unique<Mesh>();
    mesh->options = std::move(options);
    mesh->options.handshake_timeout = mesh->options.pair_timeout;
    mesh->room = std::move(room);
    mesh->self = me; mesh->peers = mesh->options.peers;
    mesh->turn = mesh->room->turn_credentials();
    if (mesh->turn && !turn_valid(*mesh->turn)) return Failure::failure(error(1519, "Invalid TURN rendezvous credentials"));
    if (mesh->options.turn_only && !mesh->turn)
        return Failure::failure(error(1519, "TURN-only connection requires relay credentials"));
    if (mesh->turn) {
        const auto servers = static_cast<std::size_t>(std::count(mesh->turn->uri.begin(), mesh->turn->uri.end(), ',')) + 1;
        for (std::size_t i = 0; i < servers; ++i) {
            mesh->turn_users += (i ? "," : "") + mesh->turn->username;
            mesh->turn_passwords += (i ? "," : "") + mesh->turn->password;
        }
    }
    for (PeerIndex seat = 0; seat < mesh->peers; ++seat) {
        mesh->slot[seat].state = seat == me ? GnsRoomSlotState::attached : GnsRoomSlotState::vacant;
        mesh->slot[seat].identity = seat == me ? name : std::string{};
    }
    mesh->slot[me].admitted = roster.revision;
    if (hosting) {
        mesh->formation.emplace(mesh->peers);
        mesh->formation->attach(0, roster.revision == 0 ? 1 : roster.revision);
        mesh->slot[0].epoch = 1;
    }
    {
        // One listener takes every incoming pair connection; the room's signals route each to its slot.
        Impl probe(true, mesh->options);
        probe.pair_link = true; probe.turn = mesh->turn; probe.turn_users = mesh->turn_users; probe.turn_passwords = mesh->turn_passwords;
        const auto config = probe.config();
        mesh->listener = runtime.sockets->CreateListenSocketP2P(0, static_cast<int>(config.size()), config.data());
    }
    if (mesh->listener == k_HSteamListenSocket_Invalid) return Failure::failure(error(1506, "GNS socket creation failed"));
    return Failure::success(std::unique_ptr<GnsTransport>(new GnsTransport(std::move(mesh))));
#else
    static_cast<void>(hosting); static_cast<void>(room); static_cast<void>(options);
    return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1500, "This build has GNS disabled"));
#endif
}
core::Result<void> GnsTransport::send(PeerIndex to, std::span<const std::uint8_t> frame) {
#if EAWR_HAS_GNS
    if (mesh_) {
        if (to >= mesh_->peers || to == mesh_->self || !mesh_->links[to] || mesh_->failure)
            return core::Result<void>::failure(error(1506, "GNS link is not open for this peer"));
        return mesh_->links[to]->send_frame(to, frame);
    }
    return impl_->send_frame(to, frame);
#else
    static_cast<void>(to); static_cast<void>(frame);
    return core::Result<void>::failure(error(1500, "This build has GNS disabled"));
#endif
}
GnsQueueProgress GnsTransport::queue_progress(PeerIndex peer) const {
    GnsQueueProgress progress;
    const Impl* link = impl_.get();
#if EAWR_HAS_GNS
    if (mesh_) {
        progress.deferred_signals = static_cast<std::size_t>(std::count_if(mesh_->repair_signals.begin(), mesh_->repair_signals.end(),
            [peer](const auto& signal) { return signal.from_slot == peer; }));
        link = peer < mesh_->peers ? mesh_->links[peer].get() : nullptr;
    }
#endif
    if (!link || peer != link->remote()) return progress;
    progress.inbound_frames = link->inbound.size(); progress.inbound_bytes = link->inbound_bytes;
#if EAWR_HAS_GNS
    progress.outbound_frames = link->outbound.size(); progress.outbound_bytes = link->outbound_bytes;
    SteamNetConnectionRealTimeStatus_t status{};
    if (link->stream != k_HSteamNetConnection_Invalid
        && link->sockets().GetConnectionRealTimeStatus(link->stream, &status, 0, nullptr) == k_EResultOK) {
        progress.native_valid = true;
        progress.native_state = static_cast<int>(status.m_eState); progress.ping_ms = status.m_nPing;
        progress.pending_reliable = status.m_cbPendingReliable; progress.unacked_reliable = status.m_cbSentUnackedReliable;
        progress.pending_unreliable = status.m_cbPendingUnreliable;
        progress.reliable_drained = status.m_eState == k_ESteamNetworkingConnectionState_Connected
            && progress.outbound_frames == 0 && status.m_cbPendingReliable == 0 && status.m_cbSentUnackedReliable == 0;
    }
#endif
    return progress;
}
std::optional<Received> GnsTransport::poll() {
    gns_log_flush();
#if EAWR_HAS_GNS
    if (mesh_) {
        // A lockstep tick drains up to one frame per remote with repeated polls: serve frames that are already queued
        // without another full pump, but pump at least every 32 frames so keepalives and deadlines keep running.
        const auto queued = [&] {
            return std::any_of(mesh_->links.begin(), mesh_->links.end(), [](const auto& link) { return link && !link->inbound.empty(); });
        };
        if (mesh_->served >= 32 || !queued()) { mesh_->poll(); mesh_->served = 0; }
        // Rotate across pair links so one busy edge cannot starve the others.
        for (std::size_t i = 0; i < mesh_->peers; ++i) {
            const auto remote = static_cast<PeerIndex>((mesh_->served + mesh_->rotate + i) % mesh_->peers);
            auto* link = mesh_->links[remote].get();
            if (!link || link->inbound.empty()) continue;
            auto frame = std::move(link->inbound.front()); link->inbound.pop_front(); link->inbound_bytes -= frame.frame.size();
            ++mesh_->served;
            return frame;
        }
        return {};
    }
    impl_->recv_budget = impl_->send_budget = 64;
    impl_->pump();
#endif
    if (!impl_ || impl_->inbound.empty()) return {};
    auto frame = std::move(impl_->inbound.front()); impl_->inbound.pop_front(); impl_->inbound_bytes -= frame.frame.size();
    return frame;
}
LinkState GnsTransport::link(PeerIndex peer) const {
#if EAWR_HAS_GNS
    if (mesh_) return mesh_->link(peer);
#endif
    return peer == impl_->remote() ? impl_->state : LinkState::closed;
}
PeerIndex GnsTransport::self() const {
#if EAWR_HAS_GNS
    if (mesh_) return mesh_->self;
#endif
    return impl_->hosting ? 0U : 1U;
}
std::uint32_t GnsTransport::peer_count() const {
#if EAWR_HAS_GNS
    if (mesh_) return mesh_->peers;
#endif
    return 2;
}
std::uint16_t GnsTransport::port() const { return impl_ ? impl_->bound_port : 0; }
const std::optional<core::Diagnostic>& GnsTransport::diagnostic() const {
#if EAWR_HAS_GNS
    if (mesh_) return mesh_->failure;
#endif
    return impl_->failure;
}
std::optional<std::chrono::microseconds> GnsTransport::connection_rtt(PeerIndex peer) const {
    const auto stats = link_stats(peer);
    return stats ? std::optional<std::chrono::microseconds>(stats->last_rtt) : std::nullopt;
}
std::optional<LinkStats> GnsTransport::link_stats(PeerIndex peer) const {
#if EAWR_HAS_GNS
    if (mesh_) {
        if (peer >= mesh_->peers || !mesh_->links[peer]) return {};
        const auto& link = *mesh_->links[peer];
        if (link.state != LinkState::open || link.closing) return {};
        return link.stats.cached();
    }
#endif
    if (peer != impl_->remote() || impl_->state != LinkState::open || impl_->closing) return {};
    return impl_->stats.cached();
}
bool GnsTransport::mesh() const { return mesh_ != nullptr; }
GnsPairDiagnostic GnsTransport::pair(PeerIndex remote) const {
#if EAWR_HAS_GNS
    if (mesh_) return mesh_->pair(remote);
#endif
    static_cast<void>(remote);
    return {};
}
GnsSignalCounts GnsTransport::signal_counts(PeerIndex remote) const {
#if EAWR_HAS_GNS
    if (mesh_) return mesh_->counts(remote);
#endif
    static_cast<void>(remote);
    return {};
}
std::optional<GnsMeshEvent> GnsTransport::take_event() {
#if EAWR_HAS_GNS
    if (mesh_ && !mesh_->events.empty()) {
        auto event = mesh_->events.front(); mesh_->events.pop_front();
        return event;
    }
#endif
    return {};
}
std::uint32_t GnsTransport::connected_seats() const {
#if EAWR_HAS_GNS
    if (mesh_) return mesh_->connected_seats();
#endif
    return 0;
}
bool GnsTransport::mesh_complete() const {
#if EAWR_HAS_GNS
    if (mesh_) return !mesh_->failure && mesh_->complete();
#endif
    return false;
}
bool GnsTransport::room_locked() const {
#if EAWR_HAS_GNS
    return mesh_ && mesh_->locked;
#else
    return false;
#endif
}
core::Result<void> GnsTransport::lock_room() {
#if EAWR_HAS_GNS
    if (!mesh_ || !mesh_->host() || mesh_->failure)
        return core::Result<void>::failure(error(1761, "Only the room host can lock a room"));
    if (!mesh_->complete()) return core::Result<void>::failure(error(1764, "The mesh is incomplete at Start"));
    auto locked = mesh_->room->lock();
    if (locked) mesh_->locked = true;
    return locked;
#else
    return core::Result<void>::failure(error(1500, "This build has GNS disabled"));
#endif
}
std::uint64_t GnsTransport::edge_identity(PeerIndex peer) const {
#if EAWR_HAS_GNS
    if (mesh_ && peer < mesh_->peers && mesh_->link(peer) == LinkState::open) return mesh_->slot[peer].generation;
#endif
    static_cast<void>(peer); return 0;
}
core::Result<void> GnsTransport::commit_room() {
#if EAWR_HAS_GNS
    if (mesh_ && mesh_->failure) return core::Result<void>::failure(*mesh_->failure);
    if (!mesh_ || mesh_->closing)
        return core::Result<void>::failure(error(1790, "Room commitment has no active mesh"));
    mesh_->locked = true;
    return core::Result<void>::success();
#else
    return core::Result<void>::failure(error(1500, "This build has GNS disabled"));
#endif
}
core::Result<void> GnsTransport::repair_closed_links(TcpRejoinAuth auth) {
#if EAWR_HAS_GNS
    if (!mesh_ || !mesh_->locked || mesh_->closing || mesh_->failure || auth.epoch == 0
        || !valid_reconnect_tokens(auth.seat_tokens, mesh_->self, mesh_->peers)
        || std::all_of(auth.match_identity.begin(), auth.match_identity.end(), [](auto byte) { return byte == 0; })
        || (mesh_->repair_auth && auth.epoch < mesh_->repair_auth->epoch))
        return core::Result<void>::failure(error(1790, "Invalid room repair credentials or epoch"));
    if (mesh_->repair_auth && auth.epoch == mesh_->repair_auth->epoch) {
        if (auth.match_identity != mesh_->repair_auth->match_identity || auth.seat_tokens != mesh_->repair_auth->seat_tokens)
            return core::Result<void>::failure(error(1790, "Repair credentials changed within an epoch"));
        return core::Result<void>::success();
    }
    for (PeerIndex peer = 0; peer < mesh_->peers; ++peer) {
        const auto* link = mesh_->links[peer].get();
        if (link && link->proof && !link->authenticated()) mesh_->drop_link(peer);
    }
    mesh_->note(-1, "repair epoch " + std::to_string(auth.epoch) + " armed");
    mesh_->repair_auth = std::move(auth); mesh_->attachment_ready = mesh_->attachment_pending = false;
    mesh_->repair_deadline.reset(); mesh_->refresh_failed = false; // Preserve early signals; GNS ignores stale native handles.
    for (auto& slot : mesh_->slot) {
        slot.repair_attempted = slot.gave_up = false; slot.repair_dials = 0; slot.repair_retry_after.reset();
    }
    return core::Result<void>::success();
#else
    static_cast<void>(auth); return core::Result<void>::failure(error(1500, "This build has GNS disabled"));
#endif
}
core::Result<void> GnsTransport::interrupt_peer(PeerIndex peer) {
#if EAWR_HAS_GNS
    if (mesh_ && mesh_->locked && peer < mesh_->peers && peer != mesh_->self && !mesh_->slot[peer].excluded) {
        mesh_->drop_link(peer); mesh_->slot[peer].reported = false;
        mesh_->slot[peer].diag = Mesh::diagnostic(GnsPairPhase::lost, 0, false, false);
        return core::Result<void>::success();
    }
#endif
    static_cast<void>(peer); return core::Result<void>::failure(error(1790, "Invalid repair peer"));
}
void GnsTransport::fail_repair(PeerIndex peer, unsigned code) {
#if EAWR_HAS_GNS
    if (mesh_ && peer < mesh_->peers && peer != mesh_->self) {
        mesh_->drop_link(peer); auto& slot = mesh_->slot[peer];
        slot.repair_attempted = slot.reported = slot.gave_up = true;
        slot.diag = Mesh::diagnostic(GnsPairPhase::lost, code, mesh_->turn.has_value(), true);
        mesh_->emit(GnsMeshEventKind::pair_failed, peer, 0, code, mesh_->turn.has_value());
    }
#else
    static_cast<void>(peer); static_cast<void>(code);
#endif
}
core::Result<void> GnsTransport::restart_links(TcpRejoinAuth auth) {
    auto armed = repair_closed_links(std::move(auth));
    if (!armed) return armed;
#if EAWR_HAS_GNS
    for (PeerIndex peer = 0; peer < mesh_->peers; ++peer) {
        if (peer == mesh_->self || mesh_->slot[peer].excluded) continue;
        mesh_->drop_link(peer); mesh_->slot[peer].reported = false;
        mesh_->slot[peer].diag = Mesh::diagnostic(GnsPairPhase::lost, 0, false, false);
    }
#endif
    return core::Result<void>::success();
}
core::Result<void> GnsTransport::close_peer(PeerIndex peer) {
#if EAWR_HAS_GNS
    if (mesh_ && peer < mesh_->peers && peer != mesh_->self) {
        if (!mesh_->locked) {
            if (mesh_->links[peer]) mesh_->links[peer]->close_link();
            return core::Result<void>::success();
        }
        mesh_->slot[peer].excluded = true; mesh_->slot[peer].diag = {};
        mesh_->drop_link(peer); return core::Result<void>::success();
    }
#endif
    static_cast<void>(peer); return core::Result<void>::failure(error(1790, "Invalid room peer"));
}
void GnsTransport::erase_reconnect_credentials() noexcept {
#if EAWR_HAS_GNS
    if (mesh_) {
        mesh_->repair_auth.reset();
        for (auto& link : mesh_->links) if (link && link->proof) {
            if (!link->authenticated()) link->cancel_link();
            link->proof.reset();
        }
    }
#endif
}
void GnsTransport::close() {
#if EAWR_HAS_GNS
    if (mesh_) { mesh_->close(); return; }
    impl_->close_link();
#endif
}
bool GnsTransport::closed() const {
#if EAWR_HAS_GNS
    if (mesh_) return mesh_->closed();
    return impl_->stream == k_HSteamNetConnection_Invalid && impl_->listener == k_HSteamListenSocket_Invalid;
#else
    return true;
#endif
}
void GnsTransport::cancel() {
#if EAWR_HAS_GNS
    if (mesh_) { mesh_->cancel(); return; }
    impl_->cancel_link();
#else
    impl_->state = LinkState::closed;
#endif
}
} // namespace eawr::net
