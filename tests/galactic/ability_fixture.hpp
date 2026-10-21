#pragma once
// Shared synthetic galaxy for the galactic ability contract tests (host, theft, corruption).
// Every layer the session validates is filled: a neutral third player with a full relation
// table, the galaxy's own routes, and the fresh human queue cap.
#include "eawr/sim/galactic/synthetic.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace ability_fixture {
namespace gc = eawr::sim::galactic;
using eawr::sim::math::Fixed;

// A hung contract fails fast instead of waiting out the ctest timeout.
inline void arm_watchdog(std::chrono::seconds limit = std::chrono::seconds(240)) {
    std::thread([limit] { std::this_thread::sleep_for(limit); std::cerr << "watchdog: contract exceeded its time limit" << std::endl; std::_Exit(3); }).detach();
}

inline Fixed whole(std::int64_t n) { return Fixed::from_raw(n * Fixed::scale); }

// Player 3 is the neutral player political control requires; it owns no planet or fleet.
inline constexpr gc::PlayerId neutral_player = 3;
inline constexpr gc::PlayerId all_players[] = {1, 2, neutral_player};

// A live ordinary actor row with the combat and garrison facts political control reads.
inline gc::CensusObject census_actor(eawr::sim::EntityId id, gc::TypeId type, gc::PlayerId owner, gc::PlanetId planet, eawr::sim::EntityId container) {
    return {id, type, owner, planet, container, false, false, type, 0, gc::ActorCombatRole::ordinary, {true, false, false, {}}, false};
}

inline gc::GalacticSetup base_setup(bool consumes = true) {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 1;
    auto s = gc::synthetic_galaxy(options);
    s.players.push_back({neutral_player, neutral_player, 0});
    s.rules.maximum_fleet_movement_distance = whole(10000); s.rules.fiscal_cycle_frames = 3000;
    s.fleets[1].orbit = s.planets[0].id;
    s.fleet_control = gc::FleetControlSetup{{}, {{1000, false, true, false}}, {}};
    s.player_economy = gc::PlayerEconomySetup{{{1, whole(1000), whole(10000), 2, 5}, {2, whole(1000), whole(10000), 2, 5},
        {neutral_player, whole(1000), whole(10000), 2, 5}}};
    s.fiscal = gc::FiscalSetup{whole(10000), {{1, true, whole(1)}, {2, true, whole(1)}, {neutral_player, true, whole(1)}}, {}};
    s.production.emplace();
    s.production->maximum_human_queue = 5; // fresh queue cap, WGEP-27
    for (const gc::PlayerId id : all_players) {
        gc::ProductionPlayer player; player.player = id; player.human = id != neutral_player; player.population_capacity = whole(50);
        player.uses_population_cap = player.human; player.pay_as_you_go = !player.human; // WGEP-28/50
        s.production->players.push_back(player);
    }
    for (const auto& planet : s.planets) {
        s.fiscal->planets.push_back({planet.id, {}});
        gc::PlanetProduction row; row.planet = planet.id; row.target_owner = planet.owner;
        row.maximum_space_level = 5; s.production->planets.push_back(row);
    }
    s.census = gc::PopulationCensus{{{1000, whole(1), 0}}, {{1, 50, false, {}}, {2, 50, false, {}}, {neutral_player, 50, false, {}}}, {}};
    for (const auto& fleet : s.fleets) for (const auto& member : fleet.members)
        s.census->objects.push_back(census_actor(member.id, member.type, fleet.owner, fleet.orbit, fleet.id));
    auto& economy = s.economy_corruption.emplace(); economy.factor_min = economy.factor_max = whole(1);
    for (const gc::PlayerId id : all_players) {
        gc::CorruptionEconomyPlayer player; player.player = id; player.faction = id * 10;
        player.human = player.playable = id != neutral_player; economy.players.push_back(player);
    }
    for (const auto& planet : s.planets) { gc::CorruptionEconomyPlanet row; row.planet = planet.id; economy.planets.push_back(row); }
    economy.planets[0].corruption = gc::corruption_fixture::Type::black_market;
    economy.items = {{91, 10, 2, 2, whole(100), whole(100), whole(1)}};
    auto& host = s.ability_host.emplace(); host.default_hero_absence = whole(360);
    gc::GalacticAbilityProfile profile; profile.type = 1000; profile.named = true;
    profile.market.causes_despawn = consumes;
    profile.market.absence_min = profile.market.absence_max = {whole(1), whole(2), whole(3), whole(4), whole(5)};
    host.profiles.push_back(profile);
    return s;
}

// Political control inputs that match the galaxy: one row per player (the neutral one flagged),
// the full relation table and one control route per galaxy route.
inline gc::PoliticalControlSetup& add_control(gc::GalacticSetup& s) {
    auto& control = s.control.emplace(); control.change_seconds = whole(10000); control.max_ground_forces = 10;
    control.players = {{1, false, true, 1}, {2, false, true, 2}, {neutral_player, true, false, neutral_player}};
    for (const gc::PlayerId a : all_players) for (const gc::PlayerId b : all_players) control.relations.push_back({a, b, a == b});
    for (const auto& route : s.routes) control.routes.push_back({route.id, whole(0)});
    return control;
}

// Live movement inputs for every original type the fleet traits know, one player/planet row each and open access.
inline gc::MovementSetup& add_movement(gc::GalacticSetup& s) {
    auto& movement = s.movement.emplace(); movement.mode_modifier = whole(1);
    for (const auto& trait : s.fleet_control->types) movement.types.push_back({trait.type, whole(1), whole(1), 0, true, false, false, false, false});
    for (const gc::PlayerId id : all_players) movement.players.push_back({id, whole(1), whole(1), false});
    for (const auto& planet : s.planets) movement.planets.push_back({planet.id, whole(1), false});
    for (const gc::PlayerId id : all_players) for (const auto& planet : s.planets) movement.access.push_back({id, planet.id, true, false});
    return movement;
}

// Constructor dispatch facts for the given original types: ordinary combat actors with live ability state.
inline void add_nested_creation(gc::GalacticSetup& s, std::initializer_list<gc::TypeId> types) {
    auto& nested = s.production->nested_creation.emplace(); nested.fleet_type = *types.begin();
    for (const gc::TypeId type : types) {
        gc::NestedCreationType row; row.type = type; row.combat_role = gc::ActorCombatRole::ordinary; row.abilities.present = true;
        nested.types.push_back(row);
    }
    for (const gc::PlayerId a : all_players) for (const gc::PlayerId b : all_players)
        nested.relations.push_back({a, b, a != b && a != neutral_player && b != neutral_player});
}

// Observer inputs for every player with all planets discovered; `types` are the original types the actors use.
inline gc::ObserverSetup& add_observer(gc::GalacticSetup& s, gc::PlayerId local, std::initializer_list<gc::TypeId> types) {
    auto& observer = s.observer.emplace(); observer.human_observers = {local}; observer.solo_reference_human = local;
    for (const gc::PlayerId id : all_players) observer.players.push_back({id, false, false, 0, 0, {}});
    for (const gc::TypeId type : types) observer.types.push_back({type, false, false, false});
    for (const auto& planet : s.planets) {
        gc::ObserverPlanet row; row.planet = planet.id;
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
        row.space_owner = row.ground_owner = planet.owner; row.space_level = row.ground_level = 1;
        observer.planets.push_back(row);
    }
    for (const gc::PlayerId id : all_players) for (const auto& planet : s.planets) observer.records.push_back({id, planet.id, true, 0, 0, 0, {}});
    return observer;
}

} // namespace ability_fixture
