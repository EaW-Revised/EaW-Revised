#pragma once

#include "particle_workers.hpp"

#include "eawr/presentation/particles/lane_guard.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

// What one lane job reported (L1.3). Engine-free, so the contracts (tests/presentation/particles) can run it.
struct ParticleLaneRun final {
    bool ok{true};                  // the job returned; false = it threw (failure says what)
    std::string failure;            // the exception's what(), empty when ok
    double wall_ms{};               // the job's time on the lane thread
    double start_delay_ms{};        // kick to the job's first statement (thread wake)
    double join_wait_ms{};          // the main thread blocked in join()
    std::uint32_t violations{};     // window guard hits between kick and join (counted, never aborting)
    double main_help_ms{};          // PF lane-serial (a): main's time building lane tasks inside join_wait_ms
    std::size_t main_help_tasks{};  // and the tasks it built
};

// L1.3 (PF particle lane, out/pf/lane-design/L1-steps-v2.md): one job per frame on the ParticleWorkers' parked thread.
// kick() posts it and opens the guard window; join() waits for it and closes the window. Never a static object: it is
// owned by the owner of the ParticleWorkers it borrows (MapMode::State), and shutdown() runs before the owners the job
// reads die. A job that throws is caught on the lane, reported as ok=false and switches the lane off: every later
// frame runs inline (the match goes on).
class ParticleLane final {
public:
    using Job = std::function<void()>;

    // `allowed` is decided once on the main thread (Separate render thread only); `reason` is what the log line says.
    ParticleLane(ParticleWorkers& workers, bool allowed, std::string reason);
    ~ParticleLane();
    ParticleLane(const ParticleLane&) = delete;
    ParticleLane& operator=(const ParticleLane&) = delete;

    [[nodiscard]] bool allowed() const noexcept { return allowed_ && !disabled_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
    [[nodiscard]] bool in_flight() const noexcept { return in_flight_; }

    // Posts `job`. False = not started (lane off, disabled by an earlier failure, shut down, busy or no thread): the
    // caller runs the work inline, as it does with the lane off. Exactly one kick per frame.
    [[nodiscard]] bool kick(Job job);
    // Waits for the job, closes the window. Returns what the job reported; empty when no job was in flight.
    std::optional<ParticleLaneRun> join();
    // join() if a job is in flight, then the ParticleWorkers' shutdown. Idempotent; first thing in the owner's destructor.
    void shutdown() noexcept;

private:
    ParticleWorkers& workers_;
    bool allowed_{};
    bool disabled_{};
    bool in_flight_{};
    std::string reason_;
    ParticleLaneRun run_{};
    std::optional<particles::lane::WindowScope> window_;
};

} // namespace eawr::presentation::godot_backend
