#pragma once

#include "eawr/checkpoint/tactical.hpp"
#include "eawr/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Restore and continue (docs/checkpoint-format.md): save at a tick, restore, continue, and
// compare every later tick's state hash with the uninterrupted run. Generic over the session
// kind; tactical_continue_case adapts a tactical replay, and the galaxy adds its own adapter.
namespace eawr::checkpoint {

struct ContinueCase {
    std::uint64_t final_tick{};
    // The uninterrupted run's state hashes; element t - 1 is tick t's.
    std::function<core::Result<std::vector<std::string>>()> reference;
    // A checkpoint taken at `tick` of a fresh run.
    std::function<core::Result<std::vector<std::uint8_t>>(std::uint64_t tick)> save_at;
    // Restores `bytes` and continues to final_tick; returns ticks tick + 1 .. final_tick's hashes.
    std::function<core::Result<std::vector<std::string>>(std::span<const std::uint8_t> bytes, std::uint64_t tick)>
        restore_and_run;
};

struct ContinueSample {
    std::uint64_t tick{};
    std::size_t checkpoint_bytes{};
    std::optional<std::uint64_t> first_divergent_tick; // none: every later tick matched
};

// Up to `count` distinct ticks spread over the run: final * k / (count + 1), k = 1 .. count.
[[nodiscard]] std::vector<std::uint64_t> sample_ticks(std::uint64_t final_tick, std::size_t count = 3);

// Fails only when a run fails; a hash difference is a sample's first_divergent_tick.
[[nodiscard]] core::Result<std::vector<ContinueSample>> run_restore_and_continue(
    const ContinueCase& subject, std::span<const std::uint64_t> ticks);

struct TacticalContinueOptions {
    // Scrambles the restored session's private storage order before it continues.
    bool scramble_after_restore{};
    std::vector<Seat> seats; // empty: default_seats of the replay's setup
};

// A tactical replay: the reference runs on `reference_executor`, each restored session continues
// on `continue_executor`. The executors and factory must outlive the case.
[[nodiscard]] ContinueCase tactical_continue_case(const sim::tactical::TacticalReplay& replay,
    const TacticalSessionFactory& factory, const sim::PartitionExecutor& reference_executor,
    const sim::PartitionExecutor& continue_executor, TacticalContinueOptions options = {});

} // namespace eawr::checkpoint
