#include "lan_match_internal.hpp"
#include "galactic_launch.hpp"
#include "eawr/platform/agreed_campaign.hpp"
#include "eawr/core/gc_load_trace.hpp"
#include "galactic_lobby_backend.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/checkpoint/game.hpp"

#include <algorithm>

namespace eawr::presentation::godot_backend {
core::Result<std::unique_ptr<platform::LiveGameSession>> GalacticLaunch::start(platform::LiveGameSession::Options live) const {
    if (finish_load_transport) {
        auto finished = finish_load_transport();
        if (!finished) return core::Result<std::unique_ptr<platform::LiveGameSession>>::failure(finished.error());
    }
    if (recovery_factory) match->configure_recovery_restore(recovery_factory(live.workers));
    live.step_driver = match->driver();
    live.services = services;
    if (!live.saves.campaign) live.saves.campaign = saved_campaign;
    return platform::LiveGameSession::start(owner, std::move(live));
}
const sim::galactic::GalacticSetup& GalacticLaunch::layout() const { return owner->session->galaxy().setup(); }
std::shared_ptr<const sim::galactic::GalacticSnapshot> GalacticLaunch::snapshot() const { return snapshot(sim::SeatId{seat}); }
std::shared_ptr<const sim::galactic::GalacticSnapshot> GalacticLaunch::snapshot(sim::galactic::SeatId requested) const {
    const auto& galaxy = owner->session->galaxy();
    const auto& seats = galaxy.setup().seats;
    const auto binding = std::find_if(seats.begin(), seats.end(), [&](const auto& row) { return row.seat_id == requested; });
    if (binding == seats.end() || !binding->active || binding->kind != sim::SeatKind::human) return {};
    return galaxy.snapshot()->observer() ? galaxy.view_snapshot({binding->player}) : galaxy.snapshot();
}
bool GalacticLaunch::native_ai_installed() const { return owner->session->native_ai_installed(); }
namespace lan_detail {

void TcpLanMatch::pump_galactic_start() {
    if (handed_over_) return;
    if (!lobby_result_) {
        auto pumped = lobby_->pump(lobby_clock_);
        flush_lobby_closes();
        if (!pumped) { set_failure(pumped.error()); return; }
        if (lobby_->snapshot().phase != net::LobbyPhase::starting && lobby_->snapshot().phase != net::LobbyPhase::committed) {
            set_failure(galactic_start_aborted(lobby_->notice())); return;
        }
        if (!view().host && !lobby_setup_accepted_ && lobby_->setup_phase() == net::SetupPhase::reviewing_offer) {
            auto accepted = lobby_->accept_setup();
            if (!accepted) { set_failure(accepted.error()); return; }
            core::gc_load::trace.mark("setup_accepted");
            lobby_setup_accepted_ = true;
        }
        if (!lobby_->result_ready()) return;
        auto committed = io_->transport.tcp->commit_room();
        if (!committed) { set_failure(committed.error()); return; }
        auto ready = lobby_->take_result();
        if (!ready) { set_failure(ready.error()); return; }
        core::gc_load::trace.mark("setup_committed");
        lobby_result_ = std::make_shared<net::LobbyResult>(std::move(ready).value());
        io_->room_lifetime = lobby_result_; io_->application_stream = lobby_stream_;
        lobby_room_committed_ = true;
        galactic_flush_started_ = lobby_clock_;
    }
    // send() queues the host commitment. Flush it before the UI stops pumping this owner for scene loading;
    // otherwise a peer waiting for its launch cannot receive the commitment until our live driver starts.
    auto raw = io_->transport.poll_socket();
    if (raw) {
        constexpr auto max_pending_bytes = 8U * 1024U * 1024U;
        if (io_->transport.pending.size() >= 256 || io_->transport.pending_bytes > max_pending_bytes
            || raw->frame.size() > max_pending_bytes - io_->transport.pending_bytes) {
            set_failure(net::lobby_diagnostic(net::LobbyReason::resource_limit,
                "Ordered campaign input exceeds the launch handover budget.")); return;
        }
        io_->transport.pending_bytes += raw->frame.size();
        io_->transport.pending.push_back(std::move(*raw));
    }
    for (net::PeerIndex peer = 0; peer < io_->transport.peer_count(); ++peer)
        if (peer != io_->transport.self() && io_->transport.link(peer) == net::LinkState::closed) {
            set_failure(io_->transport.tcp->diagnostic().value_or(net::lobby_diagnostic(net::LobbyReason::link_closed,
                "A campaign peer disconnected while sending the launch commitment."))); return;
        }
    constexpr std::uint64_t commit_flush_timeout_ms = 30'000;
    if (io_->transport.tcp->has_pending_output() && galactic_flush_started_
        && lobby_clock_ - *galactic_flush_started_ >= commit_flush_timeout_ms)
        set_failure(net::lobby_diagnostic(net::LobbyReason::link_closed,
            "The campaign commitment could not be sent before the launch deadline."));
}

std::shared_ptr<GalacticLaunch> TcpLanMatch::take_galactic_launch() {
    if (!galactic_start_ || !lobby_result_ || handed_over_ || view().phase == LanPhase::ended
        || !io_->transport.tcp || io_->transport.tcp->has_pending_output()) return {};
    const auto agreement = galactic_->agreement();
    if (!agreement || !agreement->session || !agreement->plan || !io_->monitor || !lobby_stream_) {
        set_failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "The agreed campaign owner is unavailable.")); return {};
    }
    core::gc_load::trace.mark("launch_handover");
    auto launch = std::make_shared<GalacticLaunch>();
    launch->options = galactic_start_->options;
    launch->roster = lobby_result_->snapshot();
    launch->plan = agreement->plan;
    launch->owner = std::make_shared<platform::PreparedLiveAgreement>(); launch->owner->session = agreement->session;
    launch->owner->load_notes = agreement->load_notes;
    launch->saved_campaign = galactic_->save_campaign_source(launch->options);
    launch->owner->initial_tick = agreement->resume ? agreement->resume->completed_tick : 0;
    launch->services = std::static_pointer_cast<const platform::CampaignServiceConfiguration>(agreement->runtime_services);
    GalacticLanOptions options;
    const auto& offer = lobby_result_->setup().offer();
    options.recovery_offer = offer;
    // A separate immutable private factory; no live agreement enters its worker.
    launch->recovery_factory = [backend = galactic_, offer](std::size_t workers) { return backend->recovery_factory(offer, workers); };
    if (mesh_repair_auth_) {
        auto auth = *mesh_repair_auth_; auth.epoch = 1;
        options.repair_recovery_links = [this, auth = std::move(auth)] { return io_->transport.tcp->repair_closed_links(auth); };
    }
    // The live HUD consumes a copy projected onto the authenticated saved IDs.
    // The staging room retains its own ephemeral admission identities.
    for (std::size_t i = 0; i < launch->roster.seats.size(); ++i) {
        launch->roster.seats[i].seat = offer.seats[i].seat;
        launch->roster.seats[i].player = offer.seats[i].player;
    }
    options.initial_checkpoint = galactic_->checkpoint_bytes();
    // Reuse the verified selected save on every peer. New games capture once
    // during loading; an unavailable optional base never prevents the launch.
    prepare_galactic_recovery_base(options, *agreement->session,
        dynamic_cast<net::GalacticSessionAdapter*>(&lobby_result_->setup().adapter()), launch->owner->initial_tick);
    options.input_delay = offer.input_delay; options.final_tick_count = offer.final_tick_count;
    std::vector<net::PeerIndex> native_peers;
    for (const auto& seat : launch->roster.seats) native_peers.push_back(seat.peer);
    options.quarantine_recovery_peer = [this, native_peers = std::move(native_peers)](net::PeerIndex peer) {
        if (peer < native_peers.size()) static_cast<void>(io_->transport.tcp->close_peer(native_peers[peer]));
    };
    options.final_output_drained = [this] { return !io_->transport.tcp->has_pending_output(); };
    for (const auto& seat : offer.seats) options.seats.push_back({seat.seat, true});
    if (lobby_stream_->self() >= offer.seats.size()) {
        set_failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "The local campaign seat is absent.")); return {};
    }
    launch->seat = offer.seats[lobby_stream_->self()].seat.value;
    launch->match = std::make_shared<GalacticLanMatch>(*lobby_stream_, std::move(options), lobby_result_, *io_->monitor,
        [this](bool allowed) { gate_.advance_allowed = allowed; }, shared_from_this());
    handed_over_ = true;
    { const std::lock_guard lock(mutex_); view_.phase = LanPhase::running; }
    launch->finish_load_transport = [self = shared_from_this()]() -> core::Result<void> {
        if (auto failure = self->stop_load_pump()) { self->set_failure(*failure); return core::Result<void>::failure(*failure); }
        return core::Result<void>::success();
    };
    start_load_pump();
    return launch;
}

} // namespace lan_detail
} // namespace eawr::presentation::godot_backend
