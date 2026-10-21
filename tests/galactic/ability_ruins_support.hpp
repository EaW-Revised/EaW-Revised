#pragma once

#include "../support/result.hpp"

#include "ability_fixture.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/income.hpp"

#include <algorithm>
#include <utility>

// Ruins scenario for the ruins, hacking and destruction contracts, on the shared ability fixture.
namespace ability_fixture {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using eawr::testing::take;

inline Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }
constexpr eawr::sim::EntityId transport = 60000, hero = 60001, companion = 60002, ground_structure = 60003, orbital_structure = 60004;
constexpr gc::TypeId transport_type = 2000, companion_type = 3001, ground_type = 3100, orbital_type = 3101;
constexpr std::uint64_t source_ability = 9;
// The scenario builds on the shared ability fixture (ability_fixture.hpp, #2912): its base galaxy
// already carries the seatless Neutral player 3 and every validated layer.
inline gc::GalacticSetup ruins_base_setup() {
    auto s = base_setup();
    s.fiscal->players.back().earns_planet_income = false; // the Neutral player earns no planet income
    auto& host = *s.ability_host; auto& profile = host.profiles.front();
    profile.has_market = false; profile.corruption = gc::CorruptionAbilityProfile{"ACT", true};
    host.activated_corruption_abilities = {"ACT"};
    s.economy_corruption->planets[0].corruption = gc::corruption_fixture::Type::none;
    return s;
}
// Planet 0 holds: one enemy landed transport carrying a named hero (and optionally a generic
// companion), a ground and an orbital special structure, a siphon, political modifiers,
// active corruption and a nonzero ground base. Planet 1 stays untouched.
inline gc::GalacticSetup ruins_setup(bool with_companion) {
    auto s = ruins_base_setup(); const auto planet = s.planets[0].id;
    auto& host = *s.ability_host; auto& control = s.control.emplace();
    control.change_seconds = f(10000); control.max_ground_forces = 10;
    control.types = {{1000, 0, false, false}, {transport_type, 0, false, false}};
    for (const auto& a : s.players) {
        control.players.push_back({a.player_id, a.player_id == neutral_player, a.player_id != neutral_player, a.faction_id});
        for (const auto& b : s.players) control.relations.push_back({a.player_id, b.player_id, a.player_id == b.player_id});
    }
    for (const auto& route : s.routes) control.routes.push_back({route.id, {}});
    s.census->types.push_back({3000, {}, 0}); s.census->types.push_back({transport_type, {}, 0});
    s.census->types.push_back({companion_type, f(1), 0}); s.census->types.push_back({ground_type, {}, 0});
    s.census->types.push_back({orbital_type, {}, 0});
    std::sort(s.census->types.begin(), s.census->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    s.fleet_control->types.push_back({transport_type, true, true, false});
    s.fleet_control->types.push_back({companion_type, false, true, false});
    std::sort(s.fleet_control->types.begin(), s.fleet_control->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    for (const auto& row : s.planets) {
        gc::ControlPlanet surface; surface.planet = row.id; surface.type = 3000; surface.retains_residual = true;
        surface.surface_accessible = true; surface.ground_base = {row.owner, 1}; surface.starbase = {row.owner, 1};
        control.planets.push_back(surface);
        gc::CorruptionPlanet p; p.planet = row.id; p.type = 3000; p.retains_influence = true;
        p.slots[0].type = gc::corruption_fixture::Type::piracy; p.slots[0].price = f(100);
        host.corruption_planets.push_back(p);
    }
    for (const auto& row : s.players) { gc::CorruptionPlayer player; player.player = row.player_id; host.corruption_players.push_back(player); }
    auto& surface = control.planets[0];
    surface.ground_base = {1, 2}; surface.landed = {{transport, transport_type, 2}};
    surface.transition_modifiers = {{1, 77777, f(2), 0}};
    auto& row = s.production->planets[0];
    row.ground_slots = 3; row.space_slots = 3; row.maximum_ground_forces = 10; row.landed_transports = 1;
    row.objects = {{ground_structure, ground_type, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure},
        {orbital_structure, orbital_type, 1, {}, gc::ProductionDomain::space, gc::ProductionKind::structure}};
    s.census->objects.push_back({transport, transport_type, 2, planet, planet, false, false, transport_type, 0,
        gc::ActorCombatRole::transport});
    s.census->objects.push_back({hero, 1000, 2, planet, transport, false, false, 1000, 0, gc::ActorCombatRole::ordinary});
    if (with_companion) s.census->objects.push_back({companion, companion_type, 2, planet, transport, false, false,
        companion_type, 0, gc::ActorCombatRole::ordinary});
    auto& movement = s.movement.emplace(); movement.mode_modifier = f(1);
    movement.types = {{1000, f(1), f(1), 0, true, false, false, false, false},
        {transport_type, f(1), f(1), 0, true, false, false, false, true}, {companion_type, f(1), f(1), 0, true, false, false, false, false}};
    std::vector<gc::FleetMember> children{{hero, 1000}};
    if (with_companion) children.push_back({companion, companion_type});
    movement.contents = {{transport, children}};
    for (const auto& who : s.players) movement.players.push_back({who.player_id, f(1), f(1), false});
    for (const auto& p : s.planets) movement.planets.push_back({p.id, f(1), false});
    for (const auto& who : s.players) for (const auto& p : s.planets) movement.access.push_back({who.player_id, p.id, true, false});
    auto& economy = *s.economy_corruption;
    economy.planets[0].corruption = gc::corruption_fixture::Type::piracy;
    economy.planets[0].siphon = {true, 2, gc::corruption_fixture::one, 1000000};
    s.fiscal->planets[0].credits = f(100);
    host.ruined_income = {{planet, f(10)}};
    auto& observer = s.observer.emplace(); observer.human_observers = {2}; observer.solo_reference_human = 2;
    for (const auto& who : s.players) observer.players.push_back({who.player_id, false, false, 0, 0, {}});
    observer.types = {{1000, false, false, false}, {3000, false, false, false}, {companion_type, false, false, false}};
    for (const auto& p : s.planets) {
        gc::ObserverPlanet value; value.planet = p.id; value.allegiance = value.target_owner = value.current_owner = value.current_controller = p.owner;
        value.space_owner = value.ground_owner = p.owner; value.space_level = value.ground_level = 1;
        if (p.id == planet) { value.landed_transports = {{transport, 2, false, false}}; value.ground_level = 2; }
        observer.planets.push_back(value);
        for (const auto& who : s.players) observer.records.push_back({who.player_id, p.id, true, 0, 0, 0, {}});
    }
    std::sort(observer.records.begin(), observer.records.end(), [](const auto& a, const auto& b) { return std::pair{a.player, a.planet} < std::pair{b.player, b.planet}; });
    for (auto& record : observer.records) if (record.player == 2 && record.planet == planet)
        record.sources.push_back({ground_structure, source_ability, {}, 0, 1000000});
    return s;
}
// Registers one more fleet-member type in every per-type table (all remain ascending by type).
inline void register_type(gc::GalacticSetup& s, gc::TypeId type, bool stealth = false) {
    s.census->types.push_back({type, f(1), 0});
    s.fleet_control->types.push_back({type, false, true, false});
    s.control->types.push_back({type, 0, stealth, false});
    s.movement->types.push_back({type, f(1), f(1), 0, true, false, false, false, false});
    s.observer->types.push_back({type, stealth, false, false});
}
inline bool present(const gc::GalacticSession& world, eawr::sim::EntityId id) {
    const auto records = world.snapshot()->actor_records();
    return std::any_of(records.begin(), records.end(), [&](const auto& row) { return row.id == id; });
}
inline const gc::ProductionObject* object(const gc::GalacticSession& world, eawr::sim::EntityId id) {
    for (const auto& row : world.snapshot()->production()->planets[0].objects) if (row.id == id) return &row;
    return nullptr;
}
} // namespace ability_fixture
