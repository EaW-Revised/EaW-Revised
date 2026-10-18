#pragma once

#include "session_types.hpp"
#include <tuple>

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreCommands {
    std::uint64_t completed_tick{};
    EntityId next_id{};
    std::uint64_t rng_state{};
    std::map<CommandKey, PlayerCommand> pending;
    std::map<PlayerId, std::pair<std::uint64_t, std::uint64_t>> last_submitted;
    std::vector<PlayerCommand> executed;
    explicit SessionStoreCommands(const TacticalSetup& source) : rng_state(source.seed) {}
};

struct SessionStoreObjectTurn {
    // #3644 object turn: the provider (not state), the re-entrancy guard held while it runs, the replayed
    // uses that wait for their tick's seam, and the uses already applied (the replay's object_turns).
    std::shared_ptr<ObjectTurnService> object_turn;
    bool object_turn_running{};
    std::map<std::tuple<std::uint64_t, EntityId, std::uint64_t>, ObjectTurnRecord> object_turn_pending;
    std::vector<ObjectTurnRecord> object_turns_executed;
};

} // namespace eawr::sim::tactical::session_detail
