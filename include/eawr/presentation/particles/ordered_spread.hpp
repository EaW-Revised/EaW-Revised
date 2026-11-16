#pragma once

#include "eawr/presentation/particles/render.hpp"

#include <algorithm>
#include <cstddef>

namespace eawr::presentation::particles {

// PF particle spread (E): runs the pure part of a per-item loop on the particle workers and keeps its stateful part on
// the calling thread, in item order.
//
// For items 0 .. count - 1, `prepare(i)` may run on any thread of `executor` (the calling thread included) and
// concurrently with the prepares of other items and with `commit` of earlier items; `commit(i)` runs on the calling
// thread, one item after another in index order, each after its own prepare. So a loop whose iteration i reads only
// item i's own state and shared read-only inputs before it writes anything gives the same result here as the plain
// `for (i) { prepare(i); commit(i); }` that runs without an executor (or with a count below `min_items`): the order of
// every write is the item order, whatever the thread count.
//
// The items are cut into contiguous slices (at least `min_slice` items each, at most StepExecutor::max_ordered
// slices); `slices_run`, when given, receives how many slices an executor ran (0 for the inline loop).
// False when the executor could not run every prepare (a prepare that threw, a pool failure). Failure is not
// all-or-nothing, as StepExecutor::run_ordered: the commits of the slices before the failed one stand.
template <class Prepare, class Commit>
[[nodiscard]] bool spread_ordered(const StepExecutor* const executor, const std::size_t count,
                                  const std::size_t min_slice, Prepare&& prepare, Commit&& commit,
                                  std::size_t* const slices_run = nullptr) {
    if (slices_run != nullptr) *slices_run = 0;
    const std::size_t floor = std::max<std::size_t>(min_slice, 1);
    if (executor == nullptr || executor->participants() < 2 || count < 2 * floor) {
        for (std::size_t index = 0; index < count; ++index) {
            prepare(index);
            commit(index);
        }
        return true;
    }
    const std::size_t slices = std::clamp<std::size_t>(count / floor, 1, StepExecutor::max_ordered);
    const auto first_of = [count, slices](const std::size_t slice) { return count * slice / slices; };
    const auto build = [&](const std::size_t slice) {
        for (std::size_t index = first_of(slice), last = first_of(slice + 1); index < last; ++index) prepare(index);
    };
    const auto consume = [&](const std::size_t slice) {
        for (std::size_t index = first_of(slice), last = first_of(slice + 1); index < last; ++index) commit(index);
    };
    if (slices_run != nullptr) *slices_run = slices;
    return executor->run_ordered(slices, build, consume);
}

} // namespace eawr::presentation::particles
