#pragma once

#include <cstdint>

namespace eawr::script::foc::ai {
// Work in the last completed execution-service call on this thread. Diagnostics
// only: never retained, serialized, hashed, or consulted by AI decisions.
struct GalacticExecutionWork {
    std::uint64_t indexed_tasks{}, reduced_tasks{}, failure_tasks{};
    std::uint64_t stage_preparations{}, stage_reuses{}, production_sites{};
};
[[nodiscard]] GalacticExecutionWork galactic_execution_work() noexcept;
}
