#pragma once

#include "eawr/sim/world.hpp"

#include <cstddef>
#include <functional>
#include <memory>
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
class ThreadWorkerAdapter final : public sim::PartitionExecutor {
public:
    static constexpr std::size_t max_worker_count = 256;

    explicit ThreadWorkerAdapter(std::size_t worker_count);
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

private:
    class Pool;
    std::size_t worker_count_;
    std::unique_ptr<Pool> pool_;
};

// The worker counts every determinism test compares: 1, 2, 4, 8 and hardware_worker_count(),
// ascending and without duplicates.
[[nodiscard]] std::vector<std::size_t> determinism_worker_counts();

} // namespace eawr::platform
