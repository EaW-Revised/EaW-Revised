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
    // The collection-boxes phase's per-partition output and the concatenated lists (never state, never encoded): the
    // buffers keep their capacity from tick to tick, so the phase adds no per-tick allocations; contents are cleared
    // before each use.
    struct BoxScratch {
        struct Part {
            std::vector<detail::CollectionTrees::Member> members, collidables;
            std::vector<detail::CollectionTrees::Change> member_changes, collidable_changes;
            std::optional<core::Diagnostic> error;
        };
        std::array<Part, tick_partition_count> parts;
        std::vector<detail::CollectionTrees::Member> members, collidables;
        std::vector<detail::CollectionTrees::Change> member_changes, collidable_changes;
    };
    BoxScratch box_scratch;
    // The registry-commit phase's per-partition output (never state, never encoded): the fixed 64 index ranges of the
    // survivor vector each fill their own slot, and the ordered tail replays the structural ops of slots 0..63 in order.
    // Buffers keep their capacity from tick to tick; they hold only sparse ops, so they stay small.
    struct CommitPartition {
        struct Op {
            entt::entity handle{entt::null}; // null: the unit has no entity yet
            std::uint32_t index{};           // position in the survivor vector
            std::uint32_t mask{};            // components whose presence differs, plus a bit for a new entity
        };
        std::vector<Op> ops;
        std::vector<std::pair<std::size_t, std::size_t>> ownership; // (before, after) key slots of a changed identity
        std::uint64_t writes{};
        bool waypoints{};
    };
    std::array<CommitPartition, tick_partition_count> commit_scratch;
};

} // namespace eawr::sim::tactical::session_detail
