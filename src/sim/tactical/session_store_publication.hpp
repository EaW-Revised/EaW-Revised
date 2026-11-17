#pragma once

#include "session_types.hpp"

#include <unordered_map>

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreHistory {
    // WBF-45: immutable history is shared by snapshots between sparse notification commits.
    std::shared_ptr<const std::vector<BattleLoss>> losses = std::make_shared<const std::vector<BattleLoss>>();
    std::shared_ptr<const std::vector<BattleProduction>> productions = std::make_shared<const std::vector<BattleProduction>>();
    std::shared_ptr<const std::vector<BattleEconomyCue>> economy_cues = std::make_shared<const std::vector<BattleEconomyCue>>();
    std::vector<SnapshotPlayer> snapshot_players;
    std::map<PlayerId, TeamId> teams;
};

struct SessionStorePublication {
    bool waypoint_queues_present{}; // Derived component-commit gate, outside canonical state.
    // PL-01: the immutable copy of each moving ship's move plan, kept while the plan stands so every snapshot
    // publishes a pointer. Derived presentation cache: never hashed, never checkpointed. Workers only read it;
    // the serial tail of the systems phase adds a plan when it changes.
    // An entry carries the stamp of the plan it copied (start tick, target and the shape of the nodes), so
    // a tick checks a moving ship in constant time instead of comparing every node.
    struct PredictedPathEntry {
        std::shared_ptr<const PredictedPath> path;
        std::uint64_t start_tick{};
        math::Vec3 target{};
    };
    std::unordered_map<EntityId, PredictedPathEntry> predicted_paths;
    // PL-01: sparse disjoint staging retains capacity across ticks; snapshots own only live rows.
    std::array<std::vector<std::pair<std::size_t, SnapshotPredictedPath>>, tick_partition_count> prediction_scratch;
    std::uint64_t predicted_path_builds{}; // plans copied so far (work counter)
    std::uint64_t predicted_path_visits{};
    std::shared_ptr<const TacticalSnapshot> current_snapshot;
    std::uint64_t registry_emplacement_count{};
    std::function<void(std::string_view, bool)> commit_observer;
    std::shared_ptr<StateHasher> hasher; // #637: null hashes on the stepping thread
    StateHashPolicy hash_policy{StateHashPolicy::always}; // on_demand: steps skip the encode + hash
};

} // namespace eawr::sim::tactical::session_detail
