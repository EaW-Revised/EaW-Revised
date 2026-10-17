#pragma once

#include "eawr/sim/galactic/fleet_intelligence.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {

// WGUI-72..74: the fleet projection leaves an enemy group untyped (a bare count, no icon) when its type has no
// display trait, so a live campaign supplies one trait per type from the authored object. `Art` maps an object name
// to a row with `hero`, `named_hero`, `force_sensitive` and an optional `max_health` (Tactical_Health).
// `damage` carries the authored maximum health as the ranking key of the strongest-ship rule (not a current value).
template <class Art>
[[nodiscard]] std::vector<sim::galactic::FleetDisplayType> live_fleet_display_traits(
    const std::map<sim::galactic::TypeId, std::string>& names, const Art& art) {
    std::vector<sim::galactic::FleetDisplayType> traits;
    traits.reserve(names.size());
    for (const auto& [type, name] : names) {
        const auto row = art.find(name);
        if (row == art.end()) continue;
        sim::galactic::FleetDisplayType trait;
        trait.type = type; trait.hero = row->second.hero; trait.named_hero = row->second.named_hero;
        trait.force_sensitive = row->second.force_sensitive;
        if (row->second.max_health)
            trait.damage = sim::math::Fixed::from_raw(static_cast<std::int64_t>(
                std::llround(static_cast<double>(*row->second.max_health) * static_cast<double>(sim::math::Fixed::scale))));
        traits.push_back(trait);
    }
    return traits;
}

} // namespace eawr::viewer::galactic
