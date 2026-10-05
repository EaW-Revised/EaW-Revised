#include "eawr/platform/sim_workers.hpp"

#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace eawr::platform {
namespace {

// The pool whose partition this thread is running, so a nested execute() runs inline.
thread_local const void* running_pool = nullptr;

[[nodiscard]] core::Diagnostic worker_diagnostic(std::string message) {
    return core::Diagnostic{
        .code = std::string(sim::diagnostic_codes::worker_failure),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("thread-worker-adapter"),
    };
}

// Parks until `value` differs from `old` and returns it. No polling first: on a machine with
// SMT, polling workers took cycles from the working ones (sim_bench, #267).
[[nodiscard]] std::uint32_t await_change(const std::atomic<std::uint32_t>& value, const std::uint32_t old) {
    for (;;) {
        value.wait(old, std::memory_order_acquire);
        const auto now = value.load(std::memory_order_acquire);
        if (now != old) {
            return now;
        }
    }
}

// The lowest partition of one run that threw, and what it threw.
class PhaseFailure final {
public:
    void record(const std::size_t partition, std::string message) {
        const std::lock_guard lock(mutex_);
        if (!first_ || partition < first_->first) {
            first_.emplace(partition, std::move(message));
        }
    }

    // Consumes the record: success, or the diagnostic for the lowest partition that threw.
    [[nodiscard]] core::Result<void> take() {
        const std::lock_guard lock(mutex_);
        if (!first_) {
            return core::Result<void>::success();
        }
        auto message = "partition " + std::to_string(first_->first) + " threw: " + first_->second;
        first_.reset();
        return core::Result<void>::failure(worker_diagnostic(std::move(message)));
    }

private:
    std::mutex mutex_;
    std::optional<std::pair<std::size_t, std::string>> first_;
};

void run_one(const std::function<void(std::size_t)>& job, const std::size_t partition, PhaseFailure& failure) {
    try {
        job(partition);
    } catch (const std::exception& error) {
        failure.record(partition, error.what());
    } catch (...) {
        failure.record(partition, "unknown exception");
    }
}

} // namespace

class ThreadWorkerAdapter::Pool final {
public:
    Pool(const std::size_t workers, const Dispatch dispatch, ClockRead clock)
        : workers_(workers), dispatch_(dispatch), budget_(ThreadWorkerAdapter::inline_budget(workers)), clock_(std::move(clock)) {
        try {
            threads_.reserve(workers - 1);
            for (std::size_t worker = 1; worker < workers; ++worker) {
                threads_.emplace_back([this, worker] { work(worker); });
            }
        } catch (const std::exception& error) {
            start_error_ = "failed to start worker threads: " + std::string(error.what());
            stop();
        }
    }

    ~Pool() { stop(); }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&) = delete;
    Pool& operator=(Pool&&) = delete;

    // `phase` empty: an unnamed phase, which always goes to the pool.
    [[nodiscard]] core::Result<void> run(
        const std::string_view phase, const std::size_t partitions, const std::function<void(std::size_t)>& job) {
        if (start_error_) {
            return core::Result<void>::failure(worker_diagnostic(*start_error_));
        }
        if (running_pool == this) {
            return run_inline(partitions, job);
        }
        const std::lock_guard phase_lock(phase_mutex_);
        if (partitions <= 1 || threads_.empty()) {
            return run_inline(partitions, job);
        }
        auto* const cost = dispatch_ == Dispatch::by_cost && !phase.empty() ? &cost_of(phase) : nullptr;
        if (cost == nullptr || !starts_inline(*cost)) {
            const auto work = dispatch(partitions, job, 0, true);
            if (cost != nullptr) {
                cost->work = work;
            }
            if (!phase.empty()) {
                pool_phases_.fetch_add(1, std::memory_order_relaxed);
            }
            return failure_.take();
        }

        // A small phase last time: partitions in order on this thread until the budget is spent.
        const auto began = now();
        auto elapsed = Clock::duration::zero();
        std::size_t next = 0;
        const auto* const outer = running_pool;
        running_pool = this;
        while (next < partitions) {
            run_one(job, next, failure_);
            ++next;
            elapsed = now() - began;
            if (elapsed > budget_) {
                break;
            }
        }
        running_pool = outer;
        if (next == partitions) {
            cost->work = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
            inline_phases_.fetch_add(1, std::memory_order_relaxed);
            return failure_.take();
        }
        cost->work = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed) + dispatch(partitions, job, next, false);
        escalated_phases_.fetch_add(1, std::memory_order_relaxed);
        return failure_.take();
    }

    [[nodiscard]] PhaseCounts counts() const noexcept {
        return PhaseCounts{inline_phases_.load(std::memory_order_relaxed),
            escalated_phases_.load(std::memory_order_relaxed), pool_phases_.load(std::memory_order_relaxed)};
    }

private:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] Clock::time_point now() const {
        return clock_ ? clock_() : Clock::now();
    }

    [[nodiscard]] core::Result<void> run_inline(const std::size_t partitions, const std::function<void(std::size_t)>& job) {
        PhaseFailure failure;
        const auto* const outer = running_pool;
        running_pool = this;
        for (std::size_t partition = 0; partition < partitions; ++partition) {
            run_one(job, partition, failure);
        }
        running_pool = outer;
        return failure.take();
    }

    // A named phase's last work and how often it went to the pool since it last started inline.
    struct PhaseCost {
        std::string name;
        std::chrono::nanoseconds work{};
        std::uint32_t pool_runs{};
    };

    // The phase's record, by name; a phase seen for the first time counts as small. Under
    // phase_mutex_.
    [[nodiscard]] PhaseCost& cost_of(const std::string_view phase) {
        for (auto& cost : costs_) {
            if (cost.name == phase) {
                return cost;
            }
        }
        return costs_.emplace_back(PhaseCost{std::string(phase), {}, 0});
    }

    // Small last time, or near the budget and not tried inline for a while: work measured on the
    // pool includes the threads' contention (SMT, memory, a loaded host), so without another try
    // a phase that grew past the budget once could stay on the pool after it shrank again. A try
    // that runs past the budget hands the rest to the pool. The timer is checked after each
    // indivisible partition, so the try can exceed the budget by that partition's work.
    [[nodiscard]] bool starts_inline(PhaseCost& cost) const noexcept {
        constexpr std::uint32_t retry_every = 16;
        if (cost.work < budget_ || (cost.work < 4 * budget_ && ++cost.pool_runs >= retry_every)) {
            cost.pool_runs = 0;
            return true;
        }
        return false;
    }

    // Wakes every pool thread for partitions `first` .. partitions - 1 and runs a share on this
    // thread; `pinned` gives worker w partition w first. Returns the phase's work: each thread's
    // time in its share, added up. Under phase_mutex_.
    [[nodiscard]] std::chrono::nanoseconds dispatch(const std::size_t partitions,
        const std::function<void(std::size_t)>& job, const std::size_t first, const bool pinned) {
        job_ = &job;
        partitions_ = partitions;
        pinned_ = pinned;
        next_.store(pinned ? workers_ : first, std::memory_order_relaxed);
        work_ns_.store(0, std::memory_order_relaxed);
        busy_.store(static_cast<std::uint32_t>(threads_.size()), std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_release);
        generation_.notify_all();

        const auto* const outer = running_pool;
        running_pool = this;
        run_share(0);
        running_pool = outer;
        for (auto left = busy_.load(std::memory_order_acquire); left != 0;
             left = busy_.load(std::memory_order_acquire)) {
            static_cast<void>(await_change(busy_, left));
        }
        job_ = nullptr;
        return std::chrono::nanoseconds(work_ns_.load(std::memory_order_relaxed));
    }

    void work(const std::size_t worker) {
        running_pool = this;
        std::uint32_t seen = 0; // generation_ starts at 0 and only dispatch() or stop() advance it
        for (;;) {
            seen = await_change(generation_, seen);
            if (stopping_.load(std::memory_order_relaxed)) {
                return;
            }
            run_share(worker);
            if (busy_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                busy_.notify_one();
            }
        }
    }

    // Worker w's pinned partition w, then partitions claimed in turn.
    void run_share(const std::size_t worker) {
        const auto began = now();
        const auto& job = *job_;
        if (pinned_ && worker < partitions_) {
            run_one(job, worker, failure_);
        }
        for (auto partition = next_.fetch_add(1, std::memory_order_relaxed); partition < partitions_;
             partition = next_.fetch_add(1, std::memory_order_relaxed)) {
            run_one(job, partition, failure_);
        }
        const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(now() - began);
        work_ns_.fetch_add(static_cast<std::uint64_t>(spent.count()), std::memory_order_relaxed);
    }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_release);
        generation_.notify_all();
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
    }

    std::size_t workers_;
    Dispatch dispatch_;
    Clock::duration budget_;
    ClockRead clock_;
    std::vector<std::thread> threads_;
    std::optional<std::string> start_error_;
    std::mutex phase_mutex_; // one phase at a time
    std::vector<PhaseCost> costs_; // by_cost only

    // The phase: written before generation_ is released, read by workers after they acquire it.
    const std::function<void(std::size_t)>* job_ = nullptr;
    std::size_t partitions_ = 0;
    bool pinned_ = true;
    std::atomic<std::size_t> next_{0};
    std::atomic<std::uint64_t> work_ns_{0};
    std::atomic<std::uint32_t> generation_{0};
    std::atomic<std::uint32_t> busy_{0}; // pool threads still inside the phase
    std::atomic<bool> stopping_{false};
    PhaseFailure failure_;
    std::atomic<std::uint64_t> inline_phases_{0};
    std::atomic<std::uint64_t> escalated_phases_{0};
    std::atomic<std::uint64_t> pool_phases_{0};
};

ThreadWorkerAdapter::ThreadWorkerAdapter(const std::size_t worker_count, const Dispatch dispatch, ClockRead clock)
    : worker_count_(worker_count) {
    if (worker_count >= 1 && worker_count <= max_worker_count) {
        pool_ = std::make_unique<Pool>(worker_count, dispatch, std::move(clock));
    }
}

ThreadWorkerAdapter::~ThreadWorkerAdapter() = default;

std::size_t ThreadWorkerAdapter::hardware_worker_count() noexcept {
    const std::size_t hardware = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hardware, 1, max_worker_count);
}

std::chrono::nanoseconds ThreadWorkerAdapter::inline_budget(const std::size_t worker_count) noexcept {
    // sim_bench --dispatch-cost (2026-09-29, a 20-thread host): waking the pool for an empty
    // phase and waiting for it took a median 3 us at 2 and 4 workers, 23 at 8 and 43 at 20, so
    // about 2 to 3 us per thread; the #601 melee put it at 0.1 to 0.2 ms at 28. A phase whose work
    // is under about that is done sooner on the calling thread.
    return std::chrono::nanoseconds(3'000 * static_cast<std::int64_t>(std::max<std::size_t>(worker_count, 1)));
}

std::size_t ThreadWorkerAdapter::worker_count() const noexcept {
    return worker_count_;
}

core::Result<void> ThreadWorkerAdapter::execute(
    const std::size_t partition_count,
    const std::function<void(std::size_t)>& partition) const {
    return execute_phase({}, partition_count, partition);
}

core::Result<void> ThreadWorkerAdapter::execute_phase(
    const std::string_view phase,
    const std::size_t partition_count,
    const std::function<void(std::size_t)>& partition) const {
    if (!pool_) {
        return core::Result<void>::failure(worker_diagnostic(
            "thread adapter needs 1 to " + std::to_string(max_worker_count) + " workers, not "
            + std::to_string(worker_count_)));
    }
    return pool_->run(phase, partition_count, partition);
}

ThreadWorkerAdapter::PhaseCounts ThreadWorkerAdapter::phase_counts() const noexcept {
    return pool_ ? pool_->counts() : PhaseCounts{};
}

namespace {

// A hash the hasher thread has yet to finish.
class PendingHash final : public sim::StateHash::Source {
public:
    [[nodiscard]] const std::string& get() const override {
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return finished_; });
        return hex_;
    }

    void finish(std::string hex) {
        {
            const std::lock_guard lock(mutex_);
            hex_ = std::move(hex);
            finished_ = true;
        }
        done_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable done_;
    bool finished_ = false;
    std::string hex_;
};

} // namespace

class ThreadStateHasher::Thread final {
public:
    using Derivation = sim::StateHasher::Derivation;

    explicit Thread(const std::size_t backlog) : backlog_(std::max<std::size_t>(backlog, 1)) {
        thread_ = std::thread([this] { work(); });
    }

    ~Thread() {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        queued_.notify_all();
        thread_.join();
    }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;

    [[nodiscard]] sim::StateHash queue(std::vector<std::uint8_t> bytes, sim::StateHash base, Derivation derivation) {
        auto pending = std::make_shared<PendingHash>();
        {
            std::unique_lock lock(mutex_);
            room_.wait(lock, [this] { return jobs_.size() < backlog_; });
            jobs_.push_back({std::move(bytes), std::move(base), std::move(derivation), pending});
        }
        queued_.notify_one();
        return sim::StateHash(std::shared_ptr<const sim::StateHash::Source>(std::move(pending)));
    }

private:
    // The SHA-256 of `bytes`, or with a derivation that of `base`'s digest.
    struct Job {
        std::vector<std::uint8_t> bytes;
        sim::StateHash base;
        Derivation derivation;
        std::shared_ptr<PendingHash> result;
    };

    // Hashes in submission order; on stop, finishes what is queued first.
    void work() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                queued_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            room_.notify_one();
            std::string hex;
            try {
                hex = job.derivation ? job.derivation(job.base.get()) : sim::sha256_hex(job.bytes);
            } catch (...) {
                hex.clear(); // an empty hash never equals a real one, so a check that reads it fails
            }
            job.result->finish(std::move(hex));
        }
    }

    std::size_t backlog_;
    std::mutex mutex_;
    std::condition_variable queued_;
    std::condition_variable room_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::thread thread_;
};

ThreadStateHasher::ThreadStateHasher(const std::size_t backlog) {
    try {
        thread_ = std::make_unique<Thread>(backlog);
    } catch (const std::exception&) {
        thread_.reset();
    }
}

ThreadStateHasher::~ThreadStateHasher() = default;

sim::StateHash ThreadStateHasher::hash(std::vector<std::uint8_t> canonical_bytes) {
    if (!thread_) {
        return sim::StateHash(sim::sha256_hex(canonical_bytes));
    }
    return thread_->queue(std::move(canonical_bytes), {}, {});
}

sim::StateHash ThreadStateHasher::derive(sim::StateHash base, Derivation derivation) {
    if (!thread_) {
        return sim::StateHash(derivation(base.get()));
    }
    return thread_->queue({}, std::move(base), std::move(derivation));
}

std::vector<std::size_t> determinism_worker_counts() {
    std::vector<std::size_t> counts{1, 2, 4, 8, ThreadWorkerAdapter::hardware_worker_count()};
    std::sort(counts.begin(), counts.end());
    counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
    return counts;
}

} // namespace eawr::platform
