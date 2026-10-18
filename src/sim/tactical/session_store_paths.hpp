#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStorePaths {
    // What plan_path plans from (PC-08): the unit's position, yaw and speed, and its limits
    // under a group move's `max_speed`.
    struct PathInputs {
        math::Vec3 position{};
        math::Fixed yaw{};
        math::Fixed speed{};
        MotionProfile limits{};
        friend bool operator==(const PathInputs&, const PathInputs&) noexcept = default;
    };
    // A path search that runs in slices and lands at `frame` (PC-08, #520): the plan the unit
    // would make at `frame` towards `target`, from where its current plan puts it then, against
    // its layer and the static layer as `world` holds them now. `inputs` is what it plans from;
    // the landing takes the plan only while the unit still plans from exactly that.
    struct SlicedSearch {
        SlicedPathSearch search;
        std::uint64_t frame{};
        math::Vec3 target{};
        std::optional<math::Fixed> max_speed;
        PathInputs inputs;
        // PC-09: the plan, once the search has ended; its unit's layer predicts it from then on.
        std::optional<MotionState> plan;
    };
    // Path searches running in slices (PC-08, #520), by unit; each unit waits for its search
    // with a sliced FormationWait of the same frame. A search's copied views and progress are a
    // function of the ticks before it, and its wait is hashed.
    std::map<EntityId, SlicedSearch> searches;
};

struct SessionStoreChaseScratch {
    // #893: query buffers belong exclusively to their phase partition; staged
    // lists retain their high-water capacity and are committed in scanner order.
    std::array<ChaseQueryScratch, tick_partition_count> chase_scratch;
    std::vector<std::vector<EntityId>> chase_candidates;
};

} // namespace eawr::sim::tactical::session_detail
