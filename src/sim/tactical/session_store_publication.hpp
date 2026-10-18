#pragma once

#include "session_types.hpp"

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
    std::shared_ptr<const TacticalSnapshot> current_snapshot;
    std::uint64_t registry_emplacement_count{};
    std::function<void(std::string_view, bool)> commit_observer;
    std::shared_ptr<StateHasher> hasher; // #637: null hashes on the stepping thread
};

} // namespace eawr::sim::tactical::session_detail
