// L1.3 particle lane contracts: the exit-code gate of out/pf/lane-design/L1-steps-v2.md section 3 (a)-(f), and the
// lane-versus-inline twin on a real std::thread. These build the viewer's engine-free lane parts
// (apps/viewer/src/particle_workers.cpp, particle_lane.cpp) next to the particle registry.
#include "render_test_support.hpp"

#include "particle_lane_thread.hpp"
#include "particle_workers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <cstdint>

namespace particle_render_contracts {

namespace {

namespace viewer = eawr::presentation::godot_backend;
namespace lane_guard = eawr::presentation::particles::lane;

// (a) A ParticleWorkers destroyed with a job started and never joined waits for it and joins its thread: no terminate.
void lane_destructor_joins() {
    std::atomic<int> ran{0};
    {
        viewer::ParticleWorkers workers(4);
        expect(workers.start_job([&ran] { ran.fetch_add(1); return true; }), "a job starts on the parked lane thread");
        expect(viewer::ParticleWorkers::live_lane_threads() == 1, "the started lane thread is counted");
    }
    expect(ran.load() == 1, "(a) the destructor let the unjoined job finish");
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "(a)(e) the destructor joined the lane thread");
}

// (b) shutdown() twice, with and without a thread ever started.
void lane_shutdown_is_idempotent() {
    viewer::ParticleWorkers started(2);
    expect(started.start_job([] { return true; }) && started.join_job(), "a job runs and joins");
    started.shutdown();
    started.shutdown();
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "(b)(e) shutdown twice leaves no thread");
    viewer::ParticleWorkers never(2);
    never.shutdown();
    never.shutdown();
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "(b)(e) shutdown without a started thread is a no-op");
}

// (c) shutdown() while a job runs waits for it; the job's output is complete afterwards.
void lane_shutdown_waits_for_the_job() {
    std::mutex mutex;
    std::condition_variable condition;
    bool running = false;
    std::atomic<int> output{0};
    viewer::ParticleWorkers workers(2);
    expect(workers.start_job([&] {
        {
            const std::lock_guard lock(mutex);
            running = true;
        }
        condition.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        output.store(42);
        return true;
    }), "a slow job starts");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return running; });
    }
    workers.shutdown();
    expect(output.load() == 42, "(c) shutdown waited for the running job: its output is complete");
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "(c)(e) the thread is joined after the wait");
}

// (d) An exception in the job: ok=false, no terminate, the thread still joins; the lane then stays off.
void lane_exception_is_reported() {
    {
        viewer::ParticleWorkers workers(2);
        viewer::ParticleLane lane(workers, true, "test");
        expect(lane.allowed() && lane.kick([] { throw std::runtime_error("boom"); }), "a throwing job starts");
        const auto run = lane.join();
        expect(run && !run->ok && run->failure == "boom", "(d) the exception is caught on the lane and reported as ok=false");
        expect(!lane.allowed(), "(d) a lane whose job threw stays off: the next frames run inline");
        expect(!lane.kick([] {}), "(d) a disabled lane does not start a job");
        expect(!lane_guard::window_open(), "(d) the guard window closed with the join");
        lane.shutdown();
    }
    {
        viewer::ParticleWorkers workers(2);
        expect(workers.start_job([]() -> bool { throw 7; }), "a job throwing a non-exception starts");
        expect(!workers.join_job(), "(d) a non-std exception ends the job with false, not with terminate");
        workers.shutdown();
    }
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "(d)(e) the lane thread joined after an exception");
}

// (f) A kick after shutdown is refused: the caller runs the work inline.
void lane_kick_after_shutdown_runs_inline() {
    viewer::ParticleWorkers workers(2);
    viewer::ParticleLane lane(workers, true, "test");
    expect(lane.kick([] {}) && lane.join().has_value(), "a job runs before the shutdown");
    lane.shutdown();
    int inline_runs = 0;
    const auto frame = [&inline_runs] { ++inline_runs; };
    if (!lane.kick(frame)) frame();
    expect(inline_runs == 1, "(f) the kick after shutdown returned not-started and the frame ran inline once");
    expect(!workers.start_job([] { return true; }), "(f) the pool refuses a job after shutdown");
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "(f)(e) nothing was started again");
}

// The lane thread's window: a main-thread entry is counted, the lane's own are not, one kick and one join per frame.
void lane_window_and_timings() {
    static_cast<void>(lane_guard::take_violations());
    viewer::ParticleWorkers workers(2);
    viewer::ParticleLane lane(workers, true, "test");
    std::atomic<bool> lane_counted{true};
    expect(lane.kick([&lane_counted] {
        lane_counted = lane_guard::guard_main();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }), "a job starts");
    // A kick with a job still in flight (a skipped join) waits for it first: two jobs never overlap.
    expect(lane.kick([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }) && lane.in_flight(), "a second kick waits for the first job and starts its own");
    expect(lane_guard::window_open(), "the window is open between the kick and the join");
    expect(lane_guard::guard_main(), "a main-thread entry inside the window is counted");
    const auto run = lane.join();
    expect(run && run->ok && run->violations == 1, "the join reports the one main-thread violation and no more");
    expect(!lane_counted.load(), "the lane thread's own call inside the window is not a violation");
    expect(run && run->wall_ms >= 15.0 && run->start_delay_ms >= 0.0 && run->join_wait_ms >= 0.0, "wall time and waits are measured");
    expect(!lane_guard::window_open() && !lane.in_flight(), "the join closed the window");
    expect(!lane.join().has_value(), "a second join with nothing in flight returns nothing");
    lane.shutdown();
}

// Main entering the pool while a lane job runs (FIX-2): counted, then it waits for the job; the join still reports the job.
void lane_pool_entry_waits_for_the_job() {
    static_cast<void>(lane_guard::take_violations());
    viewer::ParticleWorkers workers(4);
    viewer::ParticleLane lane(workers, true, "test");
    std::atomic<bool> release{false}, job_done{false};
    expect(lane.kick([&] {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        job_done = true;
    }), "a job that waits for a release starts");
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        release = true;
    });
    std::atomic<int> tasks{0};
    const bool ran = workers.run(2, [&tasks](const std::size_t) { tasks.fetch_add(1); });
    expect(job_done.load(), "(FIX-2) run() from main returned only after the lane job was done");
    expect(ran && tasks.load() >= 1, "(FIX-2) the entry then ran its own batch");
    releaser.join();
    const auto run = lane.join();
    expect(run && run->ok && run->violations == 1, "(FIX-2) the join still reports the job, ok, with the one counted entry");
    expect(workers.run(2, [](const std::size_t) {}), "with no job in flight run() does not wait");
    lane.shutdown();
}

// FIX-1: a job left in flight by a skipped join is joined by the next kick; the new job then runs alone.
void lane_skipped_join_is_joined_by_the_next_kick() {
    viewer::ParticleWorkers workers(2);
    viewer::ParticleLane lane(workers, true, "test");
    std::atomic<int> running{0}, overlap{0}, finished{0};
    const auto job = [&] {
        if (running.fetch_add(1) != 0) overlap.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        running.fetch_sub(1);
        finished.fetch_add(1);
    };
    expect(lane.kick(job), "the first job starts and its join is skipped");
    expect(lane.kick(job), "the next kick joins the left-over job and starts its own");
    expect(finished.load() >= 1, "(FIX-1) the left-over job was finished before the next job started");
    expect(lane.join().has_value() && finished.load() == 2 && overlap.load() == 0, "(FIX-1) the two jobs never overlapped");
    lane.shutdown();
}

// F-1: a left-over job that threw switches the lane off at the next kick's join; that kick starts nothing.
void lane_failed_leftover_job_keeps_the_lane_off() {
    viewer::ParticleWorkers workers(2);
    viewer::ParticleLane lane(workers, true, "test");
    std::atomic<int> second_runs{0};
    expect(lane.kick([] { throw std::runtime_error("leftover"); }), "a throwing job starts and its join is skipped");
    expect(!lane.kick([&second_runs] { second_runs.fetch_add(1); }), "(F-1) the next kick joined the failed job and started nothing");
    expect(!lane.allowed() && !lane.in_flight() && !lane_guard::window_open(), "(F-1) the lane is off and its window closed");
    lane.shutdown();
    expect(second_runs.load() == 0, "(F-1) the second job never ran");
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "(F-1)(e) no lane thread is left");
}

// The lane is the same work as the inline path: the registry script played on the lane thread, with the pool's other
// threads as helpers and the lane thread as worker 0, equals the serial script, at 1, 2, 4 and 8 particle workers.
// PF lane-serial (a): the main thread helps at the join (always), at 1, 2, 4, 6 and 8 workers.
void lane_equals_inline() {
    std::size_t helped = 0;
    for (const std::size_t count : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{6}, std::size_t{8}}) {
        viewer::ParticleWorkers workers(count);
        viewer::ParticleLane lane(workers, true, "test");
        std::optional<viewer::ParticleLaneRun> run;
        const bool same = lane_twin_matches([&](const std::function<void()>& frame) {
            expect(lane.kick(frame), "the twin frame starts on the lane");
            run = lane.join();
        }, &workers);
        expect(same, "the lane run, with main's help at the join, equals the serial run (statistics, hashes, uploads)");
        expect(run && run->ok && run->violations == 0, "the lane run threw nothing and the lane's pool use is not a violation");
        if (run) helped += run->main_help_tasks;
        lane.shutdown();
    }
    expect(helped > 0, "the main thread built lane tasks at the join");
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "(e) the twin left no lane thread");
}

// PF lane-serial (a): the help board. Every piece runs exactly once whoever claims it, and no helper is inside an
// entry once retract() returned (the entry and its counters die right after it, on the publisher's stack).
void help_board_publish_retract() {
    eawr::presentation::particles::HelpBoard board;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> late{0};
    std::vector<std::thread> helpers;
    for (int index = 0; index < 3; ++index) {
        helpers.emplace_back([&] {
            while (!stop.load()) {
                const auto seen = board.generation();
                if (board.help_once() == 0 && !stop.load()) board.wait_change(seen);
            }
        });
    }
    bool exact = true;
    for (int round = 0; round < 2000; ++round) {
        struct Pieces final {
            std::atomic<std::size_t> next{0};
            std::array<std::atomic<int>, 37> ran{};
            std::atomic<bool> retracted{false};
            std::atomic<std::uint64_t>* late{};
        } pieces;
        pieces.late = &late;
        const eawr::presentation::particles::HelpBoard::Entry entry{
            .work = [](void* context) -> std::size_t {
                auto& own = *static_cast<Pieces*>(context);
                if (own.retracted.load()) own.late->fetch_add(1);
                std::size_t built = 0;
                for (auto claim = own.next.fetch_add(1); claim < own.ran.size(); claim = own.next.fetch_add(1)) {
                    own.ran[claim].fetch_add(1);
                    ++built;
                }
                return built;
            },
            .context = &pieces};
        board.publish(&entry);
        for (auto claim = pieces.next.fetch_add(1); claim < pieces.ran.size(); claim = pieces.next.fetch_add(1))
            pieces.ran[claim].fetch_add(1);
        board.retract();
        pieces.retracted.store(true);
        for (const auto& count : pieces.ran) exact = exact && count.load() == 1;
    }
    stop.store(true);
    board.wake();
    for (auto& helper : helpers) helper.join();
    expect(exact, "every published piece ran exactly once, by the publisher or a helper");
    expect(late.load() == 0, "no helper entered a work after its retract");
}

} // namespace

void test_particle_lane_contracts() {
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "no lane thread before the lane contracts");
    lane_destructor_joins();
    lane_shutdown_is_idempotent();
    lane_shutdown_waits_for_the_job();
    lane_exception_is_reported();
    lane_kick_after_shutdown_runs_inline();
    lane_window_and_timings();
    lane_pool_entry_waits_for_the_job();
    lane_skipped_join_is_joined_by_the_next_kick();
    lane_failed_leftover_job_keeps_the_lane_off();
    lane_equals_inline();
    help_board_publish_retract();
    expect(viewer::ParticleWorkers::live_lane_threads() == 0, "(e) no lane thread is left at the end");
}

} // namespace particle_render_contracts
