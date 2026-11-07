#pragma once
#include "eawr/sim/galactic/snapshot.hpp"
#include <algorithm>
#include <cstddef>
#include <optional>

namespace eawr::testing::galactic {
// Where a live hero stands: the object a player drags for it, a fleet or a landed force, found from published actors.
struct HeroPlace {
    sim::EntityId bearer{}, dragged{};
    sim::galactic::PlanetId planet{};
    bool fleet{}, transit{};
};
[[nodiscard]] inline std::optional<HeroPlace> find_hero_place(const sim::galactic::GalacticSnapshot& snapshot,
    sim::galactic::PlayerId player, sim::galactic::TypeId type) {
    const auto actors = snapshot.actor_records();
    const auto bearer = std::find_if(actors.begin(), actors.end(), [&](const auto& row) {
        return row.owner == player && !row.dead && !row.delete_pending
            && (row.original_type == type || row.current_type == type);
    });
    if (bearer == actors.end()) return std::nullopt;
    HeroPlace place; place.bearer = bearer->id;
    auto id = bearer->id;
    for (std::size_t remaining = actors.size() + 1; id && remaining; --remaining) {
        const auto row = std::find_if(actors.begin(), actors.end(), [&](const auto& actor) { return actor.id == id; });
        if (row == actors.end()) return std::nullopt;
        const auto parent = row->container;
        const auto fleets = snapshot.fleets();
        const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& candidate) { return candidate.id == parent; });
        if (fleet != fleets.end()) {
            place.dragged = fleet->id; place.planet = fleet->orbit; place.fleet = true; place.transit = fleet->motion.transit.has_value();
            return place;
        }
        const auto planets = snapshot.planets();
        if (std::any_of(planets.begin(), planets.end(), [&](const auto& planet) { return planet.id == parent; })) {
            place.dragged = id; place.planet = parent;
            return place;
        }
        id = parent;
    }
    return std::nullopt;
}
} // namespace eawr::testing::galactic
