#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::sim {

// Every partitioned tick phase splits its N ascending-ID inputs into this many contiguous
// ranges, whatever the executor's worker count, so no result can depend on the worker count
// (docs/simulation.md, phase map).
inline constexpr std::size_t tick_partition_count = 64;

struct PartitionRange {
    std::size_t begin{};
    std::size_t end{};
};

// Partition p of N inputs: floor(p*N/64)..floor((p+1)*N/64).
[[nodiscard]] constexpr PartitionRange partition_range(const std::size_t partition, const std::size_t size) noexcept {
    return PartitionRange{partition * size / tick_partition_count, (partition + 1) * size / tick_partition_count};
}

// Runs partition(0) .. partition(partition_count - 1), each exactly once, possibly
// concurrently, and returns when all have finished. Partitions read copied inputs and write
// disjoint outputs (ADR-009), so the order and the threads that run them never change a result.
class PartitionExecutor {
public:
    virtual ~PartitionExecutor() = default;
    [[nodiscard]] virtual std::size_t worker_count() const noexcept = 0;
    [[nodiscard]] virtual core::Result<void> execute(
        std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const = 0;
    // A named tick phase (the phase map in docs/simulation.md). Executors that record or time
    // phases override it; the default runs execute().
    [[nodiscard]] virtual core::Result<void> execute_phase(
        std::string_view phase,
        std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const {
        static_cast<void>(phase);
        return execute(partition_count, partition);
    }
};

class InlineExecutor final : public PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override;
    [[nodiscard]] core::Result<void> execute(
        std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override;
};

struct TickResult {
    std::uint64_t completed_tick{};
    std::string state_sha256;
    std::shared_ptr<const RenderSnapshot> snapshot;
};

class World final {
public:
    World(World&&) noexcept;
    World& operator=(World&&) noexcept;
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] static core::Result<World> create(const Replay& replay);
    [[nodiscard]] core::Result<TickResult> step(const PartitionExecutor& executor);

    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] std::uint64_t final_tick_count() const noexcept;
    [[nodiscard]] EntityId next_entity_id() const noexcept;
    [[nodiscard]] std::uint64_t rng_state() const noexcept;
    [[nodiscard]] std::uint64_t tick_nonce() const noexcept;
    [[nodiscard]] std::vector<EntityState> entities() const;
    [[nodiscard]] std::string state_sha256() const;
    [[nodiscard]] std::shared_ptr<const RenderSnapshot> snapshot() const noexcept;

    // Test seam: changes only private ECS storage order. Authoritative ordering is unchanged.
    void scramble_storage_for_testing();

private:
    class Impl;
    explicit World(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::sim
