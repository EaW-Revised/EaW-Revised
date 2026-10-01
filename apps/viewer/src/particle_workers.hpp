#pragma once

#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/particles/render.hpp"

#include <cstddef>
#include <functional>

namespace eawr::presentation::godot_backend {

// #638: the pool the live battle's particle registries (unit emitters, battle effects, breakoff
// props) step their effects and build their vertex streams on; the main thread is worker 0 and
// alone uploads the streams to Godot afterwards (EffectRegistry::advance_all). A presentation
// pool of its own, apart from the simulation's: it never runs a tick phase, and its threads park
// between frames as the simulation's do between phases.
class ParticleWorkers final : public particles::StepExecutor {
public:
    // The default size, the main thread included (docs/performance/battle-bench.md, "The viewer's
    // particles"): a quarter of the hardware threads, at least 1 (the main thread alone) and at
    // most 4. The simulation's pool already takes all but two of them
    // (platform::LiveSession::game_worker_count), so every particle worker shares a core with a
    // simulation worker; a small pool keeps a frame that overlaps a tick from taking more than a
    // few of the tick's cores.
    [[nodiscard]] static std::size_t default_count(std::size_t hardware_threads) noexcept;

    // `count` workers, the calling thread included; 1 runs every task on the calling thread.
    explicit ParticleWorkers(std::size_t count);

    [[nodiscard]] std::size_t count() const noexcept { return pool_.worker_count(); }
    [[nodiscard]] bool run(std::size_t count, const std::function<void(std::size_t)>& task) const override;

private:
    platform::ThreadWorkerAdapter pool_;
};

} // namespace eawr::presentation::godot_backend
