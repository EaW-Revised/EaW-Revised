#include "lan_match_internal.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

std::optional<DriverResult> TcpLanMatch::drive_submit(platform::LiveStepContext& context, std::uint64_t now) {
        auto& session = core_session();
        if (auto armed = arm_mesh_repair(session); !armed) {
            set_failure(armed.error()); return DriverResult::failure(armed.error());
        }
        const auto status_before = session.reconnect_status();
        if (session.reconnect_available() && status_before.phase != net::ReconnectPhase::playing
            && (!last_repair_ms_ || now - *last_repair_ms_ >= 1000)) {
            auto repaired = io_->transport.tcp->repair_closed_links(tcp_rejoin_auth(status_before.epoch));
            if (!repaired) repair_notice_ = repair_diagnostic(repaired.error());
            else repair_notice_.reset();
            last_repair_ms_ = now;
        }
        if (session.mesh_running() || session.resyncing() || io_->monitor->frozen() || session.reconnect_status().phase != net::ReconnectPhase::playing) discard_recovery_commands();
        for (const auto& command : queued_) {
            const auto& seat = setup_result().offer().seats[lobby_stream_ ? lobby_stream_->self() : io_->transport.self()];
            if (command.key.player_id != seat.player) return fail<bool>("Command belongs to another player's seat.");
            const tactical::SeatBinding binding{seat.seat, seat.player, true, false};
            const auto frontier = session.next_seal();
            auto submitted = session.submit([&](sim::SeatCommandKey key) {
                return net::encode_tactical_input(key, binding, {command.units, command.payload});
            }, command.key.tick);
            if (!submitted) {
                if (submitted.error().code == net::session_errors::admission) { discard_command(command); continue; }
                set_failure(submitted.error()); return DriverResult::failure(submitted.error());
            }
            std::function<void(std::uint64_t, std::uint64_t)> observer;
            { const std::lock_guard lock(input_epoch_mutex_); observer = submission_observer_; }
            if (observer) observer(command.key.sequence, submitted.value().tick);
            if (trace_) trace_->submitted(submitted.value(), command_kind(command.payload), context.completed_tick(), frontier,
                command.key.sequence, command.key.tick);
        }
        queued_.clear();
        std::vector<net::MatchOptions> speed_requests;
        { const std::lock_guard lock(mutex_); speed_requests.swap(speed_requests_); }
        for (const auto options : speed_requests) {
            auto submitted = session.submit([options](sim::SeatCommandKey) { return net::encode_match_options(options); });
            if (!submitted) {
                // A departure agreement can close admission between the UI request
                // and this drain. An optional speed change must not terminate it.
                if (submitted.error().code == net::session_errors::admission) {
                    const std::lock_guard lock(mutex_); view_.input_feedback = "The speed change was cancelled because synchronization changed."; continue;
                }
                set_failure(submitted.error()); return DriverResult::failure(submitted.error());
            }
        }

        return std::nullopt;
}

} // namespace eawr::presentation::godot_backend::lan_detail
