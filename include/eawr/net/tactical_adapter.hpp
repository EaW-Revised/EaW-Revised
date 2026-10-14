#pragma once

#include "eawr/net/tactical_input.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <memory>
#include <utility>
#include <set>

namespace eawr::net {

class TacticalSessionAdapter final : public SessionAdapter {
public:
    TacticalSessionAdapter(sim::tactical::TacticalSession session, const sim::PartitionExecutor& executor,
        std::vector<sim::tactical::SeatBinding> bindings);
    std::uint64_t completed_tick() const override;
    std::string state_hash() const override;
    std::vector<std::uint8_t> canonical_state() const override;
    core::Result<void> validate(const SeatInput& input) const override;
    core::Result<void> install(std::span<const SeatInput> canonical) override;
    core::Result<std::vector<std::uint8_t>> canonical_input() const override;
    core::Result<CompletedTick> step() override;
    bool is_departure(const SeatInput& input) const override;
    [[nodiscard]] std::optional<sim::tactical::PlayerId> player_for_seat(sim::SeatId seat) const;
    [[nodiscard]] std::span<const sim::tactical::SeatBinding> bindings() const noexcept { return bindings_; }
    // Trusted engine-side commands are generated locally on EVERY peer in this same
    // completeness/monitor-gated step, never sent as human wire commands (CP-2).
    // The platform/script owner uses the existing skirmish AI host through these hooks.
    using EngineStep = std::function<core::Result<sim::tactical::TacticalTick>(
        sim::tactical::TacticalSession&, const sim::PartitionExecutor&)>;
    using TakeoverHandler = std::function<core::Result<void>(sim::tactical::PlayerId, std::uint64_t)>;
    void set_engine_step(EngineStep step) { engine_step_ = std::move(step); }
    void set_takeover_handler(TakeoverHandler handler) { takeover_handler_ = std::move(handler); }
    [[nodiscard]] const sim::tactical::TacticalSession& simulation() const noexcept { return session_; }
    [[nodiscard]] std::optional<sim::tactical::TacticalTick> take_tick() { return std::exchange(last_tick_, std::nullopt); }
private:
    [[nodiscard]] const sim::tactical::SeatBinding* binding(sim::SeatId seat) const;
    sim::tactical::TacticalSession session_;
    const sim::PartitionExecutor* executor_{};
    std::vector<sim::tactical::SeatBinding> bindings_;
    std::set<sim::SeatId> departed_seats_; // Routing state; sealed recovery retains original binding authorization.
    std::map<sim::tactical::PlayerId, std::uint64_t> faction_sequences_;
    std::vector<sim::tactical::PlayerCommand> routed_;
    std::optional<sim::tactical::TacticalTick> last_tick_;
    EngineStep engine_step_;
    TakeoverHandler takeover_handler_;
};

struct TacticalPumpResult {
    std::optional<sim::tactical::TacticalTick> completed;
    std::vector<sim::SeatCommandKey> quits;
    std::vector<DepartureOutcome> departure_outcomes;
};

// Tactical convenience owner. Only agreed setup/content can enter; prequeued simulation
// commands cannot bypass the complete-batch boundary. Executor/transport/monitor outlive it.
class TacticalLockstepSession final {
public:
    [[nodiscard]] static core::Result<TacticalLockstepSession> create(
        const sim::tactical::TacticalSetup& setup, const sim::tactical::TacticalContent& content,
        MessageTransport& transport, const sim::PartitionExecutor& executor,
        std::vector<sim::tactical::SeatBinding> bindings, SessionOptions options = {}, SessionMonitor* monitor = nullptr);
    [[nodiscard]] core::Result<sim::SeatCommandKey> submit(const LocalIntent& intent,
        std::optional<std::uint64_t> input_epoch = std::nullopt);
    [[nodiscard]] core::Result<TacticalPumpResult> pump(std::uint64_t now_ms);
    [[nodiscard]] const std::optional<WaitingFor>& waiting_for() const noexcept { return core_.waiting_for(); }
    [[nodiscard]] const sim::tactical::TacticalSession& simulation() const noexcept { return adapter_->simulation(); }
    [[nodiscard]] const std::optional<core::Diagnostic>& failure() const noexcept { return core_.failure(); }
    void set_engine_step(TacticalSessionAdapter::EngineStep step) { adapter_->set_engine_step(std::move(step)); }
    void set_takeover_handler(TacticalSessionAdapter::TakeoverHandler handler) { adapter_->set_takeover_handler(std::move(handler)); }
    [[nodiscard]] const std::vector<DepartureOutcome>& departure_outcomes() const noexcept { return outcomes_; }
    void set_message_handler(std::function<core::Result<void>(Received)> handler) { core_.set_message_handler(std::move(handler)); }
    void set_stage_observer(LockstepSession::StageObserver observer) { core_.set_stage_observer(std::move(observer)); }
    void set_departure_handler(std::function<core::Result<void>(std::span<const sim::SeatCommandKey>)> handler) {
        core_.set_departure_handler(std::move(handler));
    }
private:
    TacticalLockstepSession(std::unique_ptr<TacticalSessionAdapter> adapter, LockstepSession session,
        sim::tactical::SeatBinding local)
        : adapter_(std::move(adapter)), core_(std::move(session)), local_(local) {}
    std::unique_ptr<TacticalSessionAdapter> adapter_;
    LockstepSession core_;
    sim::tactical::SeatBinding local_;
    std::vector<DepartureOutcome> outcomes_;
};

} // namespace eawr::net
