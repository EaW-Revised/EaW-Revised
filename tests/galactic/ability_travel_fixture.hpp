#pragma once
#include "ability_fixture.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include <algorithm>

namespace ability_travel_fixture {
namespace gc = eawr::sim::galactic;
using ability_fixture::whole;
inline constexpr eawr::sim::EntityId transport = 60000, first = 60001, second = 60002;
inline gc::GalacticSetup setup(bool landed = false) {
    auto s = ability_fixture::base_setup();
    for (std::size_t i = 0; i < s.planets.size(); ++i) s.planets[i].position = {whole(static_cast<std::int64_t>(i) * 100), {}, {}};
    auto& host = *s.ability_host;
    auto& profile = host.profiles[0]; profile.has_market = false;
    profile.corruption = gc::CorruptionAbilityProfile{"MOD_CORRUPTION", true};
    profile.ground_activated = {gc::AbilityKind::corruption};
    host.activated_corruption_abilities = {"MOD_CORRUPTION"};
    auto& control = ability_fixture::add_control(s);
    control.types = {{1000, 0, false, false}, {2000, 0, false, false}};
    s.census->types.push_back({2000, {}, 0}); s.census->types.push_back({3000, {}, 0});
    s.fleet_control->types.push_back({2000, true, true, false});
    for (const auto& planet : s.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 3000; row.retains_residual = true;
        row.surface_accessible = true; row.ground_base = {planet.owner, 1}; control.planets.push_back(row);
        gc::CorruptionPlanet content; content.planet = planet.id; content.type = 3000; content.retains_influence = true;
        content.slots[0].type = gc::corruption_fixture::Type::piracy; content.slots[0].price = whole(100);
        host.corruption_planets.push_back(content);
    }
    for (auto& planet : s.economy_corruption->planets) planet.corruption = gc::corruption_fixture::Type::none;
    for (const auto id : ability_fixture::all_players) { gc::CorruptionPlayer player; player.player = id; host.corruption_players.push_back(player); }
    auto& fleet = s.fleets[1]; fleet.orbit = 2; fleet.members = {{transport, 2000}};
    s.census->objects.erase(s.census->objects.begin() + 1);
    auto root = ability_fixture::census_actor(transport, 2000, 2, 2, landed ? 2 : fleet.id);
    root.combat_role = gc::ActorCombatRole::transport;
    s.census->objects.push_back(root);
    s.census->objects.push_back(ability_fixture::census_actor(first, 1000, 2, 2, transport));
    s.census->objects.push_back(ability_fixture::census_actor(second, 1000, 2, 2, transport));
    auto& movement = ability_fixture::add_movement(s);
    movement.types[1].transport = true;
    for (auto& type : movement.types) type.hyperspace_speed = whole(10);
    movement.contents = {{transport, {{first, 1000}, {second, 1000}}}};
    ability_fixture::add_nested_creation(s, {1000, 2000});
    if (landed) { control.planets[1].landed = {{transport, 2000, 2}}; s.fleets.erase(s.fleets.begin() + 1); }
    return s;
}
inline gc::MoveFleet move(const gc::GalacticSetup& s, std::vector<gc::PlanetId> path = {2, 1}) {
    return {s.fleets[1].id, std::move(path), {}, false, transport};
}
} // namespace ability_travel_fixture
