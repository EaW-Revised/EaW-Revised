#include "lan_match_internal.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

std::optional<DriverResult> TcpLanMatch::drive_rejoin(const tactical::TacticalSetup& setup, platform::LiveStepContext& context, std::uint64_t now) {
        bool rejoining{};
        { const std::lock_guard lock(mutex_); rejoining = std::exchange(rejoin_requested_, false); }
        const auto requested_status = core_session().reconnect_status();
        if (rejoining && requested_status.phase == net::ReconnectPhase::held && core_session().reconnect_available()) {
            bool retry = last_repair_ms_ && now - *last_repair_ms_ < 1000;
            if (!retry) {
                last_repair_ms_ = now;
                auto restarted = io_->transport.tcp->restart_links(tcp_rejoin_auth(requested_status.epoch));
                if (!restarted) { repair_notice_ = repair_diagnostic(restarted.error()); retry = true; }
                else {
                    repair_notice_.reset();
                    if (!current_context_->rebuild) return DriverResult::failure(diagnostic("EAWR-NET-1015", "Authenticated rejoin factory is unavailable."));
                    auto fresh = recovery_factory_(retained_offer_.setup);
                    if (!fresh) return DriverResult::failure(fresh.error());
                    auto* returning = dynamic_cast<LiveAdapter*>(fresh.value().get());
                    if (!returning) return fail<bool>("Returning adapter has the wrong mode.");
                    auto policy = retained_options_;
                    policy.reconnect->rejoin_source = requested_status.peer;
                    policy.reconnect->rejoin_epoch = requested_status.epoch;
                    std::vector<net::PeerSeat> seats;
                    for (const auto& seat : retained_offer_.seats) seats.push_back({seat.seat, true});
                    // The package restores the executed prefix and source's ready_
                    // frontier. Another survivor may have sent a valid future batch
                    // that the source has not read yet: keep it until restoration,
                    // when ordinary receive validates it (or its retained digest).
                    // New catch-up traffic stays in pending; nothing clears it later.
                    while (!io_->transport.pending.empty()) {
                        auto frame = std::move(io_->transport.pending.front()); io_->transport.pending.pop_front();
                        bool keep{};
                        if (!frame.frame.empty() && frame.frame.front() == net::sealed_batch_kind) {
                            auto batch = net::decode_batch(frame.frame);
                            if (!batch) return DriverResult::failure(batch.error());
                            keep = batch.value().sender == frame.from && frame.from != io_->transport.self()
                                && frame.from < retained_offer_.seats.size() && batch.value().tick >= requested_status.barrier_tick
                                && batch.value().tick - requested_status.barrier_tick < core_session().input_horizon();
                        }
                        if (keep) retained_rejoin_frames_.push_back(std::move(frame));
                        else io_->transport.pending_bytes -= std::min(io_->transport.pending_bytes, frame.frame.size());
                    }
                    auto created = net::LockstepSession::create(*returning, io_->transport, std::move(seats), policy, &gate_);
                    if (!created) return DriverResult::failure(created.error());
                    rejoin_session_.emplace(std::move(created).value()); returning_adapter_ = std::move(fresh).value(); adapter_ = returning;
                    rejoin_session_->set_message_handler([](net::Received frame) {
                        if (!frame.frame.empty() && frame.frame[0] == net::setup_refuse_kind) {
                            auto refusal = net::decode_setup_refusal(frame.frame);
                            return core::Result<void>::failure(refusal ? refusal.value().diagnostic() : refusal.error());
                        }
                        return fail<void>("Unexpected recovery application frame.");
                    });
                    attach_stage_observer();
                    if (setup.departure_policy) core_session().enable_departures([this](sim::SeatCommandKey key) {
                        const auto* bound = static_cast<const LiveAdapter&>(core_session().active_adapter()).binding(key.seat);
                        if (!bound) return fail<Bytes>("Unbound departure seat.");
                        return net::encode_tactical_input(key, *bound, {{}, tactical::QuitPayload{}});
                    });
                    discard_recovery_commands();
                }
            }
            if (retry) { const std::lock_guard lock(mutex_); rejoin_requested_ = true; }
        }
        if (!adapter_->staging) adapter_->context = &context;
        sample_online_route();

        return std::nullopt;
}

} // namespace eawr::presentation::godot_backend::lan_detail
