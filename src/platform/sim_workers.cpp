#include "eawr/platform/sim_workers.hpp"

#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
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
    explicit Pool(const std::size_t workers) : workers_(workers) {
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

    [[nodiscard]] core::Result<void> run(const std::size_t partitions, const std::function<void(std::size_t)>& job) {
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
        job_ = &job;
        partitions_ = partitions;
        next_.store(workers_, std::memory_order_relaxed);
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
        return failure_.take();
    }

private:
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

    void work(const std::size_t worker) {
        running_pool = this;
        std::uint32_t seen = 0; // generation_ starts at 0 and only run() or stop() advance it
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
        const auto& job = *job_;
        if (worker < partitions_) {
            run_one(job, worker, failure_);
        }
        for (auto partition = next_.fetch_add(1, std::memory_order_relaxed); partition < partitions_;
             partition = next_.fetch_add(1, std::memory_order_relaxed)) {
            run_one(job, partition, failure_);
        }
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
    std::vector<std::thread> threads_;
    std::optional<std::string> start_error_;
    std::mutex phase_mutex_; // one phase at a time

    // The phase: written before generation_ is released, read by workers after they acquire it.
    const std::function<void(std::size_t)>* job_ = nullptr;
    std::size_t partitions_ = 0;
    std::atomic<std::size_t> next_{0};
    std::atomic<std::uint32_t> generation_{0};
    std::atomic<std::uint32_t> busy_{0}; // pool threads still inside the phase
    std::atomic<bool> stopping_{false};
    PhaseFailure failure_;
};

ThreadWorkerAdapter::ThreadWorkerAdapter(const std::size_t worker_count)
    : worker_count_(worker_count) {
    if (worker_count >= 1 && worker_count <= max_worker_count) {
        pool_ = std::make_unique<Pool>(worker_count);
    }
}

ThreadWorkerAdapter::~ThreadWorkerAdapter() = default;

std::size_t ThreadWorkerAdapter::hardware_worker_count() noexcept {
    const std::size_t hardware = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hardware, 1, max_worker_count);
}

std::size_t ThreadWorkerAdapter::worker_count() const noexcept {
    return worker_count_;
}

core::Result<void> ThreadWorkerAdapter::execute(
    const std::size_t partition_count,
    const std::function<void(std::size_t)>& partition) const {
    if (!pool_) {
        return core::Result<void>::failure(worker_diagnostic(
            "thread adapter needs 1 to " + std::to_string(max_worker_count) + " workers, not "
            + std::to_string(worker_count_)));
    }
    return pool_->run(partition_count, partition);
}

std::vector<std::size_t> determinism_worker_counts() {
    std::vector<std::size_t> counts{1, 2, 4, 8, ThreadWorkerAdapter::hardware_worker_count()};
    std::sort(counts.begin(), counts.end());
    counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
    return counts;
}

} // namespace eawr::platform
