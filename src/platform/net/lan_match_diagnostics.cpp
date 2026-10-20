#include "lan_match_internal.hpp"
#include <iostream>
#include <sstream>

namespace eawr::presentation::godot_backend::lan_detail {

bool TcpLanMatch::save_diagnostic_details()  {
        net::DiagnosticBundle bundle;
        net::DiagnosticSink sink;
        {
            const std::lock_guard lock(mutex_);
            if (!diagnostic_bundle_ || !diagnostic_sink_) return false;
            if (!view_.bundle_path.empty() && diagnostic_saved_) {
                view_.diagnostic_save_notice = (view_.diagnostic_saved_automatically ? "Diagnostic details were already saved automatically: " : "Diagnostic details are already saved locally: ") + view_.bundle_path;
                return true;
            }
            bundle = *diagnostic_bundle_; sink = diagnostic_sink_;
        }
        auto saved = sink(bundle);
        const std::lock_guard lock(mutex_);
        if (!diagnostic_bundle_ || diagnostic_bundle_->id != bundle.id) return static_cast<bool>(saved);
        diagnostic_saved_ = static_cast<bool>(saved); view_.diagnostic_saved = diagnostic_saved_; view_.diagnostic_saved_automatically = false;
        if (saved) {
            view_.bundle_path = saved.value();
            view_.diagnostic_save_notice = "Diagnostic details saved locally: " + saved.value();
        } else view_.diagnostic_save_notice = "Diagnostic details could not be saved locally.";
        return static_cast<bool>(saved);
    }

void TcpLanMatch::acknowledge_recovery()  {
        const std::lock_guard lock(mutex_);
        if (view_.recovery == LanRecovery::recovered) {
            view_.recovery = LanRecovery::none; view_.diagnostic_save_notice.clear(); view_.diagnostic_code.clear(); view_.input_feedback.clear();
        }
    }

void TcpLanMatch::acknowledge_input_feedback()  { const std::lock_guard lock(mutex_); view_.input_feedback.clear(); }

void TcpLanMatch::set_discard_observer(std::function<void(std::uint64_t)> observer)  {
        const std::lock_guard lock(input_epoch_mutex_); discard_observer_ = std::move(observer);
    }

void TcpLanMatch::discard_command(const tactical::PlayerCommand& command) {
        std::function<void(std::uint64_t)> observer;
        { const std::lock_guard lock(input_epoch_mutex_); observer = discard_observer_; }
        if (observer) observer(command.key.sequence);
        const std::lock_guard lock(mutex_);
        view_.input_feedback = "Pending gameplay input was cancelled because synchronization changed. Issue it again after play resumes.";
    }

void TcpLanMatch::discard_recovery_commands() {
        for (const auto& command : queued_) discard_command(command);
        queued_.clear();
    }

void TcpLanMatch::capture_diagnostic(const net::DesyncReport& report) {
        if (diagnostic_bundle_ && diagnostic_bundle_->id == report.bundle.id) return;
        diagnostics_->emit("desync", "hash-mismatch", diagnostics_->seat,
            "divergent_tick=" + std::to_string(report.first_divergent_tick));
        view_.desync_tick = report.first_divergent_tick;
        view_.bundle_path = report.bundle_location;
        diagnostic_bundle_ = report.bundle;
        diagnostic_saved_ = !report.bundle_location.empty() && !report.bundle_error;
        view_.diagnostic_available = !report.bundle.files.empty(); view_.diagnostic_saved = diagnostic_saved_; view_.diagnostic_saved_automatically = diagnostic_saved_;
        if (report.bundle_error) view_.diagnostic_save_notice = "Diagnostic details could not be saved locally.";
    }

void TcpLanMatch::update_desync() {
        // Protocol failures freeze without a divergence report. Keep their typed
        // diagnostic when cleanup polls again instead of relabelling them desync.
        if (!io_->monitor || !io_->monitor->frozen() || !io_->monitor->report()) return;
        const std::lock_guard lock(mutex_);
        view_.phase = LanPhase::ended; view_.reason = LanReason::desync; view_.recovery = LanRecovery::failed;
        if (const auto& report = io_->monitor->report()) {
            capture_diagnostic(*report);
        }
    }

core::Result<void> TcpLanMatch::pump_close() {
        auto pumped = io_->pump();
        if (!pumped) {
            set_failure(pumped.error(), false);
            io_->transport.tcp.reset();
            const std::lock_guard lock(mutex_);
            view_.message = pumped.error().code + ": " + pumped.error().message;
        } else update_desync();
        if (!io_->transport.tcp) {
            const std::lock_guard lock(mutex_); view_.connected = false;
        }
        return pumped;
    }

void TcpLanMatch::progress(const platform::LiveStepContext& context, std::uint64_t now) {
        if (!smoke_progress_ || !io_->transport.tcp || (last_progress_ && now - *last_progress_ < 5000)) return;
        last_progress_ = now;
        std::cerr << "EAWR LAN progress peer=" << io_->transport.self() << " tick=" << context.completed_tick()
            << " advance=" << context.advance_allowed << " setup=" << (setup_session_ ? static_cast<int>(setup_session_->phase()) : -1)
            << " runtime=" << runtime_.has_value() << " may_step=" << (io_->monitor && io_->monitor->may_step())
            << " frozen=" << (io_->monitor && io_->monitor->frozen()) << " zero_agreed=" << (io_->monitor && io_->monitor->setup_agreed())
            << " waiting=" << (io_->monitor ? io_->monitor->waiting_for_peers().size() : 0)
            << " link=" << static_cast<int>(io_->transport.link(io_->transport.self() == 0 ? 1U : 0U))
            << " sent=" << io_->transport.sent << " received=" << io_->transport.received
            << " batches_sent=" << io_->transport.batches_sent << " batches_received=" << io_->transport.batches_received << std::endl;
    }

void TcpLanMatch::input_progress(std::string_view reason) {
        if (!io_ || (!runtime_ && !lobby_result_) || !io_->transport.tcp) return;
        const auto& session = core_session();
        const auto self = lobby_stream_ ? lobby_stream_->self() : io_->transport.self();
        for (net::PeerIndex peer = 0; peer < setup_result().offer().seats.size(); ++peer) {
            if (peer == self) continue;
            const auto native = lobby_result_ && last_lobby_state_ && peer < last_lobby_state_->snapshot.seats.size()
                ? last_lobby_state_->snapshot.seats[peer].peer : peer;
            const auto queue = io_->transport.tcp->queue_progress(native);
            const auto signals = io_->transport.tcp->signal_counts(native);
            const auto sent = session.last_sent_batch(peer);
            const auto sent_tick = sent ? std::to_string(*sent) : "none";
            // Only an empty native reliable lane and application queue prove
            // every successful send acknowledged. Otherwise the ACK tick is unknown.
            std::ostringstream detail;
            detail << "input_peer=" << peer << " next_seal=" << session.next_seal() << " last_sent_batch=" << sent_tick
                << " acked_batch=" << (queue.reliable_drained ? sent_tick : "unknown")
                << " received_through=" << session.received_through(peer)
                << " queue_frames=" << queue.outbound_frames << " queue_bytes=" << queue.outbound_bytes
                << " inbound_frames=" << queue.inbound_frames << " inbound_bytes=" << queue.inbound_bytes
                << " native_valid=" << queue.native_valid << " native_state=" << queue.native_state
                << " native_ping_ms=" << queue.ping_ms << " pending_reliable=" << queue.pending_reliable
                << " unacked_reliable=" << queue.unacked_reliable << " pending_unreliable=" << queue.pending_unreliable
                << " deferred_signals=" << queue.deferred_signals
                << " signals_sent_blobs=" << signals.sent_blobs << " signals_received_blobs=" << signals.received_blobs;
            diagnostics_->emit("input-progress", reason, native, detail.str());
        }
    }

void TcpLanMatch::set_failure(const core::Diagnostic& diagnostic, bool terminal) {
        if (last_logged_failure_ != diagnostic.code) {
            if (diagnostic.code == net::session_errors::stalled) input_progress(diagnostic.code);
            diagnostics_->emit("failure", diagnostic.code, diagnostics_->seat);
            last_logged_failure_ = diagnostic.code;
        }
        const bool lost = lan_connection_loss(diagnostic.code);
        auto lost_name = view().peer_name;
        if (lost && (runtime_ || lobby_result_))
            if (const auto peer = core_session().connection_lost_peer()) lost_name = seat_name(*peer);
        LanReason reason = LanReason::refused;
        if (diagnostic.code == "EAWR-NET-1762") reason = LanReason::refused; // The named player may contain words such as "build" or "version".
        else if (diagnostic.code == net::lobby_diagnostic(net::LobbyReason::not_ready, "").code) reason = LanReason::cancelled;
        else if (diagnostic.code == "EAWR-NET-0202" || diagnostic.message.find("build") != std::string::npos) reason = LanReason::build;
        else if (diagnostic.code == "EAWR-NET-0201" || diagnostic.message.find("version") != std::string::npos) reason = LanReason::version;
        else if (diagnostic.code == "EAWR-NET-0302" || diagnostic.message.find("identity") != std::string::npos) reason = LanReason::identity;
        else if (diagnostic.code == "EAWR-NET-0207" || (diagnostic.code == "EAWR-NET-0209" && diagnostic.message.find("deadline") != std::string::npos) || diagnostic.code == "EAWR-NET-0306" || diagnostic.message.find("timeout") != std::string::npos || diagnostic.message.find("expired") != std::string::npos) reason = LanReason::timeout;
        else if ((diagnostic.code == "EAWR-NET-0206" && view().connected) || diagnostic.code == "EAWR-NET-0305" || diagnostic.message.find("closed") != std::string::npos || diagnostic.message.find("disconnect") != std::string::npos) reason = LanReason::peer_left;
        {
            const std::lock_guard lock(mutex_);
            view_.phase = LanPhase::ended; view_.reason = reason; view_.message.clear();
            view_.diagnostic_code = diagnostic.code.starts_with("EAWR-NET-1") ? diagnostic.code : std::string{}; view_.start_enabled = view_.accept_enabled = false;
            if (io_->monitor && io_->monitor->frozen() && io_->monitor->report()) {
                const auto& report = *io_->monitor->report();
                view_.reason = LanReason::desync; capture_diagnostic(report);
                view_.bundle_path = report.bundle_location;
            }
            if (view_.reason == LanReason::desync || view_.recovery == LanRecovery::resyncing)
                view_.recovery = LanRecovery::failed;
            else if (view_.recovery == LanRecovery::held || view_.recovery == LanRecovery::catching_up
                || view_.recovery == LanRecovery::proving || diagnostic.code.starts_with("EAWR-NET-10"))
                view_.recovery = LanRecovery::rejoin_failed;
            else view_.recovery = LanRecovery::none;
            view_.rejoin_enabled = false;
            if (lost) {
                view_.reason = LanReason::timeout; view_.victory.reset(); view_.recovery = LanRecovery::none;
                view_.diagnostic_code = diagnostic.code; view_.peer_name = lost_name;
                view_.message = "Connection lost: the match could not continue.\n" + lost_name + "\n" + diagnostic.code;
            }
            else if (online_options_) {
                view_.diagnostic_code = diagnostic.code;
                if (reason == LanReason::build && diagnostic.code != "EAWR-NET-1530") view_.message = "All players need the same game build.";
                else if (diagnostic.code == "EAWR-NET-1810") view_.message = "Choose between two and nine human seats.";
                else if (diagnostic.code == "EAWR-NET-1300") view_.message = "Choose two to nine humans sharing up to three owners.";
                else if (diagnostic.code == "EAWR-NET-1520") view_.message = "The online service did not accept your access credentials.";
                else if (diagnostic.code == "EAWR-NET-1522" || diagnostic.code == "EAWR-NET-1523" || diagnostic.code == "EAWR-NET-1524") view_.message = "This invitation is no longer available. Ask the host for a new one.";
                else if (diagnostic.code == "EAWR-NET-1532" || diagnostic.code == "EAWR-NET-1525" || diagnostic.code == "EAWR-NET-1527") view_.message = "The online service is busy. Try again shortly.";
                else if (diagnostic.code == "EAWR-NET-1534") view_.message = "The previous invitation is still closing. Wait for it to expire before trying again.";
                else if (diagnostic.code == "EAWR-NET-1530") { view_.reason = LanReason::refused; view_.message = "Online play is unavailable in this build or configuration."; }
                else if (diagnostic.code == "EAWR-NET-1535" || diagnostic.code == "EAWR-NET-1536" || diagnostic.code == "EAWR-NET-1760"
                    || diagnostic.code == "EAWR-NET-1762" || diagnostic.code == "EAWR-NET-1764"
                    || diagnostic.code == online_match_errors::no_room_code) view_.message = diagnostic.message;
                else if (diagnostic.code == net::mesh_errors::left && !mesh_left_text_.empty()) view_.message = mesh_left_text_;
                else if (const auto text = online_mesh_failure_text(diagnostic.code); !text.empty()) view_.message = text;
                else if (diagnostic.code == "EAWR-NET-1730") view_.message = "The online service session reached its maximum length.";
                else if (diagnostic.code == "EAWR-NET-1731") view_.message = "The online service session was not renewed in time.";
                else if (const auto endpoint = online_endpoint_.lock(); endpoint && online_relay_allocation_ended(reason, endpoint->turn_credentials(), online_paired_at_, std::chrono::steady_clock::now())) {
                    view_.message = "The connection ended after the online service credentials expired.";
                    view_.diagnostic_code = "EAWR-NET-1737";
                }
                else view_.message = "Could not connect to this game. Check the online service settings and try again.";
                view_.message += "\n" + view_.diagnostic_code;
            }
            else if (diagnostic.code.starts_with("EAWR-NET-06") || diagnostic.code.starts_with("EAWR-NET-16") || diagnostic.code.starts_with("EAWR-NET-102")) view_.message = diagnostic.message;
            else if (diagnostic.code >= "EAWR-NET-0208" && diagnostic.code <= "EAWR-NET-0212" && diagnostic.code != "EAWR-NET-0209")
                view_.message = diagnostic.code + ": " + diagnostic.message;
            else if (diagnostic.code >= "EAWR-NET-0200" && diagnostic.code <= "EAWR-NET-0209")
                view_.message.clear(); // Let lan_message retain the curated legacy reason, including host retries.
        }
        diagnostics_->view(view());
        // A failed driver will no longer be polled by LiveSession.
        if (terminal) {
            if (lobby_ && !lobby_result_) { io_->monitor.reset(); gate_.reset(); lobby_stream_ = nullptr; }
            erase_credentials();
            pump_close();
            finish_io(std::exchange(io_, std::make_unique<TerminalIo>()));
            lobby_.reset(); lobby_result_.reset();
            if (!handed_over_) last_lobby_state_.reset(); // Frozen staging report is immutable while the driver owns I/O.
            const std::lock_guard lock(mutex_); view_.connected = false;
        }
    }

} // namespace eawr::presentation::godot_backend::lan_detail
