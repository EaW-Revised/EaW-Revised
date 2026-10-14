#pragma once
#include "eawr/sim/galactic/snapshot.hpp"
namespace eawr::viewer::galactic {
[[nodiscard]] inline std::shared_ptr<const sim::galactic::GalacticSnapshot> destruction_fixture(
    sim::galactic::PlayerId local,std::string_view name) {
    namespace g=sim::galactic; namespace h=g::hero_actions;
    h::Fixture world;
    h::Account account; account.base.player.id=local; account.base.local=true; world.accounts.push_back(account);
    h::Object actor; actor.id=7001; actor.owner=local; actor.parent=601; world.objects.push_back(actor);
    h::Object source; source.id=601; source.owner=local; source.fleet=true; source.contents={7001}; world.objects.push_back(source);
    h::Planet planet; planet.id=101; planet.orbit={601};
    if (name == "destruction-contested") {
        h::Object blocker; blocker.id=602; blocker.owner=local+1; blocker.fleet=true;
        world.objects.push_back(blocker); planet.orbit.push_back(blocker.id);
    }
    world.planets.push_back(planet);
    g::ObservedPlanet row; row.player=local; row.planet=101; row.discovered=true;
    row.knowledge.fleet_count=name == "destruction-contested";
    g::DestructionPreparation prepared{7001,101,name == "destruction-contested" ? h::Refusal::contested : h::Refusal::none,601};
    auto menus=g::observed_hero_action_menus(world,local,row,7001,nullptr,nullptr,nullptr,
        name == "destruction-absent" ? nullptr : &prepared);
    g::ObserverSetup observer; observer.local_player=local;
    return std::make_shared<const g::GalacticSnapshot>(0,g::GalacticClock{},std::vector<g::GalacticPlayer>{},
        std::vector<g::Planet>{},std::vector<g::Route>{},std::vector<g::Fleet>{},std::vector<g::GalacticEvent>{},
        std::nullopt,std::nullopt,nullptr,std::nullopt,std::vector<g::Seat>{},std::nullopt,std::nullopt,
        std::nullopt,std::vector<g::PoliticalControlEffect>{},std::vector<g::PopulationTotal>{},observer,
        std::nullopt,std::nullopt,std::vector<g::ObservedProductionOffer>{},
        std::make_shared<const std::vector<g::ObservedPlanet>>(std::vector{row}),std::nullopt,nullptr,menus);
}
}
