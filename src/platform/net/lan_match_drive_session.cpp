#include "lan_match_internal.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

DriverResult TcpLanMatch::drive_session(const tactical::TacticalSetup& setup, platform::LiveStepContext& context, std::uint64_t now) {
        auto& session = core_session();
        // Close presentation admission before synchronous replay can begin.
        { const std::lock_guard lock(mutex_);
          if (session.resyncing()) { view_.phase = LanPhase::resyncing; view_.recovery = LanRecovery::resyncing; }
          else if (session.reconnect_status().phase != net::ReconnectPhase::playing)
              { view_.phase = LanPhase::reconnecting; view_.recovery = LanRecovery::held; }
          else if (io_->monitor->frozen()) view_.recovery = LanRecovery::desync; }
        diagnostics_->view(view());
        auto pumped = session.pump(now);
        if (!pumped) {
            if (online_options_ && pumped.error().code == net::mesh_errors::left) mesh_left_text_ = removed_seat_text(session);
            if (pumped.error().code.starts_with("EAWR-NET-102")) refuse_credentials(pumped.error());
            else set_failure(pumped.error());
            return DriverResult::failure(pumped.error());
        }
        // Core detection occurs inside pump. Arm before the next native poll,
        // with the agreed incident epoch; pair failures remain N4 decision evidence.
        if (auto armed = arm_mesh_repair(session); !armed) {
            set_failure(armed.error()); return DriverResult::failure(armed.error());
        }
        if (online_options_) for (const auto& result : session.mesh_results()) {
            if (result.kind != net::MeshIncidentResult::Kind::departure) continue;
            for (net::PeerIndex peer = 0; peer < net::max_match_seats; ++peer) if (result.removed & net::mesh_seat(peer)) {
                if (auto retired = retire_mesh_seat(peer); !retired) {
                    set_failure(retired.error()); return DriverResult::failure(retired.error());
                }
            }
        }
        if (io_->transport.credential_error) {
            const auto error = *io_->transport.credential_error; refuse_credentials(error);
            return DriverResult::failure(error);
        }
        if (const auto status = session.reconnect_status(); status.phase == net::ReconnectPhase::proving
            || status.phase == net::ReconnectPhase::playing) {
            // Prefix restoration made these immutable future batches admissible.
            // Their bytes stayed charged against the same recovery receive budget.
            while (!retained_rejoin_frames_.empty()) {
                io_->transport.pending.push_front(std::move(retained_rejoin_frames_.back()));
                retained_rejoin_frames_.pop_back();
            }
        }
        if (&session.active_adapter() != adapter_ || adapter_->staging) {
            auto* published = const_cast<LiveAdapter*>(dynamic_cast<const LiveAdapter*>(&session.active_adapter()));
            if (!published) return fail<bool>("Published recovery adapter has the wrong mode.");
            auto activated = activate_adapter(*published); if (!activated) return DriverResult::failure(activated.error());
        }
        const auto& completeness = session.waiting_for();
        const bool waiting = (completeness && !completeness->peers.empty()) || !io_->monitor->waiting_for_peers().empty();
        const auto missing = completeness && !completeness->peers.empty() ? completeness->peers : io_->monitor->waiting_for_peers();
        if (diagnostics_->stall(waiting, now, missing)) input_progress("stall-start");
        diagnostics_->ledger.observe(now, waiting, missing.empty() ? 0 : missing.front());
        if (!waiting) waiting_since_.reset();
        else {
            if (!waiting_since_) waiting_since_ = now;
            if (completeness && !completeness->peers.empty()) waiting_since_ = std::min(*waiting_since_, completeness->since_ms);
        }
        {
            const std::lock_guard lock(mutex_);
            view_.peer_confirmed_tick = io_->monitor->peer_confirmed_tick();
            view_.link_stats.reset();
            view_.net_peers.clear();
            // The ledger scan is bounded but the driver polls far more often than the overlay redraws.
            if (!net_stall_at_ || now < *net_stall_at_ || now - *net_stall_at_ >= 100) {
                view_.net_stall = diagnostics_->ledger.snapshot(now);
                net_stall_at_ = now;
            }
            for (net::PeerIndex peer = 0; peer < io_->transport.peer_count(); ++peer)
                if (peer != io_->transport.self()) {
                    const auto stats = io_->transport.link_stats(peer);
                    if (stats && (!view_.link_stats || stats->smoothed_rtt > view_.link_stats->smoothed_rtt)) view_.link_stats = stats;
                    constexpr double per_ms = 1.0 / 1000.0;
                    view_.net_peers.push_back(stats
                        ? presentation::ui::NetPeerLink{peer, true, static_cast<double>(stats->smoothed_rtt.count()) * per_ms,
                                                        static_cast<double>(stats->jitter.count()) * per_ms, stats->loss_pct}
                        : presentation::ui::NetPeerLink{peer, false, 0.0, 0.0, 0.0});
                }
            if (view_.phase != LanPhase::ended) {
                view_.message.clear(); view_.diagnostic_code.clear();
                const auto reconnect = session.reconnect_status();
                view_.reconnect_enabled = session.reconnect_available();
                view_.rejoin_enabled = session.reconnect_available() && reconnect.phase == net::ReconnectPhase::held;
                if (reconnect.epoch) { view_.recovery_epoch = reconnect.epoch; view_.reconnect_seat = reconnect.peer; }
                view_.phase = session.resyncing() ? LanPhase::resyncing
                    : reconnect.phase != net::ReconnectPhase::playing ? LanPhase::reconnecting : LanPhase::running;
                view_.grace_ms = retained_offer_.recovery.grace_ms;
                view_.progress_timeout_ms = retained_offer_.recovery.progress_timeout_ms;
                view_.hard_cap_ms = retained_offer_.recovery.hard_cap_ms;
                view_.resync_timeout_ms = retained_offer_.recovery.resync_timeout_ms;
                view_.grace_remaining_ms = session.reconnect_grace_remaining_ms();
                const auto previous_recovery = view_.recovery;
                if (session.resyncing()) view_.recovery = LanRecovery::resyncing;
                else if (io_->monitor->frozen()) view_.recovery = LanRecovery::desync;
                else switch (reconnect.phase) {
                case net::ReconnectPhase::agreeing:
                case net::ReconnectPhase::held: view_.recovery = LanRecovery::held; break;
                case net::ReconnectPhase::catching_up: view_.recovery = LanRecovery::catching_up; break;
                case net::ReconnectPhase::proving: view_.recovery = LanRecovery::proving; break;
                case net::ReconnectPhase::playing:
                    view_.recovery = previous_recovery == LanRecovery::held || previous_recovery == LanRecovery::catching_up
                        || previous_recovery == LanRecovery::proving || previous_recovery == LanRecovery::resyncing
                        || previous_recovery == LanRecovery::desync || previous_recovery == LanRecovery::recovered
                        ? LanRecovery::recovered : LanRecovery::none;
                    break;
                }
                if (online_options_ && session.mesh_running()) {
                    view_.phase = LanPhase::reconnecting; view_.recovery = LanRecovery::held;
                    view_.recovery_epoch = session.mesh_incident_id(); view_.rejoin_enabled = false;
                    view_.message = mesh_repair_message();
                }
                if (io_->monitor->report()) capture_diagnostic(*io_->monitor->report());
                else if (io_->monitor->recovered_report()) capture_diagnostic(*io_->monitor->recovered_report());
                if (reconnect.phase == net::ReconnectPhase::playing) repair_notice_.reset();
                if (repair_notice_) {
                    view_.diagnostic_code = repair_notice_->code;
                    view_.message.clear(); // Curated recovery text; raw transport details stay local.
                }
                else if (session.reconnect_notice()) {
                    if (session.reconnect_notice()->code != net::reconnect_errors::unavailable) view_.diagnostic_code = session.reconnect_notice()->code;
                    if (session.reconnect_notice()->code == net::reconnect_errors::unavailable)
                        view_.reconnect_warning = "Reconnect is unavailable for this match. Play can continue.";
                }
            }
            if (online_options_) {
                const auto& results = session.mesh_results();
                while (mesh_seen_ < results.size()) append_mesh_notice(results[mesh_seen_++], now);
                view_.mesh_notice = mesh_notices_.text(now);
            }
            view_.service_notice.clear();
            if (view_.phase != LanPhase::ended) {
                if (const auto endpoint = online_endpoint_.lock()) {
                    const auto notice_now = std::chrono::steady_clock::now();
                    const auto status = endpoint->status();
                    const auto renewal_failure = endpoint->renewal_failure();
                    if (renewal_failure.first != last_renewal_failure_) {
                        if (renewal_failure.first) diagnostics_->emit("relay-renewal-failure", renewal_failure.second, diagnostics_->seat,
                            "failure_count=" + std::to_string(renewal_failure.first));
                        last_renewal_failure_ = renewal_failure.first;
                    }
                    const auto lease_error = status ? std::string{} : status.error().code;
                    if (lease_error != last_lease_error_) {
                        if (!lease_error.empty()) diagnostics_->emit("relay-lease-failure", lease_error, diagnostics_->seat);
                        last_lease_error_ = lease_error;
                    }
                    // The room lease is renewed by any attached member; only its end is visible here.
                    net::GnsSessionLease lease; lease.ends = lease.hard_ends = endpoint->lease().expires;
                    view_.service_notice = online_lease_notice(lease, notice_now, status ? "" : status.error().code);
                    const auto credentials = online_credential_notice(endpoint->turn_credentials(), online_paired_at_, notice_now);
                    if (!credentials.empty()) view_.service_notice += (view_.service_notice.empty() ? "" : "\n") + credentials;
                }
                if (online_options_ && adapter_) {
                    const auto length = match_length_notice(adapter_->completed_tick(), retained_offer_.final_tick_count);
                    if (!length.empty()) view_.service_notice += (view_.service_notice.empty() ? "" : "\n") + length;
                }
            }
            if (view_.phase == LanPhase::running && waiting_since_ && now > *waiting_since_ && now - *waiting_since_ > 3000) {
                // MP-33: completeness wait is a UI message, never a pause or elapsed sim time.
                view_.recovery = LanRecovery::waiting;
                view_.message = view_.human_owners.size() > 2 || view_.human_owners[0] == view_.human_owners[1]
                    ? "Waiting for human seats..." : "Waiting for " + view_.peer_name + "...";
            }
            if (io_->monitor->frozen() && !session.resyncing() && !retained_options_.resync) {
                view_.phase = LanPhase::ended; view_.reason = LanReason::desync; view_.recovery = LanRecovery::failed;
                if (const auto& report = io_->monitor->report()) { capture_diagnostic(*report); }
            }
        }
        for (const auto& event : pumped.value().departure_events) {
            constexpr std::array departure_reasons{"quit", "disconnect", "timeout", "pair-loss"};
            const auto reason_index = static_cast<std::size_t>(event.reason);
            diagnostics_->emit("departure-applied", reason_index < departure_reasons.size() ? departure_reasons[reason_index] : "unknown",
                diagnostic_peer(event.key.seat), "command_tick=" + std::to_string(event.key.tick));
            if (online_options_) for (net::PeerIndex peer = 0; peer < setup_result().offer().seats.size(); ++peer)
                if (setup_result().offer().seats[peer].seat == event.key.seat) {
                    if (auto retired = retire_mesh_seat(peer); !retired) {
                        set_failure(retired.error()); return DriverResult::failure(retired.error());
                    }
                }
            const auto* bound = adapter_->binding(event.key.seat);
            if (!bound) continue;
            net::DepartureOutcome departure{event, bound->player, bound->player, net::DepartureState::continues};
            if (context.departures) for (const auto& control : context.departures())
                if (control.player == bound->player && control.tick == event.key.tick) {
                    departure.controller = control.controller;
                    departure.state = control.basic_ai ? net::DepartureState::continues_with_ai : net::DepartureState::continues;
                }
            if (const auto& outcome = context.snapshot()->outcome(); outcome && outcome->condition == tactical::VictoryCondition::intentional_quit) {
                const auto local = std::find_if(setup.players.begin(), setup.players.end(),
                    [&](const auto& player) { return player.player_id == local_player(); });
                departure.state = local != setup.players.end() && local->team_id == outcome->winner_team
                    ? net::DepartureState::won_by_departure : net::DepartureState::match_ended;
            }
            const std::lock_guard lock(mutex_); view_.departure = departure;
            if (event.key.seat == setup_result().offer().seats[lobby_stream_ ? lobby_stream_->self() : io_->transport.self()].seat) {
                view_.phase = LanPhase::ended; view_.reason = LanReason::peer_left;
                view_.message = "You left the match.";
            }
        }
        if (adapter_->advanced && context.snapshot()->outcome()) {
            const auto& outcome = *context.snapshot()->outcome();
            { const std::lock_guard lock(mutex_);
              if (context.completed_tick() >= outcome.end_tick) view_.phase = LanPhase::ended;
              const auto local = std::find_if(setup.players.begin(), setup.players.end(),
                  [&](const auto& player) { return player.player_id == local_player_unlocked(); });
              view_.victory = local != setup.players.end() && local->team_id == outcome.winner_team;
              if (outcome.condition == tactical::VictoryCondition::intentional_quit) {
                  view_.reason = LanReason::peer_left;
                  view_.message = *view_.victory ? "Victory - opponent left." : "Defeat - you left the match.";
              }
            }
            if (context.completed_tick() >= outcome.end_tick) {
                auto ended = io_->monitor->finish(context.canonical_state(), context.state_hash());
                if (!ended) { set_failure(ended.error()); return DriverResult::failure(ended.error()); }
            }
        }
        if (view().phase == LanPhase::ended) {
            erase_credentials();
            auto closed = pump_close();
            if (!closed) return DriverResult::failure(closed.error());
        }
        return DriverResult::success(std::exchange(adapter_->advanced, false));
}

} // namespace eawr::presentation::godot_backend::lan_detail
