#include "eawr/core/load_profile.hpp"
#include "eawr/core/sha256.hpp"
#include <stdexcept>
#include "eawr/platform/live_session.hpp"

#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/core/tick_profile.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace eawr::platform {
namespace {

namespace tactical = sim::tactical;
using Clock = std::chrono::steady_clock;

// Preserve the executor's dispatch policy and partition callbacks. Only the caller
// records completed barrier rows; workers write only their fixed callback/job slots.
class MeasuredExecutor final : public sim::PartitionExecutor {
public:
    explicit MeasuredExecutor(const sim::PartitionExecutor& executor) : executor_(executor) {}
    std::size_t worker_count() const noexcept override { return executor_.worker_count(); }
    core::Result<void> execute(const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        return execute_phase({}, count, partition);
    }
    core::Result<void> execute_phase(const std::string_view name, const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        const auto run = [&] { return name.empty() ? executor_.execute(count, partition)
            : executor_.execute_phase(name, count, partition); };
        // Nested calls may originate on any worker. Keep their original inline
        // execution and count them inside the enclosing barrier, exactly once.
        if (std::this_thread::get_id() != owner_ || measuring_) return run();
        struct Reset final { bool& flag; ~Reset() { flag = false; } } reset{measuring_};
        measuring_ = true;
        // Each callback owns its partition slot. These wall samples distinguish
        // a long callback from the surrounding dispatch/join interval; they are
        // separate diagnostic rows, never additional worker barrier cost.
        std::vector<double> partition_ms(count);
        const bool sample_jobs = name == "plan-searches" || name == "script-instances";
        std::vector<core::tick_profile::Recording> job_recordings(sample_jobs ? count : 0);
        const auto job_count = core::tick_profile::active
            ? std::exchange(core::tick_profile::active->pending_jobs, std::size_t{}) : 0;
        for (std::size_t index = 0; index < job_recordings.size(); ++index) {
            job_recordings[index].costs.reserve((index + 1) * job_count / count - index * job_count / count);
            if (name == "script-instances") job_recordings[index].auxiliary_costs = {{"memory-settle", 0.0}};
        }
        const auto started = Clock::now();
        auto result = [&] {
            const core::tick_profile::Pause pause;
            const auto sampled = [&](const std::size_t index) {
                const auto began = Clock::now();
                if (sample_jobs) {
                    // Job capture is independent of serial scopes; its vector
                    // was reserved on the caller thread before dispatch.
                    core::tick_profile::JobCapture capture(job_recordings[index]);
                    partition(index);
                } else partition(index);
                partition_ms[index] = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
            };
            return name.empty() ? executor_.execute(count, sampled) : executor_.execute_phase(name, count, sampled);
        }();
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (core::tick_profile::active) core::tick_profile::active->barriers_ms += elapsed;
        const std::string label = name.empty() ? std::string("unnamed") : std::string(name);
        const auto phase_index = phases.size();
        phases.push_back({"worker." + label, elapsed});
        double maximum = 0.0, sum = 0.0;
        for (const auto sample : partition_ms) { maximum = std::max(maximum, sample); sum += sample; }
        partition_costs.push_back({"partition-max." + label, maximum});
        partition_costs.push_back({"partition-sum." + label, sum});
        for (std::size_t index = 0; index < job_recordings.size(); ++index) {
            partition_costs.push_back({"partition-callback." + label + "." + std::to_string(phase_index)
                + "." + std::to_string(index), partition_ms[index]});
            for (const auto& job : job_recordings[index].costs) {
                if (!job.name.starts_with("job.")) continue;
                std::string job_label = "partition-job." + label + "." + std::to_string(phase_index)
                    + "." + std::to_string(index)
                    + "." + std::string(job.name.substr(4)) + "." + std::to_string(job.identity);
                // Fixed suffix: has-work, before, after, deadline, limit,
                // children, queries, leaves, narrow. Non-search jobs use zeros.
                job_label += job.work.deadline != 0 ? ".1" : ".0";
                for (const auto value : {job.work.before, job.work.after, job.work.deadline, job.work.limit,
                        job.work.children, job.work.queries, job.work.leaves, job.work.narrow})
                    job_label += "." + std::to_string(value);
                partition_costs.push_back({std::move(job_label), job.ms});
            }
            for (const auto& auxiliary : job_recordings[index].auxiliary_costs)
                partition_costs.push_back({"partition-aux." + label + "." + std::to_string(phase_index)
                    + "." + std::to_string(index) + "." + std::string(auxiliary.name), auxiliary.ms});
        }
        return result;
    }
    mutable std::vector<LivePhaseCost> phases;
    mutable std::vector<LivePhaseCost> partition_costs;
private:
    const sim::PartitionExecutor& executor_;
    const std::thread::id owner_{std::this_thread::get_id()};
    mutable bool measuring_{};
};

} // namespace

std::chrono::milliseconds LiveSession::tick_interval(const std::uint32_t target_rate) noexcept {
    const std::uint32_t rate = std::clamp<std::uint32_t>(target_rate, 1U, 1000U);
    return std::chrono::milliseconds(1000U / rate);
}

LiveEventLog::LiveEventLog(const Bounds bounds) noexcept : bounds_(bounds) {}

bool LiveEventLog::presented(const tactical::Event& event) noexcept {
    return event.kind == tactical::EventKind::unit_destroyed || event.kind == tactical::EventKind::hardpoint_destroyed
        || event.kind == tactical::EventKind::spin_away_started || event.kind == tactical::EventKind::spin_away_ended
        || event.kind == tactical::EventKind::reinforcement_unloaded || event.kind == tactical::EventKind::station_replaced
        // WBP-31: the sale consumer needs the sold child's identity after removal.
        || event.kind == tactical::EventKind::pad_structure_sold
        // Pad capture and construction cues use the XML-authored sounds in the audio consumer.
        || event.kind == tactical::EventKind::pad_captured
        || event.kind == tactical::EventKind::pad_construction_started
        || event.kind == tactical::EventKind::pad_construction_completed
        // WSW-33/34: the special weapon speech and pulse sounds are presented from these events.
        || event.kind == tactical::EventKind::special_weapon_fired || event.kind == tactical::EventKind::special_weapon_ready;
}

bool LiveEventLog::presented(const tactical::CombatEvent& event) noexcept {
    // AB-66/WAD-38: retain override shots so the view associates their projectile look;
    // ordinary shots and acquisitions stay out.
    return event.kind == tactical::CombatEventKind::projectile_hit
        || event.kind == tactical::CombatEventKind::projectile_expired
        || event.kind == tactical::CombatEventKind::point_defence_zap
        || event.kind == tactical::CombatEventKind::heal_pulse
        || (event.kind == tactical::CombatEventKind::weapon_fired
            && (event.outcome & (tactical::fired_ability_shot | tactical::fired_barrage_shot)) != 0U);
}

std::size_t LiveEventLog::record_bytes(const LiveTickEvents& record) noexcept {
    std::size_t bytes = sizeof(LiveTickEvents) + record.events.size() * sizeof(tactical::Event)
        + record.combat_events.size() * sizeof(tactical::CombatEvent)
        + record.asteroid_impacts.size() * sizeof(tactical::AsteroidImpact)
        + record.base_attacks.size() * sizeof(LiveBaseAttack)
        + record.hardpoint_losses.size() * sizeof(LiveHardpointLoss)
        + record.damage_changes.size() * sizeof(tactical::DamageChange);
    for (const auto& loss : record.hardpoint_losses) bytes += loss.standing.size() * sizeof(std::uint32_t);
    return bytes;
}

void LiveEventLog::record(const tactical::TacticalSnapshot& snapshot, const std::span<const tactical::AsteroidImpact> asteroid_impacts,
    const std::span<const tactical::TypeId> base_types, const std::span<const tactical::DamageChange> damage_changes) {
    LiveTickEvents kept{snapshot.completed_tick(), {}, {}, {asteroid_impacts.begin(), asteroid_impacts.end()}, {}, {}};
    kept.damage_changes.assign(damage_changes.begin(), damage_changes.end());
    bool ability_input = false;
    for (const tactical::Event& event : snapshot.events()) {
        if (presented(event)) kept.events.push_back(event);
        ability_input = ability_input || (event.kind == tactical::EventKind::order_accepted
            && event.order == tactical::OrderKind::ability);
    }
    // BA-64: retain the warning inputs before history eviction. Later losses in this
    // tick were still standing for an earlier notification; disabled is not destroyed.
    const auto destruction_events = snapshot.events();
    for (std::size_t at = 0; at < destruction_events.size(); ++at) {
        const auto& event = destruction_events[at];
        if (event.kind != tactical::EventKind::hardpoint_destroyed) continue;
        const auto instances = snapshot.instances();
        const auto parent = std::lower_bound(instances.begin(), instances.end(), event.unit,
            [](const auto& instance, const sim::EntityId id) { return instance.entity_id < id; });
        // WSD-04: self-destruct emits unit destruction and removes its parent before
        // publication. Missing parents therefore cannot produce hardpoint warnings.
        if (parent == instances.end() || parent->entity_id != event.unit || !parent->durability
            || parent->durability->hull <= sim::math::Fixed{}) continue;
        const auto& points = parent->durability->hardpoints;
        if (event.hardpoint >= points.size()) continue;
        LiveHardpointLoss loss;
        loss.unit = event.unit; loss.type = parent->type_id; loss.owner = parent->owner;
        loss.hardpoint = event.hardpoint;
        loss.weapon = points[event.hardpoint].role == tactical::HardpointRole::weapon;
        for (std::size_t index = 0; index < points.size(); ++index) {
            if (index == event.hardpoint) continue;
            const bool later_loss = std::any_of(destruction_events.begin() + static_cast<std::ptrdiff_t>(at + 1),
                destruction_events.end(), [&](const auto& later) {
                    return later.kind == tactical::EventKind::hardpoint_destroyed && later.unit == event.unit
                        && later.hardpoint == index;
                });
            if (points[index].state == tactical::HardpointState::destroyed && !later_loss) continue;
            loss.standing.push_back(static_cast<std::uint32_t>(index));
            loss.weapons_remaining = loss.weapons_remaining || points[index].role == tactical::HardpointRole::weapon;
        }
        loss.standing.shrink_to_fit();
        kept.hardpoint_losses.push_back(std::move(loss));
    }
    for (const tactical::CombatEvent& event : snapshot.combat_events()) {
        if (presented(event)) kept.combat_events.push_back(event);
        if (event.kind != tactical::CombatEventKind::weapon_fired || base_types.empty()) continue;
        const auto find = [&](const sim::EntityId id) -> const tactical::TacticalInstance* {
            const auto instances = snapshot.instances();
            const auto found = std::lower_bound(instances.begin(), instances.end(), id,
                [](const auto& instance, const sim::EntityId value) { return instance.entity_id < value; });
            return found != instances.end() && found->entity_id == id ? &*found : nullptr;
        };
        const auto* target = find(event.target);
        const auto* shooter = find(event.shooter);
        if (!target || !shooter || !std::binary_search(base_types.begin(), base_types.end(), target->type_id)) continue;
        kept.base_attacks.push_back({target->entity_id, target->type_id, target->owner, shooter->owner,
            {target->fixed_transform.rows[0][3], target->fixed_transform.rows[1][3], target->fixed_transform.rows[2][3]}});
    }
    // WHE-61/62: a countdown can expire without a combat event. Remember its sparse
    // presentation frame while the metadata is alive; bomb metadata may vanish on expiry.
    const auto tick = snapshot.completed_tick();
    for (const auto& spawn : snapshot.ability_spawns()) {
        if (spawn.detonated || spawn.due < tick || spawn.due == std::numeric_limits<std::uint64_t>::max()
            || spawn.due - tick >= bounds_.ticks) continue;
        const auto due = spawn.due + 1;
        if (!ability_due_ticks_.contains(due)) ability_due_ticks_.insert(due);
    }
    const bool ability_due = ability_due_ticks_.contains(tick);
    ability_due_ticks_.erase(ability_due_ticks_.begin(), ability_due_ticks_.upper_bound(tick));
    // WHE-63: accepted instant commands likewise need their frame retained, without
    // adding an authoritative event or scanning every unit for presentation state.
    if (ability_input || ability_due || !kept.events.empty() || !kept.combat_events.empty() || !kept.asteroid_impacts.empty() || !kept.base_attacks.empty() || !kept.damage_changes.empty()) {
        if (record_bytes(kept) > bounds_.bytes) {
            // Larger than the whole bound: dropped at once, the older ticks kept.
            if (!dropped_from_) dropped_from_ = kept.tick;
            dropped_through_ = kept.tick;
        } else {
            kept.events.shrink_to_fit();
            kept.combat_events.shrink_to_fit();
            kept.damage_changes.shrink_to_fit();
            kept.asteroid_impacts.shrink_to_fit();
            kept.base_attacks.shrink_to_fit();
            kept.hardpoint_losses.shrink_to_fit();
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
        std::unique_ptr<script::authoritative::ScriptedTacticalSession> scripted, const Options& options,
        std::function<core::Result<tactical::TacticalSession>()> rebuild_world, bool launch = true)
        : session_(std::move(session)), scripted_(std::move(scripted)), options_(options),
          event_log_({std::max(options.event_history, std::max<std::size_t>(options.history, 2)), options.event_bytes}),
          rebuild_world_(std::move(rebuild_world)) {
        options_.history = std::max<std::size_t>(options_.history, 2);
        // #637: each tick's state hash is computed off the simulation thread.
        auto hasher = std::make_shared<ThreadStateHasher>();
        if (session_) session_->set_state_hasher(std::move(hasher));
        else {
            scripted_->set_state_hasher(std::move(hasher));
            // #895: the live tick reads only the world's hash; the script hash is on request.
            scripted_->set_authoritative_hash(false);
            // #957: the AI step and the Lua service are timed apart (cost phases "ai" and "lua").
            scripted_->set_step_clock([] {
                return static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
            });
        }
        options_.event_history = std::max(options_.event_history, options_.history);
        if (const auto* movement = world().land_movement()) {
            for (const auto& member : movement->members())
                land_roster_.emplace_back(member.object, movement->squads()[member.squad].object);
        }
        const auto zero = world().snapshot();
        history_.push_front(zero);
        if (auto fog = capture_fog()) fog_history_.push_front(std::move(fog));
        frame_ = {zero, zero, Clock::now()};
        interval_ = tick_interval(options_.target_rate);
        paused_ = options_.initially_paused;
        if (options_.pacing == Pacing::driven) target_ = world().completed_tick();
        if (launch) thread_ = std::thread([this] { run(); });
    }

    ~Impl() { stop(); }

    core::Result<void> submit(LiveOrder order) {
        {
            const std::lock_guard lock(mutex_);
            if (const auto staging = recovery_admission_.lock(); staging && *staging) {
                core::Diagnostic error; error.code = "EAWR-PLATFORM-0600";
                error.message = "Order refused while recovery is catching up. Try again when play resumes.";
                submission_notice_ = error; rejected_.push_back(error);
                return core::Result<void>::failure(std::move(error));
            }
            submission_notice_.reset();
            inbox_.push_back(std::move(order));
        }
        wake_.notify_all();
        return core::Result<void>::success();
    }

    std::optional<core::Diagnostic> submission_notice() const {
        const std::lock_guard lock(mutex_);
        return submission_notice_;
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

    std::span<const std::pair<sim::EntityId, sim::EntityId>> land_roster() const noexcept { return land_roster_; }

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
        std::unique_lock lock(session_mutex_, std::defer_lock);
        { core::tick_profile::Scope wait("ui.lock.tick-hashes"); lock.lock(); }
        std::vector<std::string> hashes;
        hashes.reserve(hashes_.size());
        for (const auto& hash : hashes_) hashes.push_back(hash.get());
        return hashes;
    }

    tactical::TacticalReplay record() const {
        std::unique_lock lock(session_mutex_, std::defer_lock);
        { core::tick_profile::Scope wait("ui.lock.record"); lock.lock(); }
        return scripted_ ? scripted_->record() : session_->record();
    }

    LiveSession::ProductionView production_view(const tactical::PlayerId player, const tactical::TypeId type) const {
        std::unique_lock lock(session_mutex_, std::defer_lock);
        { core::tick_profile::Scope wait("ui.lock.production-view"); lock.lock(); }
        return {world().production_counts(player, type), world().snapshot()};
    }

    std::optional<bool> reinforcement_point(const tactical::PlayerId player, const tactical::TypeId type,
        const sim::math::Vec3& point, const std::optional<sim::math::Fixed> facing_yaw) const {
        const std::unique_lock lock(session_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return std::nullopt;
        const auto valid = world().reinforcement_point(player, type, point, nullptr, facing_yaw);
        return valid && valid.value();
    }

    tactical::TacticalReplay failure_record() const {
        std::unique_lock lock(session_mutex_, std::defer_lock);
        { core::tick_profile::Scope wait("ui.lock.failure-record"); lock.lock(); }
        return world().record_through_next_tick();
    }

    diagnostics::MemoryCapacity memory_capacity() const {
        std::lock_guard lock(session_mutex_);
        auto result = world().memory_capacity();
        if (scripted_) result.append(scripted_->memory_capacity());
        else result.add("lua", "vm_gc_kib_bytes_lower_bound", 0, 0);
        result.add("sim", "completed_tick", 0, world().completed_tick());
        return result;
    }

    LiveScriptReport script_report() const {
        std::unique_lock lock(session_mutex_, std::defer_lock);
        { core::tick_profile::Scope wait("ui.lock.script-report"); lock.lock(); }
        return script_report_;
    }

private:
    [[nodiscard]] const tactical::TacticalSession& world() const {
        return scripted_ ? scripted_->world() : *session_;
    }

    // #494: immutable rows of Options::fog_player's cells; null without them.
    [[nodiscard]] std::shared_ptr<const LiveFog> capture_fog() const {
        const tactical::FogCells* cells = world().fog_cells();
        if (!options_.fog_player || cells == nullptr) return nullptr;
        const auto players = world().players();
        for (std::size_t index = 0; index < players.size(); ++index) {
            if (players[index].player_id != *options_.fog_player) continue;
            return std::make_shared<const LiveFog>(LiveFog{world().completed_tick(), cells->rules(),
                *options_.fog_player, cells->value_rows(index)});
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
        const ThreadWorkerAdapter executor(options_.workers, ThreadWorkerAdapter::Dispatch::by_cost);
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
                    if (options_.step_driver) wake_.wait_for(lock, std::chrono::milliseconds(2), due);
                    else wake_.wait(lock, due);
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
            pacing_lag_ms_ = dropped_wall_ms_ = 0.0;
            if (held) {
                if (options_.step_driver && !step(executor, false)) return;
                continue;
            }

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
                if (options_.measure_phases && behind > 0U) {
                    const auto deadline = clock_start + interval * static_cast<std::int64_t>(
                        world().completed_tick() + 1U - clock_tick);
                    pacing_lag_ms_ = std::max(0.0,
                        std::chrono::duration<double, std::milli>(now - deadline).count());
                }
                if (behind > options_.max_catch_up) {
                    if (options_.measure_phases) dropped_wall_ms_ = pacing_lag_ms_;
                    // Too slow for real time: drop the missed time rather than spiral.
                    clock_start = now - interval;
                    clock_tick = world().completed_tick();
                }
                step_now = behind > 0U;
            }
            if (!step_now) continue;
            submit_due(world().completed_tick(), true);
            const auto before = world().completed_tick();
            if (!step(executor)) return;
            if (options_.step_driver && world().completed_tick() == before) {
                // A completeness wait must service I/O without a busy loop or sim time.
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(2), [&] { return stopping_; });
            }
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
                if (options_.step_driver) {
                    driver_input_.push_back(command);
                } else if (scripted_) {
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
                if (options_.step_driver) {
                    driver_input_.push_back(std::move(command));
                } else if (scripted_) {
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

    bool step(const ThreadWorkerAdapter& executor, bool advance_allowed = true) {
        MeasuredExecutor measured(executor);
        core::tick_profile::Recording serial;
        const sim::PartitionExecutor& step_executor = options_.measure_phases
            ? static_cast<const sim::PartitionExecutor&>(measured) : executor;
        std::vector<core::Diagnostic> script_refusals;
        std::shared_ptr<const LiveFog> fog;
        double step_ms = 0.0;
        double fog_ms = 0.0;
        double session_lock_ms = 0.0;
        bool held_tick = false;
        const auto lock_started = options_.measure_phases ? Clock::now() : Clock::time_point{};
        core::Result<tactical::TacticalTick> tick = [&]() -> core::Result<tactical::TacticalTick> {
            std::unique_lock driver_lock(session_mutex_);
            const auto started = Clock::now();
            if (options_.measure_phases)
                session_lock_ms = std::chrono::duration<double, std::milli>(started - lock_started).count();
            const auto local_step = [&] {
                const auto run = [&] { return scripted_ ? step_scripted(step_executor, script_refusals) : session_->step(step_executor); };
                if (!options_.measure_phases) return run();
                core::tick_profile::Capture capture(serial);
                core::tick_profile::Scope scope("serial.session");
                return run();
            };
            if (options_.step_driver) {
                const auto driver_error = [](const char* message) {
                    core::Diagnostic error; error.code = "EAWR-PLATFORM-0600"; error.message = message; return error;
                };
                std::optional<tactical::TacticalTick> driver_tick;
                LiveStepContext context{advance_allowed,
                    [&] { return world().completed_tick(); },
                    [&] {
                        // The completed tick already queued these canonical bytes on the
                        // background hasher. Resolve that same result for LAN's per-tick row.
                        // Before advancement (including tick zero), preserve the current-state query.
                        auto hash = driver_tick ? driver_tick->state_hash.get() : world().state_sha256();
                        if (scripted_) {
                            auto scripts = scripted_->script_state_hash();
                            if (!scripts) throw std::runtime_error(core::format_diagnostic(scripts.error()));
                            hash += scripts.value();
                        }
                        return core::sha256_hex({reinterpret_cast<const std::uint8_t*>(hash.data()), hash.size()});
                    },
                    [&] { return world().canonical_state_bytes(); },
                    [&](std::span<const tactical::PlayerCommand> commands) -> core::Result<void> {
                        for (const auto& command : commands) {
                            if (scripted_) input_.push_back(command);
                            else {
                                auto accepted = session_->submit(command);
                                if (!accepted) return core::Result<void>::failure(accepted.error());
                            }
                        }
                        return core::Result<void>::success();
                    }, [&] { return driver_tick ? driver_tick->snapshot : world().snapshot(); },
                    [&]() -> core::Result<void> {
                        if (!advance_allowed || driver_tick) return core::Result<void>::failure(driver_error("driver advanced a held tick or stepped twice"));
                        auto stepped = local_step();
                        if (!stepped) return core::Result<void>::failure(stepped.error());
                        driver_tick.emplace(std::move(stepped).value());
                        return core::Result<void>::success();
                    }, [&] { const auto controls = world().departure_controls();
                        return std::vector<tactical::DepartureControl>{controls.begin(), controls.end()}; },
                    [&]() -> core::Result<std::vector<std::uint8_t>> {
                        auto replay = scripted_ ? scripted_->record() : session_->record();
                        const auto completed = world().completed_tick();
                        std::erase_if(replay.commands, [completed](const auto& command) { return command.key.tick + 1 != completed; });
                        std::erase_if(replay.object_turns, [completed](const auto& command) { return command.tick + 1 != completed; });
                        if (replay.commands.empty() && replay.object_turns.empty()) return core::Result<std::vector<std::uint8_t>>::success({});
                        return tactical::write_replay(replay);
                    }, [this](std::uint32_t rate) { set_target_rate(rate); }};
                // Same value state_hash() starts from; the scripts are only hashed for exchanged rows.
                context.rebase_pacing = [this] { const std::lock_guard lock(mutex_); rebase_ = true; };
                context.world_hash = [&] { return driver_tick ? driver_tick->state_hash.get() : world().state_sha256(); };
                context.rebuild = [this, &executor, &driver_lock] {
                    driver_lock.unlock();
                    try { auto rebuilt = recovery_context(executor); driver_lock.lock(); return rebuilt; }
                    catch (...) { driver_lock.lock(); throw; }
                };
                auto commands = std::exchange(driver_input_, {});
                active_driver_lock_ = &driver_lock;
                struct ClearDriverLock { std::unique_lock<std::mutex>*& lock; ~ClearDriverLock() { lock = nullptr; } } clear_driver_lock{active_driver_lock_};
                auto driven = options_.step_driver(context, commands, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()));
                if (!driven) return core::Result<tactical::TacticalTick>::failure(driven.error());
                if (driven.value() != driver_tick.has_value()) return core::Result<tactical::TacticalTick>::failure(driver_error("driver advancement result disagrees with completed step"));
                if (!driver_tick) { held_tick = true; return core::Result<tactical::TacticalTick>::success({}); }
                auto stepped = core::Result<tactical::TacticalTick>::success(std::move(*driver_tick));
                if (!scripted_) hashes_.push_back(stepped.value().state_hash);
                fog = capture_fog();
                step_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                return stepped;
            }
            auto stepped = local_step();
            const auto stepped_at = Clock::now();
            if (stepped && !scripted_) hashes_.push_back(std::move(stepped.value().state_hash));
            if (stepped) fog = capture_fog();
            const auto finished = Clock::now();
            step_ms = std::chrono::duration<double, std::milli>(stepped_at - started).count();
            fog_ms = std::chrono::duration<double, std::milli>(finished - stepped_at).count();
            return stepped;
        }();
        if (held_tick) return true;
        if (!script_refusals.empty()) {
            const std::lock_guard lock(mutex_);
            rejected_.insert(rejected_.end(), script_refusals.begin(), script_refusals.end());
        }
        const auto publish_started = options_.measure_phases ? Clock::now() : Clock::time_point{};
        {
            const std::lock_guard lock(mutex_);
            if (!tick) {
                failure_ = tick.error();
            } else {
                rejected_.insert(rejected_.end(), tick.value().diagnostics.begin(), tick.value().diagnostics.end());
                frame_ = {frame_.latest, tick.value().snapshot, Clock::now()};
                if (const auto& outcome = tick.value().snapshot->outcome();
                    outcome && outcome->condition == tactical::VictoryCondition::intentional_quit) {
                    // WBF-43/48: departure results are immediate. Halt while publishing,
                    // before another tick can run while presentation catches up.
                    halt_ = halt_ ? std::min(*halt_, outcome->end_tick) : outcome->end_tick;
                }
                history_.push_front(tick.value().snapshot);
                if (history_.size() > options_.history) history_.pop_back();
                if (fog) {
                    fog_history_.push_front(std::move(fog));
                    if (fog_history_.size() > options_.history) fog_history_.pop_back();
                }
                event_log_.record(*tick.value().snapshot, tick.value().asteroid_impacts, options_.base_attack_types, tick.value().damage_changes);
                // #558: presentation-only wall-clock cost, never read by the simulation.
                // #957: a scripted tick's AI step and Lua service are their own phases; "step" is the rest.
                LiveTickCost cost{tick.value().completed_tick, step_ms + fog_ms, {{"step", step_ms}, {"fog", fog_ms}}};
                if (scripted_) {
                    cost.phases = {{"step", std::max(0.0, step_ms - ai_ms_ - lua_ms_)}, {"ai", ai_ms_}, {"lua", lua_ms_},
                        {"fog", fog_ms}};
                }
                if (options_.measure_phases) {
                    // Pacing lateness/discarded wall time are observations, not
                    // additional work and never inputs to the clock policy.
                    cost.phases.push_back({"pacing-lag", pacing_lag_ms_});
                    cost.phases.push_back({"pacing-discarded-wall", dropped_wall_ms_});
                    // Outside total_ms: lock admission and publication happen before/after
                    // the measured world/script step and fog capture, respectively.
                    cost.phases.push_back({"session-lock", session_lock_ms});
                    cost.phases.push_back({"publish-tail",
                        std::chrono::duration<double, std::milli>(Clock::now() - publish_started).count()});
                    double partition_ms = 0.0;
                    for (const auto& phase : measured.phases) partition_ms += phase.ms;
                    cost.phases.push_back({"worker-total", partition_ms});
                    cost.phases.push_back({"serial-remainder", std::max(0.0, step_ms - partition_ms)});
                    if (scripted_) cost.phases.push_back({"world", world_ms_});
                    cost.phases.insert(cost.phases.end(), measured.phases.begin(), measured.phases.end());
                    cost.phases.insert(cost.phases.end(), measured.partition_costs.begin(), measured.partition_costs.end());
                    for (const auto& phase : serial.costs) cost.phases.push_back({std::string(phase.name), phase.ms});
                }
                costs_.push_back(std::move(cost));
                if (costs_.size() > tick_cost_history) costs_.pop_front();
            }
        }
        published_.notify_all();
        return static_cast<bool>(tick);
    }

    core::Result<std::shared_ptr<LiveStepContext>> recovery_context(const ThreadWorkerAdapter& executor) {
        using Result = core::Result<std::shared_ptr<LiveStepContext>>;
        // Guard admission throughout building, replaying and publishing staging.
        // An abandoned/failed context expires the weak guard without touching its owner.
        auto admission = std::make_shared<bool>(true);
        { const std::lock_guard lock(mutex_); recovery_admission_ = admission; }
        auto rebuilt = rebuild_world_();
        if (!rebuilt) return Result::failure(rebuilt.error());
        std::optional<tactical::TacticalSession> session;
        std::unique_ptr<script::authoritative::ScriptedTacticalSession> scripted;
        if (options_.scripts && options_.scripts->wrap) {
            auto wrapped = options_.scripts->wrap(std::move(rebuilt).value());
            if (!wrapped) return Result::failure(wrapped.error());
            scripted = std::make_unique<script::authoritative::ScriptedTacticalSession>(std::move(wrapped).value());
        } else session.emplace(std::move(rebuilt).value());
        auto staging_options = options_;
        // A headless world must not retain the live admission/presentation owner.
        // Script factories remain immutable local content and are recreated above.
        staging_options.step_driver = {}; staging_options.command_source = {};
        auto staged = std::make_shared<Impl>(std::move(session), std::move(scripted), staging_options, rebuild_world_, false);
        auto context = std::make_shared<LiveStepContext>();
        context->completed_tick = [staged] { return staged->world().completed_tick(); };
        context->state_hash = [staged] {
            auto hash = staged->world().state_sha256();
            if (staged->scripted_) {
                auto scripts = staged->scripted_->script_state_hash();
                if (!scripts) throw std::runtime_error(core::format_diagnostic(scripts.error()));
                hash += scripts.value();
            }
            return core::sha256_hex({reinterpret_cast<const std::uint8_t*>(hash.data()), hash.size()});
        };
        context->world_hash = [staged] { return staged->world().state_sha256(); };
        context->canonical_state = [staged] { return staged->world().canonical_state_bytes(); };
        context->snapshot = [staged] { return staged->world().snapshot(); };
        context->departures = [staged] { const auto controls = staged->world().departure_controls();
            return std::vector<tactical::DepartureControl>{controls.begin(), controls.end()}; };
        context->install = [staged](std::span<const tactical::PlayerCommand> commands) -> core::Result<void> {
            for (const auto& command : commands) {
                if (staged->scripted_) staged->input_.push_back(command);
                else if (auto accepted = staged->session_->submit(command); !accepted) return accepted;
            }
            return core::Result<void>::success();
        };
        // Executor is used only during this driver's synchronous catch-up. Activation
        // replaces these closures with the next live context before another normal tick.
        context->step = [this, staged, &executor]() -> core::Result<void> {
            // Readers keep using the immutable held live world while replay runs.
            // This pointer always names this pump's owner lock, never a prior context.
            auto* owner_lock = active_driver_lock_;
            if (!owner_lock) {
                core::Diagnostic error; error.code = "EAWR-PLATFORM-0600"; error.message = "catch-up has no simulation owner";
                return core::Result<void>::failure(std::move(error));
            }
            owner_lock->unlock();
            struct Relock { std::unique_lock<std::mutex>* lock; ~Relock() { lock->lock(); } } relock{owner_lock};
            std::vector<core::Diagnostic> refusals;
            auto tick = staged->scripted_ ? staged->step_scripted(executor, refusals) : staged->session_->step(executor);
            if (!tick) return core::Result<void>::failure(tick.error());
            if (!staged->scripted_) staged->hashes_.push_back(tick.value().state_hash);
            return core::Result<void>::success();
        };
        context->set_target_rate = [staged](std::uint32_t rate) { staged->options_.target_rate = rate; };
        context->activate = [this, staged, admission]() -> core::Result<void> {
            session_ = std::move(staged->session_); scripted_ = std::move(staged->scripted_);
            hashes_ = std::move(staged->hashes_); script_report_ = std::move(staged->script_report_);
            // Preserve a sealed next tick already installed on the staging owner.
            input_ = std::move(staged->input_); driver_input_.clear();
            const auto snapshot = world().snapshot();
            auto fog = capture_fog();
            { const std::lock_guard lock(mutex_);
              // Pre-staging orders retain their tick and follow normal stale-order
              // diagnostics. No order accepted before the guard is silently erased.
              *admission = false; history_.clear(); history_.push_front(snapshot);
              fog_history_.clear(); if (fog) fog_history_.push_front(std::move(fog));
              frame_ = {snapshot, snapshot, Clock::now()};
              event_log_ = LiveEventLog({options_.event_history, options_.event_bytes});
              interval_ = tick_interval(staged->options_.target_rate); rebase_ = true;
            }
            return core::Result<void>::success();
        };
        return Result::success(std::move(context));
    }

    // Under session_mutex_: one step of the scripted session with the queued input. Its world
    // hash is the tick hash; refused input and dropped script commands are refusals.
    core::Result<tactical::TacticalTick> step_scripted(const sim::PartitionExecutor& executor,
        std::vector<core::Diagnostic>& refusals) {
        std::vector<tactical::PlayerCommand> input = std::move(input_);
        input_.clear();
        auto stepped = scripted_->step(executor, input);
        if (!stepped) return core::Result<tactical::TacticalTick>::failure(stepped.error());
        auto& result = stepped.value();
        world_ms_ = static_cast<double>(result.timing.world_ns) / 1.0e6;
        hashes_.push_back(result.world.state_hash);
        ai_ms_ = static_cast<double>(result.timing.engine_ns) / 1.0e6;
        lua_ms_ = static_cast<double>(result.timing.service_ns) / 1.0e6;
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

    std::unique_lock<std::mutex>* active_driver_lock_{};
    std::vector<tactical::PlayerCommand> driver_input_;
    std::vector<std::pair<sim::EntityId, sim::EntityId>> land_roster_;
    std::optional<tactical::TacticalSession> session_;
    std::unique_ptr<script::authoritative::ScriptedTacticalSession> scripted_;
    // Scripted mode: the commands for the next step, submitted by it; the scripts' report.
    std::vector<tactical::PlayerCommand> input_;
    LiveScriptReport script_report_;
    double ai_ms_{}; // #957: the last scripted step's AI step and Lua service, for the cost phases
    double lua_ms_{};
    double world_ms_{};
    std::set<std::string> seen_diagnostics_;
    Options options_;
    // Guards session_ and hashes_ against record() and tick_hashes() from other threads.
    mutable std::mutex session_mutex_;
    std::vector<sim::StateHash> hashes_; // resolved by tick_hashes()
    // Per issuer: one past the last sequence the session accepted, from either path.
    std::map<tactical::PlayerId, std::uint64_t> sequences_;
    // Simulation thread only: taken orders waiting for their tick, in arrival order.
    std::vector<LiveOrder> held_;

    mutable std::mutex mutex_;
    mutable std::condition_variable published_;
    std::condition_variable wake_;
    std::vector<LiveOrder> inbox_;
    std::weak_ptr<bool> recovery_admission_; // Read/write only under mutex_.
    std::optional<core::Diagnostic> submission_notice_;
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
    std::function<core::Result<tactical::TacticalSession>()> rebuild_world_;
    // #558: the newest ticks' cost, oldest first.
    std::deque<LiveTickCost> costs_;
    double pacing_lag_ms_{}, dropped_wall_ms_{}; // simulation-thread diagnostics only
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
    const tactical::AbilityTable& abilities,
    const tactical::EconomyRules& economy,
    const tactical::MapBox& map_box) {
    tactical::TacticalContent content;
    content.sensors.assign(sensors.begin(), sensors.end());
    content.durability = durability; content.motion = motion; content.combat = combat;
    content.victory = victory; content.fog = fog; content.abilities = abilities;
    content.economy = economy; content.map_box = map_box;
    return start_impl(setup, content, options, nullptr);
}

core::Result<std::unique_ptr<LiveSession>> LiveSession::start_impl(
    const tactical::TacticalSetup& setup, const tactical::TacticalContent& content,
    Options options, const tactical::TacticalReplay* replay) {
    using StartResult = core::Result<std::unique_ptr<LiveSession>>;
    if (options.workers == 0 || options.workers > ThreadWorkerAdapter::max_worker_count) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(tactical::diagnostic_codes::worker_failure);
        diagnostic.message = "live session worker count must be 1 to 256";
        return StartResult::failure(std::move(diagnostic));
    }
    if (replay) {
        if (auto valid = tactical::validate_replay(*replay); !valid) return StartResult::failure(valid.error());
        if (options.step_driver || (options.scripts && options.scripts->wrap)) {
            core::Diagnostic error;
            error.code = "EAWR-PLATFORM-0600";
            error.message = "Replay playback cannot attach live scripts or an external step driver.";
            return StartResult::failure(std::move(error));
        }
    }
    if (!replay && options.recorded_step_driver) {
        core::Diagnostic error; error.code = "EAWR-PLATFORM-0600";
        error.message = "A recorded session driver requires replay playback.";
        return StartResult::failure(std::move(error));
    }
    if (replay) options.step_driver = std::move(options.recorded_step_driver);
    // Retain the same initializer for recovery as for the original start. Replay
    // input is owned here, so a caller may release its parsed file after start().
    const auto recording = replay ? std::make_shared<const tactical::TacticalReplay>(*replay) : nullptr;
    auto rebuild_world = [setup, rules = content, recording](core::LoadNoteBuffer* load_notes = nullptr) {
        if (recording) {
            // Restore every non-player stream through the shared initializer.
            // Player commands enter at their ticks so live input can precede a
            // recording's future keys, just as in an ordinary live session.
            auto initial = *recording;
            initial.commands.clear();
            if (setup.domain == tactical::BattleDomain::land) return tactical::TacticalSession::from_replay(initial, rules, load_notes);
            return tactical::TacticalSession::from_replay(initial,
                rules.sensors, rules.durability, rules.motion, rules.fog, rules.combat,
                rules.victory, rules.abilities, rules.economy, rules.map_box, load_notes);
        }
        if (setup.domain == tactical::BattleDomain::land) return tactical::TacticalSession::create(setup, rules, load_notes);
        return tactical::TacticalSession::create(setup, rules.sensors, rules.durability, rules.motion,
            rules.fog, rules.combat, rules.victory, rules.abilities, rules.economy, rules.map_box, load_notes);
    };
    core::LoadNoteBuffer load_notes;
    auto session = rebuild_world(&load_notes);
    if (options.load_note_sink) {
        core::LoadNoteCollector collected;
        for (const auto& note : content.load_notes) collected.append(note);
        collected.merge(load_notes);
        const auto notes = collected.snapshot();
        options.load_note_sink(notes);
    }
    if (!session) return StartResult::failure(session.error());
    if (recording) {
        options.command_source = [recording, local = std::move(options.command_source)](const std::uint64_t tick) {
            std::vector<tactical::PlayerCommand> commands;
            auto at = std::lower_bound(recording->commands.begin(), recording->commands.end(), tick,
                [](const auto& command, const std::uint64_t due) { return command.key.tick < due; });
            for (; at != recording->commands.end() && at->key.tick == tick; ++at) commands.push_back(*at);
            if (local) {
                auto taken = local(tick);
                commands.insert(commands.end(), std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
            }
            return commands;
        };
    }
    if (options.scripts && options.scripts->wrap) {
        core::load_profile::Scope lua_scope(core::load_profile::Phase::lua);
        auto scripted = options.scripts->wrap(std::move(session).value());
        if (!scripted) return StartResult::failure(scripted.error());
        return StartResult::success(std::unique_ptr<LiveSession>(new LiveSession(std::make_unique<Impl>(std::nullopt,
            std::make_unique<script::authoritative::ScriptedTacticalSession>(std::move(scripted).value()), options, rebuild_world))));
    }
    return StartResult::success(std::unique_ptr<LiveSession>(
        new LiveSession(std::make_unique<Impl>(std::move(session).value(), nullptr, options, rebuild_world))));
}

core::Result<std::unique_ptr<LiveSession>> LiveSession::start(
    const tactical::TacticalSetup& setup, const tactical::TacticalContent& content, Options options) {
    return start_impl(setup, content, std::move(options), nullptr);
}

core::Result<std::unique_ptr<LiveSession>> LiveSession::start(
    const tactical::TacticalReplay& replay, const tactical::TacticalContent& content, Options options) {
    return start_impl(replay.setup, content, std::move(options), &replay);
}

core::Result<std::vector<std::string>> headless_tick_hashes(
    const tactical::TacticalReplay& replay,
    const std::span<const tactical::SensorProfile> sensors,
    const tactical::DurabilityTable& durability,
    const tactical::MotionTable& motion,
    const tactical::CombatTable& combat,
    const tactical::VictoryRules& victory,
    const std::optional<tactical::FogRules>& fog, const tactical::AbilityTable& abilities,
    const tactical::EconomyRules& economy,
    const tactical::MapBox& map_box) {
    using HashResult = core::Result<std::vector<std::string>>;
    auto session = tactical::TacticalSession::from_replay(
        replay, sensors, durability, motion, fog, combat, victory, abilities, economy, map_box);
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

core::Result<std::vector<std::string>> headless_tick_hashes(
    const tactical::TacticalReplay& replay, const tactical::TacticalContent& content) {
    if (replay.setup.domain == tactical::BattleDomain::land) {
        using Result = core::Result<std::vector<std::string>>;
        auto session = tactical::TacticalSession::from_replay(replay, content);
        if (!session) return Result::failure(session.error());
        std::vector<std::string> hashes;
        while (session.value().completed_tick() < replay.final_tick_count) {
            auto tick = session.value().step(sim::InlineExecutor{});
            if (!tick) return Result::failure(tick.error());
            hashes.push_back(std::move(tick.value().state_sha256));
        }
        return Result::success(std::move(hashes));
    }
    return headless_tick_hashes(replay, content.sensors, content.durability, content.motion, content.combat,
        content.victory, content.fog, content.abilities, content.economy, content.map_box);
}

LiveSession::LiveSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LiveSession::~LiveSession() = default;

core::Result<void> LiveSession::submit(LiveOrder order) { return impl_->submit(std::move(order)); }
std::optional<core::Diagnostic> LiveSession::submission_notice() const { return impl_->submission_notice(); }
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
std::span<const std::pair<sim::EntityId, sim::EntityId>> LiveSession::land_roster() const noexcept { return impl_->land_roster(); }
std::optional<bool> LiveSession::reinforcement_point(const tactical::PlayerId player, const tactical::TypeId type,
    const sim::math::Vec3& point, const std::optional<sim::math::Fixed> facing_yaw) const {
    return impl_->reinforcement_point(player, type, point, facing_yaw);
}

LiveSession::ProductionView LiveSession::production_view(const tactical::PlayerId player, const tactical::TypeId type) const {
    return impl_->production_view(player, type);
}
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
diagnostics::MemoryCapacity LiveSession::memory_capacity() const { return impl_->memory_capacity(); }
LiveScriptReport LiveSession::script_report() const { return impl_->script_report(); }
void LiveSession::stop() { impl_->stop(); }
std::vector<std::string> LiveSession::tick_hashes() const { return impl_->tick_hashes(); }
tactical::TacticalReplay LiveSession::record() const { return impl_->record(); }

tactical::TacticalReplay LiveSession::failure_record() const { return impl_->failure_record(); }

} // namespace eawr::platform
