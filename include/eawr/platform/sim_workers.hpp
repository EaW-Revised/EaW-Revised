#pragma once

#include "eawr/sim/state_hash.hpp"
#include "eawr/sim/world.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace eawr::platform {

// The simulation's persistent worker pool behind sim::PartitionExecutor (ADR-009,
// docs/simulation.md). The constructor starts worker_count - 1 threads once; the thread that
// calls execute() is worker 0. Between phases the threads park on an atomic wait. execute()
// takes any partition count: worker w runs partition w first, then every worker claims the
// next unrun partition until none is left. Which thread runs a partition never changes a
// result, because partitions read copied inputs and write disjoint outputs.
//
// One phase runs at a time; a call from inside a running partition runs its partitions inline.
// A partition that throws fails the phase with EAWR-SIM-0109 naming the lowest partition that
// threw, after every partition has run. Zero or more than max_worker_count workers, or a thread
// that cannot start, fail every execute() with the same code.
//
// Dispatch::by_cost (#637) keeps small named phases off the pool, because waking every parked
// thread costs more than a small phase's work. A named phase whose last run's work (the time its
// partitions took, added up over the threads) was under inline_budget(worker_count) runs on the
// calling thread in partition order; once it has run longer than that budget, the pool takes
// the partitions left. Bigger phases go to the pool at once; one under four budgets tries the
// calling thread again every 16th run, since work timed on the pool includes the threads'
// contention. Only which thread runs a partition
// changes, never a result. Dispatch::always_pool wakes the pool for every phase of more than one
// partition; the determinism tests use it, so they keep exercising the threads.
class ThreadWorkerAdapter final : public sim::PartitionExecutor {
public:
    static constexpr std::size_t max_worker_count = 256;

    enum class Dispatch { always_pool, by_cost };
    // Work below which a phase costs less on the calling thread than on the pool: the measured
    // cost of waking the pool's threads and waiting for them (docs/simulation.md).
    [[nodiscard]] static std::chrono::nanoseconds inline_budget(std::size_t worker_count) noexcept;

    // How the named phases ran so far (diagnostics; not state).
    struct PhaseCounts {
        std::uint64_t inline_phases{};    // every partition on the calling thread
        std::uint64_t escalated_phases{}; // started inline, finished on the pool
        std::uint64_t pool_phases{};      // on the pool from the start
    };

    // Optional clock for deterministic dispatch tests. It must be monotonic, safe to call
    // from every worker, and never throw; empty uses the steady clock.
    using ClockRead = std::function<std::chrono::steady_clock::time_point()>;
    explicit ThreadWorkerAdapter(std::size_t worker_count, Dispatch dispatch = Dispatch::always_pool,
        ClockRead clock = {});
    ~ThreadWorkerAdapter() override;

    ThreadWorkerAdapter(const ThreadWorkerAdapter&) = delete;
    ThreadWorkerAdapter& operator=(const ThreadWorkerAdapter&) = delete;
    ThreadWorkerAdapter(ThreadWorkerAdapter&&) = delete;
    ThreadWorkerAdapter& operator=(ThreadWorkerAdapter&&) = delete;

    // The machine's hardware threads (1 when unknown), at most max_worker_count.
    [[nodiscard]] static std::size_t hardware_worker_count() noexcept;

    [[nodiscard]] std::size_t worker_count() const noexcept override;
    [[nodiscard]] core::Result<void> execute(
        std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override;
    [[nodiscard]] core::Result<void> execute_phase(
        std::string_view phase,
        std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override;
    [[nodiscard]] PhaseCounts phase_counts() const noexcept;

private:
    class Pool;
    std::size_t worker_count_;
    std::unique_ptr<Pool> pool_;
};

// The simulation's state hasher (#637, docs/simulation.md): one thread that hashes canonical
// state bytes (and derives hashes from them) in the order they come, off the stepping thread.
// At most `backlog` jobs wait; hash() and derive() wait for room beyond that, so a stepping
// thread that outruns the hasher stays at most a couple of ticks ahead. The destructor finishes
// every job it was given, so every StateHash it returned resolves. Without a thread (it could
// not start) both work on the caller.
class ThreadStateHasher final : public sim::StateHasher {
public:
    explicit ThreadStateHasher(std::size_t backlog = 4);
    ~ThreadStateHasher() override;

    ThreadStateHasher(const ThreadStateHasher&) = delete;
    ThreadStateHasher& operator=(const ThreadStateHasher&) = delete;
    ThreadStateHasher(ThreadStateHasher&&) = delete;
    ThreadStateHasher& operator=(ThreadStateHasher&&) = delete;

    [[nodiscard]] sim::StateHash hash(std::vector<std::uint8_t> canonical_bytes) override;
    [[nodiscard]] sim::StateHash derive(sim::StateHash base, Derivation derivation) override;

private:
    class Thread;
    std::unique_ptr<Thread> thread_;
};

// The worker counts every determinism test compares: 1, 2, 4, 8 and hardware_worker_count(),
// ascending and without duplicates.
[[nodiscard]] std::vector<std::size_t> determinism_worker_counts();

} // namespace eawr::platform
