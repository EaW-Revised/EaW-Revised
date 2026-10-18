#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreSquadronRoster {
    std::vector<Squadron> squadrons; // live, ascending container ID (#271)
    explicit SessionStoreSquadronRoster(const TacticalSetup& source) : squadrons(source.squadrons) {}
};

struct SessionStoreFlightScratch {
    std::map<EntityId, math::TrigCache> craft_trig; // scratch; one writer per craft
};

struct SessionStoreFlights {
    std::map<EntityId, CraftState> crafts;     // #75: squadron craft that fly by a craft profile
    std::map<EntityId, SquadronState> minds;   // #75: by container, squadrons of a known type
    std::map<EntityId, SpawnerState> spawners; // #75: hangars of SPAWN_SQUADRON units
};

struct SessionStoreDeathSpins {
    std::vector<DeathSpin> spins;              // #447: killed craft spinning away, ascending ID
};

} // namespace eawr::sim::tactical::session_detail
