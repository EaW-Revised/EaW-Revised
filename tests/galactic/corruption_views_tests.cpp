#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include "../support/check.hpp"
#include "eawr/sim/galactic/snapshot.hpp"

#include <iostream>
#include <exception>

namespace g = eawr::sim::galactic;
namespace c = g::corruption_actions;
namespace {
void check(bool value, const char* message) {
    if (!value) eawr::testing::require(value, message);
}
c::Fixed fixed(int value) { return c::Fixed::from_integer(value).value(); }
c::Fixture fixture() {
    c::Fixture world;
    c::Actor actor; actor.id = 10; actor.owner = 1; actor.appropriate_abilities = {8, 7};
    world.actors.push_back(actor);
    c::Account account; account.player.id = 1; account.player.credits = fixed(50); account.local = true;
    world.accounts.push_back(account);
    c::Location planet; planet.id = 20; planet.type = 50; planet.owner = 2; planet.retains_influence = true;
    planet.slots[0].type = c::Type::piracy; planet.slots[0].briefing = 90;
    planet.slots[0].credits = fixed(10);
    planet.slots[2].type = c::Type::black_market; planet.slots[2].credits = fixed(1000);
    auto& requirements = planet.slots[2].requirements;
    requirements.unit_types = {60, 61}; requirements.unit_count = 2;
    requirements.mission_type = c::Type::piracy; requirements.mission_count = 2;
    requirements.planet_types = {50, 51, 999};
    world.locations.push_back(planet);
    planet.id = 21; planet.type = 51; planet.owner = 1; world.locations.push_back(planet);
    world.units = {{100, 1, 60, 200, 20, true, true}, {101, 1, 61, 200, 20, true, true}};
    return world;
}
g::ObservedPlanet observation() {
    g::ObservedPlanet row; row.player = 1; row.planet = 20; row.discovered = true; return row;
}
std::optional<g::CorruptionMenu> menu(const c::Fixture& world, g::ObservedPlanet row = observation(),
    unsigned local = 1, bool tutorial = true) {
    const std::array<c::Id, 2> names{7, 8};
    return g::observed_corruption_menu(world, local, row, 10, tutorial, names);
}
g::GalacticSnapshot snapshot(std::optional<g::CorruptionMenu> value, g::ObservedPlanet row = observation(), unsigned local = 1) {
    g::ObserverSetup observer; observer.local_player = local;
    auto observations = std::make_shared<const std::vector<g::ObservedPlanet>>(std::vector{row});
    return g::GalacticSnapshot(0, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, observer,
        {}, {}, {}, observations, {}, {}, {}, std::move(value));
}
void contracts() {
    auto world = fixture();
    auto value = menu(world);
    check(value && value->ability == 7 && value->choices.size() == 2, "authored routing and choices");
    check(value->choices[0].slot == 1 && value->choices[1].slot == 3, "authored slot gaps retained");
    const auto& last = value->choices[1];
    check(value->choices[0].prepared.choice.briefing == 90 && value->choices[0].affordable
        && !last.affordable && last.prepared.choice.credits == fixed(1000), "briefing and unaffordable option retained");
    check(!last.units_satisfied && !last.missions_satisfied && last.planets_satisfied, "independent panes despite cost failure");
    check(last.prepared.choice.requirements.planet_types == std::vector<c::Id>{50, 51}, "absent galaxy alternatives omitted");
    world.units.push_back({102, 1, 61, 0, 0, true, true});
    check(!menu(world)->choices[1].units_satisfied, "transit consumable omitted and transport deduplicated");
    world.units.back().consumable = false;
    world.accounts[0].uses[static_cast<std::size_t>(c::Type::piracy)] = 2;
    value = menu(world);
    check(value->choices[1].units_satisfied && value->choices[1].missions_satisfied
        && !value->choices[1].affordable, "global alternatives and usage remain independent of cost");
    world.accounts[0].player.credits = fixed(-1);
    const auto negative = menu(world);
    check(!negative->choices[1].affordable && negative->choices[1].units_satisfied
        && negative->choices[1].missions_satisfied && negative->choices[1].planets_satisfied,
        "negative balance cannot mask independent requirement panes");
    world.accounts[0].player.credits = fixed(50);
    world.locations[1].owner = 2;
    check(!menu(world)->choices[1].planets_satisfied, "ANY owned alternative required");
    auto published = snapshot(value);
    check(published.corruption_menu() && published.canonical_bytes() == snapshot({}).canonical_bytes(), "outside canonical bytes");
    value->choices.clear(); world.units.clear();
    check(published.corruption_menu()->choices.size() == 2, "snapshot copied storage");
    check(!menu(world, observation(), 2), "foreign observer");
    auto row = observation(); row.discovered = false;
    check(!menu(world, row) && !snapshot(*published.corruption_menu(), row).corruption_menu(), "discovery gate");
    check(!snapshot(*published.corruption_menu(), observation(), 2).corruption_menu(), "recipient gate");
    check(!menu(world, observation(), 1, false), "tutorial refusal");
    world.actors[0].owner = 2; check(!menu(world), "foreign actor");
    world = fixture(); world.actors[0].present = false; check(!menu(world), "stale actor");
    world = fixture(); world.accounts[0].local = false; check(!menu(world), "remote account");
    world = fixture(); world.accounts[0].ai = true; check(!menu(world), "AI account");
    world = fixture(); world.locations[0].model.transition.increasing = true; check(!menu(world), "transition refusal");
    world = fixture(); world.actors[0].flattened_fleet_count = 2; check(!menu(world), "fleet refusal");
    world = fixture(); world.actors[0].appropriate_abilities.clear(); check(!menu(world), "ability refusal");
    check(world.accounts[0].player.credits == fixed(50) && world.actors[0].present, "publication never executes");
}
}
int case_galactic_corruption_views_tests() {
    try { contracts(); std::cout << "corruption view contracts passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
