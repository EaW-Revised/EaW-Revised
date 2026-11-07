#pragma once

#include "eawr/sim/world.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::sim::tactical::session_detail {

// A tick that ends with fewer elements than this in every vector frees them on the stepping thread: waking the
// pool costs more than the frees. A fixed work cutoff, independent of the worker count (as the gather's is).
inline constexpr std::size_t release_parallel_cutoff = 256;

// Frees the heap memory owned by the elements of `vectors` in the fixed 64 index ranges of each vector, one phase
// for all of them, and leaves every element default-constructed (the vectors keep their size and capacity; T must be default-constructible and move-assignable). The
// caller's ordinary destruction or clear() then only walks empty elements. Destruction has no ordered effect: each
// partition touches only the elements of its own ranges, so the thread that runs a range never changes a result.
// A phase that fails or is skipped leaves untouched elements in place, which the normal destructor still frees.
// Returns how many elements were reset (a work count, never state).
template <typename T>
std::size_t release_elements(const PartitionExecutor& executor, const std::string_view phase,
    const std::span<std::vector<T>* const> vectors) {
    std::size_t largest = 0;
    for (const auto* values : vectors) largest = std::max(largest, values->size());
    if (largest < release_parallel_cutoff) return 0;
    std::array<std::size_t, tick_partition_count> released{};
    static_cast<void>(executor.execute_phase(phase, tick_partition_count, [&](const std::size_t partition) {
        std::size_t count = 0;
        for (auto* values : vectors) {
            const auto range = partition_range(partition, values->size());
            for (auto index = range.begin; index < range.end; ++index) {
                // Move-assigning a fresh element frees the old one's heap memory and leaves a valid element
                // whatever happens, which destroy-then-construct would not if the constructor threw.
                (*values)[index] = T{};
                ++count;
            }
        }
        released[partition] = count;
    }));
    std::size_t total = 0;
    for (const auto count : released) total += count;
    return total;
}

} // namespace eawr::sim::tactical::session_detail
