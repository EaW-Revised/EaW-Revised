#pragma once

#include "galactic_lan_hud.hpp"
#include "galactic_launch.hpp"

namespace eawr::viewer::galactic {
// Shared by the viewer constructor and engine-free launch contracts.
template<class Options>
void apply_lobby_launch(Options& options,
    std::shared_ptr<presentation::godot_backend::GalacticLaunch> launch) {
    options.campaign = launch->options.campaign;
    options.seat = launch->seat;
    options.lan_match = launch->match;
    options.lan_players.clear();
    for (const auto& seat : launch->roster.seats) {
        const auto peer = static_cast<net::PeerIndex>(options.lan_players.size());
        const bool coop = std::count_if(launch->roster.seats.begin(), launch->roster.seats.end(),
            [&](const auto& other) { return other.player == seat.player; }) > 1;
        options.lan_players.push_back({peer, seat.seat, seat.selection.name, seat.selection.faction,
            coop ? "remaining players control the faction" : "configured faction control takes over"});
    }
    options.lan_start = std::move(launch);
}
} // namespace eawr::viewer::galactic
