#include "gns_transport_mesh.hpp"

#include <algorithm>
#include <utility>

#if EAWR_HAS_GNS
#include "platform_socket.hpp"
#include <steam/isteamnetworkingutils.h>
#endif

namespace eawr::net {
using namespace gns_detail;

bool GnsTransport::available() { return EAWR_HAS_GNS != 0; }
std::uint64_t GnsTransport::set_log_sink(GnsLogSink sink) {
    auto& log = gns_log();
    const std::lock_guard lock(log.mutex);
    log.enabled.store(static_cast<bool>(sink));
    log.sink = std::move(sink);
    ++log.generation;
    if (!log.enabled.load()) { log.pending.clear(); log.limited.clear(); log.dropped = 0; log.has_pending.store(false); }
    return log.enabled.load() ? log.generation : 0;
}
void GnsTransport::clear_log_sink(std::uint64_t generation) {
    auto& log = gns_log();
    const std::lock_guard lock(log.mutex);
    if (generation == 0 || generation != log.generation || !log.enabled.load()) return;
    log.enabled.store(false);
    log.sink = {};
    ++log.generation;
    log.pending.clear(); log.limited.clear(); log.dropped = 0; log.has_pending.store(false);
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
        if (impl->turn && !turn_valid(*impl->turn)) {
            log_turn_refused(static_cast<int>(impl->local_slot), impl->turn);
            return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1519, "Invalid TURN rendezvous credentials"));
        }
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
    if (impl->options.turn_only && !impl->turn) {
        log_turn_refused(static_cast<int>(impl->local_slot), impl->turn);
        return core::Result<std::unique_ptr<GnsTransport>>::failure(error(1519, "TURN-only connection requires relay credentials"));
    }
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
    if (mesh->turn && !turn_valid(*mesh->turn)) {
        log_turn_refused(static_cast<int>(me), mesh->turn);
        return Failure::failure(error(1519, "Invalid TURN rendezvous credentials"));
    }
    if (mesh->options.turn_only && !mesh->turn) {
        log_turn_refused(static_cast<int>(me), mesh->turn);
        return Failure::failure(error(1519, "TURN-only connection requires relay credentials"));
    }
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
bool GnsTransport::mesh_complete(std::uint32_t active) const {
#if EAWR_HAS_GNS
    if (!mesh_ || mesh_->failure || (active & 1U) == 0 || active == 1U
        || active >= (1U << mesh_->peers)) return false;
    const auto roster = mesh_->room->roster();
    std::uint32_t attached{};
    for (PeerIndex p = 0; p < roster.capacity; ++p) {
        if (roster.slots[p].state == GnsRoomSlotState::reserved) return false;
        if (roster.slots[p].state == GnsRoomSlotState::attached) {
            if (roster.slots[p].identity != mesh_->slot[p].identity) return false;
            attached |= 1U << p;
        }
    }
    return attached == active && (!mesh_->host() || mesh_->formation->occupied() == active)
        && (mesh_->connected_seats() & active) == active;
#else
    static_cast<void>(active); return false;
#endif
}
core::Result<bool> GnsTransport::lock_roster(std::uint32_t active) {
#if EAWR_HAS_GNS
    const auto fail = [](std::string message) { return core::Result<bool>::failure(error(1764, std::move(message))); };
    if (!mesh_ || !mesh_->host() || mesh_->failure) return fail("Only the room host can lock the roster.");
    if (!mesh_->roster_lock) {
        if (!mesh_complete(active)) return fail("Start raced a player joining or leaving. Wait for the player list and connections, then accept again.");
        // Capture and check the SAME roster whose revision goes to the service.
        // Its worker can publish a same-index replacement between earlier reads.
        const auto roster = mesh_->room->roster();
        std::uint32_t attached{};
        for (PeerIndex p = 0; p < roster.capacity; ++p) {
            if (roster.slots[p].state == GnsRoomSlotState::reserved)
                return fail("Start raced a player joining. Wait for the player list and accept again.");
            if (roster.slots[p].state == GnsRoomSlotState::attached) {
                if (roster.slots[p].identity != mesh_->slot[p].identity)
                    return fail("Start raced a player replacement. Wait for the player list and accept again.");
                attached |= 1U << p;
            }
        }
        if (attached != active) return fail("Start raced a player joining or leaving. Wait for the player list and accept again.");
        mesh_->roster_lock = std::pair{roster.revision, active};
    }
    if (mesh_->roster_lock->second != active) return fail("The agreed player list changed during Start.");
    auto result = mesh_->room->lock_roster(active, mesh_->roster_lock->first);
    if (!result) mesh_->roster_lock.reset();
    else if (result.value()) mesh_->locked = true;
    return result;
#else
    static_cast<void>(active); return core::Result<bool>::failure(error(1500, "This build has GNS disabled"));
#endif
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
