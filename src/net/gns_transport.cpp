#include "eawr/net/gns_transport.hpp"
#include "gns_frame_codec.hpp"
#include "gns_link_stats.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <limits>
#include <map>
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
core::Diagnostic error(unsigned number, std::string message) {
    core::Diagnostic result;
    result.code = "EAWR-NET-" + std::to_string(number); result.message = std::move(message);
    return result;
}
std::atomic<int>& observed_route_cell() {
    static std::atomic<int> cell{static_cast<int>(GnsRoute::unknown)};
    return cell;
}
#if EAWR_HAS_GNS
static_assert(gns_identity_max_bytes < SteamNetworkingIdentity::k_cchMaxGenericString);
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
bool valid(const GnsOptions& options) {
    constexpr auto max_ms = std::numeric_limits<std::int32_t>::max();
    return options.peers == 2 && options.max_frame_size <= 16 * 1024 * 1024
        && options.queue_bytes >= static_cast<std::size_t>(options.max_frame_size) + 9
        && options.queue_bytes <= 128 * 1024 * 1024 && options.queue_frames > 0
        && !options.build_identity.empty() && options.build_identity.size() <= 1024
        && options.keepalive.count() > 0 && options.timeout > options.keepalive
        && options.timeout.count() <= max_ms && options.handshake_timeout.count() > 0
        && options.handshake_timeout.count() <= max_ms;
}
struct SignallingState {
    std::shared_ptr<GnsSignallingEndpoint> endpoint;
    std::atomic<bool> admitted{}, unavailable{}, refusing{};
};
class Signal final : public ISteamNetworkingConnectionSignaling {
public:
    explicit Signal(std::shared_ptr<SignallingState> state) : state_(std::move(state)) {}
    bool SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* data, int size) override {
        if (size <= 0 || size > 64 * 1024) return false;
        if (state_->unavailable.load()) return state_->admitted.load() || state_->refusing.load();
        const auto sent = state_->endpoint->send({static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size)});
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
    PeerIndex remote() const { return hosting ? 1U : 0U; }
#if EAWR_HAS_GNS
    struct Runtime {
        ISteamNetworkingSockets* sockets{};
        std::optional<core::Diagnostic> failure;
        std::map<HSteamListenSocket, Impl*> listeners;
        std::map<HSteamNetConnection, Impl*> connections;
        Runtime() {
            SteamNetworkingErrMsg reason{};
            if (GameNetworkingSockets_Init(nullptr, reason)) sockets = SteamNetworkingSockets();
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
                    if (reason >= 1500 && reason <= 1529) owner.end_error = static_cast<unsigned>(reason);
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
        std::vector<SteamNetworkingConfigValue_t> values(turn ? 8 : 4);
        values[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
            reinterpret_cast<void*>(Runtime::changed));
        values[1].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, static_cast<std::int32_t>(options.handshake_timeout.count()));
        values[2].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, static_cast<std::int32_t>(options.timeout.count()));
        values[3].SetInt32(k_ESteamNetworkingConfig_SendBufferSize,
            static_cast<std::int32_t>(std::max<std::size_t>(options.queue_bytes, 4096)));
        if (turn) {
            values[4].SetString(k_ESteamNetworkingConfig_P2P_TURN_ServerList, turn->uri.c_str());
            values[5].SetString(k_ESteamNetworkingConfig_P2P_TURN_UserList, turn_users.c_str());
            values[6].SetString(k_ESteamNetworkingConfig_P2P_TURN_PassList, turn_passwords.c_str());
            values[7].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, options.turn_only
                ? k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay : k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All);
        }
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
        observed_route_cell().store(static_cast<int>(GnsRoute::unknown));
        if (reason != 0 && !failure) failure = error(reason, std::move(message));
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
    void control(Bytes bytes) { outbound_bytes += bytes.size(); outbound.push_back({std::move(bytes), false, 0}); }
    void hello() {
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::hello), 'E', 'A', 'W', 'R'};
        u32(bytes, options.protocol_version); u32(bytes, static_cast<std::uint32_t>(options.build_identity.size()));
        bytes.insert(bytes.end(), options.build_identity.begin(), options.build_identity.end());
        u32(bytes, options.max_frame_size); bytes.push_back(hosting ? 0U : 1U);
        control(std::move(bytes)); hello_sent = true;
        started = last_receive = last_write = Clock::now();
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
        auto decoded = std::move(result).value();
        if (decoded.kind == TcpMessage::hello) {
            hello_seen = true;
            if (hosting) {
                Bytes reply{static_cast<std::uint8_t>(TcpMessage::accept)}; u32(reply, 1); control(std::move(reply));
                // The verified hello admits this peer before its acknowledgement
                // arrives. Signalling can disappear anywhere in that interval.
                if (signalling_state) signalling_state->admitted.store(true);
            }
        } else if (decoded.kind == TcpMessage::accept) {
            accepted = true; state = LinkState::open;
            if (signalling_state) signalling_state->admitted.store(true);
            if (!hosting) { Bytes reply{static_cast<std::uint8_t>(TcpMessage::accept)}; u32(reply, 1); control(std::move(reply)); }
            // GNS closes accepted connections with their listener, so keep it
            // until teardown and reject additional arrivals instead.
        } else if (decoded.kind == TcpMessage::data && decoded.frame) {
            inbound_bytes += decoded.frame->size(); inbound.push_back({remote(), std::move(*decoded.frame)});
        } else if (decoded.kind == TcpMessage::close) {
            stats.reset();
            peer_close = true;
            if (!closing) { control({static_cast<std::uint8_t>(TcpMessage::close)}); closing = true; started = Clock::now(); }
            state = LinkState::closed;
        }
        last_receive = Clock::now();
    }
    bool receive() {
        for (unsigned calls = 0; calls < 64 && stream != k_HSteamNetConnection_Invalid; ++calls) {
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
        for (unsigned calls = 0; calls < 64 && !outbound.empty() && stream != k_HSteamNetConnection_Invalid; ++calls) {
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
        if (ready && !hello_sent && !refusing) hello();
        // Drain queued messages before acting on a closed callback, which would
        // otherwise discard the ordered tail at CloseConnection.
        // Custom-signalling negotiation can make SDK messages readable before
        // its queued Connected callback reaches this owner. Wait for readiness
        // so our hello is queued before any reply to the peer's hello.
        const auto drained = ready ? receive() : true;
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
            const auto reason = end_error ? end_error : (peer_close ? 0U : 1506U);
            stop(reason, "GNS peer disconnected"); return;
        }
        settle_service_closure();
        if (stream == k_HSteamNetConnection_Invalid) return;
        const auto now = Clock::now();
        if (!accepted && now - started >= options.handshake_timeout) { stop(1507, "GNS handshake timed out"); return; }
        if (accepted && !closing && now - last_receive >= options.timeout) { stop(1507, "GNS application peer timed out"); return; }
        if (state == LinkState::open && !closing && stats.due(now)) {
            SteamNetConnectionRealTimeStatus_t status{};
            const auto valid_ping = sockets().GetConnectionRealTimeStatus(stream, &status, 0, nullptr) == k_EResultOK
                && status.m_eState == k_ESteamNetworkingConnectionState_Connected;
            stats.observe(now, valid_ping ? std::optional<int>(status.m_nPing) : std::nullopt);
            SteamNetConnectionInfo_t info{};
            const bool info_ok = sockets().GetConnectionInfo(stream, &info);
            const bool connected = info_ok && info.m_eState == k_ESteamNetworkingConnectionState_Connected;
            observed_route_cell().store(static_cast<int>(classify_gns_route(connected,
                (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0,
                !info.m_addrRemote.IsIPv6AllZeros(), info.m_idPOPRelay != 0 || info.m_idPOPRemote != 0)));
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
#endif
};

GnsRoute GnsTransport::observed_route() { return static_cast<GnsRoute>(observed_route_cell().load()); }
bool GnsTransport::available() { return EAWR_HAS_GNS != 0; }
GnsTransport::GnsTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
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
        if (impl->turn && (impl->turn->uri.empty() || impl->turn->username.empty() || impl->turn->password.empty()
                || !turn_uri_list(impl->turn->uri) || impl->turn->username.find(',') != std::string::npos
                || impl->turn->password.find(',') != std::string::npos || impl->turn->lifetime.count() <= 0))
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
core::Result<void> GnsTransport::send(PeerIndex to, std::span<const std::uint8_t> frame) {
#if EAWR_HAS_GNS
    if (to != impl_->remote() || impl_->state != LinkState::open || impl_->closing)
        return core::Result<void>::failure(error(1506, "GNS link is not open for this peer"));
    if (frame.size() > impl_->options.max_frame_size)
        return core::Result<void>::failure(error(1504, "GNS application frame exceeds limit"));
    if (impl_->outbound.size() >= impl_->options.queue_frames
        || frame.size() + 9 > impl_->options.queue_bytes - std::min(impl_->outbound_bytes, impl_->options.queue_bytes))
        return core::Result<void>::failure(error(1505, "GNS send queue is full; pump and retry"));
    impl_->outbound.push_back({Bytes(frame.begin(), frame.end()), true, 0}); impl_->outbound_bytes += frame.size() + 9;
    return core::Result<void>::success();
#else
    static_cast<void>(to); static_cast<void>(frame);
    return core::Result<void>::failure(error(1500, "This build has GNS disabled"));
#endif
}
std::optional<Received> GnsTransport::poll() {
#if EAWR_HAS_GNS
    impl_->pump();
#endif
    if (impl_->inbound.empty()) return {};
    auto frame = std::move(impl_->inbound.front()); impl_->inbound.pop_front(); impl_->inbound_bytes -= frame.frame.size();
    return frame;
}
LinkState GnsTransport::link(PeerIndex peer) const { return peer == impl_->remote() ? impl_->state : LinkState::closed; }
PeerIndex GnsTransport::self() const { return impl_->hosting ? 0U : 1U; }
std::uint16_t GnsTransport::port() const { return impl_->bound_port; }
const std::optional<core::Diagnostic>& GnsTransport::diagnostic() const { return impl_->failure; }
std::optional<std::chrono::microseconds> GnsTransport::connection_rtt(PeerIndex peer) const {
    const auto stats = link_stats(peer);
    return stats ? std::optional<std::chrono::microseconds>(stats->last_rtt) : std::nullopt;
}
std::optional<LinkStats> GnsTransport::link_stats(PeerIndex peer) const {
    if (peer != impl_->remote() || impl_->state != LinkState::open || impl_->closing) return {};
    return impl_->stats.cached();
}
void GnsTransport::close() {
#if EAWR_HAS_GNS
    if (impl_->closing) return;
    if (impl_->state != LinkState::open) { cancel(); return; }
    impl_->control({static_cast<std::uint8_t>(TcpMessage::close)});
    impl_->closing = true; impl_->state = LinkState::closed; impl_->started = Clock::now();
    impl_->stats.reset();
    observed_route_cell().store(static_cast<int>(GnsRoute::unknown));
#endif
}
bool GnsTransport::closed() const {
#if EAWR_HAS_GNS
    return impl_->stream == k_HSteamNetConnection_Invalid && impl_->listener == k_HSteamListenSocket_Invalid;
#else
    return true;
#endif
}
void GnsTransport::cancel() {
    impl_->cancelled = true;
#if EAWR_HAS_GNS
    impl_->stop();
#else
    impl_->state = LinkState::closed;
#endif
}
} // namespace eawr::net
