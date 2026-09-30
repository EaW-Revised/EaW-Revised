#include "eawr/platform/live_session.hpp"

#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace eawr::platform {
namespace {

namespace tactical = sim::tactical;
using Clock = std::chrono::steady_clock;

// TM-02: the wait between two logical frames is 1000 / target whole milliseconds.
[[nodiscard]] Clock::duration tick_interval(const std::uint32_t target_rate) {
    const std::uint32_t rate = std::clamp<std::uint32_t>(target_rate, 1U, 1000U);
    return std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(1000U / rate));
}

} // namespace

LiveEventLog::LiveEventLog(const Bounds bounds) noexcept : bounds_(bounds) {}

bool LiveEventLog::presented(const tactical::Event& event) noexcept {
    return event.kind == tactical::EventKind::unit_destroyed || event.kind == tactical::EventKind::hardpoint_destroyed
        || event.kind == tactical::EventKind::spin_away_started || event.kind == tactical::EventKind::spin_away_ended;
}

bool LiveEventLog::presented(const tactical::CombatEvent& event) noexcept {
    return event.kind == tactical::CombatEventKind::projectile_hit;
}

std::size_t LiveEventLog::record_bytes(const LiveTickEvents& record) noexcept {
    return sizeof(LiveTickEvents) + record.events.size() * sizeof(tactical::Event)
        + record.combat_events.size() * sizeof(tactical::CombatEvent);
}

void LiveEventLog::record(const tactical::TacticalSnapshot& snapshot) {
    LiveTickEvents kept{snapshot.completed_tick(), {}, {}};
    for (const tactical::Event& event : snapshot.events()) {
        if (presented(event)) kept.events.push_back(event);
    }
    for (const tactical::CombatEvent& event : snapshot.combat_events()) {
        if (presented(event)) kept.combat_events.push_back(event);
    }
    if (!kept.events.empty() || !kept.combat_events.empty()) {
        if (record_bytes(kept) > bounds_.bytes) {
            // Larger than the whole bound: dropped at once, the older ticks kept.
            if (!dropped_from_) dropped_from_ = kept.tick;
            dropped_through_ = kept.tick;
        } else {
            kept.events.shrink_to_fit();
            kept.combat_events.shrink_to_fit();
            bytes_ += record_bytes(kept);
            records_.push_back(std::move(kept));
        }
    }
    while (!records_.empty()
           && (records_.front().tick + bounds_.ticks <= snapshot.completed_tick() || bytes_ > bounds_.bytes)) {
        drop_front();
    }
}

void LiveEventLog::drop_front() {
    if (!dropped_from_) dropped_from_ = records_.front().tick;
    dropped_through_ = records_.front().tick;
    bytes_ -= record_bytes(records_.front());
    records_.pop_front();
}

LiveEvents LiveEventLog::after(const std::uint64_t after, const std::uint64_t through) const {
    LiveEvents result;
    // A dropped tick is only the reader's loss when it lies in the asked range.
    if (dropped_through_ && after < *dropped_through_ && *dropped_from_ <= through && after < through) {
        result.lost_through = std::min(*dropped_through_, through);
    }
    const auto first = std::partition_point(records_.begin(), records_.end(),
        [after](const LiveTickEvents& record) { return record.tick <= after; });
    for (auto record = first; record != records_.end() && record->tick <= through; ++record) {
        result.ticks.push_back(*record);
    }
    return result;
}

class LiveSession::Impl {
public:
    // Exactly one of `session` and `scripted` is set: the world alone, or the world inside the
    // scripted session that runs the scripts beside it (#79).
    Impl(std::optional<tactical::TacticalSession> session,
        std::unique_ptr<script::authoritative::ScriptedTacticalSession> scripted, const Options& options)
        : session_(std::move(session)), scripted_(std::move(scripted)), options_(options),
          event_log_({std::max(options.event_history, std::max<std::size_t>(options.history, 2)), options.event_bytes}) {
        options_.history = std::max<std::size_t>(options_.history, 2);
        options_.event_history = std::max(options_.event_history, options_.history);
        const auto zero = world().snapshot();
        history_.push_front(zero);
        if (auto fog = capture_fog()) fog_history_.push_front(std::move(fog));
        frame_ = {zero, zero, Clock::now()};
        interval_ = tick_interval(options_.target_rate);
        if (options_.pacing == Pacing::driven) target_ = world().completed_tick();
        thread_ = std::thread([this] { run(); });
    }

    ~Impl() { stop(); }

    void submit(LiveOrder order) {
        {
            const std::lock_guard lock(mutex_);
            inbox_.push_back(std::move(order));
        }
        wake_.notify_all();
    }

    void advance_to(const std::uint64_t tick) {
        {
            const std::lock_guard lock(mutex_);
            target_ = std::max(target_, tick);
        }
        wake_.notify_all();
    }

    void set_paused(const bool paused) {
        {
            const std::lock_guard lock(mutex_);
            if (paused_ == paused) return;
            paused_ = paused;
            rebase_ = true;
        }
        wake_.notify_all();
    }

    void set_target_rate(const std::uint32_t frames_per_second) {
        {
            const std::lock_guard lock(mutex_);
            const Clock::duration interval = tick_interval(frames_per_second);
            if (interval == interval_) return;
            interval_ = interval;
            rebase_ = true;
        }
        wake_.notify_all();
    }

    bool paused() const {
        const std::lock_guard lock(mutex_);
        return paused_;
    }

    void halt_at(const std::uint64_t tick) {
        {
            const std::lock_guard lock(mutex_);
            halt_ = halt_ ? std::min(*halt_, tick) : tick;
        }
        wake_.notify_all();
    }

    std::optional<std::uint64_t> halt_tick() const {
        const std::lock_guard lock(mutex_);
        return halt_;
    }

    bool wait_for(const std::uint64_t tick, const std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        return published_.wait_for(lock, timeout, [&] {
            return frame_.latest->completed_tick() >= tick || failure_ || stopping_;
        }) && frame_.latest->completed_tick() >= tick;
    }

    LiveFrame frame() const {
        const std::lock_guard lock(mutex_);
        return frame_;
    }

    std::shared_ptr<const tactical::TacticalSnapshot> snapshot_at(const std::uint64_t tick) const {
        const std::lock_guard lock(mutex_);
        for (const auto& snapshot : history_) {
            if (snapshot->completed_tick() == tick) return snapshot;
        }
        return nullptr;
    }

    std::shared_ptr<const LiveFog> fog_at(const std::uint64_t tick) const {
        const std::lock_guard lock(mutex_);
        for (const auto& fog : fog_history_) {
            if (fog->tick == tick) return fog;
        }
        return nullptr;
    }

    LiveEvents events_after(const std::uint64_t after, const std::uint64_t through) const {
        const std::lock_guard lock(mutex_);
        return event_log_.after(after, through);
    }

    std::uint64_t completed_tick() const {
        const std::lock_guard lock(mutex_);
        return frame_.latest->completed_tick();
    }

    std::vector<LiveTickCost> tick_costs_after(const std::uint64_t after) const {
        const std::lock_guard lock(mutex_);
        std::vector<LiveTickCost> costs;
        for (auto it = costs_.rbegin(); it != costs_.rend() && it->tick > after; ++it) costs.push_back(*it);
        std::reverse(costs.begin(), costs.end());
        return costs;
    }

    std::optional<core::Diagnostic> failure() const {
        const std::lock_guard lock(mutex_);
        return failure_;
    }

    std::vector<core::Diagnostic> rejected_orders() const {
        const std::lock_guard lock(mutex_);
        return rejected_;
    }

    std::size_t worker_count() const noexcept { return options_.workers; }

    void stop() {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        published_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    std::vector<std::string> tick_hashes() const {
        const std::lock_guard lock(session_mutex_);
        return hashes_;
    }

    tactical::TacticalReplay record() const {
        const std::lock_guard lock(session_mutex_);
        return scripted_ ? scripted_->record() : session_->record();
    }

    tactical::TacticalReplay failure_record() const {
        const std::lock_guard lock(session_mutex_);
        return world().record_through_next_tick();
    }

    LiveScriptReport script_report() const {
        const std::lock_guard lock(session_mutex_);
        return script_report_;
    }

private:
    [[nodiscard]] const tactical::TacticalSession& world() const {
        return scripted_ ? scripted_->world() : *session_;
    }

    // #494: a copy of Options::fog_player's cells as the world stands; null without them.
    [[nodiscard]] std::shared_ptr<const LiveFog> capture_fog() const {
        const tactical::FogCells* cells = world().fog_cells();
        if (!options_.fog_player || cells == nullptr) return nullptr;
        const auto players = world().players();
        for (std::size_t index = 0; index < players.size(); ++index) {
            if (players[index].player_id != *options_.fog_player) continue;
            const auto values = cells->values(index);
            return std::make_shared<const LiveFog>(LiveFog{world().completed_tick(), cells->rules(),
                *options_.fog_player, std::vector<std::uint8_t>(values.begin(), values.end())});
        }
        return nullptr;
    }

    // The simulation thread's entry. Nothing may escape it (std::terminate would take the
    // viewer down): an exception becomes the session's failure and the thread ends, so
    // waiters wake and stop() still joins.
    void run() noexcept {
        try {
            loop();
        } catch (const std::exception& error) {
            fail_thread("an exception: ", error.what());
        } catch (...) {
            // Also what a std::exception thrown by code built without C++ exceptions
            // (_HAS_EXCEPTIONS=0, as the Godot extension is) arrives as.
            fail_thread("a non-standard exception", "");
        }
    }

    // Simulation thread only: it alone advances session_.
    void fail_thread(const char* cause, const char* what) noexcept {
        try {
            core::Diagnostic diagnostic;
            diagnostic.code = std::string(tactical::diagnostic_codes::worker_failure);
            diagnostic.message = "the simulation thread stopped before tick " + std::to_string(world().completed_tick())
                + " on " + cause + what;
            const std::lock_guard lock(mutex_);
            failure_ = std::move(diagnostic);
        } catch (...) {
            // Nothing is left to report with; waiters still wake below and time out otherwise.
        }
        published_.notify_all();
    }

    // Worker 0 of the pool. Only it touches session_ while running.
    void loop() {
        const ThreadWorkerAdapter executor(options_.workers);
        auto clock_start = Clock::now();
        std::uint64_t clock_tick = world().completed_tick();
        Clock::duration interval{};
        for (;;) {
            bool held = false;
            {
                std::unique_lock lock(mutex_);
                // Paused, or at the halt tick: no tick may run (TM-07, BEP-02).
                const auto holding = [&] {
                    return paused_ || (halt_ && frame_.latest->completed_tick() >= *halt_);
                };
                const auto due = [&] {
                    if (stopping_ || failure_) return true;
                    if (!inbox_.empty() || rebase_) return true;
                    if (holding()) return false;
                    if (options_.pacing == Pacing::driven) return target_ > frame_.latest->completed_tick();
                    return false;
                };
                if (options_.pacing == Pacing::driven || holding()) {
                    wake_.wait(lock, due);
                } else {
                    const auto next = clock_start + interval_ * static_cast<std::int64_t>(
                        frame_.latest->completed_tick() + 1U - clock_tick);
                    wake_.wait_until(lock, next, due);
                }
                if (stopping_ || failure_) return;
                std::move(inbox_.begin(), inbox_.end(), std::back_inserter(held_));
                inbox_.clear();
                if (rebase_) {
                    // TP-01: a pause, resume or new rate restarts the clock at the current tick.
                    rebase_ = false;
                    clock_start = Clock::now();
                    clock_tick = frame_.latest->completed_tick();
                }
                interval = interval_;
                held = holding();
            }
            // Orders for a tick that already ran are refused now; the rest wait for their tick.
            submit_due(world().completed_tick(), false);
            if (held) continue;

            bool step_now = false;
            if (options_.pacing == Pacing::driven) {
                const std::lock_guard lock(mutex_);
                step_now = target_ > world().completed_tick();
            } else {
                const auto now = Clock::now();
                const auto elapsed = now - clock_start;
                const auto due_ticks = static_cast<std::uint64_t>(elapsed / interval);
                const std::uint64_t behind = clock_tick + due_ticks > world().completed_tick()
                    ? clock_tick + due_ticks - world().completed_tick() : 0U;
                if (behind > options_.max_catch_up) {
                    // Too slow for real time: drop the missed time rather than spiral.
                    clock_start = now - interval;
                    clock_tick = world().completed_tick();
                }
                step_now = behind > 0U;
            }
            if (!step_now) continue;
            submit_due(world().completed_tick(), true);
            if (!step(executor)) return;
        }
    }

    // Submits what is due before `next_tick` runs, in the session's canonical order: when
    // stepping, first the command source's commands with their own keys (UI-07), then the held
    // orders for `next_tick` and the untimed ones, which take it; without stepping only held
    // orders for executed ticks, which the session refuses. A held order takes its issuer's
    // next sequence, one past the last it accepted from either path, only now: a preloaded
    // later order never claims a key ahead of the issuer's earlier input.
    void submit_due(const std::uint64_t next_tick, const bool stepping) {
        std::vector<tactical::PlayerCommand> sourced;
        if (stepping && options_.command_source) sourced = options_.command_source(next_tick);
        std::vector<LiveOrder> due;
        std::vector<LiveOrder> kept;
        for (auto& order : held_) {
            const bool now = order.tick ? (*order.tick < next_tick || (stepping && *order.tick == next_tick)) : stepping;
            if (now && !order.tick) order.tick = next_tick;
            (now ? due : kept).push_back(std::move(order));
        }
        held_ = std::move(kept);
        if (sourced.empty() && due.empty()) return;
        std::stable_sort(due.begin(), due.end(),
            [](const LiveOrder& a, const LiveOrder& b) { return *a.tick < *b.tick; });
        std::vector<core::Diagnostic> refused;
        {
            const std::lock_guard lock(session_mutex_);
            for (const auto& command : sourced) {
                if (scripted_) {
                    // The scripted session submits it with the step; a refusal comes back then.
                    input_.push_back(command);
                    auto& sequence = sequences_[command.key.player_id];
                    sequence = std::max(sequence, command.key.sequence + 1U);
                } else if (auto submitted = session_->submit(command); !submitted) {
                    refused.push_back(submitted.error());
                } else {
                    auto& sequence = sequences_[command.key.player_id];
                    sequence = std::max(sequence, command.key.sequence + 1U);
                }
            }
            for (auto& order : due) {
                tactical::PlayerCommand command;
                auto& sequence = sequences_[order.issuer];
                command.key = {*order.tick, order.issuer, sequence};
                command.units = std::move(order.units);
                command.payload = std::move(order.payload);
                if (scripted_) {
                    input_.push_back(std::move(command));
                    ++sequence;
                } else if (auto submitted = session_->submit(command); !submitted) {
                    refused.push_back(submitted.error());
                } else {
                    ++sequence;
                }
            }
        }
        if (!refused.empty()) {
            const std::lock_guard lock(mutex_);
            rejected_.insert(rejected_.end(), refused.begin(), refused.end());
        }
    }

    bool step(const ThreadWorkerAdapter& executor) {
        std::vector<core::Diagnostic> script_refusals;
        std::shared_ptr<const LiveFog> fog;
        double step_ms = 0.0;
        double fog_ms = 0.0;
        core::Result<tactical::TacticalTick> tick = [&]() -> core::Result<tactical::TacticalTick> {
            const std::lock_guard lock(session_mutex_);
            const auto started = Clock::now();
            auto stepped = scripted_ ? step_scripted(executor, script_refusals) : session_->step(executor);
            const auto stepped_at = Clock::now();
            if (stepped && !scripted_) hashes_.push_back(stepped.value().state_sha256);
            if (stepped) fog = capture_fog();
            const auto finished = Clock::now();
            step_ms = std::chrono::duration<double, std::milli>(stepped_at - started).count();
            fog_ms = std::chrono::duration<double, std::milli>(finished - stepped_at).count();
            return stepped;
        }();
        if (!script_refusals.empty()) {
            const std::lock_guard lock(mutex_);
            rejected_.insert(rejected_.end(), script_refusals.begin(), script_refusals.end());
        }
        {
            const std::lock_guard lock(mutex_);
            if (!tick) {
                failure_ = tick.error();
            } else {
                rejected_.insert(rejected_.end(), tick.value().diagnostics.begin(), tick.value().diagnostics.end());
                frame_ = {frame_.latest, tick.value().snapshot, Clock::now()};
                history_.push_front(tick.value().snapshot);
                if (history_.size() > options_.history) history_.pop_back();
                if (fog) {
                    fog_history_.push_front(std::move(fog));
                    if (fog_history_.size() > options_.history) fog_history_.pop_back();
                }
                event_log_.record(*tick.value().snapshot);
                // #558: presentation-only wall-clock cost, never read by the simulation.
                costs_.push_back({tick.value().completed_tick, step_ms + fog_ms, {{"step", step_ms}, {"fog", fog_ms}}});
                if (costs_.size() > tick_cost_history) costs_.pop_front();
            }
        }
        published_.notify_all();
        return static_cast<bool>(tick);
    }

    // Under session_mutex_: one step of the scripted session with the queued input. Its world
    // hash is the tick hash; refused input and dropped script commands are refusals.
    core::Result<tactical::TacticalTick> step_scripted(const ThreadWorkerAdapter& executor,
        std::vector<core::Diagnostic>& refusals) {
        std::vector<tactical::PlayerCommand> input = std::move(input_);
        input_.clear();
        auto stepped = scripted_->step(executor, input);
        if (!stepped) return core::Result<tactical::TacticalTick>::failure(stepped.error());
        auto& result = stepped.value();
        hashes_.push_back(result.world.state_sha256);
        refusals = std::move(result.refused_input);
        for (const auto& routed : result.script_input) {
            if (!routed.submitted) {
                refusals.push_back(routed.dropped);
                continue;
            }
            auto& sequence = sequences_[routed.key.player_id];
            sequence = std::max(sequence, routed.key.sequence + 1U);
        }
        std::int64_t load = 0;
        for (const auto& entry : result.scripts.loads) load += entry.instructions;
        ++script_report_.ticks;
        script_report_.max_instructions = std::max(script_report_.max_instructions, load);
        script_report_.total_instructions += load;
        for (const auto& diagnostic : result.scripts.diagnostics) {
            std::string line = diagnostic.code + " " + diagnostic.message;
            if (script_report_.diagnostics.size() < 64 && seen_diagnostics_.insert(line).second) {
                script_report_.diagnostics.push_back(std::move(line));
            }
        }
        return core::Result<tactical::TacticalTick>::success(std::move(result.world));
    }

    std::optional<tactical::TacticalSession> session_;
    std::unique_ptr<script::authoritative::ScriptedTacticalSession> scripted_;
    // Scripted mode: the commands for the next step, submitted by it; the scripts' report.
    std::vector<tactical::PlayerCommand> input_;
    LiveScriptReport script_report_;
    std::set<std::string> seen_diagnostics_;
    Options options_;
    // Guards session_ and hashes_ against record() and tick_hashes() from other threads.
    mutable std::mutex session_mutex_;
    std::vector<std::string> hashes_;
    // Per issuer: one past the last sequence the session accepted, from either path.
    std::map<tactical::PlayerId, std::uint64_t> sequences_;
    // Simulation thread only: taken orders waiting for their tick, in arrival order.
    std::vector<LiveOrder> held_;

    mutable std::mutex mutex_;
    mutable std::condition_variable published_;
    std::condition_variable wake_;
    std::vector<LiveOrder> inbox_;
    std::uint64_t target_{};
    // #459 time controls and #453's halt.
    bool paused_{};
    bool rebase_{};
    Clock::duration interval_{};
    std::optional<std::uint64_t> halt_;
    bool stopping_{};
    std::optional<core::Diagnostic> failure_;
    std::vector<core::Diagnostic> rejected_;
    LiveFrame frame_;
    std::deque<std::shared_ptr<const tactical::TacticalSnapshot>> history_;
    // #494: Options::fog_player's cells beside the snapshot history, newest first.
    std::deque<std::shared_ptr<const LiveFog>> fog_history_;
    // The presentation event log (Options::event_history, Options::event_bytes).
    LiveEventLog event_log_;
    // #558: the newest ticks' cost, oldest first.
    std::deque<LiveTickCost> costs_;
    std::thread thread_;
};

std::size_t LiveSession::game_worker_count() noexcept {
    const std::size_t hardware = ThreadWorkerAdapter::hardware_worker_count();
    return hardware > 3 ? hardware - 2 : 1;
}

core::Result<std::unique_ptr<LiveSession>> LiveSession::start(
    const tactical::TacticalSetup& setup,
    const std::span<const tactical::SensorProfile> sensors,
    const tactical::DurabilityTable& durability,
    const tactical::MotionTable& motion,
    const tactical::CombatTable& combat,
    const Options options,
    const tactical::VictoryRules& victory,
    const std::optional<tactical::FogRules>& fog,
    const tactical::AbilityTable& abilities) {
    using StartResult = core::Result<std::unique_ptr<LiveSession>>;
    if (options.workers == 0 || options.workers > ThreadWorkerAdapter::max_worker_count) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(tactical::diagnostic_codes::worker_failure);
        diagnostic.message = "live session worker count must be 1 to 256";
        return StartResult::failure(std::move(diagnostic));
    }
    auto session = tactical::TacticalSession::create(
        setup, sensors, durability, motion, fog, combat, victory, abilities);
    if (!session) return StartResult::failure(session.error());
    if (options.scripts && options.scripts->wrap) {
        auto scripted = options.scripts->wrap(std::move(session).value());
        if (!scripted) return StartResult::failure(scripted.error());
        return StartResult::success(std::unique_ptr<LiveSession>(new LiveSession(std::make_unique<Impl>(std::nullopt,
            std::make_unique<script::authoritative::ScriptedTacticalSession>(std::move(scripted).value()), options))));
    }
    return StartResult::success(std::unique_ptr<LiveSession>(
        new LiveSession(std::make_unique<Impl>(std::move(session).value(), nullptr, options))));
}

core::Result<std::vector<std::string>> headless_tick_hashes(
    const tactical::TacticalReplay& replay,
    const std::span<const tactical::SensorProfile> sensors,
    const tactical::DurabilityTable& durability,
    const tactical::MotionTable& motion,
    const tactical::CombatTable& combat,
    const tactical::VictoryRules& victory,
    const std::optional<tactical::FogRules>& fog, const tactical::AbilityTable& abilities) {
    using HashResult = core::Result<std::vector<std::string>>;
    auto session = tactical::TacticalSession::from_replay(
        replay, sensors, durability, motion, fog, combat, victory, abilities);
    if (!session) return HashResult::failure(session.error());
    const sim::InlineExecutor executor;
    std::vector<std::string> hashes;
    while (session.value().completed_tick() < replay.final_tick_count) {
        auto tick = session.value().step(executor);
        if (!tick) return HashResult::failure(tick.error());
        hashes.push_back(std::move(tick.value().state_sha256));
    }
    return HashResult::success(std::move(hashes));
}

LiveSession::LiveSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LiveSession::~LiveSession() = default;

void LiveSession::submit(LiveOrder order) { impl_->submit(std::move(order)); }
void LiveSession::advance_to(const std::uint64_t tick) { impl_->advance_to(tick); }
void LiveSession::set_paused(const bool paused) { impl_->set_paused(paused); }
void LiveSession::set_target_rate(const std::uint32_t frames_per_second) { impl_->set_target_rate(frames_per_second); }
bool LiveSession::paused() const { return impl_->paused(); }
void LiveSession::halt_at(const std::uint64_t tick) { impl_->halt_at(tick); }
std::optional<std::uint64_t> LiveSession::halt_tick() const { return impl_->halt_tick(); }
bool LiveSession::wait_for(const std::uint64_t tick, const std::chrono::milliseconds timeout) const {
    return impl_->wait_for(tick, timeout);
}
LiveFrame LiveSession::frame() const { return impl_->frame(); }
std::shared_ptr<const tactical::TacticalSnapshot> LiveSession::snapshot_at(const std::uint64_t tick) const {
    return impl_->snapshot_at(tick);
}
std::shared_ptr<const LiveFog> LiveSession::fog_at(const std::uint64_t tick) const { return impl_->fog_at(tick); }
LiveEvents LiveSession::events_after(const std::uint64_t after, const std::uint64_t through) const {
    return impl_->events_after(after, through);
}
std::uint64_t LiveSession::completed_tick() const { return impl_->completed_tick(); }
std::vector<LiveTickCost> LiveSession::tick_costs_after(const std::uint64_t after) const {
    return impl_->tick_costs_after(after);
}
std::optional<core::Diagnostic> LiveSession::failure() const { return impl_->failure(); }
std::vector<core::Diagnostic> LiveSession::rejected_orders() const { return impl_->rejected_orders(); }
std::size_t LiveSession::worker_count() const noexcept { return impl_->worker_count(); }
LiveScriptReport LiveSession::script_report() const { return impl_->script_report(); }
void LiveSession::stop() { impl_->stop(); }
std::vector<std::string> LiveSession::tick_hashes() const { return impl_->tick_hashes(); }
tactical::TacticalReplay LiveSession::record() const { return impl_->record(); }

tactical::TacticalReplay LiveSession::failure_record() const { return impl_->failure_record(); }

} // namespace eawr::platform
