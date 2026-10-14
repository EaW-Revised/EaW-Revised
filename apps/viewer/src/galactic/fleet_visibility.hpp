#pragma once

#include "eawr/sim/galactic/fleet_intelligence.hpp"

namespace eawr::viewer::galactic {

// WGUI-06: the current planet visibility predicate is true independently of
// discovery. Knowledge and stealth still gate fleet awareness in WGUI-28/29.
[[nodiscard]] inline std::vector<sim::galactic::FleetDisplayVisibility> live_fleet_visibility(
    const sim::galactic::GalacticSnapshot& snapshot) {
    std::vector<sim::galactic::FleetDisplayVisibility> result;
    if (!snapshot.observer()) return result;
    for (const auto& row : snapshot.observations()) {
        if (row.player == snapshot.observer()->local_player) result.push_back({row.planet, true});
    }
    return result;
}

// WGUI-30..32 producer seams: view-everything and the range/path predicate
// need explicit live owners. Missing values never imply visibility.
struct TransitVisibility {
    sim::galactic::FleetId fleet{};
    std::optional<bool> range_path_visible;
};
struct FleetMapVisibility {
    std::span<const sim::galactic::FleetDisplayVisibility> planets;
    std::span<const TransitVisibility> transit;
    bool view_everything{};
};

} // namespace eawr::viewer::galactic
