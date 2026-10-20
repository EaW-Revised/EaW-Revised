#include <memory>
#include <optional>
#include <vector>
#include "../support/check.hpp"
#include "eawr/sim/galactic/snapshot.hpp"

#include <iostream>
#include <exception>

namespace g = eawr::sim::galactic;
namespace h = g::hero_actions;
namespace {
void check(bool value, const char* message) {
    if (!value) eawr::testing::require(value, message);
}
h::Fixed fixed(int value) { return h::Fixed::from_integer(value).value(); }
h::Fixture fixture() {
    h::Fixture world;
    h::Account account; account.base.player.id = 1; account.base.local = true;
    account.base.player.credits = fixed(50); account.tech = 2; world.accounts.push_back(account);
    h::Object actor; actor.id = 10; actor.owner = 1; actor.hero = true; world.objects.push_back(actor);
    h::Object target; target.id = 20; target.owner = 2; target.hero = true;
    target.current_type = 100; target.original_type = 101; world.objects.push_back(target);
    h::Planet planet; planet.id = 30; planet.orbit = {20}; planet.landed = {10};
    planet.siphon.fiscal.active = true; planet.siphon.company_type = 200; planet.siphon.source_type = 201;
    world.planets.push_back(planet);
    return world;
}
g::GalacticSnapshot snapshot(const std::optional<g::HeroActionMenus>& menus, g::ObservedPlanet row, unsigned local = 1) {
    g::ObserverSetup observer; observer.local_player = local;
    auto observations = std::make_shared<const std::vector<g::ObservedPlanet>>(std::vector{row});
    return g::GalacticSnapshot(0, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, observer,
        {}, {}, {}, observations, {}, {}, menus);
}
void contracts() {
    auto world = fixture();
    g::ObservedPlanet row; row.player = 1; row.planet = 30; row.discovered = true;
    row.knowledge.fleet_contents = true;
    h::TheftPreparation theft; theft.choices = {{10, 30, 501, fixed(20), fixed(1), fixed(60), false},
        {10, 30, 502, fixed(100), {}, fixed(70), false}};
    h::NeutralizationPreparation neutral; neutral.choices = {
        {10, 30, 20, 101, false, false, fixed(20), fixed(90), fixed(180)},
        {10, 30, 0, 200, true, true, fixed(100), fixed(90), fixed(180)}};
    auto menus = g::observed_hero_action_menus(world, 1, row, 10, &theft, &neutral);
    check(menus && menus->theft->size() == 2 && menus->neutralization->size() == 2, "admitted options");
    check(menus->tech == 2 && menus->theft->at(0).affordable && !menus->theft->at(1).affordable, "account affordability");
    check(!menus->neutralization->at(0).prepared.valid && menus->neutralization->at(0).affordable, "disabled separate from cost");
    check(menus->theft->at(0).prepared.chance == fixed(1) && menus->theft->at(0).prepared.absence == fixed(60)
        && menus->neutralization->at(0).prepared.actor_absence == fixed(90)
        && menus->neutralization->at(0).prepared.target_absence == fixed(180), "prepared draws copied exactly");
    check(menus->neutralization->at(1).source_type == 201 && menus->neutralization->at(1).prepared.siphon, "siphon without source actor");
    auto published = snapshot(menus, row);
    const auto baseline = snapshot({}, row);
    check(published.hero_action_menus() && published.canonical_bytes() == baseline.canonical_bytes(), "presentation excluded from canonical bytes");
    theft.choices.clear(); menus->theft->clear(); world.objects.clear();
    check(published.hero_action_menus()->theft->size() == 2, "snapshot owns immutable copied storage");
    world = fixture();
    check(!g::observed_hero_action_menus(world, 2, row, 10, nullptr, &neutral), "foreign observer");
    row.discovered = false;
    check(!g::observed_hero_action_menus(world, 1, row, 10), "undiscovered planet");
    check(!snapshot(published.hero_action_menus() ? std::optional(*published.hero_action_menus()) : std::nullopt, row).hero_action_menus(), "snapshot discovery gate");
    row.discovered = true;
    world.objects[0].owner = 2;
    check(!g::observed_hero_action_menus(world, 1, row, 10), "foreign actor");
    world = fixture(); world.accounts[0].base.local = false;
    check(!g::observed_hero_action_menus(world, 1, row, 10), "remote account");
    world = fixture(); world.objects[0].deletion_pending = true;
    check(!g::observed_hero_action_menus(world, 1, row, 10), "stale actor");
    world = fixture(); world.objects[1].visible = false;
    menus = g::observed_hero_action_menus(world, 1, row, 10, nullptr, &neutral);
    check(menus->neutralization->size() == 1, "invisible target omitted");
    world = fixture(); world.objects[1].hidden_details = true;
    menus = g::observed_hero_action_menus(world, 1, row, 10, nullptr, &neutral);
    check(menus->neutralization->size() == 1 && menus->neutralization->at(0).prepared_index == 1, "hidden target omitted without index shift");
    world = fixture(); row.knowledge.fleet_contents = false;
    menus = g::observed_hero_action_menus(world, 1, row, 10, nullptr, &neutral);
    check(menus->neutralization->size() == 1, "count-only knowledge cannot expose ordinary target");
    world.objects[1].named = true; row.knowledge.major_stealth = true;
    menus = g::observed_hero_action_menus(world, 1, row, 10, nullptr, &neutral);
    check(menus->neutralization->size() == 2, "named hero independent detection");
    world.planets[0].siphon.fiscal.active = false;
    menus = g::observed_hero_action_menus(world, 1, row, 10, nullptr, &neutral);
    check(menus->neutralization->size() == 1, "vanished siphon omitted");
    theft.choices = {{10, 30, 501, fixed(20), fixed(1), fixed(60), true}, {99, 30, 502, fixed(20), {}, {}, false}};
    menus = g::observed_hero_action_menus(world, 1, row, 10, &theft);
    check(menus->theft->size() == 1 && menus->theft->at(0).prepared.advance, "preserve provider whole-list flag");
    theft.choices[0].advance = false;
    menus = g::observed_hero_action_menus(world, 1, row, 10, &theft);
    check(!menus->theft->at(0).prepared.advance, "filtering must not infer one-item advance");
    check(!snapshot(menus, row, 2).hero_action_menus(), "snapshot local recipient gate");
    world = fixture(); row.knowledge.fleet_contents = true;
    row.knowledge.fleet_count = true;
    world.objects[1].fleet = true;
    g::HackMenuPreparation hack{{10, 30, 100, fixed(20)}, 20};
    g::DestructionPreparation destruction{10, 30, h::Refusal::contested, 10};
    menus = g::observed_hero_action_menus(world, 1, row, 10, nullptr, nullptr, &hack, &destruction);
    check(menus->hack && menus->hack->weapon == 20 && menus->hack->weapon_type == 100
        && menus->hack->prepared.prepared_price == fixed(20) && menus->hack->affordable, "prepared hack display facts");
    check(menus->destruction && menus->destruction->refusal == h::Refusal::contested
        && menus->destruction->source == 10, "known blocker refusal published without suppressing preview");
    row.knowledge.fleet_count = false;
    check(g::observed_hero_action_menus(world,1,row,10,nullptr,nullptr,nullptr,&destruction)->destruction->refusal
        == h::Refusal::none, "unscouted blocker cannot leak through refusal");
    row.knowledge.fleet_count = true; row.knowledge.fleet_contents = false;
    check(g::observed_hero_action_menus(world,1,row,10,nullptr,nullptr,nullptr,&destruction)->destruction->refusal
        == h::Refusal::contested, "count-only fleet card knowledge admits refusal");
    row.knowledge.fleet_contents = true;
    for (bool h::Object::*flag : {&h::Object::stealth, &h::Object::hidden_details, &h::Object::deletion_pending}) {
        world.objects[1].*flag = true;
        check(g::observed_hero_action_menus(world,1,row,10,nullptr,nullptr,nullptr,&destruction)->destruction->refusal
            == h::Refusal::none, "hidden or stale fleet cannot disclose refusal");
        world.objects[1].*flag = false;
    }
    world.objects[1].visible = false;
    check(g::observed_hero_action_menus(world,1,row,10,nullptr,nullptr,nullptr,&destruction)->destruction->refusal
        == h::Refusal::none, "invisible blocker cannot disclose refusal");
    world.objects[1].visible = true;
    world.accounts[0].base.allies.insert(2);
    check(g::observed_hero_action_menus(world,1,row,10,nullptr,nullptr,nullptr,&destruction)->destruction->refusal
        == h::Refusal::none, "allied fleet cannot justify contested refusal");
    world.accounts[0].base.allies.clear();
    destruction.source = 20;
    check(!g::observed_hero_action_menus(world,1,row,10,nullptr,nullptr,nullptr,&destruction)->destruction,
        "foreign original source cannot be published");
    h::Object container; container.id=11; container.owner=1; container.fleet=true;
    container.contents={10}; world.objects.push_back(container);
    destruction.source=11;
    auto nested=g::observed_hero_action_menus(world,1,row,10,nullptr,nullptr,nullptr,&destruction);
    check(nested->destruction && nested->destruction->actor==10 && nested->destruction->source==11,
        "original container remains distinct from selected inner actor");
    auto copied=snapshot(nested,row);
    world.objects.back().deletion_pending=true;
    check(!g::observed_hero_action_menus(world,1,row,10,nullptr,nullptr,nullptr,&destruction)->destruction,
        "stale original container omitted");
    check(copied.hero_action_menus()->destruction->source==11, "copied container identity stays immutable");
    destruction.source = 10;
    published = snapshot(menus, row);
    check(published.canonical_bytes() == baseline.canonical_bytes(), "weapon menus excluded from canonical state");
    hack.prepared.prepared_price = fixed(100);
    menus = g::observed_hero_action_menus(world, 1, row, 10, nullptr, nullptr, &hack);
    check(menus->hack && !menus->hack->affordable && published.hero_action_menus()->hack->affordable, "immutable prepared cost and current affordability");
    world.objects[1].hidden_details = true;
    check(!g::observed_hero_action_menus(world, 1, row, 10, nullptr, nullptr, &hack)->hack, "hidden weapon omitted");
    world.objects[1].hidden_details = false; row.knowledge.fleet_contents = false;
    check(!g::observed_hero_action_menus(world, 1, row, 10, nullptr, nullptr, &hack)->hack, "unknown weapon omitted");
    row.knowledge.superweapon = true;
    check(g::observed_hero_action_menus(world, 1, row, 10, nullptr, nullptr, &hack)->hack.has_value(), "observed superweapon exception");
    hack.prepared.required_type = 101;
    check(!g::observed_hero_action_menus(world, 1, row, 10, nullptr, nullptr, &hack)->hack, "original type not substituted");
    hack.prepared.required_type = 100; world.planets[0].destroyed = true;
    menus = g::observed_hero_action_menus(world, 1, row, 10, &theft, &neutral, &hack, &destruction);
    check(menus->hack && !menus->destruction && !menus->theft && !menus->neutralization, "hack admission does not reject ruins or reopen other menus");
    world.planets[0].destroyed = false; destruction.actor = 99;
    check(!g::observed_hero_action_menus(world, 1, row, 10, nullptr, nullptr, nullptr, &destruction)->destruction, "foreign destruction actor omitted");
}
}
int case_galactic_hero_action_views_tests() {
    try { contracts(); std::cout << "hero action view contracts passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
