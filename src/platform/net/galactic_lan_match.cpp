#include "galactic_lan_match.hpp"
#include "eawr/platform/live_game_lockstep.hpp"

#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/net/lobby_session.hpp"
#include "galactic_acceptance.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <variant>

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

class GalacticLanMatch::State {
public:
    State(net::MessageTransport& transport, GalacticLanOptions options)
        : transport_(transport), options_(std::move(options)) {}

    State(net::MessageTransport& transport, GalacticLanOptions options, std::shared_ptr<net::LobbyResult> result,
        net::DesyncMonitor& monitor, std::function<void(bool)> cadence, std::shared_ptr<void> lifetime)
        : transport_(transport), options_(std::move(options)), lifetime_(std::move(lifetime)),
          admission_(std::move(result)), external_cadence_(std::move(cadence)) {
        monitor_ = &monitor;
        adapter_ = dynamic_cast<net::GalacticSessionAdapter*>(&admission_->setup().adapter());
        session_ = &admission_->setup().session();
        session_->enable_departures([](sim::SeatCommandKey key) {
            return net::encode_galactic_input(key, galactic::SeatLeave{});
        });
        if (!adapter_) failed_ = make_diagnostic(net::session_errors::options, "The campaign lobby adapter is not galactic.");
    }

    [[nodiscard]] sim::SeatId local_seat() const {
        return transport_.self() < options_.seats.size() ? options_.seats[transport_.self()].seat : sim::SeatId{};
    }
    [[nodiscard]] GalacticLanView view() const {
        const std::lock_guard lock(mutex_);
        return view_;
    }

    Tick drive(platform::LiveGameStepContext& context, std::span<const platform::LiveGameOrder> orders, std::uint64_t now) {
        if (session_ended_) {
            auto error = make_diagnostic("EAWR-PLATFORM-0600", "A galactic LAN driver cannot be reused after its live session ended.");
            for (const auto& order : orders) { (void)order; context.reject(error); }
            return Tick::failure(std::move(error));
        }
        if (failed_) return Tick::failure(*failed_);
        if (!session_) {
            if (auto opened = open(context); !opened) return end_with_failure(opened.error());
        }
        {
            const std::lock_guard lock(mutex_);
            if (view_.phase == GalacticLanPhase::ended) {
                for (const auto& order : orders) {
                    (void)order;
                    context.reject(make_diagnostic("EAWR-PLATFORM-0600",
                        "A galactic LAN match that ended cannot accept orders."));
                }
                if (view_.reason == GalacticLanReason::desync)
                    return Tick::failure(make_diagnostic("EAWR-PLATFORM-0600", view_.message));
                return Tick::success(std::nullopt);
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
        if (external_cadence_) external_cadence_(context.advance_allowed);
        const auto started = acceptance_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto pumped = session_->pump(now);
        adapter_->set_engine_source({});
        if (!pumped) return end_with_failure(pumped.error());
        std::optional<campaign::GameTick> tick;
        if (pumped.value().completed) {
            tick = adapter_->take_tick();
            if (!tick) return end_with_failure(make_diagnostic(net::session_errors::simulation,
                "the galaxy adapter completed a tick without publishing it"));
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
            const auto& game = adapter_->game();
            view_.clock_paused = game.battle() ? game.child_paused() : game.galaxy().clock().pause_depth != 0;
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
                else if (event.kind == Kind::speed_changed) action = "changed speed";
                const bool human = std::any_of(options_.seats.begin(), options_.seats.end(),
                    [&](const auto& seat) { return seat.may_submit && seat.seat == event.producer; });
                if (!action.empty() && human) {
                    view_.clock_actor = event.producer; view_.clock_action = std::move(action);
                    view_.clock_change_tick = tick->completed_tick;
                }
            }
        }
        if (tick) clock_snapshot_ = tick->galaxy;
        if (adapter_->completed_tick() >= options_.final_tick_count && monitor_->agreed_end()) {
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

    [[nodiscard]] std::span<const sim::SeatId> clock_refusals() const noexcept {
        return adapter_ ? adapter_->clock_refusals() : std::span<const sim::SeatId>{};
    }

    void session_ended() noexcept {
        if (acceptance_) {
            try { acceptance_->finish(monitor_ && monitor_->agreed_end()); } catch (...) { /* Missing receipt fails verification. */ }
        }
        session_ended_ = true;
        session_ = nullptr;
        adapter_ = nullptr;
        owned_session_.reset();
        owned_adapter_.reset();
        // The admitted adapter no longer borrows the live worker executor after stop.
        if (admission_) {
            static const sim::InlineExecutor stopped_executor;
            if (auto* adapter = dynamic_cast<net::GalacticSessionAdapter*>(&admission_->setup().adapter()))
                adapter->bind_executor(stopped_executor);
        }
    }

private:
    core::Result<void> open(platform::LiveGameStepContext& context) {
        if (options_.seats.empty() || options_.seats.size() != transport_.peer_count() || transport_.self() >= options_.seats.size())
            return core::Result<void>::failure(make_diagnostic(net::session_errors::options,
                "the galactic LAN roster must list exactly one seat per transport peer"));
        net::DesyncOptions monitor_options;
        monitor_options.input_delay = options_.input_delay;
        monitor_options.exchange_every = options_.exchange_every;
        owned_monitor_.emplace(transport_, monitor_options);
        monitor_ = &*owned_monitor_;
        gate_.monitor = monitor_;
        net::GalacticAdapterOptions adapter_options;
        adapter_options.clock_policy = options_.clock_policy;
        owned_adapter_.emplace(context.game, context.executor, adapter_options);
        adapter_ = &*owned_adapter_;
        net::SessionOptions session_options;
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
        view_.phase = GalacticLanPhase::ended;
        view_.diagnostic_code = error.code;
        view_.message = error.message;
        if (error.code == net::session_errors::stalled) view_.reason = GalacticLanReason::stalled;
        else if (error.code == net::session_errors::link_closed || error.code == net::departure_errors::timeout)
            view_.reason = GalacticLanReason::link_lost;
        else view_.reason = GalacticLanReason::failed;
        return Tick::failure(std::move(error));
    }

    void refresh(const net::PumpResult& pumped, bool due, std::uint64_t now) {
        const std::lock_guard lock(mutex_);
        view_.completed_tick = adapter_->completed_tick();
        const auto& game = adapter_->game();
        view_.clock_paused = game.battle() ? game.child_paused() : game.galaxy().clock().pause_depth != 0;
        for (const auto& event : pumped.departure_events) {
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
            return;
        }
        if (view_.completed_tick >= options_.final_tick_count && monitor_->agreed_end() && !final_drain_started_) {
            view_.phase = GalacticLanPhase::ended;
            view_.reason = GalacticLanReason::completed;
            return;
        }
        bool connected = monitor_->setup_agreed();
        for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer)
            if (peer != transport_.self() && transport_.link(peer) == net::LinkState::connecting) connected = false;
        view_.waiting_for.clear();
        if (!connected) view_.phase = GalacticLanPhase::connecting;
        else if (!wait_peers_.empty() && now >= wait_since_ && now - wait_since_ > options_.waiting_notice_ms) {
            view_.phase = GalacticLanPhase::waiting_for_peer;
            view_.waiting_for = wait_peers_;
        } else if (!view_.departed.empty()) view_.phase = GalacticLanPhase::departed;
        else view_.phase = GalacticLanPhase::running;
    }

    net::MessageTransport& transport_;
    GalacticLanOptions options_;
public:
    std::unique_ptr<GalacticAcceptance> acceptance_;
private:
    bool acceptance_ready_{};
    mutable std::mutex mutex_;
    GalacticLanView view_;
    std::shared_ptr<const galactic::GalacticSnapshot> clock_snapshot_;
    std::optional<core::Diagnostic> failed_;
    bool session_ended_{};
    std::optional<std::uint64_t> final_drain_started_;
    // Declaration order is destruction order reversed: the session goes before the adapter, gate and monitor it uses.
    std::shared_ptr<void> lifetime_;
    std::shared_ptr<net::LobbyResult> admission_;
    std::function<void(bool)> external_cadence_;
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
        std::span<const sim::SeatId> clock_refusals() const noexcept override { return state_->clock_refusals(); }
    private:
        std::shared_ptr<State> state_;
    };
    return std::make_shared<Driver>(state_);
}
GalacticLanView GalacticLanMatch::view() const { return state_->view(); }
sim::SeatId GalacticLanMatch::local_seat() const { return state_->local_seat(); }
void GalacticLanMatch::configure_acceptance(std::filesystem::path directory, std::string campaign, bool stock) {
    if (!directory.empty()) state_->acceptance_ = std::make_unique<GalacticAcceptance>(std::move(directory), std::move(campaign), stock);
}

} // namespace eawr::presentation::godot_backend
