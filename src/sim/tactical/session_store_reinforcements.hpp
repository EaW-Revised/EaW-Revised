#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreReinforcements {
    std::map<CommandKey, TacticalSession::ReinforcementSearch> reinforcement_searches;
    std::array<std::uint8_t, 32> campaign_binding{}; // derived from the sealed initial replay setup
    std::map<std::pair<PlayerId, std::uint64_t>, TacticalSession::ReinforcementSearchResult> reinforcement_search_results;
};

} // namespace eawr::sim::tactical::session_detail
