#include "lan_match_internal.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

void TcpLanMatch::erase_credentials() noexcept {
        mesh_repair_auth_.reset(); mesh_material_revision_.reset(); mesh_retired_ = 0;
        io_->transport.credential_filter = {};
        credential_state_->clear();
        io_->transport.credentials.reset(); // Drains cannot provision a finished match again.
        const auto erase_delivery = [](auto& queue) {
            for (auto& frame : queue) if (!frame.frame.empty() && frame.frame[0] == net::lan_credentials_kind)
                net::erase_reconnect_secret(frame.frame);
        };
        erase_delivery(io_->transport.pending); erase_delivery(early_); erase_delivery(retained_rejoin_frames_);
        if (setup_session_) setup_session_->erase_reconnect_credentials();
        if (retained_options_.reconnect) retained_options_.reconnect->seat_tokens.clear();
        if (rejoin_session_) rejoin_session_->erase_reconnect_credentials();
        if (runtime_) runtime_->session().erase_reconnect_credentials();
        if (io_->transport.tcp) io_->transport.tcp->erase_reconnect_credentials();
    }

void TcpLanMatch::refuse_credentials(const core::Diagnostic& error) {
        const auto refusal = net::encode_setup_refusal({net::SetupReason::options, {}, error.code + ": " + error.message, {}, {}});
        if (refusal) for (net::PeerIndex peer = 0; peer < io_->transport.peer_count(); ++peer)
            if (peer != io_->transport.self() && io_->transport.link(peer) == net::LinkState::open)
                static_cast<void>(io_->transport.send(peer, refusal.value()));
        set_failure(error);
    }

core::Result<void> TcpLanMatch::service_recovery_io() {
        { const std::lock_guard lock(mutex_);
          if (abort_requested_) return core::Result<void>::failure(diagnostic("EAWR-NET-1801", "Local recovery cancelled.")); }
        // Replay remains on the simulation owner. Drain the socket into the bounded
        // stable wrapper to keep keepalive/chunk traffic moving between replay ticks.
        for (unsigned index = 0; index < 256 && io_->transport.pending.size() + retained_rejoin_frames_.size() < 4096; ++index) {
            auto frame = io_->transport.poll_socket(); if (!frame) break;
            if (frame->frame.size() > 8U * 1024U * 1024U - io_->transport.pending_bytes)
                return core::Result<void>::failure(diagnostic("EAWR-NET-1215", "Recovery receive buffer exceeded its limit."));
            io_->transport.pending_bytes += frame->frame.size();
            io_->transport.pending.push_back(std::move(*frame));
        }
        if (io_->transport.credential_error) return core::Result<void>::failure(*io_->transport.credential_error);
        return core::Result<void>::success();
    }

net::TcpRejoinAuth TcpLanMatch::tcp_rejoin_auth(std::uint64_t epoch) const {
        net::TcpRejoinAuth auth; auth.match_identity = retained_options_.reconnect->match_identity;
        auth.seat_tokens = retained_options_.reconnect->seat_tokens; auth.epoch = epoch; return auth;
    }

net::SetupResult& TcpLanMatch::setup_result() { return lobby_result_ ? lobby_result_->setup() : *runtime_; }

net::LockstepSession& TcpLanMatch::core_session() { return rejoin_session_ ? *rejoin_session_ : setup_result().session(); }

net::PeerIndex TcpLanMatch::diagnostic_peer(sim::SeatId seat) {
        for (const auto& value : setup_result().offer().seats) if (value.seat == seat) return value.peer;
        return diagnostics_->seat;
    }

void TcpLanMatch::attach_stage_observer() {
        if (trace_ || diagnostics_->sink) core_session().set_stage_observer([this](std::string_view event, const net::SeatInput& input, std::uint64_t tick) {
            const auto* bound = static_cast<const LiveAdapter&>(core_session().active_adapter()).binding(input.key.seat);
            if (!bound) return;
            auto decoded = net::decode_tactical_input(input, *bound);
            if (!decoded) return;
            if (trace_) trace_->stage(event, input.key, command_kind(decoded.value().payload), tick);
            if (std::holds_alternative<tactical::QuitPayload>(decoded.value().payload))
                diagnostics_->emit(event == "seal" ? "quit-sent" : "quit-received", "intentional-quit",
                    diagnostic_peer(input.key.seat), "command_tick=" + std::to_string(input.key.tick));
        });
    }

core::Diagnostic TcpLanMatch::repair_diagnostic(core::Diagnostic error) const {
        if (io_->transport.self() == 0 && io_->transport.peer_count() == 2 && error.code == "EAWR-NET-0206") {
            error.code = "EAWR-NET-1017";
            error.message = "Reconnect listener failed: " + error.message;
        }
        return error;
    }

core::Result<void> TcpLanMatch::activate_adapter(LiveAdapter& adapter) {
        if ((!runtime_ && !lobby_result_) || &core_session().active_adapter() != &adapter || !adapter.staging
            || core_session().reconnect_status().phase != net::ReconnectPhase::playing)
            return core::Result<void>::success();
        if (!adapter.staging->activate) return core::Result<void>::failure(diagnostic("EAWR-NET-1213", "Recovery publication is unavailable."));
        auto activated = adapter.staging->activate(); if (!activated) return activated;
        adapter.staging.reset(); adapter.bind_live_context(*current_context_, trace_); adapter_ = &adapter;
        queued_.clear();
        { const std::lock_guard lock(mutex_); ++view_.active_adapter_publications; }
        return core::Result<void>::success();
    }

tactical::PlayerId TcpLanMatch::local_player_unlocked() const { return view_.player; }

std::optional<std::uint64_t> TcpLanMatch::pending_input_epoch() {
        std::function<std::optional<std::uint64_t>()> provider;
        { const std::lock_guard lock(input_epoch_mutex_); provider = input_epoch_provider_; }
        return provider ? provider() : std::nullopt;
    }

} // namespace eawr::presentation::godot_backend::lan_detail
