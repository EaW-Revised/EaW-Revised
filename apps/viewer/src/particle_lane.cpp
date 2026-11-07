#include "particle_lane_thread.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace eawr::presentation::godot_backend {

namespace {
using LaneClock = std::chrono::steady_clock;
[[nodiscard]] double elapsed_ms(const LaneClock::time_point since, const LaneClock::time_point until) noexcept {
    return std::chrono::duration<double, std::milli>(until - since).count();
}
} // namespace

ParticleLane::ParticleLane(ParticleWorkers& workers, const bool allowed, std::string reason)
    : workers_(workers), allowed_(allowed), reason_(std::move(reason)) {}

ParticleLane::~ParticleLane() { shutdown(); }

bool ParticleLane::kick(Job job) {
    if (!job) return false;
    // A job left in flight by a skipped join is waited for first (its result is dropped): two jobs never overlap. The
    // join can switch the lane off (that job threw), so whether the lane is allowed is asked after it.
    static_cast<void>(join());
    if (!allowed()) return false;
    run_ = {};
    // The guard window opens before the job can start, so a main-thread entry into a lane-owned owner is counted
    // from the first instant the lane owns them.
    window_.emplace();
    const auto kicked = LaneClock::now();
    const bool started = workers_.start_job([this, job = std::move(job), kicked]() -> bool {
        const auto began = LaneClock::now();
        run_.start_delay_ms = elapsed_ms(kicked, began);
        // The lane's own calls into the guarded owners are the owner's, not violations.
        const particles::lane::LaneThreadScope on_lane;
        bool ok = true;
        try {
            job();
        } catch (const std::exception& error) {
            ok = false;
            run_.failure = error.what();
        } catch (...) {
            ok = false;
            run_.failure = "unknown exception";
        }
        run_.wall_ms = elapsed_ms(began, LaneClock::now());
        return ok;
    });
    if (!started) {
        window_.reset();
        return false;
    }
    in_flight_ = true;
    return true;
}

std::optional<ParticleLaneRun> ParticleLane::join() {
    if (!in_flight_) return std::nullopt;
    const auto waiting = LaneClock::now();
    ParticleWorkers::Help help;
    const bool ok = workers_.join_job_helping(help);
    run_.main_help_ms = help.ms;
    run_.main_help_tasks = help.tasks;
    run_.join_wait_ms = elapsed_ms(waiting, LaneClock::now());
    in_flight_ = false;
    window_.reset();
    run_.violations = particles::lane::take_violations();
    // join_job() false with no recorded exception cannot happen for a started job; report it as a failure anyway.
    run_.ok = ok && run_.failure.empty();
    if (!ok && run_.failure.empty()) run_.failure = "particle lane job did not complete";
    // A failed job leaves the owners in an unknown state: the lane stays off, the frames after it run inline.
    if (!run_.ok) disabled_ = true;
    return run_;
}

void ParticleLane::shutdown() noexcept {
    try {
        static_cast<void>(join());
    } catch (...) {
    }
    window_.reset();
    workers_.shutdown();
}

} // namespace eawr::presentation::godot_backend
