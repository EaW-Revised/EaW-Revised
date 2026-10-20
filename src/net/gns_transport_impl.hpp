#pragma once

#include "gns_transport_signalling.hpp"
#include "gns_frame_codec.hpp"
#include "gns_link_stats.hpp"
#include "gns_link_watchdog.hpp"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if EAWR_HAS_GNS
#include <steam/steamnetworkingsockets.h>
#endif

namespace eawr::net {
using gns_detail::Bytes;
using gns_detail::Clock;
#if EAWR_HAS_GNS
using gns_detail::SignallingState;
#endif

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
    Impl(bool host, GnsOptions config);
    GnsFrameDecoder decoder;
    // Roster positions of this connection's two ends. The two-peer defaults are listener 0 and dialer 1.
    PeerIndex local_slot{hosting ? 0U : 1U}, peer_slot{hosting ? 1U : 0U};
    PeerIndex remote() const;
    // Room pair link. A non-host pair also proves its host-issued secret (mesh_pair) before it opens; a link whose
    // grant has not arrived yet stays parked: no hello, no reads, bounded by the pair deadline.
    bool pair_link{}, needs_secret{}, granted{true}, pair_ok{true}, pair_sent{};
    std::array<std::uint8_t, gns_pair_secret_size> secret{};
    std::deque<Bytes> controls;
    std::unique_ptr<RejoinLinkProof> proof;
    bool proof_started{}, published{};
    bool authenticated() const;
    unsigned recv_budget{64}, send_budget{64};
    // Route of this connection, sampled by pump.
    GnsRoute route{GnsRoute::unknown};
#if EAWR_HAS_GNS
    struct Runtime {
        ISteamNetworkingSockets* sockets{};
        std::optional<core::Diagnostic> failure;
        std::map<HSteamListenSocket, Impl*> listeners;
        std::map<HSteamNetConnection, Impl*> connections;
        Runtime();
        ~Runtime();
        static Runtime& get();
        static void changed(SteamNetConnectionStatusChangedCallback_t* event);
    };
    struct ReceiveSignal final : ISteamNetworkingSignalingRecvContext {
        Impl& owner;
        explicit ReceiveSignal(Impl& impl);
        ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection connection,
            const SteamNetworkingIdentity& peer, int virtual_port) override;
        void SendRejectionSignal(const SteamNetworkingIdentity&, const void*, int) override;
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
    GnsLinkWatchdog link_watchdog;
    Clock::time_point next_ack_sample{started};
    ISteamNetworkingSockets& sockets() const;
    ~Impl();
    void release_listener();
    std::vector<SteamNetworkingConfigValue_t> config() const;
    bool adopt(HSteamNetConnection connection);
    void stop(unsigned reason = 0, std::string message = {});
    void forget_route();
    void control(Bytes bytes);
    // Pair identity proof: the shared secret plus both roster positions, as in the TCP mesh's mesh_pair.
    Bytes pair_message() const;
    void queue_proof(Bytes bytes);
    void hello();
    void message(std::span<const std::uint8_t> bytes, bool reliable);
    bool receive();
    void flush();
    void pump_signals();
    void settle_service_closure();
    void pump();
    core::Result<void> send_frame(PeerIndex to, std::span<const std::uint8_t> frame);
    void close_link();
    void cancel_link();
    bool dead() const;
#endif
};

} // namespace eawr::net
