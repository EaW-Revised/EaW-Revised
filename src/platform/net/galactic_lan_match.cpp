#include "galactic_lan_match.hpp"

#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/galactic_adapter.hpp"

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
        for (const auto& order : orders) submit(context, order);
        // The engine-side source (GC AI) is borrowed for this call only: it runs inside the gated step on every peer.
        if (context.command_source)
            adapter_->set_engine_source([&context](const campaign::GameSession&, std::uint64_t tick) {
                return core::Result<std::vector<galactic::GameCommand>>::success(context.command_source(tick));
            });
        gate_.advance_allowed = context.advance_allowed;
        auto pumped = session_->pump(now);
        adapter_->set_engine_source({});
        if (!pumped) return end_with_failure(pumped.error());
        std::optional<campaign::GameTick> tick;
        if (pumped.value().completed) {
            tick = adapter_->take_tick();
            if (!tick) return end_with_failure(make_diagnostic(net::session_errors::simulation,
                "the galaxy adapter completed a tick without publishing it"));
        }
        refresh(pumped.value(), context.advance_allowed, now);
        return Tick::success(std::move(tick));
    }

    void session_ended() noexcept {
        session_ended_ = true;
        session_.reset();
        adapter_.reset();
    }

private:
    core::Result<void> open(platform::LiveGameStepContext& context) {
        if (options_.seats.empty() || options_.seats.size() != transport_.peer_count() || transport_.self() >= options_.seats.size())
            return core::Result<void>::failure(make_diagnostic(net::session_errors::options,
                "the galactic LAN roster must list exactly one seat per transport peer"));
        net::DesyncOptions monitor_options;
        monitor_options.input_delay = options_.input_delay;
        monitor_options.exchange_every = options_.exchange_every;
        monitor_.emplace(transport_, monitor_options);
        gate_.monitor = &*monitor_;
        adapter_.emplace(context.game, context.executor);
        net::SessionOptions session_options;
        session_options.input_delay = options_.input_delay;
        session_options.final_tick_count = options_.final_tick_count;
        session_options.completeness_timeout_ms = options_.completeness_timeout_ms;
        session_options.unconfirmed_departure = net::UnconfirmedDeparture::continue_alone; // D5 configured fallback; D8 no rejoin
        auto created = net::LockstepSession::create(*adapter_, transport_, options_.seats, session_options, &gate_);
        if (!created) return core::Result<void>::failure(created.error());
        session_.emplace(std::move(created).value());
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
        if (view_.completed_tick >= options_.final_tick_count && monitor_->agreed_end()) {
            view_.phase = GalacticLanPhase::ended;
            view_.reason = GalacticLanReason::completed;
            return;
        }
        bool connected = monitor_->setup_agreed();
        for (net::PeerIndex peer = 0; peer < transport_.peer_count(); ++peer)
            if (peer != transport_.self() && transport_.link(peer) == net::LinkState::connecting) connected = false;
        view_.waiting_for.clear();
        if (!connected) view_.phase = GalacticLanPhase::connecting;
        else if (!wait_peers_.empty() && now >= wait_since_ && now - wait_since_ >= options_.waiting_notice_ms) {
            view_.phase = GalacticLanPhase::waiting_for_peer;
            view_.waiting_for = wait_peers_;
        } else if (!view_.departed.empty()) view_.phase = GalacticLanPhase::departed;
        else view_.phase = GalacticLanPhase::running;
    }

    net::MessageTransport& transport_;
    GalacticLanOptions options_;
    mutable std::mutex mutex_;
    GalacticLanView view_;
    std::optional<core::Diagnostic> failed_;
    bool session_ended_{};
    // Declaration order is destruction order reversed: the session goes before the adapter, gate and monitor it uses.
    std::optional<net::DesyncMonitor> monitor_;
    CadenceGate gate_;
    std::optional<net::GalacticSessionAdapter> adapter_;
    std::optional<net::LockstepSession> session_;
    std::vector<net::PeerIndex> wait_peers_;
    std::uint64_t wait_since_{};
};

GalacticLanMatch::GalacticLanMatch(net::MessageTransport& transport, GalacticLanOptions options)
    : state_(std::make_shared<State>(transport, std::move(options))) {}
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
    private:
        std::shared_ptr<State> state_;
    };
    return std::make_shared<Driver>(state_);
}
GalacticLanView GalacticLanMatch::view() const { return state_->view(); }
sim::SeatId GalacticLanMatch::local_seat() const { return state_->local_seat(); }

} // namespace eawr::presentation::godot_backend
