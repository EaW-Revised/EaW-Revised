#pragma once

#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/particles/render.hpp"

#include <cstddef>
#include <functional>
#include <memory>

namespace eawr::presentation::godot_backend {

// #638: the pool the live battle's particle registries (unit emitters, battle effects, breakoff
// props) step their effects and build their vertex streams on; the main thread is worker 0 and
// alone uploads the streams to Godot afterwards (EffectRegistry::advance_all). A presentation
// pool of its own, apart from the simulation's: it never runs a tick phase, and its threads park
// between frames as the simulation's do between phases.
class ParticleWorkers final : public particles::StepExecutor {
public:
    // The default size, the main thread included (docs/performance/battle-bench.md, "The viewer's
    // particles"; the rule is particles::default_particle_worker_count): a quarter of the hardware
    // threads below eight of them, half of them from eight up (at most 4, at most 6 from twelve up, at most 8
    // from twenty up),
    // at least 1 (the main thread alone). The simulation's pool already takes all but two of them
    // (platform::LiveSession::game_worker_count), so every particle worker shares a core with a
    // simulation worker; hosts under eight threads keep their earlier participant count, and the
    // cap keeps the overlap bounded. Measured melee-S work benefits from four participants on an
    // eight-hardware-thread host.
    [[nodiscard]] static std::size_t default_count(std::size_t hardware_threads) noexcept;

    // `count` workers, the calling thread included; 1 runs every task on the calling thread.
    explicit ParticleWorkers(std::size_t count);
    ~ParticleWorkers() override;
    ParticleWorkers(const ParticleWorkers&) = delete;
    ParticleWorkers& operator=(const ParticleWorkers&) = delete;

    [[nodiscard]] std::size_t count() const noexcept { return pool_.worker_count(); }
    [[nodiscard]] bool run(std::size_t count, const std::function<void(std::size_t)>& task) const override;
    [[nodiscard]] std::size_t participants() const noexcept override { return pool_.worker_count(); }
    // PF lane-serial (a): the lane's ordered batches are published here while main helps (join_job_helping); null on
    // any other thread.
    [[nodiscard]] particles::HelpBoard* help_board() const noexcept override;
    // N1: one parked thread runs the pool's phase as worker 0 while the calling thread does other work; the pool's
    // other threads are the helpers, so the batch still has `count` participants. One batch at a time.
    [[nodiscard]] bool start_async(std::size_t count, const std::function<void(std::size_t)>& task) const override;
    [[nodiscard]] bool join_async() const override;

    // L1.3 (PF particle lane): the same parked thread runs one job, the particle lane's frame. At most one job is
    // started or unjoined at a time. `start_job` is false when a job is already pending, after shutdown(), or when the
    // thread cannot start: the caller then runs the work itself. The job's result is whether it finished without an
    // exception; no exception leaves the thread (the lane reports a failure through its own output).
    using Job = std::function<bool()>;
    [[nodiscard]] bool start_job(Job job) const;
    // Blocks until the started job is done; false when none was started or it threw.
    [[nodiscard]] bool join_job() const;
    // PF lane-serial (a): join_job(), but while the job runs the calling thread builds unclaimed tasks of the lane's
    // running ordered batch (never a commit). Adds the time it spent building and the tasks it built.
    struct Help final {
        double ms{};
        std::size_t tasks{};
    };
    [[nodiscard]] bool join_job_helping(Help& help) const;
    // Join a pending job, then stop and join the lane thread. Idempotent, never throws; every owner a job reads must
    // outlive this call (MapMode::State::~State runs it first). A start after shutdown() returns false.
    void shutdown() noexcept;
    // Lane threads started and not yet joined, process-wide: 0 once every ParticleWorkers is shut down or destroyed
    // (uninitialize_eawr_viewer reports a non-zero count as EAWR-VIEWER-LANE-LEAK).
    [[nodiscard]] static int live_lane_threads() noexcept;

private:
    class AsyncLane;
    platform::ThreadWorkerAdapter pool_;
    mutable particles::HelpBoard board_;
    mutable std::unique_ptr<AsyncLane> lane_;
};

} // namespace eawr::presentation::godot_backend
