#pragma once

#include "eawr/sim/world.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Test support for the order-independence rule of the partitioned phases (ADR-009): every partition
// reads copied inputs and writes disjoint outputs, so the order partitions run in never changes a
// result. This executor runs each phase on the calling thread with the highest partition first, the
// opposite of InlineExecutor, and records every phase it was handed.
namespace eawr::test_support {

class ReverseOrderExecutor final : public sim::PartitionExecutor {
public:
    struct Phase {
        std::string name;
        std::size_t partitions{};
        std::vector<std::size_t> visited; // in the order the partitions ran
        [[nodiscard]] bool descending() const {
            for (std::size_t i = 0; i < visited.size(); ++i) if (visited[i] != visited.size() - 1 - i) return false;
            return visited.size() == partitions;
        }
    };

    // `reported_workers` is only what worker_count() answers; the partitions always run serially.
    explicit ReverseOrderExecutor(std::size_t reported_workers = 1) : reported_workers_(reported_workers) {}

    [[nodiscard]] std::size_t worker_count() const noexcept override { return reported_workers_; }
    [[nodiscard]] core::Result<void> execute(std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override {
        return execute_phase({}, partition_count, partition);
    }
    [[nodiscard]] core::Result<void> execute_phase(std::string_view phase, std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override {
        auto& record = phases.emplace_back();
        record.name = std::string(phase);
        record.partitions = partition_count;
        for (std::size_t i = partition_count; i-- > 0;) {
            record.visited.push_back(i);
            partition(i);
        }
        return core::Result<void>::success();
    }

    mutable std::deque<Phase> phases; // a deque: nested phases append while an outer record is live

private:
    std::size_t reported_workers_{};
};

} // namespace eawr::test_support
