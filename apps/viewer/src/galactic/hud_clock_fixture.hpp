#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include <algorithm>

namespace eawr::viewer::galactic {

// Explicit HUD test input, never campaign initialization. A zero-income ledger
// and a 300-frame window let the session publish fiscal cycle one and pip progress.
inline void configure_synthetic_hud_clock(sim::galactic::GalacticSetup& setup) {
    setup.rules.fiscal_cycle_frames = 300;
    setup.player_economy.emplace();
    setup.fiscal.emplace();
    const auto scale = sim::math::Fixed::scale;
    setup.fiscal->credit_cap_per_planet = sim::math::Fixed::from_raw(20000 * scale);
    for (const auto& player : setup.players) {
        // GHUD-R01..R03 fractional balance input; production fixtures replace it.
        setup.player_economy->players.push_back({player.player_id,
            sim::math::Fixed::from_raw(12422 * scale + scale / 2),
            sim::math::Fixed::from_raw(20000 * scale), 0, 0});
        setup.fiscal->players.push_back({player.player_id, false,
            sim::math::Fixed::from_raw(sim::math::Fixed::scale)});
    }
    for (const auto& planet : setup.planets) setup.fiscal->planets.push_back({planet.id, {}});
}

// Synthetic prices and durations deliberately differ from stock. Native button
// tests exercise the real production command and observer publication path.
inline void configure_synthetic_hud_production(sim::galactic::GalacticSetup& setup,
    sim::galactic::PlayerId local) {
    namespace gc = sim::galactic;
    const auto whole = [](std::int64_t value) { return sim::math::Fixed::from_raw(value * sim::math::Fixed::scale); };
    setup.fiscal->credit_cap_per_planet = whole(10000);
    setup.rules.maximum_fleet_movement_distance = whole(10000); // explicit synthetic graph input
    setup.fleet_control.emplace(); setup.production.emplace(); setup.observer.emplace();
    setup.fleet_control->types = {{1, false, true, false}, {1000, false, true, false}};
    setup.production->maximum_human_queue = 5;
    setup.observer->local_player = local; setup.observer->reveal_delay_seconds = whole(1);
    setup.observer->types = {{1, false, false, false}};
    for (auto& seat : setup.seats) if (seat.player == local) {
        seat.kind = sim::SeatKind::human; seat.human_side_id = local == 3 ? 1 : local;
    }
    for (std::size_t i = 0; i < setup.players.size(); ++i) {
        const auto player = setup.players[i].player_id;
        const bool human = std::any_of(setup.seats.begin(), setup.seats.end(),
            [player](const auto& seat) { return seat.player == player && seat.kind == sim::SeatKind::human; });
        setup.player_economy->players[i] = {player, whole(1000), whole(10000), 2, 5};
        setup.production->players.push_back({player, human, !human, human, whole(100), {}, {}, {}, {}});
        setup.observer->players.push_back({player, !human, false, 0, 1, {}});
    }
    setup.census.emplace();
    setup.census->types = {{1, whole(1), 0}, {1000, whole(1), 0}, {1001, whole(0), 0},
        {1002, whole(0), 0}, {1003, whole(1), 0}, {1004, whole(0), 0}};
    for (const auto& player : setup.players) setup.census->players.push_back({player.player_id, 100, false, {}});
    for (auto& player : setup.fiscal->players) player.earns_planet_income = true;
    for (auto& planet : setup.fiscal->planets) planet.credits = whole(135);
    // Explicit synthetic contribution inputs, independent of stock data.
    setup.fiscal->contributions.emplace();
    for (const auto& player : setup.players)
        setup.fiscal->contributions->factions.push_back({player.player_id, gc::IncomeAlignment::neutral});
    for (const auto& planet : setup.planets)
        setup.fiscal->contributions->planets.push_back({planet.id, gc::IncomeAlignment::neutral, {}});
    for (const auto& route : setup.routes)
        setup.fiscal->contributions->routes.push_back({route.id, sim::math::Fixed::from_raw(sim::math::Fixed::scale / 2)});
    setup.planets.front().owner = local;
    for (const auto& planet : setup.planets) {
        gc::PlanetProduction producer;
        producer.planet = planet.id; producer.target_owner = producer.starbase_owner = planet.owner;
        producer.ground_occupier = planet.owner; producer.enabled = producer.surface_accessible = true;
        producer.space_level = 1; producer.maximum_space_level = 5;
        producer.ground_slots = producer.space_slots = 3; producer.maximum_ground_forces = 100;
        for (gc::TypeId type : {1000U, 1001U, 1002U, 1003U, 1004U}) {
            gc::ProductionOffer offer;
            offer.type = type; offer.domain = type >= 1003 ? gc::ProductionDomain::ground : gc::ProductionDomain::space;
            offer.kind = type == 1001 ? gc::ProductionKind::starbase
                : type == 1002 ? gc::ProductionKind::technology
                : type == 1004 ? gc::ProductionKind::structure : gc::ProductionKind::unit;
            offer.price = offer.authored_price = whole(type == 1002 ? 200 : 100); offer.seconds = whole(10);
            offer.population = type == 1000 || type == 1003 ? whole(1) : whole(0);
            offer.tech = 2; offer.target_tech = 3; offer.base_level = 2;
            offer.affiliated = offer.direct_build_allowed = true;
            producer.offers.push_back(offer);
        }
        setup.production->planets.push_back(producer);
        gc::ObserverPlanet facts;
        facts.planet = planet.id; facts.allegiance = facts.target_owner = planet.owner;
        facts.current_owner = facts.current_controller = planet.owner;
        facts.space_owner = facts.ground_owner = planet.owner;
        facts.space_level = 1;
        setup.observer->planets.push_back(facts);
    }
    for (const auto& player : setup.players) for (const auto& planet : setup.planets)
        setup.observer->records.push_back({player.player_id, planet.id, true, 0, 0,
            player.player_id == planet.owner ? gc::owner_intelligence : 0U, {}});
}

} // namespace eawr::viewer::galactic
