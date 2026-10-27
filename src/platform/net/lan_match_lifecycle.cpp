#include "lan_match_internal.hpp"
#include "eawr/core/build_identity.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

void TcpLanMatch::set_diagnostic_logger(std::function<void(std::string_view)> logger)  {
        if (handed_over_) return;
        // GNS/TURN lines (#4021) go to the same log. The process-wide sink owns its own copy of the logger (never this
        // match's diagnostics, which cancel() rewrites); the transport hands the lines over on the I/O owner's poll.
        // A match that was replaced or finished clears only its own sink: the generation says whose it is.
        release_gns_log();
        diagnostics_->sink = std::move(logger);
        install_gns_log();
        io_->transport.diagnostics = diagnostics_;
    }

void TcpLanMatch::set_recovery_policy(net::SetupRecoveryPolicy policy, LanCredentialProvider credentials)  {
        if (handed_over_) return;
        recovery_policy_ = policy; credentials_ = std::move(credentials);
        default_credentials_ = false;
        if (!credentials_) recovery_policy_.reconnect = false;
        { const std::lock_guard lock(mutex_);
          view_.credential_provider_available = static_cast<bool>(credentials_);
          view_.reconnect_enabled = recovery_policy_.reconnect;
          view_.reconnect_unavailable = recovery_policy_.reconnect ? "" : "Reconnect is disabled for this match.";
        }
    }

bool TcpLanMatch::rejoin()  {
        const std::lock_guard lock(mutex_);
        if (!view_.rejoin_enabled || rejoin_requested_) return false;
        rejoin_requested_ = true; return true;
    }

bool TcpLanMatch::request_pause(bool paused) {
    const std::lock_guard lock(mutex_);
    if (!lan_gameplay_enabled(view_) || pause_requests_.size() >= net::max_batch_commands) return false;
    pause_requests_.push_back(paused); view_.input_feedback = "Waiting for the shared pause control.";
    return true;
}

bool TcpLanMatch::request_speed(std::uint32_t step)  {
        const std::lock_guard lock(mutex_);
        if (!lan_gameplay_enabled(view_) || step >= presentation::ui::speed_step_rates.size()) return false;
        if (!speed_window_.accept(view_.session_tick)) return false;
        auto options = view_.options;
        options.tactical_speed = step;
        speed_requests_.push_back(options); view_.input_feedback.clear();
        return true;
    }

TcpLanMatch::TcpLanMatch(std::filesystem::path root, std::uint64_t delay, bool smoke_progress, std::filesystem::path trace)
        : diagnostic_root_(std::move(root)), input_delay_(delay), smoke_progress_(smoke_progress),
          trace_(trace.empty() ? nullptr : std::make_shared<LanTrace>(trace)) {
        io_->transport.count_progress = smoke_progress;
        if (diagnostic_root_.empty()) diagnostic_root_ = std::filesystem::path("out") / "lan-desync";
        net::SetupRecoveryPolicy policy; policy.reconnect = policy.resync = true;
        set_recovery_policy(policy, [this](const net::SetupOffer& offer) {
            return credential_state_->provide(offer, io_->transport);
        });
        default_credentials_ = true;
    }

void TcpLanMatch::configure_factions(std::vector<std::string> factions)  {
        if (!handed_over_) factions_ = std::move(factions);
    }

void TcpLanMatch::configure_seats(std::vector<tactical::PlayerId> owners)  {
        if (handed_over_) return;
        const std::set<tactical::PlayerId> sides(owners.begin(), owners.end());
        roster_error_.reset();
        if (owners.size() < 2 || owners.size() > net::max_match_seats || sides.contains(0) || sides.size() != owners.size()) {
            roster_error_ = diagnostic("EAWR-NET-1300", "Choose two to nine humans with a separate owner for each player.");
            return;
        }
        owners_ = std::move(owners);
    }

TcpLanMatch::~TcpLanMatch()  {
        release_gns_log();
        if (online_admission_) online_admission_->cancelled = true;
        retire_room(std::exchange(online_pending_, {}));
        erase_credentials();
        if (mapping_) drain_workers().start(std::move(mapping_));
    }

void TcpLanMatch::set_port_mapping(bool enabled)  { map_port_ = enabled; }

void TcpLanMatch::host(std::uint16_t port)  {
        if (online_options_) { begin_online(true, {}); return; }
        cancel();
        io_->transport.credentials = credentials_ ? credential_state_ : nullptr;
        if (roster_error_) { set_failure(*roster_error_); return; }
        net::TcpOptions options; options.build_identity = core::build_identity();
        options.peers = static_cast<std::uint32_t>(owners_.size());
        if (lobby_builder_) {
            options.max_frame_size = static_cast<std::uint32_t>(net::max_lobby_frame_bytes);
            options.lobby_bootstrap = true;
            options.staging_capacity = capacity_staging_;
        }
        // Loading is synchronous in Godot. Preserve the stream through a bounded load.
        options.timeout = std::chrono::milliseconds(180'000);
        auto opened = net::TcpTransport::host("0.0.0.0", port, options);
        if (!opened) { set_failure(opened.error()); return; }
        io_->transport.tcp = std::make_unique<MatchTransport>(std::move(opened).value());
        if (map_port_) mapping_ = std::make_unique<net::PortMapping>(io_->transport.tcp->port());
        const std::lock_guard lock(mutex_);
        view_ = {}; view_.host = true; view_.port = io_->transport.tcp->port(); view_.phase = LanPhase::connecting;
        view_.human_owners = owners_; view_.player = owners_.front();
        view_.reconnect_enabled = recovery_policy_.reconnect && static_cast<bool>(credentials_);
        view_.credential_provider_available = static_cast<bool>(credentials_);
        if (view_.reconnect_enabled) view_.reconnect_unavailable.clear();
    }

void TcpLanMatch::join_admitted(net::OnlineRoomAdmission admission)  {
        if (online_options_) { begin_online(false, {}, std::move(admission)); return; }
        join(std::move(admission.room));
    }

void TcpLanMatch::join(std::string endpoint)  {
        if (online_options_) { begin_online(false, std::move(endpoint)); return; }
        cancel();
        io_->transport.credentials = credentials_ ? credential_state_ : nullptr;
        if (roster_error_) { set_failure(*roster_error_); return; }
        auto parsed = net::parse_join_endpoint(endpoint);
        if (!parsed) { set_failure(parsed.error()); return; }
        const auto port = parsed.value().port;
        const auto remote_endpoint = parsed.value().host + ":" + std::to_string(port);
        endpoint = std::move(parsed).value().host;
        net::TcpOptions options; options.build_identity = core::build_identity(); options.timeout = std::chrono::milliseconds(180'000);
        options.peers = static_cast<std::uint32_t>(owners_.size());
        if (lobby_builder_) {
            options.max_frame_size = static_cast<std::uint32_t>(net::max_lobby_frame_bytes);
            options.lobby_bootstrap = true;
            // The verified host hello selects its mode and capacity, independent
            // of the browser's current humans + Open vacancy count.
            options.staging_capacity = true;
        }
        auto opened = net::TcpTransport::connect(std::move(endpoint), port, options);
        if (!opened) { set_failure(opened.error()); return; }
        io_->transport.tcp = std::make_unique<MatchTransport>(std::move(opened).value());
        const std::lock_guard lock(mutex_);
        view_ = {}; view_.read_only = true; view_.port = port; view_.phase = LanPhase::connecting;
        view_.human_owners = owners_; view_.player = 2;
        view_.remote_endpoint = remote_endpoint;
        view_.reconnect_enabled = recovery_policy_.reconnect && static_cast<bool>(credentials_);
        view_.credential_provider_available = static_cast<bool>(credentials_);
        if (view_.reconnect_enabled) view_.reconnect_unavailable.clear();
    }

void TcpLanMatch::log_gns_sink(std::string_view action, std::uint64_t generation) {
        if (!diagnostics_->sink) return;
        try {
            diagnostics_->sink("EAWR net event=gns sink " + std::string(action) + " gen=" + std::to_string(generation)
                + " match=" + (online_options_ ? "online" : "lan"));
        } catch (...) {} // A diagnostic consumer cannot end the match.
    }

void TcpLanMatch::install_gns_log() {
        if (!diagnostics_->sink) return;
        gns_log_generation_ = net::GnsTransport::set_log_sink(diagnostics_->sink);
        log_gns_sink("install", gns_log_generation_);
    }

void TcpLanMatch::release_gns_log() {
        if (const auto generation = std::exchange(gns_log_generation_, std::uint64_t{})) {
            net::GnsTransport::clear_log_sink(generation);
            log_gns_sink("clear", generation);
        }
    }

void TcpLanMatch::cancel()  {
        release_gns_log();
        diagnostics_->view(view());
        if (view().phase != LanPhase::idle && view().phase != LanPhase::ended)
            diagnostics_->emit("quit-local", "cancelled", diagnostics_->seat);
        auto logger = std::move(diagnostics_->sink);
        *diagnostics_ = LanDiagnostics{}; diagnostics_->sink = std::move(logger);
        io_->transport.diagnostics = diagnostics_;
        last_logged_failure_.clear(); last_lease_error_.clear(); last_renewal_failure_ = 0;
        online_snapshot_deadline_.reset();
        online_endpoint_.reset();
        retire_room(std::exchange(online_pending_, {}));
        if (online_admission_) {
            online_admission_->cancelled = true;
            { const std::lock_guard lock(online_admission_->mutex);
              if (auto room = std::move(online_admission_->room)) {
                  online_admission_->close_confirmed = true;
                  retire_room(std::move(room));
              }
            }
            online_retiring_ = std::move(online_admission_);
        }
        erase_credentials();
        if (mapping_) drain_workers().start(std::move(mapping_));
        // The viewer stops and joins LiveSession before returning to this screen.
        reset_lobby();
        rejoin_session_.reset(); returning_adapter_.reset(); setup_session_.reset(); runtime_.reset(); io_->monitor.reset(); io_->application_stream = nullptr; io_->room_lifetime.reset(); adapter_ = nullptr;
        for (auto& frame : io_->transport.pending) net::erase_reconnect_secret(frame.frame);
        for (auto& frame : early_) net::erase_reconnect_secret(frame.frame);
        for (auto& frame : retained_rejoin_frames_) net::erase_reconnect_secret(frame.frame);
        io_->transport.pending.clear(); io_->transport.pending_bytes = 0; io_->transport.tcp.reset(); io_->deadline.reset(); io_->closing = false; handed_over_ = accepted_ = false; deferred_providers_ = {};
        io_->transport.credential_error.reset();
        offered_.reset(); pending_.reset(); offered_delay_.reset(); waiting_since_.reset(); queued_.clear();
        early_.clear(); early_bytes_ = 0;
        retained_options_ = {}; retained_offer_ = {}; recovery_factory_ = {};
        diagnostic_bundle_.reset(); diagnostic_sink_ = {}; diagnostic_saved_ = false;
        last_repair_ms_.reset(); repair_notice_.reset(); retained_rejoin_frames_.clear();
        const std::lock_guard lock(mutex_); rejoin_requested_ = false; speed_requests_.clear(); pause_requests_.clear(); speed_window_ = {}; abort_requested_ = false; view_ = {}; view_.reason = LanReason::cancelled;
    }

bool TcpLanMatch::request_abort()  {
        const std::lock_guard lock(mutex_);
        if (!lan_can_abort(view_)) return false;
        abort_requested_ = true;
        return true;
    }

void TcpLanMatch::complete_abort()  {
        auto terminal = view();
        if (terminal.phase == LanPhase::ended) {
            pump_close();
            terminal = view();
            finish_io(std::exchange(io_, std::make_unique<TerminalIo>()));
        }
        cancel();
        if (terminal.phase == LanPhase::ended) {
            const std::lock_guard lock(mutex_);
            view_ = terminal; view_.connected = false;
        }
    }

} // namespace eawr::presentation::godot_backend::lan_detail
