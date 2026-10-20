#include "lockstep_session_internal.hpp"
#include <algorithm>
#include <set>

namespace eawr::net {
using namespace lockstep_detail;

LockstepSession::LockstepSession(SessionAdapter& adapter, MessageTransport& transport,
    std::vector<PeerSeat> seats, SessionOptions options, SessionMonitor* monitor)
    : adapter_(&adapter), transport_(&transport), seats_(std::move(seats)), options_(options), self_(transport.self()),
      received_through_(seats_.size()), command_cursors_(seats_.size()), accepted_(seats_.size()), monitor_(monitor), departed_(seats_.size()), quit_in_batch_(seats_.size()), quit_seen_(seats_.size()), recovery_(seats_.size()),
      next_policy_tick_(64 * std::max<std::uint64_t>(1, options.input_delay / 2)), initial_input_delay_(options.input_delay) {
    last_sent_batch_.resize(seats_.size());
    if (options_.mesh)
        mesh_ = std::make_unique<MeshIncident>(transport, static_cast<MeshSeats>((1U << seats_.size()) - 1U), *options_.mesh);
}

core::Result<LockstepSession> LockstepSession::create(SessionAdapter& adapter, MessageTransport& transport,
    std::vector<PeerSeat> seats, SessionOptions options, SessionMonitor* monitor) {
    const auto count = transport.peer_count();
    if (count < 2 || count > max_match_seats || transport.self() >= count || seats.size() != count || adapter.completed_tick() != 0
        || options.input_delay == 0 || options.input_delay > max_input_delay || options.final_tick_count > max_session_ticks
        || options.completeness_timeout_ms == 0 || options.max_frames_per_pump == 0)
        return refused<LockstepSession>(session_errors::options, "invalid roster, initial tick or session options");
    if (options.adaptive_timing && (!options.non_lan || options.adaptive_timing->host >= count || options.input_delay > 64))
        return refused<LockstepSession>(timing_errors::options, "invalid adaptive host or initial delay");
    std::set<sim::SeatId> unique;
    for (const auto& seat : seats)
        if (seat.seat.value == 0 || !unique.insert(seat.seat).second)
            return refused<LockstepSession>(session_errors::options, "unbound or repeated seat");
    if (options.mesh) {
        const auto& mesh = *options.mesh;
        if (mesh.host != 0 || count < 2 || mesh.repair_grace.count() < 0 || mesh.repair_grace > std::chrono::minutes(10)
            || mesh.report_interval.count() <= 0 || mesh.report_interval > std::chrono::seconds(10) || mesh.report_freshness.count() <= 0
            || (options.adaptive_timing && options.adaptive_timing->host != mesh.host))
            return refused<LockstepSession>(mesh_errors::options, "invalid room mesh roster or incident options");
    }
    if (options.reconnect) {
        const auto& reconnect = *options.reconnect;
        const auto nonzero = [](const auto& bytes) { return std::any_of(bytes.begin(), bytes.end(), [](auto byte) { return byte != 0; }); };
        if (!nonzero(reconnect.match_identity) || (reconnect.allow_rejoin && !valid_reconnect_tokens(reconnect.seat_tokens, transport.self(), count))
            || (!reconnect.allow_rejoin && (reconnect.rejoin_source || !reconnect.seat_tokens.empty()))
            || reconnect.grace_ms == 0 || reconnect.grace_ms > max_recovery_timeout_ms
            || reconnect.progress_timeout_ms == 0 || reconnect.progress_timeout_ms > max_recovery_timeout_ms
            || reconnect.hard_cap_ms > max_recovery_timeout_ms
            || reconnect.hard_cap_ms < reconnect.grace_ms || reconnect.hard_cap_ms < reconnect.progress_timeout_ms
            || reconnect.hello_retry_ms == 0 || reconnect.hello_retry_ms > max_recovery_timeout_ms
            || reconnect.max_hello_attempts == 0 || reconnect.max_hello_attempts > 64
            || reconnect.max_log_bytes < reconnect.setup.size() + 1024
            || reconnect.max_log_bytes > 256U * 1024U * 1024U || reconnect.setup.empty()
            || reconnect.setup.size() > max_batch_bytes
            || (reconnect.rejoin_source && (*reconnect.rejoin_source >= count
                || *reconnect.rejoin_source == transport.self() || reconnect.rejoin_epoch == 0)))
            return refused<LockstepSession>(reconnect_errors::options, "invalid agreed reconnect envelope");
    }
    if (options.resync && (!options.reconnect || !monitor || !options.resync->rebuild
        || options.resync->timeout_ms == 0 || options.resync->timeout_ms > max_recovery_timeout_ms
        || (options.reconnect->replay_control && !options.resync->replay_control)))
        return refused<LockstepSession>(resync_errors::options, "resync requires a monitor, retained setup/log and staging factory/control replay");
    if (monitor && options.mesh) {
        if (auto enabled = monitor->on_link_repair(transport.self(), [count](PeerIndex peer) {
            return count >= 3 && peer < count;
        }); !enabled)
            return core::Result<LockstepSession>::failure(enabled.error());
    }
    return core::Result<LockstepSession>::success(LockstepSession(adapter, transport, std::move(seats), options, monitor));
}

core::Diagnostic LockstepSession::connection_loss(PeerIndex peer, std::string_view cause) {
    connection_lost_peer_ = peer;
    return diagnostic(session_errors::stalled, "Connection lost: the match could not continue. "
        + std::string(cause) + " (peer " + std::to_string(peer) + ").");
}

core::Result<PumpResult> LockstepSession::fail(core::Diagnostic error) {
    const auto code = std::find(resync_codes.begin(), resync_codes.end(), error.code);
    if (options_.resync && (resync_ || refused_resync_epoch_ || code != resync_codes.end())) {
        Bytes abort{resync_abort_kind};
        append(abort, resync_ ? resync_->epoch : refused_resync_epoch_.value_or(std::max(resync_epoch_, reconnect_epoch_) + 1), 8);
        append(abort, code == resync_codes.end() ? 3 : static_cast<std::size_t>(code - resync_codes.begin()), 1);
        // Best effort: the local failure remains authoritative if a link died.
        static_cast<void>(resync_broadcast(abort));
    }
    failure_ = std::move(error);
    return core::Result<PumpResult>::failure(*failure_);
}

core::Result<PumpResult> LockstepSession::pump(const std::uint64_t now_ms) {
    if (failure_) return core::Result<PumpResult>::failure(*failure_);
    if (encode_departure_ && departed_[self_]) return core::Result<PumpResult>::success({});
    if (last_now_ && now_ms < *last_now_) return fail(diagnostic(session_errors::clock, "caller clock went backwards"));
    last_now_ = now_ms;
    if (!recorded_zero_ && !(options_.reconnect && options_.reconnect->rejoin_source)) {
        if (monitor_) {
            const bool end = options_.final_tick_count == 0;
            const auto encoding = monitor_->needs_encoding(0, end) ? adapter_->canonical_state() : Bytes{};
            auto recorded = monitor_->record(0, adapter_->state_hash(), encoding, {}, end);
            if (!recorded) return fail(recorded.error());
        }
        recorded_zero_ = true;
    }
    // Receive even when an exchange/freeze blocks advancement, and after the final tick.
    bool receive_drained = false;
    for (std::size_t count = 0; count < options_.max_frames_per_pump; ++count) {
        auto frame = transport_->poll();
        if (!frame) { receive_drained = true; break; }
        auto accepted = receive(std::move(*frame));
        if (!accepted) return fail(accepted.error());
    }
    if (auto meshed = service_mesh(now_ms, receive_drained); !meshed) return fail(meshed.error());
    const auto tick = adapter_->completed_tick();
    // Diagnose another survivor loss before reconnect service can send to its
    // closed stream or alter the held epoch. The simulation prefix stays frozen.
    if (receive_drained)
        if (auto checked = check_held_survivors(); !checked) return fail(checked.error());
    if (options_.resync && (resync_ || (monitor_ && monitor_->frozen()))) {
        auto serviced = service_resync();
        if (!serviced) return fail(serviced.error());
        if (serviced.value()) return core::Result<PumpResult>::success({});
    }
    if (options_.reconnect && (held_ || options_.reconnect->rejoin_source)) {
        auto serviced = service_reconnect();
        if (!serviced) return fail(serviced.error());
        if (serviced.value()) return core::Result<PumpResult>::success({});
    }
    // A hash gate still requires its voters even if their current input batch
    // arrived. Diagnose a closed default-path voter only after queued frames drain.
    if ((!encode_departure_ || tick == options_.final_tick_count) && monitor_ && !monitor_->may_step() && receive_drained)
        for (const auto peer : monitor_->waiting_for_peers()) {
            if (peer >= seats_.size() || peer == self_)
                return fail(diagnostic(session_errors::options, "monitor supplied an invalid waiting voter"));
            if (!departed_[peer] && transport_->link(peer) == LinkState::closed)
                return fail(diagnostic(session_errors::link_closed, "hash gate requires a closed peer stream"));
        }
    if (tick == options_.final_tick_count) {
        // Replay admission has ended: a silent final hash voter yields a bounded
        // error in either mode rather than inventing a command beyond the replay.
        if (monitor_ && !monitor_->may_step() && !monitor_->frozen()) {
            auto needed = monitor_->waiting_for_peers();
            for (const auto peer : needed)
                if (peer >= seats_.size() || peer == self_)
                    return fail(diagnostic(session_errors::options, "monitor supplied an invalid waiting voter"));
            std::erase_if(needed, [&](PeerIndex peer) { return departed_[peer]; });
            if (needed.empty()) monitor_wait_.reset();
            else {
                if (!monitor_wait_ || monitor_wait_->tick != tick) monitor_wait_ = WaitingFor{tick, {}, now_ms};
                monitor_wait_->peers = std::move(needed);
                if (now_ms - monitor_wait_->since_ms >= options_.completeness_timeout_ms)
                    return fail(connection_loss(monitor_wait_->peers.front(), "Final hash voter timed out"));
            }
        } else monitor_wait_.reset();
        return core::Result<PumpResult>::success({});
    }
    // The opt-in policy also resolves a closed peer while a hash gate is waiting
    // on it. Drain delivered batches first, then recover/ack the common frontier;
    // legacy sessions still diagnose closure only for a missing tick batch.
    if (auto serviced = service_departures(receive_drained); !serviced) return fail(serviced.error());
    if (agreement_) return core::Result<PumpResult>::success({});
    if (held_ && tick >= held_->cutoff) return core::Result<PumpResult>::success({});
    if (!held_ && std::find(recovered_control_ticks_.begin(), recovered_control_ticks_.end(), tick) != recovered_control_ticks_.end()) {
        for (const auto& control : reconnect_controls_.at(tick)) {
            auto applied = apply_reconnect_control(control);
            if (!applied) return fail(applied.error());
        }
        std::erase(recovered_control_ticks_, tick);
    }
    if (encode_departure_ && monitor_ && !held_ && !monitor_->may_step() && !monitor_->frozen()) {
        auto needed = monitor_->waiting_for_peers();
        for (const auto peer : needed)
            if (peer >= seats_.size() || peer == self_)
                return fail(diagnostic(departure_errors::malformed, "monitor supplied an invalid waiting voter"));
        std::erase_if(needed, [&](PeerIndex peer) { return departed_[peer] || agreed_departures_.contains(peer); });
        std::sort(needed.begin(), needed.end());
        needed.erase(std::unique(needed.begin(), needed.end()), needed.end());
        if (needed.empty()) monitor_wait_.reset();
        else {
            if (!monitor_wait_ || monitor_wait_->tick != tick) monitor_wait_ = WaitingFor{tick, {}, now_ms};
            monitor_wait_->peers = std::move(needed);
            if (now_ms - monitor_wait_->since_ms >= options_.completeness_timeout_ms && mesh_ && mesh_->running())
                monitor_wait_->since_ms = now_ms; // a pair-loss incident has its own deadlines
            else if (now_ms - monitor_wait_->since_ms >= options_.completeness_timeout_ms) {
                return fail(connection_loss(monitor_wait_->peers.front(), "Required hash voter timed out"));
            }
        }
    } else monitor_wait_.reset();
    if (auto timing = advance_timing(); !timing) return fail(timing.error());
    if (auto serviced = service_departures(receive_drained); !serviced) return fail(serviced.error());
    if (agreement_ || (held_ && tick >= held_->cutoff)) return core::Result<PumpResult>::success({});
    if (timing_ && !timing_->retained) {
        const bool closed = encode_departure_ && receive_drained && [&] {
            for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
                if (peer != self_ && timing_voter(peer) && transport_->link(peer) == LinkState::closed) return true;
            return false;
        }();
        if (closed || !deferred_departures_.empty()) return core::Result<PumpResult>::success({});
    }
    if (timing_ && tick >= timing_->event.tick) return core::Result<PumpResult>::success({});
    if (auto sealed = seal(); !sealed) return fail(sealed.error());
    // A local input decoder can reserve the current tick. Its unsealed batch
    // is not a missing remote voter; wait for the producer handoff before
    // starting peer completeness/departure clocks or accessing ready_.
    if (next_seal_ <= tick) {
        waiting_.reset();
        return core::Result<PumpResult>::success({});
    }
    std::vector<PeerIndex> missing;
    const auto& batches = ready_.at(tick);
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
        if (!departed_[peer] && !(agreed_departures_.contains(peer) && tick >= agreed_departures_.at(peer).key.tick) && !batches.contains(peer)) missing.push_back(peer);
    if (!missing.empty()) {
        for (const auto peer : missing) {
            // At the poll budget there may still be a delivered batch behind duplicates.
            // A closed stream is conclusive only after the currently available queue drains.
            if (receive_drained && transport_->link(peer) == LinkState::closed) {
                waiting_ = WaitingFor{tick, missing, now_ms};
                if (!encode_departure_) return fail(diagnostic(session_errors::link_closed, "missing tick batch from a closed peer stream"));
                // The lost pair may be only ours: the mesh incident decides who leaves, never this seat alone.
                if (mesh_ && mesh_->owns_closed_link(peer)) return core::Result<PumpResult>::success({});
                auto begun = begin_departure(peer, options_.closed_link_reason ? options_.closed_link_reason(peer) : DepartureReason::disconnect);
                if (!begun) return fail(begun.error());
                return core::Result<PumpResult>::success({});
            }
        }
        // MP-33: a monitor exchange/freeze is a separate admission gate. Start a fresh
        // completeness clock on resume so its blocked time never reaches the overlay.
        if (monitor_ && !monitor_->may_step()) {
            waiting_.reset();
            return core::Result<PumpResult>::success({});
        }
        if (!waiting_ || waiting_->tick != tick) waiting_ = WaitingFor{tick, {}, now_ms};
        waiting_->peers = std::move(missing);
        if ((!monitor_ || monitor_->may_step()) && now_ms - waiting_->since_ms >= options_.completeness_timeout_ms && mesh_ && mesh_->running())
            waiting_->since_ms = now_ms; // a pair-loss incident has its own deadlines
        else if ((!monitor_ || monitor_->may_step()) && now_ms - waiting_->since_ms >= options_.completeness_timeout_ms) {
            return fail(connection_loss(waiting_->peers.front(), "Required input batch timed out"));
        }
        return core::Result<PumpResult>::success({});
    }
    waiting_.reset();
    if (monitor_ && !monitor_->may_step() && !held_) return core::Result<PumpResult>::success({});
    // Repaired streams can fill the old gap before the decision arrives. Only
    // the shared confirmation permits continuation; ordinary convergence and
    // confirmed departures above still run while this gate holds the sim.
    if (mesh_ && mesh_->running() && mesh_->state() != MeshLossState::settling)
        return core::Result<PumpResult>::success({});
    std::vector<SeatInput> canonical;
    for (const auto& [peer, batch] : batches) {
        if (departed_[peer]) continue;
        canonical.insert(canonical.end(), batch.commands.begin(), batch.commands.end());
    }
    for (const auto& [peer, departure] : agreed_departures_) {
        static_cast<void>(peer);
        if (departed_[peer] || quit_in_batch_[peer] || departure.key.tick != tick) continue;
        auto bytes = encode_departure_(departure.key);
        if (!bytes) return fail(bytes.error());
        canonical.push_back({departure.key, std::move(bytes).value()});
    }
    std::sort(canonical.begin(), canonical.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    PumpResult result;
    for (const auto& input : canonical)
        if (adapter_->is_departure(input)) {
            result.departures.push_back(input.key);
            DepartureReason reason = DepartureReason::quit;
            for (const auto& [peer, departure] : agreed_departures_) {
                static_cast<void>(peer);
                if (departure.key == input.key) reason = departure.reason;
            }
            result.departure_events.push_back({input.key, reason});
        }
    auto installed = adapter_->install(canonical);
    if (!installed) return fail(installed.error());
    auto stepped = adapter_->step();
    if (!stepped) return fail(stepped.error());
    if (stepped.value().completed_tick != tick + 1 || adapter_->completed_tick() != tick + 1)
        return fail(diagnostic(session_errors::simulation, "adapter did not advance exactly one tick"));
    if (options_.reconnect && reconnect_available_) {
        Bytes row;
        append(row, tick, 8);
        const auto controls = reconnect_controls_.find(tick);
        append(row, controls == reconnect_controls_.end() ? 0 : controls->second.size(), 4);
        if (controls != reconnect_controls_.end()) for (const auto& control : controls->second) {
            append(row, control.size(), 4); row.insert(row.end(), control.begin(), control.end());
        }
        append(row, canonical.size(), 4);
        for (const auto& input : canonical) {
            append(row, input.key.seat.value, 4); append(row, input.key.sequence, 8);
            append(row, input.bytes.size(), 4); row.insert(row.end(), input.bytes.begin(), input.bytes.end());
        }
        if (row.size() + 4 > options_.reconnect->max_log_bytes - reconnect_bytes_)
            disable_reconnect("history budget exhausted; play continues without reconnect");
        else {
            reconnect_bytes_ += row.size() + 4;
            reconnect_log_.push_back(std::move(row));
        }
        reconnect_controls_.erase(tick);
    }
    ready_.erase(tick);
    if (mesh_ && tick > input_horizon()) {
        for (auto row = recovery_[self_].begin(); row != recovery_[self_].lower_bound(tick - input_horizon()); ++row) {
            const auto encoded = encode_batch(row->second);
            if (encoded) mesh_tail_bytes_ -= encoded.value().size();
        }
    }
    for (auto& history : recovery_) {
        const auto window = input_horizon();
        if (tick > window) history.erase(history.begin(), history.lower_bound(tick - window));
    }
    for (const auto& event : result.departure_events)
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
            if (seats_[peer].seat == event.key.seat) {
                departed_[peer] = true;
                if (monitor_ && !agreed_departures_.contains(peer)) {
                    auto changed = monitor_->on_departure(peer, tick);
                    if (!changed) return fail(changed.error());
                }
            }
    result.completed = std::move(stepped).value();
    if (monitor_ && !held_) {
        // Engine-side AI can generate inputs inside the same gated step. Capture
        // its executed replay commands too, after all simulation/script commit.
        auto encoded = adapter_->canonical_input();
        if (!encoded) return fail(encoded.error());
        auto input = std::move(encoded).value();
        const auto& completed = *result.completed;
        const bool end = completed.completed_tick == options_.final_tick_count;
        const bool exchange = monitor_->needs_encoding(completed.completed_tick, end);
        const auto encoding = exchange ? adapter_->canonical_state() : Bytes{};
        auto recorded = monitor_->record(completed.completed_tick,
            completed.world_only && exchange ? adapter_->state_hash() : completed.state_sha256, encoding, input, end);
        if (!recorded) return fail(recorded.error());
    }
    if (departure_handler_ && !result.departures.empty()) {
        auto handled = departure_handler_(result.departures);
        if (!handled) return fail(handled.error());
    }
    return core::Result<PumpResult>::success(std::move(result));
}

} // namespace eawr::net
