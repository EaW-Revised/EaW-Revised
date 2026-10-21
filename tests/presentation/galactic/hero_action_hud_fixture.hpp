#pragma once

#include "eawr/sim/galactic/snapshot.hpp"

namespace eawr::viewer::galactic {
[[nodiscard]] inline bool hero_action_fixture(std::string_view name) {
    return name == "theft" || name == "theft-single" || name == "neutralization";
}
// Explicit copied snapshot fixture, never campaign state or action execution.
[[nodiscard]] inline std::shared_ptr<const sim::galactic::GalacticSnapshot> hero_action_fixture_snapshot(
    sim::galactic::PlayerId local, std::string_view name) {
    namespace g = sim::galactic;
    namespace h = g::hero_actions;
    const auto f = [](int value) { return h::Fixed::from_integer(value).value(); };
    h::Fixture world;
    h::Account account; account.base.player.id = local; account.base.local = true;
    account.base.player.credits = f(1000); account.tech = 2; world.accounts.push_back(account);
    h::Object actor; actor.id = 10; actor.owner = local; actor.hero = true; actor.named = true;
    world.objects.push_back(actor);
    h::Object target; target.id = 20; target.owner = local + 10; target.hero = true;
    target.named = true; target.current_type = 1002; target.original_type = 1002;
    world.objects.push_back(target); target.id = 21; target.current_type = 1003; target.named = false;
    world.objects.push_back(target);
    h::Planet planet; planet.id = 101; planet.orbit = {20,21}; planet.landed = {10};
    planet.siphon.fiscal.active = true; planet.siphon.company_type = 1004; planet.siphon.source_type = 1004;
    world.planets.push_back(planet);
    g::ObservedPlanet row; row.player = local; row.planet = 101; row.discovered = true;
    row.knowledge.fleet_contents = true;
    h::TheftPreparation theft;
    theft.choices = {{10,101,1000,f(250),h::Fixed::from_ratio(3,4).value(),f(60),name == "theft-single"}};
    if (name != "theft-single") theft.choices.push_back({10,101,1001,f(2000),f(1),f(70),false});
    h::NeutralizationPreparation neutral;
    neutral.choices = {{10,101,20,1002,false,true,f(200),f(120),f(240)},
        {10,101,21,1003,false,false,f(400),f(120),f(300)},
        {10,101,0,1004,true,true,f(2000),f(150),f(360)}};
    auto menus = g::observed_hero_action_menus(world, local, row, 10,
        name == "neutralization" ? nullptr : &theft, name == "neutralization" ? &neutral : nullptr);
    g::ObserverSetup observer; observer.human_observers = {local}; observer.solo_reference_human = local;
    return std::make_shared<const g::GalacticSnapshot>(0, g::GalacticClock{}, std::vector<g::GalacticPlayer>{},
        std::vector<g::Planet>{}, std::vector<g::Route>{}, std::vector<g::Fleet>{}, std::vector<g::GalacticEvent>{},
        std::nullopt, std::nullopt, nullptr, std::nullopt, std::vector<g::Seat>{}, std::nullopt, std::nullopt,
        std::nullopt, std::vector<g::PoliticalControlEffect>{}, std::vector<g::PopulationTotal>{}, observer,
        std::nullopt, std::nullopt, std::vector<g::ObservedProductionOffer>{},
        std::make_shared<const std::vector<g::ObservedPlanet>>(std::vector{row}), std::nullopt, nullptr, menus);
}
} // namespace eawr::viewer::galactic
