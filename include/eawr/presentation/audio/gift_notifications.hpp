#pragma once

#include "eawr/sim/tactical/types.hpp"
#include <set>
#include <span>
#include <tuple>

namespace eawr::presentation::audio {
// Owner gift cue: committed transfers notify both seats, once per command rather
// than once per root/member. The caller consumes each reached tick only once.
[[nodiscard]] inline std::size_t gift_notifications(
    std::span<const sim::tactical::Event> events, sim::SeatId local) {
    std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint64_t, std::uint32_t>> gifts;
    if (local.value == 0) return 0;
    for (const auto& event : events) {
        if (event.kind != sim::tactical::EventKind::controller_changed
            || event.order != sim::tactical::OrderKind::gift
            || event.previous_controller.value == 0 || event.controller.value == 0
            || event.previous_controller == event.controller
            || (local != event.previous_controller && local != event.controller)) continue;
        // One command = one cue, even when its units came from several previous seats.
        gifts.emplace(event.tick, event.player, event.sequence, event.controller.value);
    }
    return gifts.size();
}
} // namespace eawr::presentation::audio
