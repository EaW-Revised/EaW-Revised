#include "gns_transport_mesh.hpp"

#include <algorithm>
#include <utility>

#if EAWR_HAS_GNS
namespace eawr::net::gns_detail {
// Repair dials per pair and epoch, and the pause before the second (it doubles each time): 0.5 + 1 + 2 + 4 s.
constexpr unsigned gns_repair_dial_limit = 5;
constexpr std::chrono::milliseconds gns_repair_redial_pause{500};
} // namespace eawr::net::gns_detail
#endif

#if EAWR_HAS_GNS
namespace eawr::net {
using namespace gns_detail;

ISteamNetworkingSockets& GnsTransport::Mesh::sockets() const { return *Impl::Runtime::get().sockets; }

bool GnsTransport::Mesh::host() const { return self == 0; }

GnsPairDiagnostic GnsTransport::Mesh::diagnostic(GnsPairPhase phase, unsigned code, bool relay_offered, bool relay_failed,
    GnsRoute route) {
    GnsPairDiagnostic value;
    value.phase = phase; value.code = code; value.relay_offered = relay_offered; value.relay_failed = relay_failed; value.route = route;
    return value;
}

void GnsTransport::Mesh::add(MeshSeats& seats, PeerIndex seat) { seats = static_cast<MeshSeats>(seats | mesh_seat(seat)); }

void GnsTransport::Mesh::emit(GnsMeshEventKind kind, PeerIndex seat, PeerIndex other, unsigned code, bool relay) {
    if (events.size() >= 64) events.pop_front();
    events.push_back({kind, seat, other, code, relay});
}

void GnsTransport::Mesh::fatal(unsigned code, std::string message, PeerIndex other, bool relay) {
    if (failure) return;
    failure = error(code, std::move(message));
    for (PeerIndex p = 0; p < peers; ++p) if (links[p]) { links[p]->cancel_link(); }
    emit(GnsMeshEventKind::left_room, self, other, code, relay);
}

std::unique_ptr<GnsTransport::Impl> GnsTransport::Mesh::make_link(PeerIndex remote, bool acceptor) {
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
    link->turn = locked ? turn : room->turn_credentials();
    if (!locked && ((link->turn && !turn_valid(*link->turn)) || (options.turn_only && !link->turn))) {
        log_turn_refused(static_cast<int>(self), link->turn);
        fail_pair(remote, 1762, "No valid relay credentials for a fresh ICE attempt");
        return {};
    }
    if (link->turn) {
        const auto count = static_cast<std::size_t>(std::count(link->turn->uri.begin(), link->turn->uri.end(), ',')) + 1;
        for (std::size_t i = 0; i < count; ++i) {
            link->turn_users += (i ? "," : "") + link->turn->username;
            link->turn_passwords += (i ? "," : "") + link->turn->password;
        }
    }
    link->signalling_state = std::make_shared<SignallingState>();
    link->signalling_state->room = room; link->signalling_state->destination = remote;
    if (!locked && self != 0 && remote != 0) link->signalling_state->formation_grant = slot[remote].secret;
    retire_signals(remote);
    slot[remote].signals = link->signalling_state;
    return link;
}

void GnsTransport::Mesh::retire_signals(PeerIndex remote) {
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

void GnsTransport::Mesh::dial(PeerIndex remote) {
    auto link = make_link(remote, false);
    if (!link) return;
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

void GnsTransport::Mesh::note(int peer, const std::string& text) const { gns_log_push(static_cast<int>(self), peer, text); }

void GnsTransport::Mesh::drop_link(PeerIndex remote) {
    if (!links[remote]) return;
    links[remote]->cancel_link(); links[remote].reset();
}

bool GnsTransport::Mesh::parked(PeerIndex remote) const { return links[remote] && !links[remote]->granted; }

std::size_t GnsTransport::Mesh::parked_count() const {
    std::size_t count = 0;
    for (PeerIndex p = 0; p < peers; ++p) if (parked(p)) ++count;
    return count;
}

void GnsTransport::Mesh::put(Bytes& bytes, std::uint32_t value) { u32(bytes, value); }

bool GnsTransport::Mesh::queue_control(PeerIndex to, Bytes bytes) {
    auto* link = links[to].get();
    if (!link || link->state != LinkState::open || link->closing) return false;
    if (link->outbound.size() >= options.queue_frames || bytes.size() + 4 > options.queue_bytes
        || link->outbound_bytes > options.queue_bytes - std::min(options.queue_bytes, bytes.size() + 4)) {
        // One slow pair ends alone and follows the normal pair-failure path; the room goes on.
        link->stop(gns_error_mesh_control, "Mesh control queue capacity exceeded"); return false;
    }
    link->control(std::move(bytes)); return true;
}

void GnsTransport::Mesh::fail_pair(PeerIndex remote, unsigned code, std::string message, bool was_open_link,
    const PairSecret* attempt) {
    auto& s = slot[remote];
    if (!locked && attempt && *attempt != s.secret) return; // a retired native attempt cannot fail its replacement
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
    const auto& failed_grant = attempt ? *attempt : s.secret;
    bytes.insert(bytes.end(), failed_grant.begin(), failed_grant.end());
    queue_control(0, std::move(bytes));
}

void GnsTransport::Mesh::harvest(bool deaths) {
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
            } else if (!redial_repair(remote, link, code)) fail_pair(remote, code, "Pair closed", s.was_open, &link.secret);
            if (links[remote]) { links[remote].reset(); }
        }
    }
}

bool GnsTransport::Mesh::redial_repair(PeerIndex remote, const Impl& link, unsigned code) {
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

void GnsTransport::Mesh::poll() {
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

core::Result<bool> GnsTransport::Mesh::refresh_attachment() {
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

void GnsTransport::Mesh::repair_tick() {
    if (!repair_auth) return;
    bool needed = false;
    for (PeerIndex p = 0; p < peers; ++p)
        if (p != self && slot[p].was_open && !slot[p].excluded && link(p) != LinkState::open) needed = true;
    // The grace belongs to one stretch of repair: once every pair is open again the next loss starts a fresh one, rather
    // than meeting the expired deadline of a long previous incident and being given up on the instant it happens (1793
    // on the first pair lost after a 39 s incident, N9-H, before the next epoch is armed).
    if (!needed) { repair_deadline.reset(); return; }
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

void GnsTransport::Mesh::release_listener() {
    if (listener == k_HSteamListenSocket_Invalid) return;
    sockets().CloseListenSocket(listener); listener = k_HSteamListenSocket_Invalid;
}

void GnsTransport::Mesh::close() {
    if (closing) return;
    closing = true;
    for (PeerIndex p = 0; p < peers; ++p) if (links[p]) links[p]->close_link();
}

void GnsTransport::Mesh::cancel() {
    closing = true;
    for (PeerIndex p = 0; p < peers; ++p) if (links[p]) links[p]->cancel_link();
    release_listener();
}

bool GnsTransport::Mesh::closed() const {
    if (listener != k_HSteamListenSocket_Invalid) return false;
    return std::none_of(links.begin(), links.end(), [](const auto& l) { return l && l->stream != k_HSteamNetConnection_Invalid; });
}

GnsTransport::Mesh::~Mesh() {
    for (auto& link : links) link.reset();
    release_listener();
}

LinkState GnsTransport::Mesh::link(PeerIndex remote) const {
    if (remote >= peers || remote == self) return LinkState::closed;
    if (slot[remote].excluded) return LinkState::closed;
    if (!links[remote]) return slot[remote].gone || slot[remote].diag.phase == GnsPairPhase::failed
        || slot[remote].diag.phase == GnsPairPhase::lost ? LinkState::closed : LinkState::connecting;
    const auto& state = links[remote]->state;
    return links[remote]->closing ? LinkState::closed : state;
}

GnsPairDiagnostic GnsTransport::Mesh::pair(PeerIndex remote) const {
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

GnsSignalCounts GnsTransport::Mesh::counts(PeerIndex remote) const {
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

MeshSeats GnsTransport::Mesh::connected_seats() const {
    if (host()) {
        MeshSeats result = 0;
        for (PeerIndex p = 0; p < peers; ++p) if (formation->connected(p)) add(result, p);
        return result;
    }
    MeshSeats result = bit(self);
    for (PeerIndex p = 0; p < peers; ++p) if (links[p] && links[p]->state == LinkState::open && !links[p]->closing) add(result, p);
    return result;
}

bool GnsTransport::Mesh::complete() const {
    if (host()) return formation->complete();
    std::uint32_t occupied = 1;
    for (PeerIndex p = 0; p < peers; ++p) if (p != self && slot[p].state == GnsRoomSlotState::attached) ++occupied;
    return occupied == peers && (connected_seats() == static_cast<MeshSeats>((1U << peers) - 1U));
}

} // namespace eawr::net
#endif
