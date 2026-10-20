#pragma once

#include "eawr/sim/galactic/snapshot.hpp"

namespace eawr::viewer::galactic {
[[nodiscard]] inline bool corruption_fixture(std::string_view name) {
    return name == "corruption" || name == "corruption-gaps" || name == "corruption-locked";
}
[[nodiscard]] inline std::shared_ptr<const sim::galactic::GalacticSnapshot> corruption_fixture_snapshot(
    sim::galactic::PlayerId local, std::string_view name) {
    namespace g = sim::galactic;
    namespace c = g::corruption_actions;
    const auto f = [](int value) { return c::Fixed::from_integer(value).value(); };
    c::Fixture world;
    c::Account account; account.player.id = local; account.player.credits = f(1000); account.local = true;
    account.uses[static_cast<std::size_t>(c::Type::piracy)] = 1; world.accounts.push_back(account);
    c::Actor actor; actor.id = 10; actor.owner = local; actor.appropriate_abilities = {7}; world.actors.push_back(actor);
    c::Location planet; planet.id = 101; planet.type = 2000; planet.owner = local + 10; planet.retains_influence = true;
    planet.slots[0].type = c::Type::piracy; planet.slots[0].credits = f(250);
    planet.slots[0].requirements.unit_types = {1000, 1001}; planet.slots[0].requirements.unit_count = 1;
    planet.slots[1].type = c::Type::bribery; planet.slots[1].credits = f(2000);
    planet.slots[1].briefing = 90;
    planet.slots[1].requirements.mission_type = c::Type::piracy; planet.slots[1].requirements.mission_count = 1;
    planet.slots[2].type = c::Type::black_market; planet.slots[2].credits = f(400);
    planet.slots[2].briefing = 91;
    planet.slots[2].requirements.unit_types = {1001}; planet.slots[2].requirements.unit_count = 2;
    planet.slots[2].requirements.mission_type = c::Type::piracy; planet.slots[2].requirements.mission_count = 2;
    planet.slots[2].requirements.planet_types = {2000, 2001};
    if (name == "corruption-gaps") planet.slots[1] = {};
    world.locations.push_back(planet);
    planet.id = 102; planet.type = 2001; planet.owner = local; world.locations.push_back(planet);
    world.units.push_back({100, local, 1000, 0, 101});
    g::ObservedPlanet row; row.player = local; row.planet = 101; row.discovered = true;
    const std::array<c::Id, 1> names{7};
    auto menu = g::observed_corruption_menu(world, local, row, 10, true, names);
    g::ObserverSetup observer; observer.local_player = local;
    return std::make_shared<const g::GalacticSnapshot>(0, g::GalacticClock{}, std::vector<g::GalacticPlayer>{},
        std::vector<g::Planet>{}, std::vector<g::Route>{}, std::vector<g::Fleet>{}, std::vector<g::GalacticEvent>{},
        std::nullopt, std::nullopt, nullptr, std::nullopt, std::vector<g::Seat>{}, std::nullopt, std::nullopt,
        std::nullopt, std::vector<g::PoliticalControlEffect>{}, std::vector<g::PopulationTotal>{}, observer,
        std::nullopt, std::nullopt, std::vector<g::ObservedProductionOffer>{},
        std::make_shared<const std::vector<g::ObservedPlanet>>(std::vector{row}), std::nullopt, nullptr, std::nullopt, menu);
}
} // namespace eawr::viewer::galactic
