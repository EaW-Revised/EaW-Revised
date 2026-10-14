#pragma once

#include "eawr/platform/live_session.hpp"

namespace eawr::platform {

// ADR-019 / A2: the battle view reads one immutable feed and sends commands.
// An adapter may own a standalone live battle or borrow the game's retained child.
// Neither the view nor this contract exposes mutable simulation state.
class TacticalFeed {
public:
    virtual ~TacticalFeed() = default;
    virtual void submit(LiveOrder order) = 0;
    // Retained feeds use ordinary queued admission; standalone recovery can
    // refuse immediately while its replacement world is staged.
    virtual core::Result<void> try_submit(LiveOrder order) {
        submit(std::move(order)); return core::Result<void>::success();
    }
    [[nodiscard]] virtual std::optional<core::Diagnostic> submission_notice() const { return {}; }
    virtual void advance_to(std::uint64_t tick) = 0;
    virtual void set_paused(bool paused) = 0;
    [[nodiscard]] virtual bool paused() const = 0;
    virtual void set_target_rate(std::uint32_t rate) = 0;
    virtual void halt_at(std::uint64_t tick) = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> halt_tick() const = 0;
    [[nodiscard]] virtual bool wait_for(std::uint64_t tick, std::chrono::milliseconds timeout) const = 0;
    [[nodiscard]] virtual LiveFrame frame() const = 0;
    [[nodiscard]] virtual std::shared_ptr<const sim::tactical::TacticalSnapshot> snapshot_at(std::uint64_t tick) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const LiveFog> fog_at(std::uint64_t tick) const = 0;
    [[nodiscard]] virtual LiveEvents events_after(std::uint64_t after, std::uint64_t through) const = 0;
    [[nodiscard]] virtual std::vector<LiveTickCost> tick_costs_after(std::uint64_t after) const = 0;
    [[nodiscard]] virtual std::uint64_t completed_tick() const = 0;
    [[nodiscard]] virtual std::optional<core::Diagnostic> failure() const = 0;
    [[nodiscard]] virtual std::vector<core::Diagnostic> rejected_orders() const = 0;
    [[nodiscard]] virtual std::size_t worker_count() const noexcept = 0;
    [[nodiscard]] virtual LiveScriptReport script_report() const = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual std::vector<std::string> tick_hashes() const = 0;
    [[nodiscard]] virtual sim::tactical::TacticalReplay record() const = 0;
    [[nodiscard]] virtual sim::tactical::TacticalReplay failure_record() const = 0;
    [[nodiscard]] virtual std::optional<bool> reinforcement_point(sim::tactical::PlayerId player,
        sim::tactical::TypeId type, const sim::math::Vec3& point, std::optional<sim::math::Fixed> facing = {}) const = 0;
    // WHE-40: counts and the snapshot they were read at, taken under one lock.
    [[nodiscard]] virtual LiveSession::ProductionView production_view(sim::tactical::PlayerId player,
        sim::tactical::TypeId type) const = 0;
    // Borrowed feeds install the seat's scheduler before opening the readiness gate.
    virtual void command_source(std::function<std::vector<sim::tactical::PlayerCommand>(std::uint64_t)> source) = 0;
    // Authored base types opt into the same durable attack journal as M2.
    virtual void configure_notifications(std::vector<sim::tactical::TypeId>) {}
};

class StandaloneTacticalFeed final : public TacticalFeed {
public:
    explicit StandaloneTacticalFeed(std::unique_ptr<LiveSession> session) : session_(std::move(session)) {}
    void submit(LiveOrder order) override { (void)try_submit(std::move(order)); }
    core::Result<void> try_submit(LiveOrder order) override { return session_->submit(std::move(order)); }
    std::optional<core::Diagnostic> submission_notice() const override { return session_->submission_notice(); }
    void advance_to(std::uint64_t tick) override { session_->advance_to(tick); }
    void set_paused(bool paused) override { session_->set_paused(paused); }
    bool paused() const override { return session_->paused(); }
    void set_target_rate(std::uint32_t rate) override { session_->set_target_rate(rate); }
    void halt_at(std::uint64_t tick) override { session_->halt_at(tick); }
    std::optional<std::uint64_t> halt_tick() const override { return session_->halt_tick(); }
    bool wait_for(std::uint64_t tick, std::chrono::milliseconds timeout) const override { return session_->wait_for(tick, timeout); }
    LiveFrame frame() const override { return session_->frame(); }
    std::shared_ptr<const sim::tactical::TacticalSnapshot> snapshot_at(std::uint64_t tick) const override { return session_->snapshot_at(tick); }
    std::shared_ptr<const LiveFog> fog_at(std::uint64_t tick) const override { return session_->fog_at(tick); }
    LiveEvents events_after(std::uint64_t after, std::uint64_t through) const override { return session_->events_after(after, through); }
    std::vector<LiveTickCost> tick_costs_after(std::uint64_t after) const override { return session_->tick_costs_after(after); }
    std::uint64_t completed_tick() const override { return session_->completed_tick(); }
    std::optional<core::Diagnostic> failure() const override { return session_->failure(); }
    std::vector<core::Diagnostic> rejected_orders() const override { return session_->rejected_orders(); }
    std::size_t worker_count() const noexcept override { return session_->worker_count(); }
    LiveScriptReport script_report() const override { return session_->script_report(); }
    void stop() override { session_->stop(); }
    std::vector<std::string> tick_hashes() const override { return session_->tick_hashes(); }
    sim::tactical::TacticalReplay record() const override { return session_->record(); }
    sim::tactical::TacticalReplay failure_record() const override { return session_->failure_record(); }
    std::optional<bool> reinforcement_point(sim::tactical::PlayerId player, sim::tactical::TypeId type,
        const sim::math::Vec3& point, std::optional<sim::math::Fixed> facing = {}) const override {
        return session_->reinforcement_point(player, type, point, facing);
    }
    LiveSession::ProductionView production_view(sim::tactical::PlayerId player, sim::tactical::TypeId type) const override {
        return session_->production_view(player, type);
    }
    void command_source(std::function<std::vector<sim::tactical::PlayerCommand>(std::uint64_t)>) override {}
private:
    std::unique_ptr<LiveSession> session_;
};

// UI-C2: the caller has admitted a finished battle. Join before recording, exactly as
// the replay-output path does; the serializer receives no presentation state.
[[nodiscard]] inline core::Result<std::vector<std::uint8_t>> serialize_finished_replay(TacticalFeed& feed) {
    feed.stop();
    return sim::tactical::write_replay(feed.record());
}

} // namespace eawr::platform
