#include "galactic_lan_match.hpp"
#include "eawr/platform/live_game_lockstep.hpp"
#include "eawr/checkpoint/game.hpp"

#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/net/lobby_session.hpp"
#include "galactic_acceptance.hpp"

#include <algorithm>
#include <atomic>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <deque>
#include <limits>

namespace eawr::presentation::godot_backend {
namespace {
namespace galactic = sim::galactic;
using Tick = core::Result<std::optional<campaign::GameTick>>;

core::Diagnostic make_diagnostic(std::string_view code, std::string message) {
    core::Diagnostic error;
    error.code = code;
    error.message = std::move(message);
    return error;
}

std::optional<net::RecoveryDigest> recovery_digest(std::string_view text) {
    if (text.size() != 64) return {};
    net::RecoveryDigest digest{};
    const auto nibble = [](char value) -> std::optional<unsigned> {
        if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a') + 10U;
        return {};
    };
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto high = nibble(text[index * 2]), low = nibble(text[index * 2 + 1]);
        if (!high || !low) return {};
        digest[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
    }
    return digest;
}

// Publish before the view lock is released, including early returns. Only pause notice inputs invalidate the cache;
// ordinary ticks, waiting notices and speed events do not. A reader seeing the revision can then take its snapshot.
class PauseNoticePublication final {
public:
    PauseNoticePublication(const GalacticLanView& view, std::atomic<std::uint64_t>& revision) noexcept
        : view_(view), revision_(revision), paused_(view.clock_paused && galactic_lan_gameplay_enabled(view)),
          actor_(paused_ ? view.clock_actor : sim::SeatId{}) {}
    ~PauseNoticePublication() {
        const bool paused = view_.clock_paused && galactic_lan_gameplay_enabled(view_);
        const auto actor = paused ? view_.clock_actor : sim::SeatId{};
        if (paused_ != paused || actor_ != actor) revision_.fetch_add(1, std::memory_order_release);
    }
private:
    const GalacticLanView& view_;
    std::atomic<std::uint64_t>& revision_;
    bool paused_;
    sim::SeatId actor_;
};

// The core advances only while the cadence allows a tick, but it keeps servicing the transport (as the tactical
// LanMatch gate does): a held presentation never stops receive, seal or send.
class CadenceGate final : public net::SessionMonitor {
public:
    net::DesyncMonitor* monitor{};
    bool advance_allowed{};
    bool needs_encoding(std::uint64_t tick, bool end = false) const override { return monitor->needs_encoding(tick, end); }
    core::Result<void> record(std::uint64_t tick, std::string_view hash, std::span<const std::uint8_t> state,
        std::span<const std::uint8_t> input, bool end = false) override { return monitor->record(tick, hash, state, input, end); }
    core::Result<void> receive(const net::Received& frame) override { return monitor->receive(frame); }
    core::Result<void> on_input_delay(std::uint64_t delay, std::uint64_t retained) override {
        return monitor->on_input_delay(delay, retained);
    }
    core::Result<void> on_departure(net::PeerIndex peer, std::uint64_t tick) override { return monitor->on_departure(peer, tick); }
    std::vector<net::PeerIndex> waiting_for_peers() const override { return monitor->waiting_for_peers(); }
    bool frozen() const override { return monitor->frozen(); }
    bool ready_for_resync() const override { return monitor->ready_for_resync(); }
    core::Result<void> on_resync(std::uint64_t tick, std::string_view hash) override { return monitor->on_resync(tick, hash); }
    core::Result<void> on_link_repair(net::PeerIndex peer, std::function<bool(net::PeerIndex)> ownership = {}) override {
        return monitor->on_link_repair(peer, std::move(ownership));
    }
    bool may_step() const override { return advance_allowed && monitor->may_step(); }
    core::Result<void> on_reconnect(std::uint64_t tick, std::string_view hash, std::span<const net::PeerIndex> inactive) override {
        return monitor->on_reconnect(tick, hash, inactive);
    }
};
} // namespace

void prepare_galactic_recovery_base(GalacticLanOptions& options, campaign::GameSession& game,
    net::GalacticSessionAdapter* adapter, std::uint64_t initial_tick) {
    if (options.initial_checkpoint) return;
    options.initial_recovery_refusal = "checkpoint_unavailable";
    if (!adapter || &adapter->game() != &game || initial_tick != 0 || game.completed_tick() != initial_tick) return;
    if (!game.retain_network_resume_state(adapter->checkpoint_network_state())) return;
    auto captured = checkpoint::save_game_checkpoint(game, checkpoint::game_identities(game.galaxy().setup()));
    if (!captured) return;
    options.initial_checkpoint = std::make_shared<const std::vector<std::uint8_t>>(std::move(captured).value());
    options.initial_recovery_refusal.clear();
}

class GalacticLanMatch::State {
public:
    State(net::MessageTransport& transport, GalacticLanOptions options)
        : transport_(transport), options_(std::move(options)) {}

    State(net::MessageTransport&, GalacticLanOptions options, std::shared_ptr<net::LobbyResult> result,
        net::DesyncMonitor& monitor, std::function<void(bool)> cadence, std::shared_ptr<void> lifetime)
        : transport_(result->transport()), options_(std::move(options)), lifetime_(std::move(lifetime)),
          admission_(std::move(result)), external_cadence_(std::move(cadence)) {
        monitor_ = &monitor;
        adapter_ = dynamic_cast<net::GalacticSessionAdapter*>(&admission_->setup().adapter());
        session_ = &admission_->setup().session();
        session_->enable_departures([](sim::SeatCommandKey key) {
            return net::encode_galactic_input(key, galactic::SeatLeave{});
        });
        if (!adapter_) failed_ = make_diagnostic(net::session_errors::options, "The campaign lobby adapter is not galactic.");
    }

    ~State() {
        if (recovery_publication_) recovery_publication_->result.set_value(net::GalacticRecoveryRefusal::unavailable);
        if (recovery_begin_) recovery_begin_->result.set_value(net::GalacticRecoveryRefusal::unavailable);
        if (recovery_incident_ && recovery_incident_->result) recovery_incident_->result->set_value(net::GalacticRecoveryRefusal::unavailable);
        for (auto& query : recovery_queries_) query.result.set_value(core::Result<net::GalacticRecoveryPackage>::failure(
            make_diagnostic(net::reconnect_errors::unavailable, "checkpoint_unavailable")));
    }

    void configure_recovery_restore(net::GalacticRecoveryWorker::Restore restore) { options_.recovery_restore = std::move(restore); }

    [[nodiscard]] sim::SeatId local_seat() const {
        return transport_.self() < options_.seats.size() ? options_.seats[transport_.self()].seat : sim::SeatId{};
    }
    [[nodiscard]] GalacticLanView view() const {
        const std::lock_guard lock(mutex_);
        return view_;
    }
    [[nodiscard]] std::uint64_t clock_events() const noexcept { return clock_events_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t pause_notice_revision() const noexcept { return pause_notice_revision_.load(std::memory_order_acquire); }

    Tick drive(platform::LiveGameStepContext& context, std::span<const platform::LiveGameOrder> orders, std::uint64_t now) {
        if (session_ended_) {
            auto error = make_diagnostic("EAWR-PLATFORM-0600", "A galactic LAN driver cannot be reused after its live session ended.");
            for (const auto& order : orders) { (void)order; context.reject(error); }
            return Tick::failure(std::move(error));
        }
        service_recovery_queries();
        if (!session_) {
            if (auto opened = open(context); !opened) return end_with_failure(opened.error());
        }
        attach_recovery_receiver(); recovery_now_ = now;
        const bool private_candidate = recovery_incident_ && (transport_.self() == recovery_incident_->target
            || recovery_incident_->local_published || recovery_incident_->commit_seen);
        const bool published = recovery_incident_ && recovery_incident_->local_published;
        if (!session_->pump_galactic_recovery(private_candidate, published)) {
            quarantine_live_recovery(net::GalacticRecoveryRefusal::limit);
            return Tick::success(std::nullopt);
        }
        service_live_recovery(context, now);
        if (service_recovery_publication(context)) return Tick::success(std::nullopt);
        if (recovery_incident_ && (transport_.self() == recovery_incident_->target
            || recovery_incident_->local_published || recovery_incident_->commit_seen)) {
            for (const auto& order : orders) { (void)order; context.reject(make_diagnostic(net::reconnect_errors::unavailable, "recovery_publication_pending")); }
            return Tick::success(std::nullopt);
        }
        if (failed_) {
            if (!recovery_listening_) return Tick::failure(*failed_);
            if (options_.repair_recovery_links && !recovery_quarantined_) static_cast<void>(options_.repair_recovery_links());
            return Tick::success(std::nullopt);
        }
        {
            const std::lock_guard lock(mutex_);
            if (view_.phase == GalacticLanPhase::ended) {
                for (const auto& order : orders) {
                    (void)order;
                    context.reject(make_diagnostic("EAWR-PLATFORM-0600",
                        "A galactic LAN match that ended cannot accept orders."));
                }
                if (view_.reason == GalacticLanReason::desync && !options_.recovery_restore)
                    return Tick::failure(make_diagnostic("EAWR-PLATFORM-0600", view_.message));
                if (view_.reason != GalacticLanReason::desync) return Tick::success(std::nullopt);
            }
        }
        if (&adapter_->game() != &context.game)
            return end_with_failure(make_diagnostic(net::session_errors::options,
                "The live campaign differs from the agreed lobby owner."));
        if (acceptance_ && !acceptance_ready_) {
            acceptance_->initialize(context.game, local_seat());
            acceptance_->delay = options_.input_delay;
            session_->set_stage_observer([this](std::string_view event, const net::SeatInput& input, std::uint64_t tick) {
                acceptance_->event(event, input, tick);
            });
            acceptance_ready_ = true;
        }
        adapter_->bind_executor(context.executor);
        if (!save_service_ready_) {
            adapter_->set_save_host(options_.seats.front().seat);
            auto* game = &context.game;
            if (!recovery_adapter_ && !game->network_resume_state().empty())
                if (auto restored = adapter_->restore_network_state(game->network_resume_state()); !restored)
                    return end_with_failure(restored.error());
            net::CheckpointCallbacks callbacks;
            callbacks.host = 0; // The transport roster uses options_.seats order; front is peer zero.
            callbacks.capture = [this, game](const auto& sequences) -> core::Result<net::CheckpointProof> {
                if (!active_save_) return core::Result<net::CheckpointProof>::failure(
                    make_diagnostic(net::session_errors::options, "save capture has no accepted request"));
                if (auto retained = game->retain_network_producer_sequences(sequences); !retained)
                    return core::Result<net::CheckpointProof>::failure(retained.error());
                if (auto retained = game->retain_network_resume_state(adapter_->checkpoint_network_state()); !retained)
                    return core::Result<net::CheckpointProof>::failure(retained.error());
                auto bytes = checkpoint::save_game_checkpoint(*game, checkpoint::game_identities(game->galaxy().setup()));
                if (!bytes) return core::Result<net::CheckpointProof>::failure(bytes.error());
                auto container = checkpoint::parse_checkpoint(bytes.value());
                if (!container) return core::Result<net::CheckpointProof>::failure(container.error());
                auto metadata = checkpoint::inspect_game_resume(container.value());
                if (!metadata) return core::Result<net::CheckpointProof>::failure(metadata.error());
                captured_save_ = std::move(bytes).value();
                return core::Result<net::CheckpointProof>::success({metadata.value().capture_commitment,
                    metadata.value().monitor_digest, metadata.value().frontier_digest});
            };
            callbacks.commit = [this, game]() -> core::Result<void> {
                retain_recovery_checkpoint(std::make_shared<const std::vector<std::uint8_t>>(captured_save_));
                auto boundary = game->history_frontier();
                if (!boundary) return core::Result<void>::failure(boundary.error());
                game->schedule_history_rebase(std::move(boundary).value());
                if (transport_.self() == 0 && active_save_)
                    agreed_save_ = platform::AgreedGameSave{*active_save_, game->completed_tick(), std::move(captured_save_), {}};
                captured_save_.clear(); active_save_.reset();
                return core::Result<void>::success();
            };
            callbacks.refused = [this, game](core::Diagnostic error) {
                if (transport_.self() == 0 && active_save_)
                    agreed_save_ = platform::AgreedGameSave{*active_save_, game->completed_tick(), {}, std::move(error)};
                captured_save_.clear(); active_save_.reset();
            };
            session_->enable_checkpoint_barrier(std::move(callbacks));
            if (!recovery_log_) recovery_log_.emplace(options_.recovery_log_bytes);
            // The agreed setup, including saved logical seat IDs, is the match
            // lineage. A split invalidates this base before another query runs.
            const auto hash = adapter_->state_hash();
            if (lineage_ == net::RecoveryDigest{}) lineage_ = core::sha256({reinterpret_cast<const std::uint8_t*>(hash.data()), hash.size()});
            if (admission_ && recovery_log_->frontier().epoch == 0) {
                auto offer = net::encode_setup_offer(admission_->setup().offer());
                if (offer) lineage_ = core::sha256(offer.value());
            }
            if (!options_.initial_recovery_refusal.empty()) recovery_refusal_ = options_.initial_recovery_refusal;
            if (options_.initial_checkpoint) {
                retain_recovery_checkpoint(options_.initial_checkpoint);
                options_.initial_checkpoint.reset();
            }
            session_->set_commit_observer([this](auto tick, auto inputs, const auto& completed) {
                if (const auto digest = recovery_digest(completed.state_sha256)) {
                    const auto refusal = recovery_log_->append(tick, inputs, *digest);
                    if (refusal != net::GalacticRecoveryRefusal::none && refusal != net::GalacticRecoveryRefusal::unavailable)
                        recovery_refusal_ = std::string(net::galactic_recovery_reason(refusal));
                } else { recovery_log_->invalidate(); recovery_refusal_ = "catchup_state_hash_mismatch"; }
            });
            save_service_ready_ = true;
        }
        if (acceptance_) acceptance_->script(context.game, local_seat(), context.game.completed_tick(), [&](const auto& payload) {
            submit(context, platform::LiveGameOrder{local_seat(), payload, {}});
        });
        for (const auto& order : orders) submit(context, order);
        // The engine-side source (GC AI) is borrowed for this call only: it runs inside the gated step on every peer.
        if (context.command_source)
            adapter_->set_engine_source([&context](const campaign::GameSession&, std::uint64_t tick) {
                return core::Result<std::vector<galactic::GameCommand>>::success(context.command_source(tick));
            });
        gate_.advance_allowed = context.advance_allowed;
        if (recovery_gate_) recovery_gate_->advance_allowed = context.advance_allowed;
        if (external_cadence_) external_cadence_(context.advance_allowed);
        const auto started = acceptance_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto pumped = session_->pump(now);
        adapter_->set_engine_source({});
        // A refusal callback can quarantine this owner inside receive(). Keep
        // that same pump from reopening simulation or replacement connections.
        if (recovery_quarantined_) return Tick::success(std::nullopt);
        // Preserve the old core's D5/mesh decision before arming any replacement
        // connection: repair must not turn a conclusive close into a new grace.
        if (options_.repair_recovery_links && !recovery_split_) static_cast<void>(options_.repair_recovery_links());
        if (!pumped) return end_with_failure(pumped.error());
        std::optional<campaign::GameTick> tick;
        if (pumped.value().completed) {
            tick = adapter_->take_tick();
            if (!tick) return end_with_failure(make_diagnostic(net::session_errors::simulation,
                "the galaxy adapter completed a tick without publishing it"));
            for (const auto& event : tick->save_events) {
                if (event.kind != galactic::GalacticEventKind::save_requested) continue;
                if (session_->checkpoint_pending()) active_save_ = event;
                else if (transport_.self() == 0) agreed_save_ = platform::AgreedGameSave{event, context.game.completed_tick(), {},
                    make_diagnostic(net::session_errors::admission, "Save failed: a roster or timing agreement overlaps capture.")};
            }
            if (acceptance_) acceptance_->observe(context.game, *tick,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        }
        if (acceptance_) {
            acceptance_->stall(context.advance_allowed && session_->waiting_for() && !session_->waiting_for()->peers.empty());
            if (!acceptance_->healthy()) return end_with_failure(make_diagnostic(net::session_errors::options,
                "Cannot write GC acceptance evidence."));
        }
        if (tick && tick->galaxy) {
            const std::lock_guard lock(mutex_);
            const PauseNoticePublication publish_pause(view_, pause_notice_revision_);
            const auto& game = adapter_->game();
            view_.clock_paused = game.battle() ? game.child_paused()
                : game.galaxy().clock().pause_depth > (game.pending_conflict() || game.battle_summary_hold() ? 1U : 0U);
            if (view_.clock_paused && adapter_->pause_actor()) {
                view_.clock_actor = *adapter_->pause_actor(); view_.clock_action = "paused";
            }
            for (const auto seat_id : adapter_->clock_refusals()) {
                const auto at = std::find_if(view_.clock_refusals.begin(), view_.clock_refusals.end(),
                    [&](const auto& row) { return row.seat == seat_id; });
                if (at == view_.clock_refusals.end()) view_.clock_refusals.push_back({seat_id, tick->completed_tick});
                else at->completed_tick = tick->completed_tick;
            }
            if (tick->galaxy != clock_snapshot_) for (const auto& event : tick->galaxy->events()) {
                using Kind = galactic::GalacticEventKind;
                std::string action;
                if (event.kind == Kind::galaxy_paused) action = "paused";
                else if (event.kind == Kind::galaxy_resumed) action = "resumed";
                else if (event.kind == Kind::speed_changed)
                    action = speed_action_text(event.subject != 0U, static_cast<std::int64_t>(event.value));
                const bool human = std::any_of(options_.seats.begin(), options_.seats.end(),
                    [&](const auto& seat) { return seat.may_submit && seat.seat == event.producer; });
                if (!action.empty() && human) {
                    view_.clock_actor = event.producer; view_.clock_action = std::move(action);
                    view_.clock_change_tick = tick->completed_tick;
                    clock_events_.fetch_add(1, std::memory_order_release); // under mutex_: a reader seeing it reads this view
                }
            }
        }
        if (tick) clock_snapshot_ = tick->galaxy;
        if (adapter_->completed_tick() >= options_.final_tick_count && monitor_->agreed_end() && !session_->checkpoint_pending()) {
            if (options_.final_output_drained && !options_.final_output_drained()) {
                if (!final_drain_started_) final_drain_started_ = now;
                if (now - *final_drain_started_ >= options_.completeness_timeout_ms)
                    return end_with_failure(make_diagnostic(net::session_errors::link_closed,
                        "Final campaign hash output did not drain before the close deadline."));
            } else final_drain_started_.reset();
        }
        refresh(pumped.value(), context.advance_allowed, now);
        return Tick::success(std::move(tick));
    }

    [[nodiscard]] bool may_save(sim::SeatId seat) const noexcept {
        const std::lock_guard lock(mutex_);
        return view_.phase != GalacticLanPhase::ended && transport_.self() == 0 && !options_.seats.empty() && seat == options_.seats.front().seat;
    }
    [[nodiscard]] std::optional<platform::AgreedGameSave> take_agreed_save() { return std::exchange(agreed_save_, {}); }
    [[nodiscard]] std::span<const sim::SeatId> clock_refusals() const noexcept {
        return adapter_ ? adapter_->clock_refusals() : std::span<const sim::SeatId>{};
    }

    void session_ended() noexcept {
        if (acceptance_) {
            try { acceptance_->finish(monitor_ && monitor_->agreed_end()); } catch (...) { /* Missing receipt fails verification. */ }
        }
        {
            const std::lock_guard lock(mutex_);
            session_ended_ = true;
            recovery_worker_.cancel();
            if (recovery_begin_) { recovery_begin_->result.set_value(net::GalacticRecoveryRefusal::unavailable); recovery_begin_.reset(); }
            if (recovery_incident_ && recovery_incident_->result) recovery_incident_->result->set_value(net::GalacticRecoveryRefusal::unavailable);
            recovery_incident_.reset();
            for (auto& query : recovery_queries_) query.result.set_value(core::Result<net::GalacticRecoveryPackage>::failure(
                make_diagnostic(net::reconnect_errors::unavailable, "checkpoint_unavailable")));
            recovery_queries_.clear();
            if (recovery_publication_) {
                recovery_publication_->result.set_value(net::GalacticRecoveryRefusal::unavailable);
                recovery_publication_.reset();
            }
        }
        session_ = nullptr;
        adapter_ = nullptr;
        recovery_session_.reset(); recovery_adapter_.reset(); recovery_gate_.reset(); recovery_monitor_.reset();
        owned_session_.reset();
        owned_adapter_.reset();
        // The admitted adapter no longer borrows the live worker executor after stop.
        if (admission_) {
            static const sim::InlineExecutor stopped_executor;
            if (auto* adapter = dynamic_cast<net::GalacticSessionAdapter*>(&admission_->setup().adapter()))
                adapter->bind_executor(stopped_executor);
        }
    }

    [[nodiscard]] std::future<core::Result<net::GalacticRecoveryPackage>> recovery_package(
        net::PeerIndex peer, net::GalacticRecoveryRequest request) {
        RecoveryQuery query{peer, std::move(request), {}};
        auto result = query.result.get_future();
        const std::lock_guard lock(mutex_);
        if (session_ended_ || !recovery_queries_.empty()) {
            query.result.set_value(core::Result<net::GalacticRecoveryPackage>::failure(make_diagnostic(
                net::reconnect_errors::unavailable, session_ended_ ? "checkpoint_unavailable" : "catchup_in_progress")));
        } else recovery_queries_.push_back(std::move(query));
        return result;
    }

    [[nodiscard]] std::future<net::GalacticRecoveryRefusal> request_recovery(net::PeerIndex source) {
        RecoveryBegin begin{source, {}}; auto result = begin.result.get_future();
        const std::lock_guard lock(mutex_);
        if (session_ended_ || recovery_begin_) begin.result.set_value(session_ended_ ? net::GalacticRecoveryRefusal::unavailable : net::GalacticRecoveryRefusal::busy);
        else recovery_begin_ = std::move(begin);
        return result;
    }

    [[nodiscard]] std::future<net::GalacticRecoveryRefusal> publish_recovery(net::GalacticRecoveryPackage package,
        net::GalacticRecoveryWorker::Result candidate, std::vector<net::GalacticRecoveryProof> proofs,
        std::optional<net::SeatInput> rejoin) {
        RecoveryPublication publication{std::move(package), std::move(candidate), std::move(proofs), std::move(rejoin), {}};
        auto result = publication.result.get_future();
        const std::lock_guard lock(mutex_);
        if (session_ended_ || recovery_publication_)
            publication.result.set_value(session_ended_ ? net::GalacticRecoveryRefusal::unavailable : net::GalacticRecoveryRefusal::busy);
        else recovery_publication_ = std::move(publication);
        return result;
    }

private:
#include "galactic_lan_match_recovery.inc"

    bool service_recovery_publication(platform::LiveGameStepContext& context) {
        using Refusal = net::GalacticRecoveryRefusal;
        std::optional<RecoveryPublication> publication;
        { const std::lock_guard lock(mutex_); publication.swap(recovery_publication_); }
        if (!publication) return false;
        auto& request = *publication;
        const auto refuse = [&](Refusal reason) {
            recovery_refusal_ = std::string(net::galactic_recovery_reason(reason)); request.result.set_value(reason); return false;
        };
        auto& package = request.package;
        if (recovery_split_ || (package.session_frontier && package.session_frontier->split)) return refuse(Refusal::divergent_epoch);
        if (request.candidate.refusal != Refusal::none) return refuse(request.candidate.refusal);
        auto* adapter = dynamic_cast<net::GalacticSessionAdapter*>(request.candidate.owner.get());
        if (!adapter || (!request.preserve_owner && (!adapter->owned_game() || !context.adopt_recovered_owner)) || !package.session_frontier
            || request.candidate.frontier != package.frontier || package.seats.size() != options_.seats.size()) return refuse(Refusal::restore);
        if (context.game.completed_tick() > package.frontier.completed_tick) return refuse(Refusal::stale_barrier);
        if (options_.seats.size() == 2 && context.game.completed_tick() == package.frontier.completed_tick
            && adapter_->state_hash() != adapter->state_hash()) return refuse(Refusal::divergent_epoch);
        const auto state = recovery_digest(adapter->state_hash());
        if (!state || *state != package.frontier.state || adapter->completed_tick() != package.frontier.completed_tick)
            return refuse(Refusal::state_hash);
        for (std::size_t peer = 0; peer < options_.seats.size(); ++peer)
            if (package.seats[peer].seat != options_.seats[peer].seat || package.seats[peer].may_submit != options_.seats[peer].may_submit)
                return refuse(Refusal::seat_binding);
        if (package.session_frontier->peers.size() != options_.seats.size()) return refuse(Refusal::malformed);
        auto encoded = net::encode_galactic_recovery_log(package);
        if (!encoded) return refuse(Refusal::malformed);
        const auto digest = core::sha256(encoded.value());
        std::vector<bool> proved(options_.seats.size());
        for (const auto& proof : request.proofs) {
            if (proof.peer >= proved.size() || proved[proof.peer] || proof.frontier != package.frontier || proof.sealed_log != digest)
                return refuse(Refusal::state_hash);
            proved[proof.peer] = true;
        }
        for (std::size_t peer = 0; peer < proved.size(); ++peer) {
            const bool active = !package.session_frontier->peers[peer].departed
                || (request.rejoin && request.rejoin->key.seat == options_.seats[peer].seat);
            if (proved[peer] != active) return refuse(Refusal::seat_binding);
        }
        // Do not erase newly accepted local input between proof and publication.
        // A late candidate must obtain a new barrier instead of rolling it back.
        const auto current = session_->recovery_frontier(!request.preserve_owner);
        if (!current) return refuse(Refusal::busy);
        if (current.value().split) return refuse(Refusal::divergent_epoch);
        for (const auto& batch : current.value().future) {
            if (batch.tick < package.frontier.completed_tick) {
                if (batch.tick < package.checkpoint.completed_tick) return refuse(Refusal::log_too_old);
                const auto index = batch.tick - package.checkpoint.completed_tick;
                if (index >= package.ticks.size() || package.ticks[static_cast<std::size_t>(index)].tick != batch.tick)
                    return refuse(Refusal::stale_barrier);
                const auto& recorded = package.ticks[static_cast<std::size_t>(index)].inputs;
                for (const auto& input : batch.commands)
                    if (std::find(recorded.begin(), recorded.end(), input) == recorded.end()) return refuse(Refusal::stale_barrier);
                continue;
            }
            const auto expected = std::find_if(package.session_frontier->future.begin(), package.session_frontier->future.end(), [&](const auto& row) {
                return row.sender == batch.sender && row.tick == batch.tick;
            });
            if (expected == package.session_frontier->future.end()) return refuse(Refusal::stale_barrier);
            const auto actual_bytes = net::encode_batch(batch), expected_bytes = net::encode_batch(*expected);
            if (!actual_bytes || !expected_bytes || actual_bytes.value() != expected_bytes.value()) return refuse(Refusal::stale_barrier);
        }
        net::GalacticRecoveryLog retained(options_.recovery_log_bytes);
        if (retained.checkpoint(package.checkpoint, package.bytes, package.seats) != Refusal::none) return refuse(Refusal::malformed);
        for (const auto& row : package.ticks)
            if (retained.append(row.tick, row.inputs, row.state) != Refusal::none) return refuse(Refusal::limit);
        auto owned_adapter = std::unique_ptr<net::GalacticSessionAdapter>(static_cast<net::GalacticSessionAdapter*>(request.candidate.owner.release()));
        owned_adapter->bind_executor(context.executor);
        net::DesyncOptions monitor_options; monitor_options.initial_tick = package.frontier.completed_tick;
        monitor_options.input_delay = options_.input_delay; monitor_options.exchange_every = options_.exchange_every;
        auto monitor = std::make_unique<net::DesyncMonitor>(transport_, monitor_options);
        auto gate = std::make_unique<CadenceGate>(); gate->monitor = monitor.get();
        net::SessionOptions config = session_->recovery_options();
        config.initial_tick = package.frontier.completed_tick; config.input_delay = options_.input_delay;
        config.producer_sequences = owned_adapter->game().producer_sequences();
        config.final_tick_count = options_.final_tick_count; config.completeness_timeout_ms = options_.completeness_timeout_ms;
        config.unconfirmed_departure = net::UnconfirmedDeparture::continue_alone;
        auto created = net::LockstepSession::create(*owned_adapter, transport_, options_.seats, config, gate.get());
        if (!created) return refuse(Refusal::restore);
        auto session = std::make_unique<net::LockstepSession>(std::move(created).value());
        session->enable_departures([](sim::SeatCommandKey key) { return net::encode_galactic_input(key, galactic::SeatLeave{}); });
        if (!session->restore_recovery_frontier(*package.session_frontier, std::move(request.rejoin))) return refuse(Refusal::malformed);
        if (!request.preserve_owner) {
            const auto services = std::static_pointer_cast<const platform::CampaignServiceConfiguration>(owned_adapter->runtime_services());
            auto adopted = context.adopt_recovered_owner({owned_adapter->owned_game(), services, package.frontier.completed_tick});
            if (!adopted) return refuse(Refusal::restore);
        }
        // Every fallible proof/restore check precedes the owner callback. The
        // following pointer exchange publishes the corresponding lockstep core.
        recovery_session_ = std::move(session); recovery_adapter_ = std::move(owned_adapter);
        recovery_gate_ = std::move(gate); recovery_monitor_ = std::move(monitor);
        session_ = recovery_session_.get(); adapter_ = recovery_adapter_.get(); monitor_ = recovery_monitor_.get();
        attach_recovery_receiver();
        recovery_log_ = std::move(retained); lineage_ = package.frontier.lineage;
        failed_.reset(); recovery_listening_ = false; recovery_auto_attempts_ = 0;
        recovery_refusal_.clear(); save_service_ready_ = false; captured_save_.clear(); active_save_.reset(); clock_snapshot_.reset();
        if (acceptance_) session_->set_stage_observer([this](std::string_view event, const net::SeatInput& input, std::uint64_t tick) {
            acceptance_->event(event, input, tick);
        });
        {
            const std::lock_guard lock(mutex_);
            const PauseNoticePublication publish_pause(view_, pause_notice_revision_);
            view_.phase = GalacticLanPhase::running; view_.reason = GalacticLanReason::none; view_.diagnostic_code.clear(); view_.message.clear();
            view_.completed_tick = package.frontier.completed_tick; view_.recovery_refusal.clear(); view_.recovery_anchor = package.checkpoint;
            view_.departed.clear();
            for (std::size_t peer = 0; peer < package.session_frontier->peers.size(); ++peer)
                if (package.session_frontier->peers[peer].departed && !proved[peer]) view_.departed.push_back(options_.seats[peer].seat);
        }
        request.result.set_value(Refusal::none);
        return true;
    }

    void service_recovery_queries() {
        std::deque<RecoveryQuery> queries;
        { const std::lock_guard lock(mutex_); queries.swap(recovery_queries_); }
        for (auto& query : queries) {
            net::GalacticRecoveryPackage package;
            const auto refusal = recovery_split_ ? net::GalacticRecoveryRefusal::divergent_epoch
                : recovery_log_ ? recovery_log_->package(query.peer, query.request, package)
                : net::GalacticRecoveryRefusal::unavailable;
            if (refusal == net::GalacticRecoveryRefusal::none) {
                auto frontier = session_->recovery_frontier();
                if (!frontier || frontier.value().split) {
                    query.result.set_value(core::Result<net::GalacticRecoveryPackage>::failure(make_diagnostic(
                        net::reconnect_errors::unavailable, frontier ? "divergent_epoch" : "frontier_agreement_in_progress")));
                } else {
                    package.session_frontier = std::move(frontier).value();
                    query.result.set_value(core::Result<net::GalacticRecoveryPackage>::success(std::move(package)));
                }
            }
            else query.result.set_value(core::Result<net::GalacticRecoveryPackage>::failure(make_diagnostic(
                net::reconnect_errors::unavailable, std::string(net::galactic_recovery_reason(refusal)))));
        }
    }
    void retain_recovery_checkpoint(std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
        if (!recovery_log_ || recovery_split_) return;
        const auto state = recovery_digest(adapter_->state_hash());
        if (!state) { recovery_log_->invalidate(); recovery_refusal_ = "catchup_state_hash_mismatch"; return; }
        const net::GalacticRecoveryAnchor anchor{1, adapter_->completed_tick(), lineage_, *state};
        const auto refusal = recovery_log_->checkpoint(anchor, std::move(bytes), options_.seats);
        if (refusal != net::GalacticRecoveryRefusal::none) recovery_refusal_ = std::string(net::galactic_recovery_reason(refusal));
        else recovery_refusal_.clear();
    }
    core::Result<void> open(platform::LiveGameStepContext& context) {
        if (options_.seats.empty() || options_.seats.size() != transport_.peer_count() || transport_.self() >= options_.seats.size())
            return core::Result<void>::failure(make_diagnostic(net::session_errors::options,
                "the galactic LAN roster must list exactly one seat per transport peer"));
        net::DesyncOptions monitor_options;
        monitor_options.input_delay = options_.input_delay;
        monitor_options.exchange_every = options_.exchange_every;
        monitor_options.initial_tick = context.game.completed_tick();
        owned_monitor_.emplace(transport_, monitor_options);
        monitor_ = &*owned_monitor_;
        gate_.monitor = monitor_;
        net::GalacticAdapterOptions adapter_options;
        adapter_options.clock_policy = options_.clock_policy;
        owned_adapter_.emplace(context.game, context.executor, adapter_options);
        adapter_ = &*owned_adapter_;
        if (!context.game.network_resume_state().empty()) {
            auto restored = adapter_->restore_network_state(context.game.network_resume_state());
            if (!restored) return restored;
        }
        net::SessionOptions session_options;
        session_options.initial_tick = context.game.completed_tick();
        session_options.producer_sequences = context.game.producer_sequences();
        session_options.input_delay = options_.input_delay;
        session_options.final_tick_count = options_.final_tick_count;
        session_options.completeness_timeout_ms = options_.completeness_timeout_ms;
        session_options.unconfirmed_departure = net::UnconfirmedDeparture::continue_alone; // D5 configured fallback; D8 no rejoin
        auto created = net::LockstepSession::create(*adapter_, transport_, options_.seats, session_options, &gate_);
        if (!created) return core::Result<void>::failure(created.error());
        owned_session_.emplace(std::move(created).value());
        session_ = &*owned_session_;
        // GCMP-09: SeatLeave applies WGS-P02/P04 at the agreed tick; D5 keeps the campaign running with its fallback.
        session_->enable_departures([](sim::SeatCommandKey key) {
            return net::encode_galactic_input(key, galactic::SeatLeave{});
        });
        return core::Result<void>::success();
    }

    // Local orders are untimed seat inputs: the core stamps them when it seals the batch of the next open tick.
    void submit(platform::LiveGameStepContext& context, const platform::LiveGameOrder& order) {
        if (recovery_incident_ && recovery_incident_->barrier && adapter_->completed_tick() >= *recovery_incident_->barrier)
            return context.reject(make_diagnostic(net::reconnect_errors::unavailable, "recovery_publication_pending"));
        if (order.tick) return context.reject(make_diagnostic("EAWR-PLATFORM-0600",
            "A lockstep live game takes untimed orders only; the agreed batch stamps the tick."));
        if (order.seat != local_seat()) return context.reject(make_diagnostic("EAWR-PLATFORM-0600",
            "A lockstep live game takes orders for its own seat only."));
        auto stamped = session_->submit([&order](sim::SeatCommandKey key) {
            return net::encode_galactic_input(key, order.payload);
        });
        if (!stamped) context.reject(stamped.error());
        else if (acceptance_) {
            auto encoded = net::encode_galactic_input(stamped.value(), order.payload);
            if (encoded) acceptance_->event("input", {stamped.value(), std::move(encoded).value()}, context.game.completed_tick());
        }
    }

    Tick end_with_failure(core::Diagnostic error) {
        failed_ = error;
        const std::lock_guard lock(mutex_);
        const PauseNoticePublication publish_pause(view_, pause_notice_revision_);
        view_.phase = GalacticLanPhase::ended;
        view_.diagnostic_code = error.code;
        view_.message = error.message;
        if (error.code == net::session_errors::stalled) view_.reason = GalacticLanReason::stalled;
        else if (error.code == net::session_errors::link_closed || error.code == net::departure_errors::timeout)
            view_.reason = GalacticLanReason::link_lost;
        else view_.reason = GalacticLanReason::failed;
        const bool lost = error.code == net::session_errors::stalled || error.code == net::session_errors::link_closed
            || error.code == net::departure_errors::timeout || error.code == "EAWR-NET-1778" || error.code == "EAWR-NET-1779"
            || error.code == "EAWR-NET-1780" || error.code == "EAWR-NET-1781" || error.code == "EAWR-NET-1782";
        if (lost && options_.recovery_restore && options_.seats.size() >= 3) {
            recovery_listening_ = true; return Tick::success(std::nullopt);
        }
        return Tick::failure(std::move(error));
    }

    void refresh(const net::PumpResult& pumped, bool due, std::uint64_t now) {
        const std::lock_guard lock(mutex_);
        const PauseNoticePublication publish_pause(view_, pause_notice_revision_);
        view_.completed_tick = adapter_->completed_tick();
        view_.recovery_refusal = recovery_refusal_;
        view_.recovery_anchor = recovery_log_ && recovery_log_->frontier().epoch && !recovery_split_
            ? std::optional{recovery_log_->frontier()} : std::nullopt;
        const auto& game = adapter_->game();
        view_.clock_paused = game.battle() ? game.child_paused()
            : game.galaxy().clock().pause_depth > (game.pending_conflict() || game.battle_summary_hold() ? 1U : 0U);
        for (const auto& event : pumped.departure_events) {
            if (options_.seats.size() == 2 && event.reason != net::DepartureReason::quit) {
                recovery_split_ = true;
                if (recovery_log_) recovery_log_->invalidate();
                recovery_refusal_ = view_.recovery_refusal = "divergent_epoch";
                view_.recovery_anchor.reset();
            }
            view_.departed.push_back(event.key.seat);
            if (event.key.seat == local_seat()) {
                view_.phase = GalacticLanPhase::ended;
                view_.reason = GalacticLanReason::local_left;
                view_.message = "You left the Galactic match at agreed tick " + std::to_string(event.key.tick)
                    + ". Rejoin is disabled for Galactic multiplayer.";
            } else if (view_.reason != GalacticLanReason::local_left)
                view_.message = "Seat " + std::to_string(event.key.seat.value) + " departed at agreed tick "
                    + std::to_string(event.key.tick) + ". You can continue playing or quit. "
                    "Rejoin is disabled for Galactic multiplayer.";
        }
        if (view_.phase == GalacticLanPhase::ended) return;
        // The core only reports a completeness wait while the cadence gate is open; keep the last one otherwise.
        if (due) {
            const auto& waiting = session_->waiting_for();
            if (waiting && !waiting->peers.empty()) {
                wait_peers_ = waiting->peers;
                wait_since_ = waiting->since_ms;
            } else wait_peers_.clear();
        }
        if (monitor_->frozen()) {
            view_.phase = GalacticLanPhase::ended;
            view_.reason = GalacticLanReason::desync;
            view_.message = "The match state of the players diverged.";
            if (options_.recovery_restore && options_.seats.size() == 2) {
                recovery_split_ = true; if (recovery_log_) recovery_log_->invalidate();
                recovery_refusal_ = view_.recovery_refusal = "divergent_epoch"; view_.recovery_anchor.reset();
            }
            return;
        }
        if (view_.completed_tick >= options_.final_tick_count && monitor_->agreed_end() && !final_drain_started_
            && !session_->checkpoint_pending()) {
            view_.phase = GalacticLanPhase::ended;
            view_.reason = GalacticLanReason::completed;
            return;
        }
        bool connected = monitor_->setup_agreed();
        // A seat that left by an agreed departure is not coming back (D8: no rejoin). Its transport edge may sit in
        // `connecting` while the host keeps a listener open; that must not turn the running match into "connecting".
        const auto departed_peer = [&](net::PeerIndex peer) {
            return peer < options_.seats.size() && std::find(view_.departed.begin(), view_.departed.end(), options_.seats[peer].seat) != view_.departed.end();
        };
        for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer)
            if (peer != transport_.self() && !departed_peer(peer) && transport_.link(peer) == net::LinkState::connecting) connected = false;
        view_.waiting_for.clear();
        if (!connected) view_.phase = GalacticLanPhase::connecting;
        else if (!wait_peers_.empty() && now >= wait_since_ && now - wait_since_ > options_.waiting_notice_ms) {
            view_.phase = GalacticLanPhase::waiting_for_peer;
            view_.waiting_for = wait_peers_;
        } else if (!view_.departed.empty()) view_.phase = GalacticLanPhase::departed;
        else view_.phase = GalacticLanPhase::running;
    }

    struct RecoveryQuery {
        net::PeerIndex peer{};
        net::GalacticRecoveryRequest request;
        std::promise<core::Result<net::GalacticRecoveryPackage>> result;
    };
    struct RecoveryPublication {
        net::GalacticRecoveryPackage package;
        net::GalacticRecoveryWorker::Result candidate;
        std::vector<net::GalacticRecoveryProof> proofs;
        std::optional<net::SeatInput> rejoin;
        std::promise<net::GalacticRecoveryRefusal> result;
        bool preserve_owner{};
    };
    std::optional<RecoveryPublication> recovery_publication_;
    std::deque<RecoveryQuery> recovery_queries_;
    std::optional<net::GalacticRecoveryLog> recovery_log_;
    net::RecoveryDigest lineage_{};
    std::string recovery_refusal_;
    bool recovery_split_{}, recovery_listening_{}, recovery_quarantined_{};
    net::MessageTransport& transport_;
    GalacticLanOptions options_;
public:
    std::unique_ptr<GalacticAcceptance> acceptance_;
private:
    bool acceptance_ready_{}, save_service_ready_{};
    std::optional<galactic::GalacticEvent> active_save_;
    std::vector<std::uint8_t> captured_save_;
    std::optional<platform::AgreedGameSave> agreed_save_;
    mutable std::mutex mutex_;
    GalacticLanView view_;
    std::atomic<std::uint64_t> pause_notice_revision_{};
    std::atomic<std::uint64_t> clock_events_{}; // #5355: see GalacticLanMatch::clock_events()
    std::shared_ptr<const galactic::GalacticSnapshot> clock_snapshot_;
    std::optional<core::Diagnostic> failed_;
    bool session_ended_{};
    std::optional<std::uint64_t> final_drain_started_;
    // Declaration order is destruction order reversed: the session goes before the adapter, gate and monitor it uses.
    std::shared_ptr<void> lifetime_;
    std::shared_ptr<net::LobbyResult> admission_;
    std::function<void(bool)> external_cadence_;
    std::unique_ptr<net::DesyncMonitor> recovery_monitor_;
    std::unique_ptr<CadenceGate> recovery_gate_;
    std::unique_ptr<net::GalacticSessionAdapter> recovery_adapter_;
    std::unique_ptr<net::LockstepSession> recovery_session_;
    std::optional<net::DesyncMonitor> owned_monitor_;
    CadenceGate gate_;
    std::optional<net::GalacticSessionAdapter> owned_adapter_;
    std::optional<net::LockstepSession> owned_session_;
    net::DesyncMonitor* monitor_{};
    net::GalacticSessionAdapter* adapter_{};
    net::LockstepSession* session_{};
    std::vector<net::PeerIndex> wait_peers_;
    std::uint64_t wait_since_{};
};

GalacticLanMatch::GalacticLanMatch(net::MessageTransport& transport, GalacticLanOptions options)
    : state_(std::make_shared<State>(transport, std::move(options))) {}
GalacticLanMatch::GalacticLanMatch(net::MessageTransport& transport, GalacticLanOptions options,
    std::shared_ptr<net::LobbyResult> result, net::DesyncMonitor& monitor,
    std::function<void(bool)> cadence, std::shared_ptr<void> lifetime)
    : state_(std::make_shared<State>(transport, std::move(options), std::move(result), monitor,
        std::move(cadence), std::move(lifetime))) {}
GalacticLanMatch::~GalacticLanMatch() = default;

std::shared_ptr<platform::LiveGameStepDriver> GalacticLanMatch::driver() {
    class Driver final : public platform::LiveGameStepDriver {
    public:
        explicit Driver(std::shared_ptr<State> state) : state_(std::move(state)) {}
        core::Result<std::optional<campaign::GameTick>> step(platform::LiveGameStepContext& context,
            std::span<const platform::LiveGameOrder> orders, std::uint64_t now_ms) override {
            return state_->drive(context, orders, now_ms);
        }
        void session_ended() noexcept override { state_->session_ended(); }
        bool may_save(sim::SeatId seat) const noexcept override { return state_->may_save(seat); }
        std::optional<platform::AgreedGameSave> take_agreed_save() override { return state_->take_agreed_save(); }
        std::span<const sim::SeatId> clock_refusals() const noexcept override { return state_->clock_refusals(); }
    private:
        std::shared_ptr<State> state_;
    };
    return std::make_shared<Driver>(state_);
}
std::future<core::Result<net::GalacticRecoveryPackage>> GalacticLanMatch::recovery_package(
    net::PeerIndex peer, net::GalacticRecoveryRequest request) {
    return state_->recovery_package(peer, std::move(request));
}
std::future<net::GalacticRecoveryRefusal> GalacticLanMatch::request_recovery(net::PeerIndex source) {
    return state_->request_recovery(source);
}
std::future<net::GalacticRecoveryRefusal> GalacticLanMatch::publish_recovery(net::GalacticRecoveryPackage package,
    net::GalacticRecoveryWorker::Result candidate, std::vector<net::GalacticRecoveryProof> proofs, std::optional<net::SeatInput> rejoin) {
    return state_->publish_recovery(std::move(package), std::move(candidate), std::move(proofs), std::move(rejoin));
}
GalacticLanView GalacticLanMatch::view() const { return state_->view(); }
std::uint64_t GalacticLanMatch::clock_events() const noexcept { return state_->clock_events(); }
std::uint64_t GalacticLanMatch::pause_notice_revision() const noexcept { return state_->pause_notice_revision(); }
sim::SeatId GalacticLanMatch::local_seat() const { return state_->local_seat(); }
void GalacticLanMatch::configure_recovery_restore(net::GalacticRecoveryWorker::Restore restore) {
    state_->configure_recovery_restore(std::move(restore));
}
void GalacticLanMatch::configure_acceptance(std::filesystem::path directory, std::string campaign, bool stock) {
    if (!directory.empty()) state_->acceptance_ = std::make_unique<GalacticAcceptance>(std::move(directory), std::move(campaign), stock);
}

} // namespace eawr::presentation::godot_backend
