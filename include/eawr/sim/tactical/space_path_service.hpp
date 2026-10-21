#pragma once

#include "eawr/sim/tactical/path_service.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

namespace eawr::sim::tactical {

// Space's state lattice implements the shared service without changing its
// search order, accounting, copied inputs or checkpoint representation.
struct SpacePathBackend final {
    using Search = SlicedPathSearch;
    using Checkpoint = Search::Checkpoint;
    using Stats = PathSearchStats;

    template <class... Args>
    [[nodiscard]] static Search begin(Args&&... args) { return Search(std::forward<Args>(args)...); }
    [[nodiscard]] static auto restore(const MotionTable& table, const Checkpoint& checkpoint) {
        return Search::from_checkpoint(table, checkpoint);
    }
    template <class... Args>
    [[nodiscard]] static auto plan(Args&&... args) { return plan_space_move(std::forward<Args>(args)...); }
    template <class... Args>
    [[nodiscard]] static auto plan_within(Args&&... args) { return plan_space_move_within(std::forward<Args>(args)...); }
};

using SpacePathService = PathService<SpacePathBackend>;

} // namespace eawr::sim::tactical
