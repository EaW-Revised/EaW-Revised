#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreRetreat {
    std::optional<RetreatConfig> retreat;
    RetreatState retreat_state;
    std::optional<PlayerId> midbattle_solve;
};

struct SessionStoreOutcome {
    std::vector<StarbaseEntry> starbases;   // counted star bases standing, ascending ID (#77)
    std::optional<BattleOutcome> outcome;   // hashed once decided (#77)
    std::vector<DepartureControl> departure_controls;
    std::vector<PlayerId> basic_ai; // MP-37: departed factions retaining ownership under an AI controller
    std::vector<PlayerQuit> quits; // WBF-48: committed departure status, ascending player ID
    std::vector<BeaconState> beacons; // HCB-12: at most one active beacon per issuer.
};

} // namespace eawr::sim::tactical::session_detail
