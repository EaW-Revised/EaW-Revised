#pragma once

#include "eawr/platform/live_game_session.hpp"

#include <algorithm>

namespace eawr::viewer::galactic {
// #8182: what the battle child of `frame` is to one seat. A seat bound to a battle player enters it and commands (its own
// faction, or the side it joined). A seat of a side that took the spectate role enters it read-only, through the agreed
// observer perspective: it sees the battle, never selects a unit and never orders. Any other seat (no battle player and no
// published spectate role, including a role that is still pending) does not enter: it stays on the galaxy, so a pending
// role never yields a commandable view.
struct BattleSeatView {
    bool enter{};
    // The battle player whose perspective the view follows (the observer perspective for a spectator).
    sim::tactical::PlayerId view_player{};
    // Whether the seat may select units and issue orders in the view (false for a spectator).
    bool commandable{};
    friend constexpr bool operator==(const BattleSeatView&, const BattleSeatView&) noexcept = default;
};

[[nodiscard]] inline BattleSeatView battle_seat_view(const platform::LiveGameFrame& frame, const sim::SeatId seat) {
    if (!frame.conflict) return {};
    const auto binding = std::find_if(frame.conflict->seats.begin(), frame.conflict->seats.end(),
        [&](const auto& row) { return row.seat == seat; });
    if (binding == frame.conflict->seats.end()) return {};
    if (binding->player != 0) return {true, binding->player, true};
    const auto spectator = std::find_if(frame.battle_spectators.begin(), frame.battle_spectators.end(),
        [&](const auto& row) { return row.seat == seat; });
    if (spectator == frame.battle_spectators.end() || spectator->observer == 0) return {};
    return {true, spectator->observer, false};
}
} // namespace eawr::viewer::galactic
