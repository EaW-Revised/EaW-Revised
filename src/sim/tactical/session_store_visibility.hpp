#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreVisibility {
    std::optional<FogCells> fog;     // bound fog rules only (#274)
    // Per dynamic tracking layer (#71, AV-03): the frame of its last rebuild; its windows roll
    // every tracking interval from there.
    std::array<std::uint64_t, 4> tracking_anchor{};
};

} // namespace eawr::sim::tactical::session_detail
