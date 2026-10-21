#pragma once

#include "gns_transport_impl.hpp"
#include "eawr/net/mesh_policy.hpp"

namespace eawr::net {
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
    std::optional<std::pair<std::uint64_t, std::uint32_t>> roster_lock;
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
    // Every report echoes its grant secret so delayed failures cannot spend the next attempt.
    using PairSecret = GnsFormationSecret;
    std::array<std::array<PairSecret, max_match_seats>, max_match_seats> grants{};
    // Member side
    MeshSeats sent_mask{};
    bool mask_sent{};

    ISteamNetworkingSockets& sockets() const;
    bool host() const;
    static constexpr MeshSeats bit(PeerIndex seat) { return mesh_seat(seat); }
    static GnsPairDiagnostic diagnostic(GnsPairPhase phase, unsigned code, bool relay_offered, bool relay_failed,
        GnsRoute route = GnsRoute::unknown);
    static void add(MeshSeats& seats, PeerIndex seat);

    void emit(GnsMeshEventKind kind, PeerIndex seat, PeerIndex other = 0, unsigned code = 0, bool relay = false);
    void fatal(unsigned code, std::string message, PeerIndex other = 0, bool relay = false);

    std::unique_ptr<Impl> make_link(PeerIndex remote, bool acceptor);
    // A regrant replaces the pair's link: its signal counts stay with the slot. A few replaced states stay live because
    // GNS threads may still send through them; older ones are folded into plain totals.
    void retire_signals(PeerIndex remote);
    // Dial the remote slot with the grant (or, for the host pair, the roster identity) already known.
    void dial(PeerIndex remote);
    // Repair trace for godot.log (#4021): which link a seat re-dialled, replaced or gave up, with this seat and the peer.
    void note(int peer, const std::string& text) const;
    void drop_link(PeerIndex remote);
    bool parked(PeerIndex remote) const;
    std::size_t parked_count() const;

    // ---- control messages ----
    static void put(Bytes& bytes, std::uint32_t value);
    bool queue_control(PeerIndex to, Bytes bytes);
    static std::array<std::uint8_t, gns_pair_secret_size> random_secret();
    void send_grant(PeerIndex to, PeerIndex remote, bool dial_flag, const std::array<std::uint8_t, gns_pair_secret_size>& secret);
    // reason 1 (could not connect to `other`) carries the failing pair's own reason code and relay flag.
    void send_vacated(PeerIndex to, PeerIndex seat, std::uint8_t reason, PeerIndex other, unsigned code = 0, bool relay = false);
    // Later admission dials, as in the formation policy (admission revision, then seat).
    bool later(PeerIndex a, PeerIndex b) const;
    void grant_pair(PeerIndex a, PeerIndex b);

    // ---- pair failure bookkeeping ----
    // A pair that never opened ("failed") or one that closed after opening ("lost"). Reports the cause to the host.
    void fail_pair(PeerIndex remote, unsigned code, std::string message, bool was_open_link = false,
        const PairSecret* attempt = nullptr);

    // ---- roster ----
    void on_attached(PeerIndex seat, std::uint64_t at);
    void vacated(PeerIndex seat, std::uint8_t reason);
    void sync_roster();

    // ---- signalling ----
    struct Receive final : ISteamNetworkingSignalingRecvContext {
        Mesh& mesh; PeerIndex from{}; const std::string* identity{}; bool deferred{};
        explicit Receive(Mesh& owner);
        ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection connection,
            const SteamNetworkingIdentity& peer, int virtual_port) override;
        void SendRejectionSignal(const SteamNetworkingIdentity&, const void*, int) override;
    };
    ISteamNetworkingConnectionSignaling* accept_request(HSteamNetConnection connection,
        const SteamNetworkingIdentity& peer, int virtual_port, PeerIndex from, const std::string& identity, bool& deferred);
    void pump_room();

    // ---- control consumption ----
    void member_control(Bytes bytes);
    void evaluate_pair(PeerIndex a, PeerIndex b);
    void host_control(PeerIndex from, Bytes bytes);

    // ---- per-poll state machine ----
    // Openings first, then the controls a pair delivered, then deaths: a closing host pair can carry its last control
    // (the vacancy notice) in the same pump as its close, and that notice must be read before the close is acted on.
    void harvest(bool deaths);
    // A repair dial that died before it opened is made again, after a growing pause, until the dial budget or the repair
    // deadline is spent. The acceptor may still hold the previous epoch's material when the first request arrives (its
    // incident arms later than ours), and that attempt is lost however the two arms are ordered (#4021).
    bool redial_repair(PeerIndex remote, const Impl& link, unsigned code);
    void member_tick();
    void host_tick();
    void poll();
    core::Result<bool> refresh_attachment();
    void repair_tick();
    void release_listener();
    void close();
    void cancel();
    bool closed() const;
    ~Mesh();

    // ---- observation ----
    LinkState link(PeerIndex remote) const;
    GnsPairDiagnostic pair(PeerIndex remote) const;
    // Per pair, across every link the slot's current occupant has had (a regrant adds to the totals, never resets them).
    GnsSignalCounts counts(PeerIndex remote) const;
    MeshSeats connected_seats() const;
    bool complete() const;
};
#else
struct GnsTransport::Mesh {};
#endif
} // namespace eawr::net
