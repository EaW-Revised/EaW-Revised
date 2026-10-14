#pragma once

#include "hud_clock_fixture.hpp"

namespace eawr::viewer::galactic {
// An explicit synthetic campaign before conquest. The live LandFleet order below
// must update ownership and observer permissions; no post-capture facts are injected.
inline void configure_capture_hud_fixture(sim::galactic::GalacticSetup& setup,
    sim::galactic::PlayerId local) {
    namespace gc = sim::galactic;
    const auto whole = [](std::int64_t value) { return sim::math::Fixed::from_raw(value * sim::math::Fixed::scale); };
    configure_synthetic_hud_production(setup, local);
    const auto defender = local == 1 ? 2U : 1U;
    setup.production->technology_types = {{1003, 2, false, false, {defender}},
        {1004, 2, false, false, {local}}};
    setup.planets.front().owner = defender;
    setup.fleets = {{601, local, 101, {{7001, 1003}}}};
    setup.fleet_control->types.push_back({1003, true, true, false});
    setup.observer->types.push_back({1003, false, false, false});
    auto& control = setup.control.emplace();
    control.change_seconds = whole(1); control.max_ground_forces = 7;
    control.types = {{1003, 0, false, true}};
    for (const auto& player : setup.players) {
        control.players.push_back({player.player_id, player.player_id == setup.players.back().player_id,
            player.player_id != setup.players.back().player_id, player.faction_id});
        for (const auto& other : setup.players)
            control.relations.push_back({player.player_id, other.player_id, player.player_id == other.player_id});
    }
    for (const auto& planet : setup.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = planet.id; row.surface_accessible = true;
        row.retains_residual = true;
        if (planet.id != 101) row.ground_base = {planet.owner, 1};
        control.planets.push_back(row);
    }
    for (const auto& route : setup.routes) control.routes.push_back({route.id, {}});
    for (auto& row : setup.production->planets) if (row.planet == 101) {
        row.target_owner = defender; row.starbase_owner = row.ground_occupier = 0;
        row.space_level = row.ground_level = 0; row.maximum_ground_forces = control.max_ground_forces;
        row.ground_slots = 5;
        std::erase_if(row.offers, [](const auto& offer) { return offer.type != 1004; });
        row.offers.front().affiliated = false; // Cached initial-owner result must not hide the capturer's type.
        auto enemy = row.offers.front(); enemy.type = 1003; enemy.affiliated = true; enemy.population = whole(1);
        row.offers.insert(row.offers.begin(), enemy);
        row.objects = {{8001, 1004, defender, whole(100), gc::ProductionDomain::ground, gc::ProductionKind::structure}};
    }
    for (auto& row : setup.observer->planets) if (row.planet == 101) {
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = defender;
        row.ground_owner = row.space_owner = 0; row.ground_level = row.space_level = 0;
    }
    for (auto& row : setup.observer->records) if (row.planet == 101) {
        row.discovered = true; row.mask = row.player == defender ? gc::owner_intelligence : 0U;
    }
}
} // namespace eawr::viewer::galactic
