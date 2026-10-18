#pragma once

#include "session_types.hpp"
#include "combat_internal.hpp"
#include "../../../third_party/entt/single_include/entt/entt.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreCollections {
    detail::CollectionTrees collection;        // #469: the candidate order of target scans
    detail::CollectionTrees projectile_collection; // DG-30: owned projectile-collidable history
};

struct SessionStoreRegistry {
    entt::registry registry;
    std::map<EntityId, entt::entity> handles;
    // The tick's gather buffers keep their capacity from tick to tick (never state, never encoded): the tick
    // takes them in gather() and hands them back when it ends. Contents are rebuilt every tick.
    struct GatherScratch {
        std::vector<std::pair<EntityId, entt::entity>> handles;
        std::vector<std::pair<EntityId, math::Vec3>> starts;
        std::vector<std::optional<CraftView>> crafts;
        std::vector<LiveUnit> units; // empty, capacity of the last tick's unit vector
    };
    GatherScratch gather_scratch;
};

} // namespace eawr::sim::tactical::session_detail
