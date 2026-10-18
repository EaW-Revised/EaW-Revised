#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreStationSpawn {
    struct FreeGarrisonState {
        FreeGarrisonSetup setup;
        std::vector<EntityId> registered;
        std::optional<std::uint64_t> due_frame;
        std::vector<TypeId> pending;
    };
    std::vector<FreeGarrisonState> free_garrisons; // FL-14: player order, committed only after successful ticks
};

} // namespace eawr::sim::tactical::session_detail
