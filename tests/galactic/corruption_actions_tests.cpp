#include "eawr/sim/galactic/corruption_actions.hpp"

#include <iostream>
#include <stdexcept>

namespace ca = eawr::sim::galactic::corruption_actions;
using ca::Fixture;
using ca::Fixed;
using ca::Id;
using ca::Type;
using ca::Refusal;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Fixed f(const char* text) { return Fixed::from_decimal(text).value(); }
Fixture fixture() {
    Fixture world;
    ca::Actor actor; actor.id = 10; actor.owner = 1; actor.appropriate_abilities = {8, 7};
    world.actors.push_back(actor);
    ca::Account a; a.player.id = 1; a.player.credits = f("100"); a.local = true;
    world.accounts.push_back(a);
    a.player.id = 2; a.local = false; world.accounts.push_back(a);
    ca::Location p; p.id = 20; p.type = 50; p.owner = 2; p.retains_influence = true;
    p.model.target_allegiance = 1; // deliberately differs from object owner
    p.slots[0].type = Type::piracy; p.slots[0].credits = f("10");
    p.slots[0].briefing = 90;
    p.slots[2].type = Type::black_market; p.slots[2].credits = f("1000");
    world.locations.push_back(p);
    return world;
}
ca::Preparation prepare(const Fixture& world) {
    const std::array<Id, 2> names{7, 8};
    return ca::prepare(world, 10, 20, true, names);
}
void admission() {
    auto world = fixture();
    auto p = prepare(world);
    check(p.refusal == Refusal::none && p.ability == 7 && p.choices.size() == 2, "authored routing and slots");
    check(p.choices[0].choice.briefing == 90 && p.choices[1].choice.credits == f("1000"), "unaffordable choice retained");
    const std::array<Id, 1> names{7};
    check(ca::prepare(world, 10, 20, false, names).refusal == Refusal::tutorial, "tutorial gate first");
    auto expect = [&](Refusal reason) { check(prepare(world).refusal == reason, "admission refusal"); world = fixture(); };
    world.actors[0].present = false; expect(Refusal::actor);
    world.actors[0].owner = 9; expect(Refusal::owner);
    world.locations[0].present = false; expect(Refusal::planet);
    world.actors[0].flattened_fleet_count = 2; expect(Refusal::fleet);
    world.actors[0].flattened_fleet_count = 1; check(prepare(world).refusal == Refusal::none, "single-member fleet");
    world.actors[0].appropriate_abilities.clear(); expect(Refusal::ability);
    world.locations[0].owner = 1; expect(Refusal::allied_owner);
    world.accounts[0].allies.insert(2); expect(Refusal::allied_owner);
    world.locations[0].neutral_owner = true; expect(Refusal::neutral_owner);
    world.locations[0].destroyed = true; expect(Refusal::destroyed);
    world.locations[0].model.transition.increasing = true; expect(Refusal::transitioning);
    world.locations[0].model.transition.current = Type::bribery; expect(Refusal::corrupted);
    world.locations[0].retains_influence = false; expect(Refusal::residual_influence);
    world.locations[0].slots = {}; expect(Refusal::no_choices);
    world.locations[0].slots[0].requirements.planet_types = {50, 999};
    check(prepare(world).choices[0].choice.requirements.planet_types == std::vector<Id>{50}, "absent galaxy types omitted");
}
void prerequisites_and_ai() {
    auto world = fixture(); auto c = prepare(world).choices[0].choice;
    c.requirements.unit_types = {60, 61}; c.requirements.unit_count = 2;
    world.units = {{100, 1, 60, 200, 20, true, true}, {101, 1, 61, 200, 20, true, true}};
    check(ca::prerequisites(world, world.accounts[0], c) == Refusal::units, "transport dedup across alternatives");
    world.units.push_back({102, 1, 61, 0, 0, true, true});
    check(ca::prerequisites(world, world.accounts[0], c) == Refusal::units, "consumable transit excluded");
    world.units.back().consumable = false;
    check(ca::prerequisites(world, world.accounts[0], c) == Refusal::none, "nonconsumable transit counts");
    c.requirements.mission_type = Type::piracy; c.requirements.mission_count = 2;
    check(ca::prerequisites(world, world.accounts[0], c) == Refusal::mission_usage, "usage prerequisite");
    world.accounts[0].uses[2] = 2;
    c.requirements.planet_types = {50, 51};
    check(ca::prerequisites(world, world.accounts[0], c) == Refusal::required_planet, "required planet ownership");
    ca::Location owned; owned.id = 21; owned.owner = 1; owned.type = 51; world.locations.push_back(owned);
    check(ca::prerequisites(world, world.accounts[0], c) == Refusal::none, "ANY planet alternative passes");
    c.credits = f("101"); check(ca::prerequisites(world, world.accounts[0], c) == Refusal::credits, "money gate");
    c.credits = {}; c.requirements = {}; c.requirements.unit_count = 99;
    check(ca::prerequisites(world, world.accounts[0], c) == Refusal::none, "empty alternatives pass");
    const std::array<Fixed, 9> expected{{{}, f("2"), f("2"), f("0.75"), f("1.5"), f("1.5"), f("0.25"), f("2.5"), f("0.75")}};
    for (unsigned i = 0; i < expected.size(); ++i) check(ca::ai_score(static_cast<Type>(i), 0) == expected[i], "all AI scores");
    check(ca::ai_score(Type::intimidation, 3) == f("0.5") && ca::ai_score(Type::piracy, 3) == f("1.25")
        && ca::ai_score(Type::militia, 3) == f("0.75"), "uses score decay");
    world = fixture(); auto p = prepare(world);
    check(ca::select_ai(world, p, {}).value()->choice.type == Type::piracy, "AI filters price");
    world.accounts[0].player.credits = {};
    check(!ca::select_ai(world, p, {}).value(), "no affordable AI choice");
    world.accounts[0].player.credits = f("2000");
    check(ca::select_ai(world, p, f("0.99")).value()->choice.type == Type::black_market, "weighted last option");
    check(!ca::select_ai(world, p, ca::one), "half-open synchronized draw");
}
void execution() {
    auto world = fixture(); auto p = prepare(world).choices[0];
    world.actors[0].owner = 2; world.accounts[1].local = true;
    // WGCH-72: stale preparation gates are not re-run.
    p.choice.requirements.planet_types = {999}; p.choice.requirements.mission_type = Type::bribery;
    p.choice.requirements.mission_count = 99;
    world.locations[0].destroyed = true;
    auto result = ca::execute(world, p, {false, false, true, true, 30, 7}).value();
    check(result.accepted() && world.accounts[0].player.credits == f("100") && world.accounts[1].player.credits == f("90"), "current owner charged");
    check(world.locations[0].model.transition.end == 157 && world.locations[0].model.corrupting_player == 2, "direct five-second request");
    check(world.accounts[1].uses[2] == 1 && !world.actors[0].present, "usage and actor removed");
    const std::vector<ca::Event> expected{{ca::Effect::debit, 10, 2}, {ca::Effect::begin_transition, 20, 2},
        {ca::Effect::increment_usage, 20, 2}, {ca::Effect::hero_arrival, 10, 2}, {ca::Effect::local_success, 10, 2},
        {ca::Effect::schedule_actor_default, 10, 2}, {ca::Effect::remove_actor, 10, 2}};
    check(result.events == expected, "ordered direct effects");
    world = fixture(); p = prepare(world).choices[0]; p.choice.requirements.unit_types = {60}; p.choice.requirements.unit_count = 1;
    result = ca::execute(world, p, {}).value();
    check(result.refusal == Refusal::units && world.accounts[0].player.credits == f("90") && world.actors[0].present, "stale units fail after debit");
    world = fixture(); world.accounts[0].player.credits = {}; result = ca::execute(world, p, {}).value();
    check(result.refusal == Refusal::credits && result.events.empty(), "no charge before affordability");
    world = fixture(); world.actors[0].present = false; check(ca::execute(world, p, {}).value().refusal == Refusal::actor, "stale actor");
    world = fixture(); world.locations[0].present = false; check(ca::execute(world, p, {}).value().refusal == Refusal::planet, "stale planet");
}
void consumption_and_missions() {
    auto world = fixture(); auto p = prepare(world).choices[0];
    p.choice.requirements.unit_types = {60}; p.choice.requirements.unit_count = 1;
    world.units = {{100, 1, 60, 200, 20, true, true, false, 4, 0},
        {101, 1, 60, 200, 20, true, true, false, 4, 0}, {102, 1, 60, 201, 20, true, true, false, 1, 0}};
    auto result = ca::execute(world, p, {}).value();
    check(result.accepted() && !world.units[0].present && !world.units[1].present && world.units[2].present, "one container consumes one count");
    world = fixture(); world.units = {{100, 1, 60, 0, 20, true, true, false, 4, 0},
        {101, 1, 60, 0, 20, true, true, false, 4, 0}};
    check(!ca::execute(world, p, {}), "unresolved equal-weight ordering is diagnostic");
    check(world.units[0].present && world.units[1].present, "unresolved tie consumes nothing");
    for (bool named_first : {false, true}) {
        world = fixture();
        const ca::Unit generic{101, 1, 60, 0, 20, true, true, false, 4, 0};
        const ca::Unit named{100, 1, 60, 0, 20, true, true, true, 4, 0};
        world.units = named_first ? std::vector<ca::Unit>{named, generic} : std::vector<ca::Unit>{generic, named};
        const auto tied = ca::execute(world, p, {});
        check(!tied && tied.error().message == "U-03 unresolved prerequisite tie order",
            "named runner-up tie is diagnostic in both list orders");
        check(world.units[0].present && world.units[1].present && world.actors[0].present,
            "named runner-up tie consumes neither prerequisite nor actor");
    }
    world = fixture(); world.units = {{100, 1, 60, 0, 20, true, true, false, 4, 1},
        {101, 1, 60, 0, 20, true, true, false, 4, 0}};
    check(ca::execute(world, p, {}).value().accepted() && world.units[0].present && !world.units[1].present,
        "explicit host tie ordering, not stable identity");
    world = fixture(); world.units = {{100, 1, 60, 0, 20, true, true, true}};
    result = ca::execute(world, p, {}).value();
    check(result.events[1].effect == ca::Effect::schedule_prerequisite_default && result.events[2].effect == ca::Effect::destroy_prerequisite, "named schedules before destroy");
    world = fixture(); world.units = {{100, 1, 60, 200, 20, true, true, true},
        {101, 1, 61, 200, 20, true, true, false}};
    result = ca::execute(world, p, {}).value();
    check(result.accepted() && !world.units[0].present && world.units[1].present
        && result.events[2].object == 100, "named prerequisite preserves transport siblings");
    world = fixture(); world.units = {{100, 1, 60, 0, 0, true, false}};
    check(ca::execute(world, p, {}).value().accepted() && world.units[0].present, "nonconsumable retained");
    world = fixture(); p = prepare(world).choices[0]; p.choice.map = 80; p.choice.script = 81; p.choice.mode = ca::MissionMode::land;
    result = ca::execute(world, p, {}).value();
    check(result.mission && !world.locations[0].model.transition.active() && !world.actors[0].present, "mission queued before transition");
    check(world.accounts[0].uses[2] == 0, "queued mission does not increment direct usage");
    auto mission = *result.mission;
    check(ca::finish_mission(world, mission, ca::MissionState::succeeded, 30, 100).value().accepted(), "mission success");
    check(world.locations[0].model.transition.end == 250, "mission return starts five seconds at current frame");
    check(world.accounts[0].uses[2] == 1, "mission success increments usage");
    check(!ca::finish_mission(world, mission, ca::MissionState::succeeded, 30, 101).value().accepted(), "mission only once");
    std::vector<ca::Player> players{world.accounts[0].player, world.accounts[1].player};
    check(eawr::sim::galactic::corruption_fixture::complete(world.locations[0].model, players, 249, {}).value().empty(), "before mission deadline");
    check(!eawr::sim::galactic::corruption_fixture::complete(world.locations[0].model, players, 250, {}).value().empty(), "public model completion");
    for (auto outcome : {ca::MissionState::failed, ca::MissionState::cancelled}) {
        world = fixture(); result = ca::execute(world, p, {}).value(); mission = *result.mission;
        auto end = ca::finish_mission(world, mission, outcome, 30, 50).value();
        check(end.accepted() && !world.locations[0].model.transition.active() && world.accounts[0].player.credits == f("90"), "failed/cancelled no refund or transition");
        check(end.events.size() == (outcome == ca::MissionState::failed ? 1U : 0U), "failure story differs from cancellation seam");
    }
    world = fixture(); result = ca::execute(world, p, {false, false, false}).value();
    check(result.refusal == Refusal::mission_queue && world.accounts[0].player.credits == f("90") && world.actors[0].present, "queue failure after charge leaves actor");
    world = fixture(); p.choice.requirements.unit_types = {60}; p.choice.requirements.unit_count = 1;
    world.units = {{100, 1, 60, 0, 20, true, true, false, 4, 0}};
    result = ca::execute(world, p, {false, false, false}).value();
    check(result.refusal == Refusal::mission_queue && !world.units[0].present, "queue failure after consumption is not rolled back");
    p.choice.requirements = {};
    for (unsigned bypass = 0; bypass < 3; ++bypass) {
        world = fixture(); world.accounts[0].ai = bypass == 0;
        result = ca::execute(world, p, {bypass == 1, bypass == 2}).value();
        check(result.accepted() && !result.mission && world.locations[0].model.transition.active(), "AI multiplayer debug direct bypass");
    }
}
} // namespace
int main() {
    try { admission(); prerequisites_and_ai(); execution(); consumption_and_missions(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "corruption action fixture contracts passed\n";
}
