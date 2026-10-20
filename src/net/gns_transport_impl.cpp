#include "gns_transport_impl.hpp"

#include <algorithm>
#include <utility>

#if EAWR_HAS_GNS
#include <steam/isteamnetworkingutils.h>
#endif

namespace eawr::net {
using namespace gns_detail;

#if EAWR_HAS_GNS
GnsTransport::Impl::Runtime::Runtime() {
    SteamNetworkingErrMsg reason{};
    gns_log(); // built first, so it outlives the GNS threads that post to it
    if (GameNetworkingSockets_Init(nullptr, reason)) {
        sockets = SteamNetworkingSockets();
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, gns_spew);
    }
    else failure = error(1506, "GNS initialization failed");
}

GnsTransport::Impl::Runtime::~Runtime() { if (sockets) GameNetworkingSockets_Kill(); }

GnsTransport::Impl::Runtime& GnsTransport::Impl::Runtime::get() { static Runtime runtime; return runtime; }

void GnsTransport::Impl::Runtime::changed(SteamNetConnectionStatusChangedCallback_t* event) {
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

GnsTransport::Impl::ReceiveSignal::ReceiveSignal(Impl& impl) : owner(impl) {}

ISteamNetworkingConnectionSignaling* GnsTransport::Impl::ReceiveSignal::OnConnectRequest(HSteamNetConnection connection,
    const SteamNetworkingIdentity& peer, int virtual_port) {
    const auto* identity = peer.GetGenericString();
    if (!owner.hosting || virtual_port != 0 || !identity
        || identity != owner.signalling->rendezvous().remote_identity
        || owner.stream != k_HSteamNetConnection_Invalid || owner.cancelled) return nullptr;
    if (!owner.adopt(connection)) return nullptr;
    return new Signal(owner.signalling_state);
}

void GnsTransport::Impl::ReceiveSignal::SendRejectionSignal(const SteamNetworkingIdentity&, const void*, int) {}

#endif
GnsTransport::Impl::Impl(bool host, GnsOptions config) : options(std::move(config)), hosting(host), decoder(options, host) {}

PeerIndex GnsTransport::Impl::remote() const { return peer_slot; }

bool GnsTransport::Impl::authenticated() const { return !proof || proof->proven(); }

#if EAWR_HAS_GNS
ISteamNetworkingSockets& GnsTransport::Impl::sockets() const { return *Runtime::get().sockets; }

GnsTransport::Impl::~Impl() {
    if (stream != k_HSteamNetConnection_Invalid) {
        Runtime::get().connections.erase(stream);
        sockets().CloseConnection(stream, 0, nullptr, false);
    }
    release_listener();
}

void GnsTransport::Impl::release_listener() {
    if (listener == k_HSteamListenSocket_Invalid) return;
    Runtime::get().listeners.erase(listener);
    sockets().CloseListenSocket(listener); listener = k_HSteamListenSocket_Invalid;
}

std::vector<SteamNetworkingConfigValue_t> GnsTransport::Impl::config() const {
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

bool GnsTransport::Impl::adopt(HSteamNetConnection connection) {
    if (cancelled || accepted || stream != k_HSteamNetConnection_Invalid) {
        sockets().CloseConnection(connection, 1508, "EAWR-NET-1508", false); return false;
    }
    stream = connection;
    Runtime::get().connections.emplace(stream, this);
    failure.reset(); started = Clock::now();
    if (sockets().AcceptConnection(stream) != k_EResultOK) { stop(1506, "GNS accept failed"); return false; }
    return true;
}

void GnsTransport::Impl::stop(unsigned reason, std::string message) {
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

void GnsTransport::Impl::forget_route() {
    route = GnsRoute::unknown;
}

void GnsTransport::Impl::control(Bytes bytes) { outbound_bytes += bytes.size(); outbound.push_back({std::move(bytes), false, 0}); }

Bytes GnsTransport::Impl::pair_message() const {
    Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_pair)};
    bytes.insert(bytes.end(), secret.begin(), secret.end()); u32(bytes, local_slot); u32(bytes, peer_slot); return bytes;
}

void GnsTransport::Impl::queue_proof(Bytes bytes) {
    outbound_bytes += bytes.size() + 9;
    outbound.push_back({std::move(bytes), true, 0});
}

void GnsTransport::Impl::hello() {
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

void GnsTransport::Impl::message(std::span<const std::uint8_t> bytes, bool reliable) {
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

bool GnsTransport::Impl::receive() {
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

void GnsTransport::Impl::flush() {
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

void GnsTransport::Impl::pump_signals() {
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

void GnsTransport::Impl::settle_service_closure() {
    if (service_closed_deadline && Clock::now() >= *service_closed_deadline
        && !signalling_state->admitted.load() && !refusing) {
        cancelled = true; stop(1517, "GNS introduction service failed");
    }
}

void GnsTransport::Impl::pump() {
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
        // m_flConnectionQualityLocal is the share of packets that arrived intact, or negative while unmeasured.
        // Built with emplace rather than a conditional expression: GCC -Wmaybe-uninitialized flags the
        // optional's storage when it is copied into observe() from a ternary (#4386).
        std::optional<double> loss;
        if (valid_ping && status.m_flConnectionQualityLocal >= 0.0F)
            loss.emplace(std::clamp(1.0 - static_cast<double>(status.m_flConnectionQualityLocal), 0.0, 1.0) * 100.0);
        stats.observe(now, valid_ping ? std::optional<int>(status.m_nPing) : std::nullopt, loss);
        SteamNetConnectionInfo_t info{};
        const bool info_ok = sockets().GetConnectionInfo(stream, &info);
        const bool connected = info_ok && info.m_eState == k_ESteamNetworkingConnectionState_Connected;
        route = classify_gns_route(connected, (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0,
            !info.m_addrRemote.IsIPv6AllZeros(), info.m_idPOPRelay != 0 || info.m_idPOPRemote != 0);
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

core::Result<void> GnsTransport::Impl::send_frame(PeerIndex to, std::span<const std::uint8_t> frame) {
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

void GnsTransport::Impl::close_link() {
    if (closing) return;
    if (state != LinkState::open) { cancel_link(); return; }
    control({static_cast<std::uint8_t>(TcpMessage::close)});
    closing = true; state = LinkState::closed; started = Clock::now();
    stats.reset();
    forget_route();
}

void GnsTransport::Impl::cancel_link() { cancelled = true; stop(); }

bool GnsTransport::Impl::dead() const { return stream == k_HSteamNetConnection_Invalid && state == LinkState::closed; }

#endif
} // namespace eawr::net
