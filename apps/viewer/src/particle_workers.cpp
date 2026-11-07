#include "particle_workers.hpp"
#include "eawr/presentation/particles/lane_guard.hpp"

#include "eawr/presentation/particles/worker_count.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace eawr::presentation::godot_backend {

namespace {
// L1.3 leak check: lane threads started and not yet joined, over every ParticleWorkers of the process.
std::atomic<int> lane_threads{0};
using HelpClock = std::chrono::steady_clock;

void name_current_thread() noexcept {
#if defined(_WIN32)
    // The thread sampler of the rig reads this name.
    static_cast<void>(SetThreadDescription(GetCurrentThread(), L"EAWR particle lane"));
#endif
}
} // namespace

std::size_t ParticleWorkers::default_count(const std::size_t hardware_threads) noexcept {
    return particles::default_particle_worker_count(hardware_threads);
}

int ParticleWorkers::live_lane_threads() noexcept { return lane_threads.load(std::memory_order_acquire); }

// One parked thread owned by the ParticleWorkers (never a static or thread_local, L1-steps-v2 section 3): the caller of the
// pool's phase while the viewer's main thread is elsewhere (N1), or one job of the particle lane (L1.3). Parked on a
// condition variable between jobs; at most one job is started or unjoined at a time.
class ParticleWorkers::AsyncLane final {
public:
    explicit AsyncLane(particles::HelpBoard& board) : board_(board) {}
    // Backstop: an owner that dies with a job in flight waits for it, then joins the thread. No std::terminate.
    ~AsyncLane() { stop(); }
    AsyncLane(const AsyncLane&) = delete;
    AsyncLane& operator=(const AsyncLane&) = delete;

    [[nodiscard]] bool start(Job job) {
        const std::lock_guard lock(mutex_);
        if (stopped_ || pending_ || !job) return false;
        if (!thread_.joinable()) {
            try {
                thread_ = std::thread([this] { loop(); });
            } catch (const std::exception&) {
                return false;
            }
            lane_threads.fetch_add(1, std::memory_order_acq_rel);
        }
        job_ = std::move(job);
        kicker_ = std::this_thread::get_id();
        pending_ = true;
        finished_ = false;
        ok_ = false;
        wake_.notify_all();
        return true;
    }

    [[nodiscard]] bool join() {
        std::unique_lock lock(mutex_);
        if (!pending_) return false;
        done_.wait(lock, [this] { return finished_; });
        const bool ok = ok_;
        pending_ = false;
        finished_ = false;
        return ok;
    }

    // PF lane-serial (a): join(), building what the lane publishes on the board until the job is done. A finish or a
    // publish advances the board's generation, so a wait that started before either returns at once.
    [[nodiscard]] bool join_helping(Help& help) {
        for (;;) {
            const auto seen = board_.generation();
            {
                const std::lock_guard lock(mutex_);
                if (!pending_ || finished_) break;
            }
            const auto began = HelpClock::now();
            if (const std::size_t built = board_.help_once(); built != 0) {
                help.tasks += built;
                help.ms += std::chrono::duration<double, std::milli>(HelpClock::now() - began).count();
                continue;
            }
            board_.wait_change(seen);
        }
        return join();
    }

    // The thread that started the job, while it is in flight and not yet done, blocks here until the job is done
    // (the pool belongs to the job meanwhile: render.hpp StepExecutor::run). Any other thread, the job's own
    // helpers included, returns at once. The job stays pending: join() still reports its result.
    void wait_done_as_kicker() {
        std::unique_lock lock(mutex_);
        if (!pending_ || kicker_ != std::this_thread::get_id()) return;
        done_.wait(lock, [this] { return finished_; });
    }

    // Idempotent; waits for a pending job first so its output is complete and the owners it reads are still alive.
    void stop() noexcept {
        try {
            static_cast<void>(join());
            {
                const std::lock_guard lock(mutex_);
                stopped_ = true;
                stopping_ = true;
            }
            wake_.notify_all();
            if (thread_.joinable()) {
                thread_.join();
                lane_threads.fetch_sub(1, std::memory_order_acq_rel);
            }
        } catch (...) {
        }
    }

private:
    void loop() {
        name_current_thread();
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stopping_ || (pending_ && !finished_ && !running_); });
            // stop() joins a pending job before it sets `stopping_`, so a job is never dropped here.
            if (stopping_) return;
            Job job = std::move(job_);
            job_ = nullptr;
            running_ = true;
            lock.unlock();
            bool ok = false;
            try {
                ok = job();
            } catch (...) {
            }
            // The job's captures (frame input, closures over the owners) die here, before the join can return.
            try {
                job = nullptr;
            } catch (...) {
            }
            lock.lock();
            running_ = false;
            ok_ = ok;
            finished_ = true;
            done_.notify_all();
            board_.wake();
        }
    }

    particles::HelpBoard& board_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    Job job_;
    bool pending_{};
    bool running_{};
    bool finished_{};
    bool ok_{};
    bool stopping_{};
    bool stopped_{};
    std::thread::id kicker_{};
    std::thread thread_;
};

ParticleWorkers::ParticleWorkers(const std::size_t count) : pool_(std::max<std::size_t>(count, 1)) {}

particles::HelpBoard* ParticleWorkers::help_board() const noexcept {
    // Only the lane publishes: on any other thread nobody would be waiting to help.
    return particles::lane::detail::on_lane_thread ? &board_ : nullptr;
}

bool ParticleWorkers::join_job_helping(Help& help) const {
    if (lane_ == nullptr) return false;
    return lane_->join_helping(help);
}

ParticleWorkers::~ParticleWorkers() { shutdown(); }

bool ParticleWorkers::start_job(Job job) const {
    if (!lane_) lane_ = std::make_unique<AsyncLane>(board_);
    return lane_->start(std::move(job));
}

bool ParticleWorkers::join_job() const { return lane_ != nullptr && lane_->join(); }

void ParticleWorkers::shutdown() noexcept {
    // Keeps the lane object (now stopped) so a later start_job returns false instead of reviving the thread.
    if (!lane_) {
        try {
            lane_ = std::make_unique<AsyncLane>(board_);
        } catch (...) {
            return;
        }
    }
    lane_->stop();
}

bool ParticleWorkers::start_async(const std::size_t count, const std::function<void(std::size_t)>& task) const {
    // The caller joins before `task` dies (join_async), as before.
    return start_job([this, count, &task] { return static_cast<bool>(pool_.execute(count, task)); });
}

bool ParticleWorkers::join_async() const { return join_job(); }

bool ParticleWorkers::run(const std::size_t count, const std::function<void(std::size_t)>& task) const {
    // L1.2: the pool belongs to the lane while a lane job is in flight; the main thread entering it then is a violation.
    // A main-thread entry while the job runs is counted (never aborting) and then waits for the job: two batches never
    // share the pool.
    if (particles::lane::guard_main() && lane_) lane_->wait_done_as_kicker();
    return static_cast<bool>(pool_.execute(count, task));
}

} // namespace eawr::presentation::godot_backend
