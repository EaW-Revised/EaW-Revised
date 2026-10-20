#include "gns_transport_mesh.hpp"
#include "gns_deferred_signals.hpp"

#include <algorithm>
#include <random>
#include <utility>

#if EAWR_HAS_GNS
#include <steam/isteamnetworkingutils.h>
#endif

#if EAWR_HAS_GNS
namespace eawr::net {
using namespace gns_detail;

GnsTransport::Mesh::Receive::Receive(Mesh& owner) : mesh(owner) {}

ISteamNetworkingConnectionSignaling* GnsTransport::Mesh::Receive::OnConnectRequest(HSteamNetConnection connection,
    const SteamNetworkingIdentity& peer, int virtual_port) {
    return mesh.accept_request(connection, peer, virtual_port, from, *identity, deferred);
}

void GnsTransport::Mesh::Receive::SendRejectionSignal(const SteamNetworkingIdentity&, const void*, int) {}

std::array<std::uint8_t, gns_pair_secret_size> GnsTransport::Mesh::random_secret() {
    std::random_device random;
    std::array<std::uint8_t, gns_pair_secret_size> value{};
    for (auto& byte : value) byte = static_cast<std::uint8_t>(random());
    return value;
}

void GnsTransport::Mesh::send_grant(PeerIndex to, PeerIndex remote, bool dial_flag, const std::array<std::uint8_t, gns_pair_secret_size>& secret) {
    Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_grant)};
    put(bytes, remote); put(bytes, slot[remote].epoch); bytes.push_back(dial_flag ? 1U : 0U);
    bytes.insert(bytes.end(), secret.begin(), secret.end());
    queue_control(to, std::move(bytes));
}

void GnsTransport::Mesh::send_vacated(PeerIndex to, PeerIndex seat, std::uint8_t reason, PeerIndex other, unsigned code, bool relay) {
    Bytes bytes{static_cast<std::uint8_t>(TcpMessage::mesh_vacated)};
    put(bytes, seat); put(bytes, slot[seat].epoch); bytes.push_back(reason); put(bytes, other);
    put(bytes, code); bytes.push_back(relay ? 1U : 0U);
    queue_control(to, std::move(bytes));
}

bool GnsTransport::Mesh::later(PeerIndex a, PeerIndex b) const {
    return slot[a].admitted > slot[b].admitted || (slot[a].admitted == slot[b].admitted && a > b);
}

void GnsTransport::Mesh::grant_pair(PeerIndex a, PeerIndex b) {
    const auto secret = random_secret();
    grants[a][b] = grants[b][a] = secret;
    const auto dialer = later(a, b) ? a : b, other = dialer == a ? b : a;
    introduced[a] = static_cast<MeshSeats>(introduced[a] | bit(b)); introduced[b] = static_cast<MeshSeats>(introduced[b] | bit(a));
    failed[a] &= static_cast<MeshSeats>(~bit(b)); failed[b] &= static_cast<MeshSeats>(~bit(a));
    failed_since[a][b].reset(); failed_since[b][a].reset();
    reported_by[a][b] = {}; reported_by[b][a] = {};
    claimed[a] &= static_cast<MeshSeats>(~bit(b)); claimed[b] &= static_cast<MeshSeats>(~bit(a));
    send_grant(dialer, other, true, secret);
    send_grant(other, dialer, false, secret);
}

void GnsTransport::Mesh::on_attached(PeerIndex seat, std::uint64_t at) {
    auto& s = slot[seat];
    s.admitted = at; s.gone = false; s.refused = false;
    s.reported = false; s.was_open = false; s.diag = {}; s.expect_until.reset();
    s.signals.reset(); s.retired.clear(); s.folded = {}; // a different occupant is a different pair
    if (host()) {
        ++s.epoch;
        formation->attach(seat, at);
    }
}

void GnsTransport::Mesh::vacated(PeerIndex seat, std::uint8_t reason) {
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

void GnsTransport::Mesh::sync_roster() {
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

ISteamNetworkingConnectionSignaling* GnsTransport::Mesh::accept_request(HSteamNetConnection connection,
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
    if (!link) return nullptr;
    if (!host() && s.have_grant && s.role == Slot::Role::accept) { link->secret = s.secret; link->granted = true; }
    auto* raw = link.get();
    links[from] = std::move(link);
    s.expect_until.reset();
    {
        // A lobby regrant also uses the latest service-issued credentials,
        // overriding the shared listener without touching healthy streams.
        if (!raw->turn) {
            // Incoming streams inherit their listener's original config.
            // An explicit no-TURN refresh must clear that old credential.
            for (const auto option : {k_ESteamNetworkingConfig_P2P_TURN_ServerList,
                k_ESteamNetworkingConfig_P2P_TURN_UserList, k_ESteamNetworkingConfig_P2P_TURN_PassList}) {
                if (!SteamNetworkingUtils()->SetConnectionConfigValueString(connection, option, "")) {
                    if (locked) {
                        s.diag = diagnostic(GnsPairPhase::lost, 1791, false, true);
                        s.reported = true; s.gave_up = true; emit(GnsMeshEventKind::pair_failed, from, 0, 1791);
                        drop_link(from);
                    } else { drop_link(from); fail_pair(from, 1762, "Could not clear relay credentials"); }
                    return nullptr;
                }
            }
        }
        const auto settings = raw->config();
        // The first four settings (callback, timeouts, queue bound) are
        // unchanged and already inherited from the listener.
        for (std::size_t i = 4; i < settings.size(); ++i) {
            if (!SteamNetworkingUtils()->SetConfigValueStruct(settings[i], k_ESteamNetworkingConfig_Connection, connection)) {
                if (locked) {
                    s.diag = diagnostic(GnsPairPhase::lost, 1791, raw->turn.has_value(), true);
                    s.reported = true; s.gave_up = true; emit(GnsMeshEventKind::pair_failed, from, 0, 1791, raw->turn.has_value());
                    drop_link(from);
                } else { drop_link(from); fail_pair(from, 1762, "Could not install relay credentials"); }
                return nullptr;
            }
        }
    }
    if (!raw->adopt(connection)) { drop_link(from); return nullptr; }
    return new Signal(raw->signalling_state);
}

void GnsTransport::Mesh::pump_room() {
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
        std::span<const std::uint8_t> payload = signal.bytes;
        const bool formation_pair = !locked && self != 0 && signal.from_slot != 0;
        if (formation_pair || (locked && has_room_signal_envelope(payload))) {
            const auto envelope = decode_room_signal(payload);
            if (!envelope) continue;
            const auto& source = slot[signal.from_slot];
            // Native ICE retransmits an early signal once its host grant arrives.
            if (formation_pair && (!source.have_grant || !room_signal_matches_grant(*envelope, source.secret))) {
                note(static_cast<int>(signal.from_slot), "ignored superseded room signal");
                continue;
            }
            if (locked) {
                const auto& live = links[signal.from_slot];
                if (!live || !live->signalling_state->formation_grant
                    || !room_signal_matches_grant(*envelope, *live->signalling_state->formation_grant)) {
                    note(static_cast<int>(signal.from_slot), "ignored superseded room signal");
                    continue;
                }
            }
            payload = envelope->payload;
        }
        context.from = signal.from_slot; context.identity = &signal.from_identity;
        context.deferred = false;
        // Late signals for a closed connection are normal in a mesh; GNS ignores what it cannot place.
        static_cast<void>(sockets().ReceivedP2PCustomSignal(payload.data(), static_cast<int>(payload.size()), &context));
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

void GnsTransport::Mesh::member_control(Bytes bytes) {
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

void GnsTransport::Mesh::evaluate_pair(PeerIndex a, PeerIndex b) {
    const auto result = formation->pair_failed(a, b, {(failed[a] & bit(b)) != 0, (failed[b] & bit(a)) != 0});
    if (result.action == MeshFormationAction::regrant) {
        note(static_cast<int>(b), "lobby regrants pair " + std::to_string(a) + "/" + std::to_string(b));
        grant_pair(a, b);
        emit(GnsMeshEventKind::pair_failed, result.seat, result.unreachable, 0, turn.has_value());
    } else if (result.action == MeshFormationAction::refuse) {
        const auto guest = result.seat, unreachable = result.unreachable;
        note(static_cast<int>(unreachable), "lobby exhausted pair " + std::to_string(guest) + "/" + std::to_string(unreachable));
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

void GnsTransport::Mesh::host_control(PeerIndex from, Bytes bytes) {
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
        if (!(introduced[from] & bit(remote))
            || !std::equal(grants[from][remote].begin(), grants[from][remote].end(), body.begin() + 10)) return;
        reported_by[from][remote] = {word(6), body[5] != 0}; // validated by the decoder
        claimed[from] &= static_cast<MeshSeats>(~bit(remote));
        add(failed[from], remote);
        if (!failed_since[from][remote]) failed_since[from][remote] = Clock::now();
        evaluate_pair(from, remote);
    } else fatal(gns_error_mesh_control, "Unexpected mesh control from a member");
}

void GnsTransport::Mesh::member_tick() {
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

void GnsTransport::Mesh::host_tick() {
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

} // namespace eawr::net
#endif
