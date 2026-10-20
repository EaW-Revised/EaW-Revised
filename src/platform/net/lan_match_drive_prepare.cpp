#include "lan_match_internal.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/platform/publish_files.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

std::optional<DriverResult> TcpLanMatch::drive_prepare(const tactical::TacticalSetup& setup, platform::LiveStepContext& context,
    std::span<const tactical::PlayerCommand> commands) {
        {
            const std::lock_guard lock(mutex_);
            if (abort_requested_) return DriverResult::success(false);
        }
        if (view().phase == LanPhase::ended) {
            auto pumped = pump_close();
            if (!pumped) return DriverResult::failure(pumped.error());
            return DriverResult::success(false);
        }
        if (!io_->transport.tcp) return fail<bool>("LAN transport is unavailable.");
        current_context_ = &context;
        gate_.advance_allowed = context.advance_allowed;
        queued_.insert(queued_.end(), commands.begin(), commands.end());
        if (adapter_ && !adapter_->staging) adapter_->context = &context;
        if (!io_->monitor) {
            if (!setup.match_identity) return fail<bool>("Local match identity is missing.");
            auto bytes = net::encode_skirmish_setup(setup);
            if (!bytes) { set_failure(bytes.error()); return DriverResult::failure(bytes.error()); }
            net::SetupOffer offer; offer.seed = setup.seed; offer.setup = bytes.value();
            offer.input_delay = view().host ? input_delay_ : offered_delay_.value_or(input_delay_);
            monitor_delay_ = offer.input_delay;
            offer.identity = {std::string(core::build_identity()), *setup.match_identity};
            offer.recovery = recovery_policy_;
            // Recovery requires the live source to recreate its own authoritative hooks.
            offer.recovery.resync = offer.recovery.resync && static_cast<bool>(context.rebuild);
            offer.recovery.reconnect = offer.recovery.reconnect && static_cast<bool>(context.rebuild);
            { const std::lock_guard lock(mutex_);
              view_.reconnect_enabled = offer.recovery.reconnect;
              view_.reconnect_unavailable = offer.recovery.reconnect ? "" : "Reconnect is disabled for this match.";
            }
            if (lobby_) for (const auto& seat : lobby_->snapshot().seats)
                offer.seats.push_back({static_cast<net::PeerIndex>(offer.seats.size()), seat.seat, seat.player});
            else for (net::PeerIndex peer = 0; peer < owners_.size(); ++peer)
                offer.seats.push_back({peer, sim::SeatId{peer + 1}, owners_[peer]});
            net::DesyncOptions options;
            if (online_options_) options.exchange_every = online_options_->hash_exchange_every;
            options.diagnostic_sink = [root = diagnostic_root_](const net::DiagnosticBundle& bundle) -> core::Result<std::string> {
                std::error_code error;
                std::filesystem::create_directories(root, error);
                if (error) return fail<std::string>("Cannot create the desync diagnostic folder: " + error.message());
                std::filesystem::path location;
                for (unsigned index = 0; index < 10000; ++index) {
                    location = root / (bundle.id + "-" + std::to_string(index));
                    if (std::filesystem::create_directory(location, error)) break;
                    if (error || index == 9999) return fail<std::string>("Cannot create a unique desync diagnostic folder: " + error.message());
                }
                std::vector<platform::PublishedFile> files;
                for (const auto& file : bundle.files) files.push_back({file.name, (location / file.name).string(), {file.bytes.begin(), file.bytes.end()}});
                const auto published = platform::publish_files(files);
                if (published.failure) {
                    std::string message = published.failure->message;
                    for (const auto& leftover : published.leftovers) message += "; " + leftover.message;
                    return fail<std::string>(std::move(message));
                }
                return core::Result<std::string>::success(std::filesystem::absolute(location).string());
            };
            diagnostic_sink_ = options.diagnostic_sink;
            options.input_delay = offer.input_delay;
            options.build_identity = offer.identity.build;
            options.match_setup = offer.setup;
            options.match_identity = core::sha256_hex(sim::canonical_match_identity(offer.identity.content));
            io_->monitor = std::make_unique<net::DesyncMonitor>(lobby_stream_ ? *lobby_stream_ : static_cast<net::MessageTransport&>(io_->transport), std::move(options));
            net::SetupProviders providers;
            providers.identify = [identity = offer.identity](const net::SetupOffer&) { return core::Result<sim::MatchAgreement>::success(identity); };
            providers.build = [this, setup, bytes = bytes.value()](const net::SetupOffer& agreed) -> core::Result<std::unique_ptr<net::SessionAdapter>> {
                if (agreed.input_delay != monitor_delay_) return fail<std::unique_ptr<net::SessionAdapter>>("Monitor delay differs from the agreed input delay.");
                if (agreed.setup != bytes) return fail<std::unique_ptr<net::SessionAdapter>>("The locally built match differs from the host setup.");
                auto adapter = std::make_unique<LiveAdapter>(setup, agreed.seats, trace_);
                adapter->options_applied = [this](net::MatchOptions options, std::uint64_t tick) {
                    const std::lock_guard lock(mutex_); view_.options = options; view_.options_tick = tick;
                };
                adapter_ = adapter.get(); adapter_->context = current_context_;
                return core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(adapter));
            };
            providers.configure_session = [this, setup](net::SessionOptions& policy, const net::SetupOffer& agreed) -> core::Result<void> {
                if (agreed.recovery.reconnect != (recovery_policy_.reconnect && static_cast<bool>(current_context_->rebuild)))
                    return core::Result<void>::failure(diagnostic("EAWR-NET-1025", "Local reconnect policy differs from the host offer."));
                if (!agreed.recovery.reconnect && !agreed.recovery.resync) return core::Result<void>::success();
                net::ReconnectOptions retention;
                retention.allow_rejoin = agreed.recovery.reconnect;
                retention.setup = agreed.setup;
                auto envelope = net::encode_setup_offer(agreed);
                if (!envelope) return core::Result<void>::failure(envelope.error());
                retention.match_identity = core::sha256(envelope.value());
                if (retention.allow_rejoin) {
                    if (!credentials_) return core::Result<void>::failure(diagnostic("EAWR-NET-1025", "Reconnect credential provider is unavailable."));
                    auto tokens = credentials_(agreed); if (!tokens) return core::Result<void>::failure(tokens.error());
                    retention.seat_tokens = std::move(tokens).value();
                }
                retention.grace_ms = agreed.recovery.grace_ms; retention.progress_timeout_ms = agreed.recovery.progress_timeout_ms;
                retention.hard_cap_ms = agreed.recovery.hard_cap_ms; retention.max_log_bytes = static_cast<std::size_t>(agreed.recovery.max_log_bytes);
                policy.reconnect = std::move(retention);
                if (agreed.recovery.resync || agreed.recovery.reconnect) {
                    net::ResyncOptions recovery; recovery.timeout_ms = agreed.recovery.resync_timeout_ms;
                    recovery.rebuild = [this, setup, seats = agreed.seats, expected = agreed.setup](std::span<const std::uint8_t> bytes)
                        -> core::Result<std::unique_ptr<net::SessionAdapter>> {
                        using Result = core::Result<std::unique_ptr<net::SessionAdapter>>;
                        if (!std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()) || !current_context_->rebuild)
                            return Result::failure(diagnostic("EAWR-NET-1214", "Verified recovery setup or local factory differs."));
                        auto rebuilt = current_context_->rebuild(); if (!rebuilt) return Result::failure(rebuilt.error());
                        auto fresh = std::make_unique<LiveAdapter>(setup, seats, nullptr);
                        fresh->staging = std::move(rebuilt).value(); fresh->context = fresh->staging.get();
                        fresh->activate_if_current = [this](LiveAdapter& value) { return activate_adapter(value); };
                        fresh->service_io = [this, staged = fresh.get()] {
                            { const std::lock_guard lock(mutex_);
                              view_.phase = core_session().resyncing() ? LanPhase::resyncing : LanPhase::reconnecting;
                              view_.recovery = core_session().resyncing() ? LanRecovery::resyncing
                                  : core_session().reconnect_status().phase == net::ReconnectPhase::proving ? LanRecovery::proving : LanRecovery::catching_up;
                              view_.catch_up_tick = staged->completed_tick(); }
                            diagnostics_->view(view());
                            return service_recovery_io();
                        };
                        fresh->options_applied = [this](net::MatchOptions options, std::uint64_t tick) {
                            const std::lock_guard lock(mutex_); view_.options = options; view_.options_tick = tick;
                        };
                        return Result::success(std::move(fresh));
                    };
                    recovery_factory_ = recovery.rebuild;
                    if (agreed.recovery.resync) policy.resync = std::move(recovery);
                }
                retained_options_ = policy; retained_offer_ = agreed;
                return core::Result<void>::success();
            };
            auto bound_monitor = gate_.bind(*io_->monitor);
            if (!bound_monitor) { set_failure(bound_monitor.error()); return DriverResult::failure(bound_monitor.error()); }
            providers.monitor = &gate_;
            providers.credentials_delivered = [this, enabled = offer.recovery.reconnect] {
                return !enabled || !default_credentials_ || credential_state_->delivered();
            };
            providers.tick_zero_agreed = [this] { return io_->monitor->setup_agreed(); };
            providers.tick_zero_mismatch = [this]() -> std::optional<net::TickZeroMismatch> {
                const auto& report = io_->monitor->report();
                return report && report->first_divergent_tick == 0 ? std::optional<net::TickZeroMismatch>{{report->local_hash, report->remote_hash, report->channel}} : std::nullopt;
            };
            net::SetupOptions policy; policy.timeout_ms = 180'000; policy.auto_accept = false;
            policy.session.keep_admission_tick_open = true;
            policy.session.pending_input_epoch = [this] { return pending_input_epoch(); };
            if (lobby_) {
                auto frozen_bytes = prepared_setup_ && prepared_setup_->match_identity ? net::encode_skirmish_setup(*prepared_setup_)
                    : lobby_fail<Bytes>(net::LobbyReason::setup_failed, "Local staging match identity is unavailable.");
                if (!frozen_bytes || frozen_bytes.value() != bytes.value())
                    return lobby_fail<bool>(net::LobbyReason::setup_failed, "Live setup differs from frozen staging setup.");
                if (adapter_) adapter_->bind_live_context(context, trace_);
                if (!view().host) {
                    deferred_providers_ = std::move(providers);
                }
            }
            else if (setup_session_) deferred_providers_ = std::move(providers);
            else {
                auto created = view().host ? net::SetupSession::host(io_->transport, std::move(offer), std::move(providers), policy)
                                          : net::SetupSession::join(io_->transport, std::move(providers), policy);
                if (!created) { set_failure(created.error()); return DriverResult::failure(created.error()); }
                setup_session_.emplace(std::move(created).value());
            }
        }

        return std::nullopt;
}

} // namespace eawr::presentation::godot_backend::lan_detail
