#pragma once

#include "eawr/net/gns_room.hpp"
#include "eawr/net/gns_signalling.hpp"
#include "eawr/net/tcp_transport.hpp"

#include <cstddef>
#include <functional>
#include <string_view>

namespace eawr::net {
// Path of the established GNS connection: kind of ICE route in use. `unknown` until
// the first sample, and whenever the SDK text names neither a relay nor a direct kind.
enum class GnsRoute { unknown, direct, relayed };
// Receives one already formatted diagnostic line ("EAWR net event=gns ..."): UTC time, this seat, the peer when the line
// belongs to one pair, and the text. Addresses are replaced by "<addr>"; no credentials or payloads.
using GnsLogSink = std::function<void(std::string_view)>;
// GNS hello layout revision (the TCP/LAN protocol stays at 1). 2 added the sender's roster slot and the roster size to the
// hello (N-peer slice N2); 3 binds mesh signalling and failure reports to their grant secret.
// The decoder reads magic and version before the rest, so a peer on another revision gets the
// typed EAWR-NET-1502 and never a malformed-hello error.
inline constexpr std::uint32_t gns_protocol_version = 3;
struct GnsOptions : TcpOptions {
    GnsOptions() { protocol_version = gns_protocol_version; }
    // Qualification/deployment policy: require TURN instead of direct ICE.
    bool turn_only{};
    // Room mesh: the longest one pair may take from its first signal to a verified pair (ICE and any TURN allocation included).
    std::chrono::milliseconds pair_timeout{20000};
    // Full in-battle incident repair budget, including a late-noticing endpoint,
    // fresh credentials and its pair handshake. Match the agreed mesh grace.
    std::chrono::milliseconds repair_grace{30000};
    // Comma-separated numeric "stun:host:port" URIs for server-reflexive candidates (P2P_STUN_ServerList). GNS makes none
    // without them, because it reads only the relayed address from a TURN allocation. coturn answers STUN on its TURN
    // port, so the introduction service can hand out the same host (docs/gns-transport.md, "STUN").
    std::string stun_servers;
    // A Connected link with reliable bytes in flight and neither validated reliable inbound messages nor a shrinking
    // total reliable send window for this long ends with EAWR-NET-1507. This is link liveness, not proof of individual
    // ACKs: new sends can replace acknowledged bytes between samples. Native GNS ACKs still drain a busy/loading
    // peer's window at lockstep volumes; a dead relay path does not. Keep below the lockstep completeness timeout.
    std::chrono::milliseconds ack_silence_timeout{15000};
};

// Room mesh observation. Causes and whether a relay candidate was offered; never addresses.
enum class GnsPairPhase : std::uint8_t { absent, signalling, handshaking, open, failed, lost, closed,
    refreshing, repairing, authenticating };
struct GnsPairDiagnostic {
    GnsPairPhase phase{GnsPairPhase::absent};
    unsigned code{}; // EAWR-NET-<code>: 1762 unreachable, 1766 roster mismatch, 1767 pair secret, 1768 timeout, 1769 lost, 1771 no grant
    bool relay_offered{};
    // The pair failed or was lost and nothing is left to try: GNS runs direct and relay candidates together, so a closed
    // link has exhausted any relay that was offered, and with none offered the direct failure is the whole evidence.
    // This is what N4 puts in MeshLinkReport::relay_failed (docs/gns-transport.md, "Without TURN").
    bool relay_failed{};
    // Route of this pair's own connection while it is open (sampled by poll; unknown otherwise).
    GnsRoute route{GnsRoute::unknown};
};
// Introduction signals carried for one pair (ICE exchange cost), counted at the adapter.
struct GnsSignalCounts {
    std::uint64_t sent_blobs{}, sent_bytes{}, received_blobs{}, received_bytes{};
};
// I/O-owner snapshot. Native counters cover the reliable lane, including controls;
// they do not provide an acknowledgement for each application frame.
struct GnsQueueProgress {
    std::size_t outbound_frames{}, outbound_bytes{}, inbound_frames{}, inbound_bytes{}, deferred_signals{};
    bool native_valid{}, reliable_drained{};
    int native_state{}, ping_ms{}, pending_reliable{}, unacked_reliable{}, pending_unreliable{};
};
enum class GnsMeshEventKind : std::uint8_t {
    pair_open,     // seat = remote
    pair_failed,   // seat = remote that never opened; at the host: the pair (seat, other) or a newcomer that never reached it
    pair_lost,     // an open pair closed
    seat_refused,  // host: newcomer `seat` was refused because it could not reach `other`; its slot was released
    seat_vacated,  // a seat left the room before Start
    left_room      // this transport can no longer take part (code says why); seat = this seat, other = the unreachable seat
};
struct GnsMeshEvent {
    GnsMeshEventKind kind{};
    PeerIndex seat{}, other{};
    unsigned code{};
    bool relay_offered{};
};

// GNS adapter with TcpTransport's message/lifecycle contract: two peers directly or through a paired introduction, or
// an N-peer room mesh (host_room/join_room). All
// transports in a process share one standalone GNS runtime and must be owned
// by the same I/O thread. poll drives callbacks, control traffic and bounded
// message I/O. GNS itself owns its UDP/background threads. Destruction aborts;
// an orderly close requires pumping both sides until closed().
class GnsTransport final : public MessageTransport {
public:
    static bool available();
    // Process-wide diagnostics (#4021): GNS warnings and the ICE/TURN lines (allocation and permission replies with their
    // error codes, nonce repeats, refresh timeouts), plus the reason every link stops (EAWR-NET-1507 included). Quiet in
    // a healthy match: a few lines per TURN refresh, never one per packet. Lines are queued from any thread and `poll`
    // hands them to the sink on the I/O owner's thread, so the sink needs no locking. An empty sink turns it off.
    // The sink is process-wide. Installing one returns its generation (0 for an empty sink); clear_log_sink(generation) removes
    // it only while it is still the installed one, so a match that was replaced or finished cannot silence its successor.
    static std::uint64_t set_log_sink(GnsLogSink sink);
    static void clear_log_sink(std::uint64_t generation);
    static core::Result<std::unique_ptr<GnsTransport>> host(std::string address = "0.0.0.0",
        std::uint16_t port = TcpOptions::default_port, GnsOptions options = {});
    static core::Result<std::unique_ptr<GnsTransport>> connect(std::string address,
        std::uint16_t port = TcpOptions::default_port, GnsOptions options = {});
    // Custom signalling uses the rendezvous's generic identities. There is one
    // GNS identity per process; changing it while another adapter is alive is
    // rejected. A rendezvous must be paired before either factory is called.
    static core::Result<std::unique_ptr<GnsTransport>> host_introduced(
        std::shared_ptr<GnsSignallingEndpoint> endpoint, GnsOptions options = {});
    static core::Result<std::unique_ptr<GnsTransport>> connect_introduced(
        std::shared_ptr<GnsSignallingEndpoint> endpoint, GnsOptions options = {});
    // N-peer room mesh (docs/net-online-npeer.md, section 3). options.peers is the agreed roster size and must equal the
    // room's capacity; the endpoint's slot is this transport's PeerIndex (the host is slot 0). One pair link per remote
    // slot; the host introduces pairs and the mesh completes incrementally as members attach. Same ownership and
    // threading rules as the two-peer factories.
    static core::Result<std::unique_ptr<GnsTransport>> host_room(
        std::shared_ptr<GnsRoomEndpoint> room, GnsOptions options = {});
    static core::Result<std::unique_ptr<GnsTransport>> join_room(
        std::shared_ptr<GnsRoomEndpoint> room, GnsOptions options = {});
    ~GnsTransport() override;
    core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> frame) override;
    std::optional<Received> poll() override;
    LinkState link(PeerIndex peer) const override;
    PeerIndex self() const override;
    std::uint32_t peer_count() const override;
    std::uint16_t port() const;
    [[nodiscard]] GnsQueueProgress queue_progress(PeerIndex peer) const;
    const std::optional<core::Diagnostic>& diagnostic() const;
    // Cached native ping observations, sampled by poll at a one-second cadence.
    // Unknown until the first valid estimate; query on the owning I/O thread.
    std::optional<LinkStats> link_stats(PeerIndex peer) const override;
    std::uint64_t edge_identity(PeerIndex peer) const override;
    void fail_repair(PeerIndex peer, unsigned code) override;
    std::optional<std::chrono::microseconds> connection_rtt(PeerIndex peer) const;
    // Room mesh only (empty/false for the two-peer factories).
    bool mesh() const;
    GnsPairDiagnostic pair(PeerIndex remote) const;
    GnsSignalCounts signal_counts(PeerIndex remote) const;
    std::optional<GnsMeshEvent> take_event();
    // Seats (bit n = seat n) known to be fully connected: the host's formation view, a member's own open pairs.
    std::uint32_t connected_seats() const;
    // Host: every seat of the agreed roster is present and every pair is verified. Member: all of its own pairs are open.
    bool mesh_complete() const;
    // Cached native lock view; reads no socket and is false when GNS is disabled.
    bool room_locked() const;
    // Host only: Lock the room (Start). Refused with EAWR-NET-1764 while the mesh is incomplete.
    core::Result<void> lock_room();
    // I/O owner: apply the validated lobby commitment before battle handoff.
    // The host's GNS commitment may beat the service's TLS Lock roster push.
    core::Result<void> commit_room();
    // Arm authenticated in-battle repair, as TCP repair_closed_links. Call on
    // the I/O owner with the agreed incident/held-seat epoch and pair keys.
    // poll then repairs closed pairs once per epoch; a failed attempt is final
    // evidence for the existing mesh decision, never a transport-wide failure.
    core::Result<void> repair_closed_links(TcpRejoinAuth auth);
    // Retire one seat after a confirmed departure. Later ordinary repair epochs
    // keep it retired; this does not end the room or its other pairs.
    core::Result<void> close_peer(PeerIndex peer);
    core::Result<void> restart_links(TcpRejoinAuth auth);
    // Abort one native stream without retiring the seat (I/O recovery owner and
    // fault-injection tests). The armed repair epoch controls any replacement.
    core::Result<void> interrupt_peer(PeerIndex peer);
    void erase_reconnect_credentials() noexcept;
    void close();
    bool closed() const;
    void cancel();
private:
    struct Impl;
    struct Mesh;
    explicit GnsTransport(std::unique_ptr<Impl> impl);
    explicit GnsTransport(std::unique_ptr<Mesh> mesh);
    static core::Result<std::unique_ptr<GnsTransport>> create(bool hosting, std::string address,
        std::uint16_t port, GnsOptions options, std::shared_ptr<GnsSignallingEndpoint> endpoint);
    static core::Result<std::unique_ptr<GnsTransport>> create_room(bool hosting,
        std::shared_ptr<GnsRoomEndpoint> room, GnsOptions options);
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<Mesh> mesh_;
};
} // namespace eawr::net
