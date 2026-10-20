#include "lan_match_internal.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

std::optional<DriverResult> TcpLanMatch::drive_commit(const tactical::TacticalSetup& setup, platform::LiveStepContext& context, std::uint64_t now) {
        if (lobby_ && !lobby_result_) {
            lobby_clock_ = std::max(lobby_clock_, now);
            auto pumped = lobby_->pump(lobby_clock_); // Steady-clock milliseconds, independent of sim ticks.
            flush_lobby_closes();
            if (lobby_->snapshot().phase == net::LobbyPhase::staging || lobby_->snapshot().phase == net::LobbyPhase::closed) {
                io_->monitor.reset(); gate_.reset(); lobby_stream_ = nullptr;
                const auto reason = lobby_->notice() ? lobby_->notice()->reason : net::LobbyReason::setup_failed;
                const auto message = reason == net::LobbyReason::kicked ? "TEXT_INTERNET_PLAYER_KICKED"
                    : reason == net::LobbyReason::host_closed ? "TEXT_HOST_LEFT_GAME" : "The room ended before Start was committed.";
                return lobby_fail<bool>(reason, message);
            }
            if (!pumped) return DriverResult::failure(pumped.error());
            if (!view().host && !lobby_setup_accepted_ && lobby_->setup_phase() == net::SetupPhase::reviewing_offer) {
                auto accepted = lobby_->accept_setup(); if (!accepted) return DriverResult::failure(accepted.error());
                lobby_setup_accepted_ = true;
            }
            if (lobby_->snapshot().phase != net::LobbyPhase::committed || !context.advance_allowed)
                return DriverResult::success(false);
            // Validated host commitment freezes native formation immediately;
            // the independent TLS roster push need not have reached this guest.
            if (!lobby_room_committed_) {
                auto committed = io_->transport.tcp->commit_room();
                if (!committed) return DriverResult::failure(committed.error());
                lobby_room_committed_ = true;
            }
            // The host commits on its own readiness, which does not prove this guest holds every peer's tick-zero
            // summary yet. Wait for the local barrier (bounded by the setup timeout); never take the result early (#3929).
            if (!lobby_->result_ready()) return DriverResult::success(false);
            auto ready = lobby_->take_result(); if (!ready) return DriverResult::failure(ready.error());
            lobby_result_ = std::make_shared<net::LobbyResult>(std::move(ready).value());
            io_->room_lifetime = lobby_result_; io_->application_stream = lobby_stream_;
            { const std::lock_guard lock(mutex_); view_.phase = LanPhase::running; view_.input_delay = monitor_delay_; }
            attach_stage_observer();
            if (setup.departure_policy) core_session().enable_departures([this](sim::SeatCommandKey key) {
                const auto* bound = static_cast<const LiveAdapter&>(core_session().active_adapter()).binding(key.seat);
                if (!bound) return fail<Bytes>("Unbound departure seat.");
                return net::encode_tactical_input(key, *bound, {{}, tactical::QuitPayload{}});
            });
        }
        if (!runtime_ && !lobby_result_) {
            // Adapter creation can occur in this pump, so refresh its context in the provider.
            if (accepted_ && setup_session_->phase() == net::SetupPhase::reviewing_offer) {
                auto accepted = setup_session_->accept();
                if (!accepted) { set_failure(accepted.error()); return DriverResult::failure(accepted.error()); }
            }
            auto pumped = setup_session_->pump(now);
            if (!pumped) { set_failure(pumped.error()); return DriverResult::failure(pumped.error()); }
            if (io_->transport.credential_error) {
                const auto error = *io_->transport.credential_error; refuse_credentials(error);
                return DriverResult::failure(error);
            }
            if (setup_session_->phase() != net::SetupPhase::ready || !context.advance_allowed) return DriverResult::success({});
            {
                const std::lock_guard lock(mutex_);
                if (abort_requested_) return DriverResult::success(false);
                auto ready = setup_session_->take_result();
                if (!ready) return DriverResult::failure(ready.error());
                runtime_.emplace(std::move(ready).value());
                if (setup_result().offer().input_delay != monitor_delay_)
                    return fail<bool>("Monitor delay differs from the ready session.");
                view_.phase = LanPhase::running; view_.input_delay = monitor_delay_;
            }
            attach_stage_observer();
            if (setup.departure_policy) core_session().enable_departures([this](sim::SeatCommandKey key) {
                const auto* bound = static_cast<const LiveAdapter&>(core_session().active_adapter()).binding(key.seat);
                if (!bound) return fail<Bytes>("Unbound departure seat.");
                return net::encode_tactical_input(key, *bound, {{}, tactical::QuitPayload{}});
            });
        }

        return std::nullopt;
}

} // namespace eawr::presentation::godot_backend::lan_detail
