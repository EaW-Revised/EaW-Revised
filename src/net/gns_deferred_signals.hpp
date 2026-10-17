#pragma once

#include "eawr/net/gns_room.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>

namespace eawr::net {
// Preserve the existing single-pair allowance for delayed incident arming and
// native retries. Each authenticated sender owns its quota independently.
inline constexpr std::size_t max_deferred_signals_per_peer = 32;

[[nodiscard]] inline bool retain_deferred_signal(std::deque<GnsRoomSignal>& pending, const GnsRoomSignal& signal) {
    const auto count = static_cast<std::size_t>(std::count_if(pending.begin(), pending.end(),
        [&](const auto& previous) { return previous.from_slot == signal.from_slot; }));
    if (count >= max_deferred_signals_per_peer) return false;
    pending.push_back(signal);
    return true;
}
} // namespace eawr::net
