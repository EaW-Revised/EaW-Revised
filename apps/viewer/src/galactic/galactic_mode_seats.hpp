#pragma once

#include "galaxy_snapshot_stub.hpp"

#include <algorithm>
#include <limits>

namespace eawr::viewer::galactic {
// Authored campaigns are single-player unless the preview explicitly opts into
// co-op. Seat and player IDs remain independent; an AI is added once per player.
[[nodiscard]] inline std::optional<StubSeat> configure_campaign_seats(
    GalaxySnapshotStub& snapshot, SeatId selected, bool coop_preview) {
    const StubSeat* observer = nullptr;
    for (const auto& seat : snapshot.seats) {
        if (!seat.active || !seat.human) continue;
        if (selected ? seat.id == selected : (!observer || seat.id < observer->id)) observer = &seat;
    }
    if (!observer) return std::nullopt;
    const auto local = *observer;
    if (!coop_preview) snapshot.seats = {local};
    SeatId next{};
    for (const auto& seat : snapshot.seats) next = std::max(next, seat.id);
    for (const auto& player : snapshot.players) {
        if (std::any_of(snapshot.seats.begin(), snapshot.seats.end(), [&](const auto& seat) {
            return seat.active && seat.player == player.id;
        })) continue;
        if (next == std::numeric_limits<SeatId>::max()) return std::nullopt;
        snapshot.seats.push_back({++next, player.id, false, true});
    }
    return local;
}
} // namespace eawr::viewer::galactic
