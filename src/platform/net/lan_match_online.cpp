#include "lan_match_internal.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

void TcpLanMatch::set_public_invitation(bool value)  { if (online_options_) online_options_->service.public_alias = value; }

void TcpLanMatch::configure_online(OnlineMatchOptions options) {
        online_options_ = std::move(options);
        recovery_policy_ = {}; // Repair/lifetime qualification belongs to the online repair slice.
    }

std::string TcpLanMatch::seat_name(net::PeerIndex position) const {
        if (last_lobby_state_ && position < last_lobby_state_->snapshot.seats.size() && !last_lobby_state_->snapshot.seats[position].selection.name.empty())
            return last_lobby_state_->snapshot.seats[position].selection.name;
        return "Player " + std::to_string(position + 1);
    }

core::Result<void> TcpLanMatch::arm_mesh_repair(const net::LockstepSession& session) {
        if (!online_options_ || !session.mesh_running()) return core::Result<void>::success();
        if (!mesh_repair_auth_ || session.mesh_incident_id() == 0)
            return core::Result<void>::failure(diagnostic("EAWR-NET-1790", "Match-bound mesh repair material is unavailable."));
        auto auth = *mesh_repair_auth_; auth.epoch = session.mesh_incident_id();
        return io_->transport.tcp->repair_closed_links(std::move(auth));
    }

core::Result<void> TcpLanMatch::retire_mesh_seat(net::PeerIndex dense) {
        if (!online_options_) return core::Result<void>::success();
        if (!last_lobby_state_ || dense >= last_lobby_state_->snapshot.seats.size())
            return core::Result<void>::failure(diagnostic("EAWR-NET-1790", "Frozen room seat is unavailable for retirement."));
        const auto native = last_lobby_state_->snapshot.seats[dense].peer;
        if (native == io_->transport.self() || (mesh_retired_ & net::mesh_seat(native))) return core::Result<void>::success();
        auto closed = io_->transport.tcp->close_peer(native);
        if (closed) mesh_retired_ |= net::mesh_seat(native);
        return closed;
    }

std::string TcpLanMatch::mesh_repair_message() const {
        if (last_lobby_state_) for (net::PeerIndex dense = 0; dense < last_lobby_state_->snapshot.seats.size(); ++dense) {
            const auto native = last_lobby_state_->snapshot.seats[dense].peer;
            if (native == io_->transport.self() || (mesh_retired_ & net::mesh_seat(native))) continue;
            const auto pair = io_->transport.tcp->pair(native);
            if (pair.phase != net::GnsPairPhase::open)
                return "Connection to " + seat_name(dense) + ": " + online_pair_message(pair) + ". Play is paused.";
        }
        return "Your connections are ready. Waiting for all players to confirm. Play is paused.";
    }

std::string TcpLanMatch::removed_seat_text(const net::LockstepSession& session) const {
        const auto self = lobby_stream_ ? lobby_stream_->self() : io_->transport.self();
        for (auto result = session.mesh_results().rbegin(); result != session.mesh_results().rend(); ++result) {
            if (result->kind != net::MeshIncidentResult::Kind::leave || self >= net::max_match_seats) continue;
            auto pick = static_cast<net::MeshSeats>(result->lost[self] & result->survivors);
            if (pick == 0) pick = result->lost[self];
            for (net::PeerIndex other = 0; other < net::max_match_seats && pick != 0; ++other)
                if (pick & net::mesh_seat(other)) return "Connection to " + seat_name(other) + " could not be restored. You have left the match.";
        }
        return "Your connection to the other players could not be restored. You have left the match.";
    }

void TcpLanMatch::append_mesh_notice(const net::MeshIncidentResult& result, std::uint64_t now_ms) {
        if (result.kind != net::MeshIncidentResult::Kind::departure) return;
        const auto name = [&](net::PeerIndex position) { return seat_name(position); };
        for (net::PeerIndex removed = 0; removed < net::max_match_seats; ++removed) {
            if (!(result.removed & net::mesh_seat(removed))) continue;
            // The first survivor the removed seat could not reach; failing that, any seat it could not reach.
            const auto lost = result.lost[removed];
            net::MeshSeats pick = static_cast<net::MeshSeats>(lost & result.survivors);
            if (pick == 0) pick = lost;
            std::string line = name(removed) + " left";
            for (net::PeerIndex other = 0; other < net::max_match_seats && pick != 0; ++other)
                if (pick & net::mesh_seat(other)) { line += ": their connection to " + name(other) + " failed."; pick = 0; }
            if (line.back() != '.') line += ".";
            mesh_notices_.push(std::move(line), now_ms);
        }
    }

void TcpLanMatch::retire_room(std::shared_ptr<net::NetworkGnsRoom> room) {
        if (room) drain_workers().start(std::function<void()>([room = std::move(room)]() mutable { room.reset(); }));
    }

std::uint64_t TcpLanMatch::room_edge(net::PeerIndex peer) const {
        if (!io_->transport.tcp->mesh()) return io_->transport.tcp->edge_identity(peer);
        const auto room = online_endpoint_.lock();
        if (!room || peer >= net::max_match_seats || io_->transport.link(peer) != net::LinkState::open) return 0;
        // The roster identity is the service's own name for the occupant: a different occupant is a different edge.
        return std::hash<std::string>{}(room->roster().slots[peer].identity) | 1U;
    }

void TcpLanMatch::sample_online_route() {
        if (!online_options_ || !io_->transport.tcp || !io_->transport.tcp->mesh()) return;
        const auto sample_now = std::chrono::steady_clock::now();
        if (sample_now < online_sample_at_) return;
        online_sample_at_ = sample_now + std::chrono::milliseconds(250);
        auto route = net::GnsRoute::unknown;
        std::vector<std::string> connections;
        std::uint32_t connected = 1;
        const auto endpoint = online_endpoint_.lock();
        const auto roster = endpoint ? endpoint->roster() : net::GnsRoomRoster{};
        const auto seats = lobby_ ? lobby_->snapshot().seats
            : last_lobby_state_ ? last_lobby_state_->snapshot.seats : std::vector<net::LobbySeat>{};
        for (net::PeerIndex peer = 0; peer < io_->transport.peer_count(); ++peer) {
            const auto seat = std::find_if(seats.begin(), seats.end(), [peer](const auto& value) { return value.peer == peer; });
            std::string name = seat != seats.end() ? seat->selection.name
                : peer == io_->transport.self() ? lobby_name_ : "Player " + std::to_string(peer + 1);
            std::string line = "Seat " + std::to_string(peer + 1) + " - " + name;
            if (peer == io_->transport.self()) line += " (you): Connected";
            else if (peer < roster.capacity && roster.slots[peer].state == net::GnsRoomSlotState::vacant)
                line = "Seat " + std::to_string(peer + 1) + ": Waiting for a player";
            else if (peer < roster.capacity && roster.slots[peer].state == net::GnsRoomSlotState::reserved)
                line = "Seat " + std::to_string(peer + 1) + ": Player joining";
            else line += ": " + online_pair_message(io_->transport.tcp->pair(peer));
            connections.push_back(std::move(line));
            if (peer != io_->transport.self() && io_->transport.link(peer) == net::LinkState::open) {
                ++connected;
                const auto seen = io_->transport.tcp->pair(peer).route;
                if (seen == net::GnsRoute::relayed || (seen == net::GnsRoute::direct && route == net::GnsRoute::unknown)) route = seen;
            }
        }
        const std::lock_guard lock(mutex_);
        if (route != net::GnsRoute::unknown) view_.online_route = route;
        view_.online_capacity = io_->transport.peer_count(); view_.online_connected = connected;
        view_.online_connections = std::move(connections);
    }

std::string TcpLanMatch::admission_identity(net::PeerIndex peer, std::uint64_t edge) const {
        if (io_->transport.tcp->mesh())
            if (const auto room = online_endpoint_.lock(); room && peer < net::max_match_seats)
                return "room-" + room->roster().slots[peer].identity;
        return "lan-edge-" + std::to_string(edge);
    }

bool TcpLanMatch::drain_mesh_events() {
        if (!io_->transport.tcp || !io_->transport.tcp->mesh()) return true;
        if (view().host && !lobby_) return true; // the host lobby handles seat refusals: keep them until it exists
        while (const auto event = io_->transport.tcp->take_mesh_event()) {
            if (event->kind == net::GnsMeshEventKind::seat_refused && lobby_ && view().host) {
                (void)lobby_feedback(lobby_->refuse_unreachable(event->seat, event->other, event->relay_offered));
            } else if (event->kind == net::GnsMeshEventKind::left_room) {
                core::Diagnostic error; error.code = "EAWR-NET-" + std::to_string(event->code);
                if (event->code == 1762) {
                    // The host's closed notice cannot reach a seat whose transport is already fatal: name the player here, from
                    // what the host stream still holds (the refusal can overtake the admission snapshot) or the roster seen so far.
                    std::vector<net::Received> queued;
                    while (queued.size() < 16)
                        if (auto raw = io_->transport.tcp->poll()) queued.push_back(std::move(*raw)); else break;
                    const auto seats = lobby_ ? lobby_->snapshot().seats : std::vector<net::LobbySeat>{};
                    error.message = online_refusal_text(queued, seats, event->other, event->relay_offered);
                }
                else if (event->code == 1502 || event->code == 1503) error.message = "Game build identities differ"; // the build refusal text
                else if (event->code == 1760) error.message = "The room was closed.";
                else error.message = "You could not take part in this room.";
                set_failure(error); return false;
            }
        }
        return true;
    }

void TcpLanMatch::begin_online(bool hosting, std::string code, std::optional<net::OnlineRoomAdmission> admission) {
        cancel();
        // The screen configures its logger before host/join. cancel() retires the old sink but keeps that logger; the new
        // online attempt must install it again before creating or polling GNS, including after a failed admission retry.
        install_gns_log();
        if (online_retiring_ && !online_retiring_->finished.load()) {
            set_failure(diagnostic("EAWR-NET-1534", "The previous online request is still closing.")); return;
        }
        if (online_retiring_ && !online_retiring_->close_confirmed.load()) {
            const std::lock_guard lock(online_retiring_->mutex);
            if (std::chrono::steady_clock::now() < online_retiring_->expires) {
                set_failure(diagnostic("EAWR-NET-1534", "Wait for the previous invitation to expire before publishing again.")); return;
            }
        }
        online_retiring_.reset();
        if (!hosting) { owners_ = {1, 2}; roster_error_.reset(); }
        if (roster_error_) { set_failure(*roster_error_); return; }
        if (owners_.size() < 2 || owners_.size() > net::max_match_seats) {
            set_failure(diagnostic("EAWR-NET-1810", "Choose between two and nine human seats.")); return;
        }
        if (!net::GnsTransport::available() || !net::NetworkGnsIntroduction::available()) {
            set_failure(diagnostic("EAWR-NET-1530", "Online play is disabled in this build.")); return;
        }
        { const std::lock_guard lock(mutex_);
          view_ = {}; view_.host = hosting; view_.read_only = !hosting; view_.phase = LanPhase::connecting;
          view_.human_owners = owners_; view_.player = hosting ? owners_.front() : owners_.back();
          view_.message = hosting ? "Creating invitation..." : "Finding game...";
        }
        online_alias_.clear(); mesh_seen_ = 0; mesh_notices_.clear(); mesh_left_text_.clear(); online_sample_at_ = {};
        online_admission_ = std::make_shared<OnlineAdmission>();
        drain_workers().start(std::function<void()>([state = online_admission_, options = *online_options_, hosting, code = std::move(code), admission = std::move(admission)]() mutable {
            state->run(std::move(options), hosting, std::move(code), std::move(admission));
            state->finished = true;
        }));
    }

bool TcpLanMatch::pump_online() {
        std::optional<core::Diagnostic> error;
        if (!online_pending_) {
            std::shared_ptr<net::NetworkGnsRoom> room;
            std::string alias;
            { const std::lock_guard lock(online_admission_->mutex);
              error = online_admission_->failure; room = std::move(online_admission_->room); alias = online_admission_->code;
              if (!room) { const std::lock_guard view_lock(mutex_); view_.invitation_code = alias; view_.invitation_expires = online_admission_->expires; }
            }
            if (error) { online_admission_->cancelled = true; online_retiring_ = std::move(online_admission_); set_failure(*error); return false; }
            if (!room) return false;
            if (online_options_->turn_credentials_observer)
                if (const auto credentials = room->turn_credentials()) online_options_->turn_credentials_observer(*credentials);
            online_pending_ = std::move(room); online_alias_ = alias;
            online_pending_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            const std::lock_guard lock(mutex_);
            view_.invitation_code = alias; view_.invitation_expires = online_pending_->lease().expires; view_.invitation_consumed = !view_.host;
        }
        // The admission reply carries no roster: the service pushes it right after. The agreed room size and this seat come from it.
        if (const auto status = online_pending_->status(); !status) {
            error = status.error();
            online_admission_.reset(); retire_room(std::exchange(online_pending_, {})); set_failure(*error); return false;
        }
        const auto roster = online_pending_->roster();
        if (roster.capacity == 0 || roster.slots[online_pending_->local_slot() % net::max_match_seats].state != net::GnsRoomSlotState::attached) {
            if (std::chrono::steady_clock::now() >= online_pending_deadline_) {
                online_admission_.reset(); retire_room(std::exchange(online_pending_, {}));
                set_failure(diagnostic("EAWR-NET-1528", "The room did not report its roster in time.")); return false;
            }
            return false;
        }
        online_admission_.reset();
        auto room = std::exchange(online_pending_, {});
        const std::string alias = online_alias_;
        const auto capacity = roster.capacity;
        if (view().host && capacity != owners_.size()) {
            retire_room(std::move(room));
            set_failure(diagnostic("EAWR-NET-1535", "The published room does not have the seat count of this match.")); return false;
        }
        net::GnsOptions options; options.build_identity = online_options_->build_identity;
        options.timeout = std::chrono::milliseconds(180000); options.turn_only = online_options_->turn_only;
        options.repair_grace = net::MeshIncidentOptions{}.repair_grace;
        options.peers = capacity;
        if (lobby_builder_) options.max_frame_size = static_cast<std::uint32_t>(net::max_lobby_frame_bytes);
        auto opened = view().host ? net::GnsTransport::host_room(room, options) : net::GnsTransport::join_room(room, options);
        online_endpoint_ = room; online_paired_at_ = std::chrono::steady_clock::now(); online_alias_ = alias;
        { const std::lock_guard lock(mutex_);
          view_.invitation_code = alias; view_.invitation_expires = room->lease().expires; view_.invitation_consumed = !view_.host;
          view_.message = view_.host ? "Waiting for a player to join..." : "Player connecting...";
        }
        if (!opened) { online_endpoint_.reset(); retire_room(std::move(room)); set_failure(opened.error()); return false; }
        if (online_options_->transport_observer) online_options_->transport_observer(*opened.value());
        io_->transport.tcp = std::make_unique<MatchTransport>(std::move(opened).value());
        if (lobby_builder_ && capacity >= 3) {
            io_->transport.credentials = credential_state_;
            io_->transport.credential_filter = [this](const net::Received& frame) { return current_mesh_credentials(frame); };
        }
        return true;
    }

} // namespace eawr::presentation::godot_backend::lan_detail
