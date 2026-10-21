#pragma once
#include "eawr/sim/galactic/types.hpp"

namespace raid_test {
inline eawr::sim::galactic::GalacticSetup setup() {
    namespace gc = eawr::sim::galactic;
    const auto f = [](std::int64_t n) { return eawr::sim::math::Fixed::from_integer(n).value(); };
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::human, 1, true, 2}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 2}};
    s.routes = {{3, 1, 2, f(1)}};
    s.rules.maximum_fleet_movement_distance = f(12); s.rules.fiscal_cycle_frames = 1000;
    s.fleets = {{10, 1, 1, {{20, 1}}}, {11, 2, 2, {{22, 3}}}};
    s.fleet_control.emplace();
    s.fleet_control->types = {{1, true, true, false}, {2, false, true, false}, {3, false, true, false}};
    s.movement.emplace(); auto& m = *s.movement;
    m.mode_modifier = f(1);
    m.types = {{1, f(1), {}, 0, true, false, false, false, true},
        {2, {}, {}, 0, false, false, false, false, false},
        {3, f(1), {}, 0, true, false, false, false, false}};
    m.contents = {{20, {{21, 2}}}};
    m.players = {{1, f(1), {}, false}, {2, f(1), {}, false}, {3, f(1), {}, false}};
    m.planets = {{1, f(1), false}, {2, f(1), false}};
    for (const auto& player : s.players) for (const auto& planet : s.planets)
        m.access.push_back({player.player_id, planet.id, true, planet.id == 2});
    m.raid = gc::RaidSetup{{1, 1, 2, 1, 2}, {{1, 0, false, false}, {2, 2, false, false}, {3, 0, false, false}}};
    s.control.emplace(); auto& c = *s.control;
    c.change_seconds = f(7); c.max_ground_forces = 10;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players)
        c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 0, false, true}, {2, 1, false, false}, {3, 1, false, false}};
    c.ground_capability_types = {{1, {}, true, false}, {2, f(7), false, false}, {3, f(7), false, false}};
    for (const auto& planet : s.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1};
        c.planets.push_back(row);
    }
    c.routes = {{3, f(1)}};
    return s;
}
} // namespace raid_test
