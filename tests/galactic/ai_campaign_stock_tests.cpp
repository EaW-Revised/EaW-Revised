// R5 runtime preparation contracts. --release is an honest provider/progression
// gate; --installed proves stock preparation and explicit unavailable refusal.
#include "galactic_support.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/campaign/galactic_ai_runtime.hpp"
#include "ai_campaign_gate.hpp"
#include "ai_learning_gate.hpp"
#include "galactic_ai_battle_listener.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/setup.hpp"
#include "../../src/platform/galactic/save_repository.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace {
using namespace galactic_test;
namespace cp = eawr::campaign;
namespace ck = eawr::checkpoint;
namespace auth = eawr::script::authoritative;
namespace ga = eawr::script::foc::galactic_ai;
template<class T> T must(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + " " + result.error().message);
    return std::move(result).value();
}
void must(eawr::core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void worker_verdict_contracts() {
    namespace gate = galactic_campaign_gate;
    const std::array<std::size_t, 4> standard{1, 2, 4, 8};
    const std::array<std::size_t, 5> with_hardware{1, 2, 4, 8, 20};
    std::set<std::uint32_t> workers{1, 2, 4, 8};
    expect(gate::expected_workers_present(workers, standard), "standard four-worker evidence passes its captured set");
    expect(!gate::expected_workers_present(workers, with_hardware), "missing hardware-worker evidence refuses");
    workers.insert(20);
    expect(gate::expected_workers_present(workers, with_hardware), "fifth hardware-worker evidence passes its captured set");
    expect(!gate::expected_workers_present(workers, standard), "unexpected worker evidence refuses");
    workers.erase(1); workers.insert(16);
    expect(!gate::expected_workers_present(workers, with_hardware), "matching count cannot replace a missing required worker");
    std::map<std::uint32_t, std::uint64_t> continuations;
    for (auto worker : with_hardware) continuations.emplace(static_cast<std::uint32_t>(worker), gate::minimum_branch_frames);
    expect(gate::continuation_workers_valid(continuations, with_hardware), "all five compared branches pass their frame minimum");
    --continuations.at(20);
    expect(!gate::continuation_workers_valid(continuations, with_hardware), "short hardware-worker branch still refuses");
    continuations.at(20) = gate::minimum_branch_frames; continuations.erase(2);
    expect(!gate::continuation_workers_valid(continuations, with_hardware), "missing standard branch still refuses");
    const auto expected = eawr::platform::determinism_worker_counts(); workers.clear();
    for (auto worker : expected) workers.insert(static_cast<std::uint32_t>(worker));
    expect(gate::expected_workers_present(workers, expected), "actual host capture and verdict use the same worker set");
}
ck::GameLoadOptions::GalacticSchedulers schedulers() {
    auth::SessionConfig config; config.galactic_clock = true; config.tick_duration = {1, 30};
    return {must(auth::ScriptScheduler::create(config, {})), must(auth::ScriptScheduler::create(config, {}))};
}
class ExternalFixture final : public ga::TrackedStateContributor {
public:
    explicit ExternalFixture(std::string key = "TEST_EXTERNAL") : key_(std::move(key)) {}
    std::uint8_t value{1}; bool fail_once{};
    void set(std::uint8_t next) { value = next; mark_state_changed(); }
    void refuse_capture() { refuse_capture_ = true; mark_state_changed(); }
    eawr::core::Result<ga::NativeState> capture() const override {
        if (refuse_capture_) {
            refuse_capture_ = false;
            eawr::core::Diagnostic error; error.code = "TEST-EXTERNAL"; error.message = "external capture refuses once";
            return eawr::core::Result<ga::NativeState>::failure(std::move(error));
        }
        ga::NativeState state; state.components.emplace(key_, std::vector<std::uint8_t>{value});
        return eawr::core::Result<ga::NativeState>::success(std::move(state));
    }
    eawr::core::Result<void> restore(const ga::NativeState& state) override {
        const auto at = state.components.find(key_);
        if (at != state.components.end() && at->second.size() == 1) {
            value = at->second.front(); mark_state_changed();
            if (!fail_once) return eawr::core::Result<void>::success();
        }
        fail_once = false;
        eawr::core::Diagnostic error; error.code = "TEST-EXTERNAL"; error.message = "external fixture refuses after partial write";
        return eawr::core::Result<void>::failure(std::move(error));
    }
    eawr::core::Result<std::string> state_hash() const override { return ga::native_state_hash(must(capture())); }
private:
    std::string key_; mutable bool refuse_capture_{};
};
void qualified_safety_contracts() {
    std::vector<std::optional<bool>> baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        ga::QualifiedFreestore retained;
        ga::FreestoreFacts facts; facts.object.id = 20; facts.object.type = facts.current_type = 1000;
        facts.object.owner = 1; facts.object.planet = 1; facts.object.transport = true;
        facts.parent = 11; facts.qualified = true; facts = ga::ordinary_freestore_birth(facts);
        must(retained.update(facts)); const std::array offers{facts.object.id}; must(retained.initialize(1, offers));
        expect(must(retained.reserve(1, facts.object.id, 7)), "qualified safety retains an actual reservation");
        const auto raw = retained.encode(); const auto source = must(retained.publish(executor));
        const std::array traits{ga::FreestoreType{1000, ga::FreestoreKind::unit, true}};
        std::vector<std::optional<bool>> results;
        for (unsigned scenario = 0; scenario != 9; ++scenario) {
            auto setup = galactic::synthetic_galaxy({42, 2, 2, 1, 1, true, false});
            setup.planets.front().owner = scenario == 1 || scenario == 8 ? 2 : 1;
            galactic::PoliticalControlSetup control; control.max_ground_forces = 3;
            control.relations.push_back({1, 2, scenario == 8});
            galactic::ControlPlanet surface; surface.planet = 1; surface.surface_accessible = scenario != 2;
            if (scenario == 3) for (eawr::sim::EntityId id = 30; id != 33; ++id) {
                galactic::SurfaceForce unit; unit.id = id; unit.type = 1000; unit.owner = 1; surface.landed.push_back(unit);
            }
            if (scenario == 4 || scenario == 8) {
                galactic::SurfaceForce unit; unit.id = 30; unit.type = 1000; unit.owner = 2; surface.landed.push_back(unit);
            }
            control.planets.push_back(surface);
            const galactic::GalacticSnapshot snapshot(1, {}, setup.players, setup.planets, {}, {}, {},
                std::nullopt, std::nullopt, {}, std::nullopt, {}, std::nullopt, std::nullopt,
                scenario == 5 || scenario == 6 ? std::nullopt : std::optional{control});
            auto published = std::make_shared<ga::FreestorePool>(*source);
            published->objects.front().safe = true; // Missing input must clear even a stale derived answer.
            if (scenario == 6) published->objects.front().transit = true;
            if (scenario == 7) published->objects.front().directly_parented_to_planet = true;
            must(ga::prepare_freestore_safety(*published, snapshot, traits, executor));
            std::optional<bool> expected{};
            if (scenario != 5) expected.emplace(scenario == 0 || scenario >= 6);
            results.push_back(published->objects.front().safe);
            expect(results.back() == expected, "WGA-30 qualified safety uses actual alliance, surface, transport owners and authored capacity");
            expect(retained.encode() == raw && !source->objects.front().safe && retained.contains(1, 20)
                && retained.reservations(1)->at(20) == 7 && published->reservations == source->reservations,
                "derived safety publication changes no raw facts, membership, reservations or archive bytes");
            auth::ModuleManifest manifest;
            const std::string code = expected ? (*expected
                ? "function ReadSafety() local a,b=SafetyArgs(); assert(a.Is_Unit_Safe(b)==true) end"
                : "function ReadSafety() local a,b=SafetyArgs(); assert(a.Is_Unit_Safe(b)==false) end")
                : "function ReadSafety() local a,b=SafetyArgs(); a.Is_Unit_Safe(b) end";
            must(manifest.add("Safety.lua", code)); auth::SessionConfig config; config.galactic_clock = true;
            auto scripts = must(auth::ScriptScheduler::create(config, std::move(manifest)));
            const auth::ValueList args{auth::Value{auth::Handle{ga::handle_galactic_freestore, 1}},
                auth::Value{auth::Handle{eawr::script::foc::handle_game_object, 20}}};
            must(scripts.register_binding("SafetyArgs", [args](auto&, const auto&) { return eawr::core::Result<auth::ValueList>::success(args); }));
            auto input = std::make_shared<ga::FreestoreInput>(); input->pool = published;
            must(ga::register_freestore_bindings(scripts, input)); must(scripts.create_instance(1, "Safety.lua"));
            auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, 1, 0}; event.target = 1;
            event.kind = auth::ScriptEvent::Kind::call; event.name = "ReadSafety"; must(scripts.submit_event(event));
            const auto report = must(scripts.service(executor));
            expect(expected ? report.diagnostics.empty() : report.diagnostics.size() == 1
                && report.diagnostics.front().code == auth::codes::missing_api,
                "actual qualified Lua Is_Unit_Safe publishes known true/false or named unavailable input");
            expect(report.commands.empty(), "qualified safety query emits no mutation");
        }
        if (baseline.empty()) baseline = results; else expect(results == baseline, "qualified safety answers match on 1/2/4/8 workers");
    }
}
void no_source_transit_contracts() {
    std::map<std::string, std::vector<std::uint8_t>, std::less<>> baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto setup = galactic::synthetic_galaxy({42, 2, 2, 1, 1, true, false});
        const auto f = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
        setup.rules.maximum_fleet_movement_distance = f(1000); setup.rules.fiscal_cycle_frames = 10000;
        for (auto& seat : setup.seats) { seat.kind = galactic::SeatKind::ai; seat.human_side_id = 0; }
        setup.movement.emplace(); setup.movement->mode_modifier = f(1);
        setup.player_economy.emplace(); setup.fiscal.emplace(); setup.production.emplace();
        setup.census.emplace(); setup.fleet_control.emplace(); setup.production->maximum_human_queue = 1;
        for (const auto& player : setup.players) {
            const bool human = std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& seat) {
                return seat.player == player.player_id && seat.kind == galactic::SeatKind::human;
            });
            galactic::GalacticPlayerEconomy economy; economy.player = player.player_id;
            setup.player_economy->players.push_back(economy);
            setup.fiscal->players.push_back({player.player_id, false, eawr::sim::math::Fixed::from_integer(1).value()});
            setup.production->players.push_back({player.player_id, human, !human, human, {}, {}, {}, {}, {}});
            setup.census->players.push_back({player.player_id, 0, false, {}});
            setup.movement->players.push_back({player.player_id, f(1), f(1), false});
            for (const auto& planet : setup.planets) setup.movement->access.push_back({player.player_id, planet.id, true, false});
        }
        std::set<galactic::TypeId> types;
        for (const auto& fleet : setup.fleets) {
            const auto type = fleet.members.front().type; types.insert(type);
            setup.census->objects.push_back({fleet.id, type, fleet.owner, fleet.orbit, fleet.orbit, false, false, type});
            for (const auto& member : fleet.members) {
                types.insert(member.type);
                setup.census->objects.push_back({member.id, member.type, fleet.owner, fleet.orbit, fleet.id, false, false, member.type});
            }
        }
        for (const auto type : types) {
            setup.census->types.push_back({type, {}}); setup.fleet_control->types.push_back({type, false, true, false});
            setup.movement->types.push_back({type, f(1), f(100), 0, true, false, false, false});
        }
        for (const auto& planet : setup.planets) {
            setup.fiscal->planets.push_back({planet.id, {}});
            galactic::PlanetProduction row; row.planet = planet.id; setup.production->planets.push_back(row);
            setup.movement->planets.push_back({planet.id, f(1), false});
        }
        std::sort(setup.census->objects.begin(), setup.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        auto world = must(galactic::GalacticSession::create(setup));
        must(world.step(executor));
        const auto stationary = world.snapshot();
        const auto fleet = world.snapshot()->fleets().front(); const auto actor = fleet.members.front();
        const auto destination = world.snapshot()->planets().back().id;
        expect(must(world.admit_native_fleet(fleet.owner, galactic::MoveFleet{fleet.id, {fleet.orbit, destination}, {}})).admitted,
            "no-source contract enters actual native fleet transit");
        expect(world.snapshot()->fleets().front().motion.transit.has_value(), "native fleet is actually in transit");
        const auto records = world.snapshot()->actor_records();
        const auto parent = std::find_if(records.begin(), records.end(), [&](const auto& row) { return row.id == fleet.id; });
        expect(parent != records.end() && !parent->container && !parent->planet, "departed fleet has an actual absent source parent");
        ga::NativeActions actions(world, std::make_shared<eawr::script::foc::ai::GalacticFunding>(), {}, {}, {}, &executor);
        ga::FreestoreFacts facts; facts.object.id = actor.id; facts.object.type = facts.current_type = actor.type;
        facts.object.owner = fleet.owner; facts.parent = fleet.id; facts.qualified = true;
        facts = ga::ordinary_freestore_birth(facts); facts.enroute = facts.object.transit = true;
        std::size_t permission_reads{}; bool permit = true;
        const cp::GalacticAiUsability usable = [&](auto, const auto&, auto source) {
            ++permission_reads; expect(source == fleet.orbit, "source permission receives the actual containing planet");
            return eawr::core::Result<bool>::success(permit);
        };
        auto input = must(cp::read_stock_galactic_ai_freestore_move(usable, fleet.owner, facts, *world.snapshot()));
        expect(input.actor_valid && input.source_ai_usable && !input.grandparent && permission_reads == 0,
            "WGA-400..404 no-source helper reads no permission or containment before transit refusal");
        auto sourced = facts; sourced.object.planet = fleet.orbit; sourced.enroute = false; sourced.tracked_movement = true;
        input = must(cp::read_stock_galactic_ai_freestore_move(usable, fleet.owner, sourced, *stationary));
        expect(input.source_ai_usable && permission_reads == 1, "known source permission still precedes transit refusal");
        permit = false; input = must(cp::read_stock_galactic_ai_freestore_move(usable, fleet.owner, sourced, *stationary));
        expect(!input.source_ai_usable && permission_reads == 2, "source restriction is retained for transit actors");
        expect(!cp::read_stock_galactic_ai_freestore_move({}, fleet.owner, sourced, *stationary),
            "known source with missing permission provider remains an explicit refusal");
        auto& retained = actions.qualified_freestore(); must(retained.update(facts));
        const std::array offers{actor.id}; must(retained.initialize(fleet.owner, offers));
        expect(must(retained.reserve(fleet.owner, actor.id, 7)), "transit actor retains an actual offered reservation");
        actions.bind_admission_gate([] { return false; });
        actions.bind_freestore_movement([&](auto player, const auto& row, const auto& snapshot) {
            return cp::read_stock_galactic_ai_freestore_move(usable, player, row, snapshot);
        });
        actions.bind_freestore_path([](auto, const auto&, auto, auto) -> eawr::core::Result<ga::NativeActions::FreestorePathInput> {
            throw std::runtime_error("transit refusal must precede route/launch/split/block");
        }, [] { return false; });
        expect(must(actions.preflight_freestore_move(retained, fleet.owner, actor.id, fleet.orbit)) == ga::NativeActions::FreestoreMove::refused,
            "no-source transit refuses even a destination matching its previous source");
        const auto before = world.state_sha256(); const auto frontier = world.next_entity_id();
        auth::ModuleManifest manifest;
        must(manifest.add("Transit.lua", "function Refused() local a,b,c=MoveArgs() assert(a.Move_Object(b,c)==false) end"));
        auth::SessionConfig config; config.galactic_clock = true;
        auto scripts = must(auth::ScriptScheduler::create(config, std::move(manifest)));
        const auth::ValueList args{auth::Value{auth::Handle{ga::handle_galactic_freestore, fleet.owner}},
            auth::Value{auth::Handle{eawr::script::foc::handle_game_object, actor.id}},
            auth::Value{auth::Handle{eawr::script::foc::handle_game_object, fleet.orbit}}};
        must(scripts.register_binding("MoveArgs", [args](auto&, const auto&) { return eawr::core::Result<auth::ValueList>::success(args); }));
        auto free = std::make_shared<ga::FreestoreInput>(); free->pool = must(retained.publish(executor));
        must(ga::register_freestore_bindings(scripts, free)); must(scripts.set_immediate_effect_host(actions.immediate_host({}, 16), 1));
        must(scripts.create_instance(1, "Transit.lua"));
        auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, 1, 0}; event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call; event.name = "Refused"; must(scripts.submit_event(event));
        const auto report = must(scripts.service(executor));
        expect(report.diagnostics.empty() && report.commands.empty() && actions.freestore_move_refusals() == 0
            && actions.movements().empty() && permission_reads == 2 && world.state_sha256() == before && world.next_entity_id() == frontier,
            "sourced native Lua false creates no move, IDs, missing-reader count or permission reads");
        expect(retained.contains(fleet.owner, actor.id) && retained.reservations(fleet.owner)->at(actor.id) == 7,
            "WGA-404 native transit refusal preserves offered membership and its reservation");
        actions.bind_freestore_validation([&world, &retained, actor, fleet](const ga::QualifiedFreestore& restored) {
            const auto current = world.snapshot()->current_type(actor.id);
            const auto* facts = restored.facts(actor.id);
            if (!current || current.value() != actor.type || !facts || !facts->qualified
                || facts->object.owner != fleet.owner || facts->parent != fleet.id
                || restored.encode() != retained.encode())
                throw std::runtime_error("transit restore lost reconstructed actor, membership or reservation facts");
            return eawr::core::Result<void>::success();
        });
        const auto saved = must(actions.capture());
        if (baseline.empty()) baseline = saved.components; else expect(saved.components == baseline, "no-source native bytes are equal on 1/2/4/8");
        must(actions.restore(saved)); expect(must(actions.capture()).components == saved.components, "transit refusal archive round trip is exact");
    }
}
void nested_actor_facts_contracts() {
    namespace ai = eawr::script::foc::ai;
    const auto f = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    auto content = std::make_shared<ai::GalacticSelectionContent>();
    for (const auto id : {1U, 2U, 3U, 4U, 100U, 101U}) {
        auto type = std::make_shared<eawr::units::GalacticType>();
        type->ground_company_member = false; type->squadron_member = id == 3;
        if (id == 1 || id == 2) type->behavior = {"FLEET"};
        if (id == 4) type->behavior = {"TRANSPORT"};
        content->types[id].original = std::move(type);
    }
    const auto members = cp::freestore_member_qualifier(content);
    std::string baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto setup = galactic::synthetic_galaxy({42, 2, 2, 0, 0, true, false});
        const auto neutral = setup.players.back().player_id + 1;
        setup.players.push_back({neutral, neutral, 0}); // explicit synthetic neutral control owner
        for (auto& seat : setup.seats) { seat.kind = galactic::SeatKind::ai; seat.human_side_id = 0; }
        setup.planets[0].position = {}; setup.planets[1].position = {f(12), {}, {}};
        setup.rules.maximum_fleet_movement_distance = f(12);
        setup.fleets = {{10, 1, 1, {{20, 2}}}};
        setup.fleet_control.emplace(); setup.fleet_control->types = {{1, false, true, false}, {2, false, true, true},
            {3, false, true, false}, {4, true, true, true}};
        setup.movement.emplace(); auto& movement = *setup.movement; movement.mode_modifier = f(1);
        movement.types = {{1, f(1), {}, 0, true, false, false, false},
            {2, {}, {}, 0, false, false, true, false}, {3, f(1), {}, 0, true, false, false, false},
            {4, f(1), {}, 0, false, false, false, false, true}};
        movement.contents = {{20, {{30, 3}}}, {40, {}}};
        setup.census.emplace(); setup.census->types = {{1, {}}, {2, {}}, {3, {}}, {4, {}}, {100, {}}, {101, {}}};
        setup.census->objects = {{1, 100, 1, 1, 0, false, false, 100, 0, galactic::ActorCombatRole::ordinary},
            {2, 101, 2, 2, 0, false, false, 101, 0, galactic::ActorCombatRole::ordinary},
            {10, 1, 1, 1, 1, false, false, 1, 0, galactic::ActorCombatRole::fleet},
            {20, 2, 1, 1, 10, false, false, 2, 0, galactic::ActorCombatRole::fleet},
            {30, 3, 1, 1, 20, false, false, 3, 0, galactic::ActorCombatRole::ordinary},
            {40, 4, 1, 1, 1, false, false, 4, 0, galactic::ActorCombatRole::transport}};
        setup.control.emplace(); auto& control = *setup.control;
        control.change_seconds = f(7); // same named synthetic duration as control contracts
        control.max_ground_forces = 3;
        control.types = {{1, 0, false, false}, {2, 0, false, false}, {3, 1, false, false}, {4, 0, false, true}};
        for (const auto& player : setup.players)
            control.players.push_back({player.player_id, player.player_id == neutral,
                player.player_id != neutral, player.faction_id});
        for (const auto& player : setup.players) for (const auto& other : setup.players)
            control.relations.push_back({player.player_id, other.player_id, player.player_id == other.player_id});
        for (const auto& planet : setup.planets) {
            galactic::ControlPlanet surface; surface.planet = planet.id; surface.surface_accessible = true;
            const auto original = std::find_if(setup.census->objects.begin(), setup.census->objects.end(),
                [&](const auto& actor) { return actor.id == planet.id; });
            surface.type = original->original_type; surface.ground_base = {planet.owner, 1};
            if (planet.id == 1) surface.landed = {{40, 4, 1}};
            control.planets.push_back(std::move(surface));
        }
        for (const auto& route : setup.routes) control.routes.push_back({route.id, {}});
        setup.production.emplace(); setup.production->maximum_human_queue = 1;
        setup.rules.fiscal_cycle_frames = 10000;
        setup.fiscal.emplace(); setup.player_economy.emplace();
        for (const auto& player : setup.players) {
            movement.players.push_back({player.player_id, f(1), f(1), false});
            setup.census->players.push_back({player.player_id, 0, false, {}});
            setup.production->players.push_back({player.player_id, false, true, false, {}, {}, {}, {}, {}});
            setup.fiscal->players.push_back({player.player_id, false, f(1)});
            setup.player_economy->players.push_back({player.player_id, {}, {}});
            for (const auto& planet : setup.planets) movement.access.push_back({player.player_id, planet.id, true, false});
        }
        for (const auto& planet : setup.planets) {
            movement.planets.push_back({planet.id, f(1), false});
            galactic::PlanetProduction production{}; production.planet = planet.id;
            setup.production->planets.push_back(production); setup.fiscal->planets.push_back({planet.id, {}});
        }
        auto world = must(galactic::GalacticSession::create(setup));
        must(world.step(executor));
        const auto stationary = must(cp::read_stock_galactic_ai_actor_facts(*content, members, *world.snapshot(), 30));
        expect(stationary.parent == 20 && !stationary.parent_is_planet && stationary.current_type_member
            && stationary.object.planet == 1 && !stationary.enroute && !stationary.object.transit,
            "nested actor facts retain literal parent and actual stationary root-fleet location");
        const auto landed = must(cp::read_stock_galactic_ai_actor_facts(*content, members, *world.snapshot(), 40));
        expect(landed.object.transport && landed.parent == 1 && landed.parent_is_planet
            && landed.object.directly_parented_to_planet && landed.object.planet == 1
            && !landed.enroute && !landed.object.transit && landed.object.safe == true,
            "actual landed transport retains its literal planet parent and safe stationary location");
        expect(must(world.admit_native_fleet(1, galactic::MoveFleet{10, {1, 2}, {}})).admitted,
            "nested actor contract enters actual native transit");
        const auto moving = must(cp::read_stock_galactic_ai_actor_facts(*content, members, *world.snapshot(), 30));
        expect(moving.parent == 20 && moving.current_type_member && moving.object.planet == 0
            && moving.enroute && moving.object.transit, "nested actor facts inherit actual transit without flattening the parent");
        const auto trace = world.state_sha256();
        if (baseline.empty()) baseline = trace; else expect(trace == baseline, "nested actor/transit state equals on 1/2/4/8 workers");
    }
}
void actor_parent_depth_contracts() {
    namespace ai = eawr::script::foc::ai;
    auto content = std::make_shared<ai::GalacticSelectionContent>();
    for (const auto id : {2U, 3U}) {
        auto type = std::make_shared<eawr::units::GalacticType>();
        type->ground_company_member = false; type->squadron_member = id == 3;
        if (id == 2) type->behavior = {"FLEET"};
        content->types[id].original = std::move(type);
    }
    const auto members = cp::freestore_member_qualifier(content);
    const auto setup = galactic::synthetic_galaxy({42, 2, 2, 0, 0, true, false});
    for (const auto levels : {32U, 33U}) {
        // Exercise the immutable reader's defensive bound, independently of
        // native admission, which already rejects overdeep world input.
        auto records = std::make_shared<std::vector<galactic::CensusObject>>();
        auto movement = std::make_shared<galactic::MovementSetup>();
        for (unsigned level = 0; level < levels; ++level) {
            const auto id = eawr::sim::EntityId{100} + level;
            records->push_back({id, 2, 1, 1, level + 1 == levels ? 1 : id + 1,
                false, false, 2, 0, galactic::ActorCombatRole::fleet});
            movement->contents.push_back({id, {{level ? id - 1 : 200, level ? 2U : 3U}}});
        }
        records->push_back({200, 3, 1, 1, 100, false, false, 3, 0, galactic::ActorCombatRole::ordinary});
        const galactic::GalacticSnapshot snapshot(1, {}, setup.players, setup.planets, {}, {}, {},
            std::nullopt, std::nullopt, movement, std::nullopt, {}, std::nullopt, std::nullopt,
            std::nullopt, {}, {}, std::nullopt, std::nullopt, std::nullopt, {}, {}, std::nullopt,
            {}, std::nullopt, std::nullopt, std::nullopt, {}, 0, {}, records);
        const auto facts = cp::read_stock_galactic_ai_actor_facts(*content, members, snapshot, 200);
        if (levels == 32) expect(facts && facts.value().parent == 100 && facts.value().object.planet == 1,
            "actor facts accept the native 32-level parent boundary without flattening it");
        else expect(!facts && facts.error().code == "EAWR-GCAI-RUNTIME-0001"
            && facts.error().message.find("bounded literal fleet parent hierarchy") != std::string::npos,
            "overdeep immutable actor parents refuse with the named hierarchy capability");
    }
}
void sparse_actor_change_contracts() {
    using Change = galactic::GalacticSession::NativeActorChange;
    std::vector<std::uint8_t> baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        ga::QualifiedFreestore owner;
        ga::FreestoreFacts source;
        source.object.id = 10; source.object.type = 500; source.current_type = 501;
        source.object.owner = 1; source.object.planet = source.parent = 100;
        source.object.directly_parented_to_planet = source.parent_is_planet = true;
        source = ga::ordinary_freestore_birth(source); source.ai_usable = false;
        source.unit_ai = true;
        must(owner.update(source)); must(owner.initialize(1, {})); must(owner.initialize(2, {}));
        must(owner.set_prefer_ground(source.object.id, false));
        std::map<eawr::sim::EntityId, ga::FreestoreFacts> facts;
        for (const auto id : {10ULL, 20ULL, 21ULL, 22ULL, 23ULL, 30ULL}) {
            auto row = source; row.object.id = id; row.ai_usable = true;
            facts.emplace(id, row);
        }
        const cp::GalacticAiActorFactsReader reader = [&facts](auto id) {
            return eawr::core::Result<ga::FreestoreFacts>::success(facts.at(id));
        };
        const std::vector<Change> births{{Change::Kind::created, 20, 0, {}, {}, {}}, {Change::Kind::created, 21, 10, {}, {}, {}}};
        must(cp::apply_galactic_ai_actor_changes(owner, births, reader, executor));
        expect(owner.contains(1, 20) && !owner.contains(1, 21) && !owner.facts(21)->ai_usable,
            "ordinary birth offers once while generated actor inherits false source flag before admission");
        const auto before_refusal = owner.encode();
        facts.at(23).qualified = false;
        const std::vector<Change> refused{{Change::Kind::created, 22, 0, {}, {}, {}}, {Change::Kind::created, 23, 0, {}, {}, {}}};
        expect(!cp::apply_galactic_ai_actor_changes(owner, refused, reader, executor)
            && owner.encode() == before_refusal && !owner.facts(22),
            "a later unqualified event leaves all facts, membership and added callbacks unchanged");
        const std::vector<Change> missing_source{{Change::Kind::created, 30, 999, {}, {}, {}}};
        expect(!cp::apply_galactic_ai_actor_changes(owner, missing_source, reader, executor)
            && owner.encode() == before_refusal, "missing generated source refuses the whole creation batch");
        facts.at(10).object.owner = 2; facts.at(10).current_type = 502;
        facts.at(20).object.planet = facts.at(20).parent = 200;
        Change representation{Change::Kind::changed, 20, 0, {}, {}, {}}; representation.death_clone = true;
        const std::vector<Change> changed{{Change::Kind::changed, 10, 0, {}, {}, {}}, representation};
        must(cp::apply_galactic_ai_actor_changes(owner, changed, reader, executor));
        expect(owner.facts(10)->current_type == 502 && !owner.facts(10)->ai_usable && !owner.contains(2, 10),
            "owner/current representation changes preserve retained source flags instead of repeating birth defaults");
        expect(!owner.facts(10)->prefer_ground, "WGA-409 owner/current representation and return-location changes preserve false ground preference");
        expect(owner.contains(1, 20) && owner.facts(20)->parent == 200 && owner.facts(20)->death_clone,
            "raw flag and parent changes preserve membership until an actual removal event");
        const std::vector<Change> repeated_birth{{Change::Kind::created, 20, 0, {}, {}, {}}};
        const auto changed_bytes = owner.encode();
        expect(!cp::apply_galactic_ai_actor_changes(owner, repeated_birth, reader, executor)
            && owner.encode() == changed_bytes, "repeated birth cannot reset a retained actor");
        auto retained = *owner.facts(20); retained.tracked_movement = true; must(owner.update(retained));
        Change deleting{Change::Kind::changed, 20, 0, {}, {}, {}}; deleting.deleting = true;
        must(cp::apply_galactic_ai_actor_changes(owner, std::span(&deleting, 1), reader, executor));
        expect(owner.facts(20) && owner.facts(20)->deleting && owner.facts(20)->tracked_movement
            && !owner.contains(1, 20) && owner.additions(1).empty(),
            "delete-pending removes membership and queued additions while retaining flags and actual block state until native retirement");
        const std::vector<Change> loss{{Change::Kind::lost, 20, 0, {}, {}, {}}};
        must(cp::apply_galactic_ai_actor_changes(owner, loss, reader, executor));
        expect(!owner.facts(20) && !owner.contains(1, 20), "actual loss retires both actor facts and retained membership");
        const auto bytes = owner.encode();
        if (baseline.empty()) baseline = bytes;
        expect(bytes == baseline, "sparse event preparation and ordered commit are equal on 1/2/4/8 workers");
    }
}
// Supporting real native-owner transport contract. The declared return records
// exercise the adapter; they are not installed opponent or tactical outcome proof.
void free_fleet_signal_contracts() {
    std::string baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::string trace;
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
            auto setup = galactic::synthetic_galaxy({42, 2, 2, 1, 1, true, false});
            // Qualify the native destination graph at the authored synthetic
            // spacing; global deferral still must precede every route read.
            setup.rules.maximum_fleet_movement_distance = must(eawr::sim::math::subtract(
                setup.planets.back().position.x, setup.planets.front().position.x));
            auto world = must(galactic::GalacticSession::create(setup)); must(world.step(executor));
            const auto fleet = world.snapshot()->fleets().front(); const auto actor = fleet.members.front();
            const auto destination = world.snapshot()->planets().back().id;
            auto actions = std::make_shared<ga::NativeActions>(world, std::make_shared<eawr::script::foc::ai::GalacticFunding>(),
                ga::NativeActions::Placement{}, std::map<galactic::PlayerId, eawr::sim::math::Fixed>{});
            ga::FreestoreFacts facts; facts.object.id = actor.id; facts.object.type = actor.type;
            facts.object.owner = fleet.owner; facts.object.planet = fleet.orbit; facts.parent = fleet.id;
            facts.current_type = actor.type; facts.qualified = true;
            must(actions->qualified_freestore().update(ga::ordinary_freestore_birth(facts)));
            const std::array offers{actor.id}; must(actions->qualified_freestore().initialize(fleet.owner, offers));
            actions->bind_admission_gate([] { return false; });
            actions->bind_freestore_movement([&](auto player, const auto& row, const auto& snapshot) {
                ga::NativeActions::FreestoreMoveInput input; input.actor_valid = player == fleet.owner && row.object.id == actor.id
                    && std::any_of(snapshot.fleets().begin(), snapshot.fleets().end(), [&](const auto& retained) { return retained.id == fleet.id; });
                input.source_ai_usable = true; input.grandparent = fleet.orbit;
                return eawr::core::Result<ga::NativeActions::FreestoreMoveInput>::success(input);
            });
            actions->bind_freestore_path([](auto, const auto&, auto, auto) -> eawr::core::Result<ga::NativeActions::FreestorePathInput> {
                throw std::runtime_error("pending global conflict must defer before any route read");
            }, [] { return true; });
            expect(must(actions->admit_freestore_move(fleet.owner, actor.id, destination)), "real free native request registers while globally deferred");
            const auto request = actions->movements().begin()->first;
            const auto listener = ga::retained_movement_battle_listener(actions, {}, std::function<auth::ScriptScheduler*()>{[] { return nullptr; }});
            cp::ConflictRecord conflict; conflict.id = 100000; conflict.planet = fleet.orbit;
            conflict.invader = fleet.owner; conflict.defender = 2; conflict.forces = {{actor.id, actor.type, fleet.owner, fleet.id}};
            must(listener({cp::GalacticAiBattleKind::entered, 1, conflict, *world.snapshot(), {}, false}));
            expect(!actions->movements().at(request).advance_requested && !actions->movements().at(request).finished,
                "fleet-subject begin clears both flags without TaskForce owners");
            cp::BattleResult returned; returned.conflict = conflict.id; returned.winner = fleet.owner;
            returned.forces = {{actor.id, scenario == 1 ? cp::ForceDisposition::retreated : cp::ForceDisposition::survived}};
            if (scenario == 1) returned.retreat_player = fleet.owner;
            conflict.layer = cp::BattleLayer::land;
            const auto before_ground = must(actions->capture());
            must(listener({cp::GalacticAiBattleKind::returned, 2, conflict, *world.snapshot(), &returned, true}));
            expect(must(actions->capture()).components == before_ground.components, "ground return never forwards a free space signal");
            conflict.layer = cp::BattleLayer::space;
            expect(!actions->invalidate_freestore_actor(request, fleet.id + 1000)
                && must(actions->capture()).components == before_ground.components, "wrong actor subject refuses without changing block");
            auto unrelated = conflict; unrelated.forces.front().parent = fleet.id + 1000;
            must(listener({cp::GalacticAiBattleKind::returned, 2, unrelated, *world.snapshot(), &returned, false}));
            expect(must(actions->capture()).components == before_ground.components, "unrelated same-planet fleet return leaves the block unchanged");
            if (scenario >= 2) { const std::array lost{actor.id}; must(world.apply_battle_losses(lost)); returned.forces.front().disposition = cp::ForceDisposition::destroyed; }
            if (scenario == 3) must(actions->retire_freestore_movement(actor.id, true));
            must(listener({cp::GalacticAiBattleKind::returned, 3, conflict, *world.snapshot(), &returned, false}));
            const auto& block = actions->movements().at(request);
            expect(block.advance_requested == (scenario != 3) && block.finished == (scenario != 0) && block.actor == (scenario ? 0 : fleet.id),
                "space end and early loss retirement preserve their advance flags while clearing invalid fleet actors");
            expect(actions->freestore_move_refusals() == 0, "fully bound native request has no silent unavailable refusal");
            if (scenario == 1) {
                const auto saved = must(actions->capture());
                ga::NativeActions restored(world, std::make_shared<eawr::script::foc::ai::GalacticFunding>(), {}, {});
                restored.bind_freestore_validation([&world](const ga::QualifiedFreestore& retained) {
                    const auto view = world.snapshot();
                    for (const auto& [id, row] : retained.inventory())
                        if (std::none_of(view->fleets().begin(), view->fleets().end(), [&](const auto& fleet) {
                            return fleet.owner == row.object.owner && std::any_of(fleet.members.begin(), fleet.members.end(),
                                [&](const auto& member) { return member.id == id && member.type == row.object.type; });
                        })) throw std::runtime_error("declared signal fixture lost its actual selected actor");
                    return eawr::core::Result<void>::success();
                });
                restored.bind_admission_gate([] { return false; });
                restored.bind_freestore_path([](auto, const auto&, auto, auto) -> eawr::core::Result<ga::NativeActions::FreestorePathInput> {
                    throw std::runtime_error("global deferral must preserve pending retreat before route read");
                }, [] { return true; });
                must(restored.restore(saved));
                expect(must(restored.capture()).components == saved.components, "pending retreat archive preserves actor0/finished/requested-advance without initialization");
                // object.transit is derived from the actual live fleet (stationary under global deferral),
                // not from the tracked row, so qualify the expected copy from the real snapshot.
                const auto live = world.snapshot()->fleets();
                const auto live_fleet = std::find_if(live.begin(), live.end(), [&](const auto& value) { return value.id == fleet.id; });
                expect(live_fleet != live.end() && !live_fleet->motion.transit, "pending retreat leaves the actual fleet stationary under global deferral");
                auto untracked = *actions->qualified_freestore().facts(actor.id); untracked.tracked_movement = false;
                untracked.object.transit = !untracked.object.planet || untracked.enroute;
                must(actions->service_freestore_movement(fleet.owner, actor.id)); must(restored.service_freestore_movement(fleet.owner, actor.id));
                expect(*actions->qualified_freestore().facts(actor.id) == untracked,
                    "retreat cleanup clears only tracked membership and preserves live raw parent/location");
                expect(must(actions->capture()).components == must(restored.capture()).components,
                    "retreat cleanup clears only tracked membership and preserves live raw parent/location after restore");
            }
            trace += world.state_sha256() + must(actions->state_hash());
        }
        if (baseline.empty()) baseline = trace;
        expect(trace == baseline, "free begin/end/ground/retreat/loss owner bytes equal on1/2/4/8");
    }
}
// Supporting composition fixture, with explicitly empty gameplay inputs. All
// retained owners are production implementations; this is not opponent proof.
void real_factory_contracts(bool configured_cadence = false) {
    std::string baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        auto executor = std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers);
        auto layout = galactic::synthetic_galaxy({.seed = 42, .planets = 2, .players = 2, .fleets_per_player = 0});
        layout.player_economy = galactic::PlayerEconomySetup{{{1, eawr::sim::math::Fixed::from_raw(100 * eawr::sim::math::Fixed::scale),
            eawr::sim::math::Fixed::from_raw(1000 * eawr::sim::math::Fixed::scale), 1, 5},
            {2, {}, eawr::sim::math::Fixed::from_raw(1000 * eawr::sim::math::Fixed::scale), 1, 5}}};
        // EAWR-SIM-0501 fleet control requires graph rules; this empty composition has no routes,
        // so it authors the same named zero-distance graph seam as the other empty provider fixtures.
        layout.rules.maximum_fleet_movement_distance = eawr::sim::math::Fixed{};
        layout.fleet_control = galactic::FleetControlSetup{};
        layout.rules.fiscal_cycle_frames = 18000;
        layout.fiscal = galactic::FiscalSetup{eawr::sim::math::Fixed::from_raw(1000 * eawr::sim::math::Fixed::scale), {}, {}};
        layout.production = galactic::ProductionSetup{};
        layout.production->maximum_human_queue = 1;
        for (const auto& player : layout.players) {
            layout.fiscal->players.push_back({player.player_id, false, eawr::sim::math::Fixed::from_raw(eawr::sim::math::Fixed::scale)});
            galactic::ProductionPlayer builder; builder.player = player.player_id; builder.human = builder.uses_population_cap = true;
            layout.production->players.push_back(builder);
        }
        for (const auto& planet : layout.planets) {
            layout.fiscal->planets.push_back({planet.id, {}});
            galactic::PlanetProduction location; location.planet = planet.id; location.target_owner = planet.owner;
            layout.production->planets.push_back(location);
        }
        auto setup = std::make_shared<cp::GalacticAiRuntimeSetup>(); setup->executor = executor;
        const ga::ServicePeriods configured_periods{300, 3, 10, 10, 300, 5};
        if (configured_cadence) setup->lifecycle.service_periods = configured_periods;
        auto content = std::make_shared<eawr::script::foc::ai::GalacticSelectionContent>(); setup->content = content;
        setup->immediate_instances = setup->immediate_requests = 16; setup->content_identity = std::string(64, 'a');
        setup->perception.seed = 42; setup->perception.players = {{1, 1, "A", "NORMAL", false, true, false, 0}, {2, 2, "B", "NORMAL", false, false, false, 0}};
        for (const auto& planet : layout.planets) setup->perception.planets.push_back({planet.id, "P" + std::to_string(planet.id), planet.id, {}, {}, {}, {}, {}});
        setup->perception.cache_intervals.emplace("ENEMYFORCE.SPACETOTALUNNORMALIZED", eawr::script::foc::ai::real(0));
        eawr::script::foc::AiType budget_type; budget_type.type_id = 500; budget_type.name = "BUDGET_TYPE";
        setup->perception.types.push_back(budget_type);
        eawr::script::foc::ai::GalacticEconomyType budget_economy; budget_economy.type = 500;
        budget_economy.generic_price = eawr::script::foc::ai::real(100); budget_economy.generic_seconds = eawr::script::foc::ai::real(2);
        setup->perception.economy_types.push_back(budget_economy);
        setup->lifecycle.players = {{1, "FIXTURE", "NORMAL", 2, {}}};
        setup->lifecycle.categories = {{"OFFENSIVE", 1}};
        setup->lifecycle.xml = {
            {"data/xml/ai/players/fixture.xml", R"(<AIPlayerType><Name>Fixture</Name><Templates><Galactic>Default</Galactic></Templates><Difficulty_Adjustments><Normal>Normal</Normal></Difficulty_Adjustments></AIPlayerType>)"},
            {"data/xml/ai/templates/fixture.xml", R"(<AITemplates><Default><Budget><Offensive>One</Offensive></Budget></Default></AITemplates>)"},
            {"data/xml/ai/perceptualequations/fixture.xml", R"(<Equations><One>1</One><Timeline>Game.Timeline</Timeline><Budget>Variable_Self.BudgetFractionToBuild {Parameter_Type="BUDGET_TYPE"}</Budget><Category>Variable_Self.BudgetFractionToBuild {Parameter_Type="BUDGET_TYPE",Parameter_Category=1}</Category></Equations>)"},
            {"data/xml/difficultyadjustments.xml", R"(<Difficulty_Adjustments><Difficulty_Adjustment Name="Normal"/></Difficulty_Adjustments>)"}
        };
        for (const auto& [path, text] : setup->lifecycle.xml) { static_cast<void>(text); setup->lifecycle.xml_paths.push_back(path); }
        setup->modules = {{"Data/Scripts/Library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={} FriendlyContrastTypes={} end"}};
        cp::GalacticAiRuntimeProviders providers;
        providers.execution_input = [](auto player, const auto&, const auto&, const auto&, const auto*) {
            ga::ExecutionRouteInput input; input.player = player;
            return eawr::core::Result<ga::ExecutionRouteInput>::success(std::move(input));
        };
        // Explicit supporting fixture: it authors no plans/build heads. Stock
        // production remains incomplete until R1's actual publisher is bound.
        providers.build_budgets = [rows = std::make_shared<const std::vector<eawr::script::foc::ai::GalacticBudgetBuild>>()]
            (const auto&, const auto& actions, const auto&) {
            expect(actions.purchases().empty(), "empty-head fixture has no native build purchases");
            return eawr::core::Result<cp::GalacticAiRuntimeProviders::BuildBudgetPublication>::success(rows);
        };
        providers.initialize_freestore = [](auto player, auto& actions, const auto&, const auto&) {
            return actions.qualified_freestore().initialize(player, {});
        };
        providers.freestore_types = {{1, ga::FreestoreKind::unit, false}};
        providers.prepare_connectivity = [](const auto&, auto&, auto&, const auto&) {
            return eawr::core::Result<eawr::script::foc::ai::GalacticConnectivity>::success({});
        };
        providers.bind_actions = [](auto&, auto&, auto&, auto, auto, auto) { return eawr::core::Result<void>::success(); };
        providers.prepare_child_scripts = [](auto, auto, auto) {
            cp::GalacticAiRuntimeProviders::ChildScripts pair;
            pair.first = [](const auto&) -> eawr::core::Result<auth::ScriptScheduler> { throw std::runtime_error("fixture must not create child scripts"); };
            pair.second = [](auto&) -> eawr::core::Result<void> { throw std::runtime_error("fixture must not configure a child"); };
            return eawr::core::Result<cp::GalacticAiRuntimeProviders::ChildScripts>::success(std::move(pair));
        };
        providers.prepare_battle = [](auto, auto, auto) {
            return cp::GalacticAiRuntimeProviders::BattleListener([](const auto&) -> eawr::core::Result<void> { throw std::runtime_error("fixture must not create a battle"); });
        };
        providers.placement = [](const auto&, const auto&, auto) -> eawr::core::Result<galactic::GalacticSession::NativeForcePlacement> {
            throw std::runtime_error("fixture must not fabricate placement");
        };
        providers.shadow_placement = [](const auto&, const auto&, auto) -> eawr::core::Result<galactic::GalacticSession::NativeForcePlacement> {
            throw std::runtime_error("fixture must not fabricate shadow placement");
        };
        auto configuration = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
        bool refuse_outer_scheduler{};
        auth::ScriptScheduler* refusal_scripts{};
        std::vector<std::shared_ptr<unsigned>> route_reads;
        std::vector<std::shared_ptr<eawr::script::foc::ai::GalacticWorld>> worlds;
        std::vector<std::shared_ptr<eawr::script::foc::ai::Engine>> engines;
        std::vector<std::shared_ptr<ExternalFixture>> external;
        std::vector<std::shared_ptr<ExternalFixture>> tails;
        std::vector<std::weak_ptr<const cp::ComposedGalacticAiState>> states;
        std::vector<std::weak_ptr<const ga::Manager>> managers;
        struct CopiedInput { std::uint8_t value{}; bool fail_once{}; };
        std::vector<std::shared_ptr<CopiedInput>> copied_inputs;
        unsigned births{};
        must(cp::configure_galactic_ai_runtime(*configuration, setup, [providers, &route_reads, &worlds, &engines, &external, &tails, &states, &managers, &copied_inputs, &births,
            &refuse_outer_scheduler, &refusal_scripts, instance_bound = setup->immediate_instances] {
            auto fresh = providers;
            fresh.prepare_fresh_freestore = [&births](auto& actions, const auto&, const auto&) {
                must(actions.capture()); ++births;
                return eawr::core::Result<void>::success();
            };
            external.push_back(std::make_shared<ExternalFixture>());
            fresh.external.emplace("TEST_EXTERNAL", external.back());
            tails.push_back(std::make_shared<ExternalFixture>("TEST_Z_EXTERNAL"));
            fresh.external.emplace("TEST_Z_EXTERNAL", tails.back());
            copied_inputs.push_back(std::make_shared<CopiedInput>());
            // This supporting world has no gameplay actors. Publish its actual
            // initialized empty owner while exercising derived-copy rollback.
            fresh.publish_freestore = [derived = copied_inputs.back(), source = external.back()](const auto& actions, auto& input, const auto&, const auto& executor) {
                must(actions.capture()); // The final production owner is readable.
                input.pool = must(actions.qualified_freestore().publish(executor));
                derived->value = source->value;
                if (derived->fail_once) {
                    derived->fail_once = false;
                    eawr::core::Diagnostic error; error.code = "TEST-DERIVED-PUBLICATION";
                    error.message = "derived input publication refuses after its partial copy";
                    return eawr::core::Result<void>::failure(std::move(error));
                }
                return eawr::core::Result<void>::success();
            };
            fresh.observe = [&states, &managers](const auto& view) { states.push_back(view.state); managers.push_back(view.manager); };
            auto reads = std::make_shared<unsigned>();
            fresh.bind_actions = [&worlds, &engines, &route_reads, &refuse_outer_scheduler, &refusal_scripts, reads](auto& parent, auto&, auto&, auto engine, auto world, auto) {
                if (refuse_outer_scheduler) refusal_scripts = &parent.galactic_scripts()->ai_scheduler();
                worlds.push_back(std::move(world)); engines.push_back(std::move(engine)); route_reads.push_back(reads);
                return eawr::core::Result<void>::success();
            };
            fresh.execution_input = [reads](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto* reconstruction) {
                ga::ExecutionRouteInput input; input.player = player; input.maximum_distance = eawr::script::foc::ai::real(++*reads == 1 ? 125 : 1);
                const auto equation = must(eawr::script::foc::ai::parse_equation("FIXTURE_RAW_SPACE",
                    "Variable_Target.EnemyForce.SpaceTotalUnnormalized", {}));
                const auto before = must(world.state_hash());
                eawr::script::foc::ai::AiRandom random;
                for (const auto& planet : snapshot.planets()) {
                    const auto* target = engine.target_of_object(planet.id);
                    expect(target != nullptr, "real execution reconstruction has retained targets");
                    const eawr::script::foc::ai::Context context{player, 2, {}, target};
                    const auto power = reconstruction ? world.reconstruction_enemy_space(*reconstruction, context)
                        : world.perception(engine, equation, context, random);
                    expect(power.has_value(), "qualified raw enemy-space getter resolves in real empty-force fixture");
                    input.planets.push_back({planet.id, planet.position, {}, *power, {}, true});
                }
                if (reconstruction) expect(must(world.state_hash()) == before,
                    "derived restore reads archived or absent raw rows without native cache/visibility writes");
                if (*reads > 1) std::reverse(input.planets.begin(), input.planets.end());
                return eawr::core::Result<ga::ExecutionRouteInput>::success(std::move(input));
            };
            if (refuse_outer_scheduler) {
                struct RefusedImmediateHost final : auth::ImmediateEffectHost {
                    static eawr::core::Diagnostic refusal() {
                        eawr::core::Diagnostic error; error.code = "TEST-S6-OUTER-CAPTURE";
                        error.message = "S6 fixture outer capture refused"; return error;
                    }
                    eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>> capture() const override {
                        return eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>>::failure(refusal());
                    }
                    eawr::core::Result<void> publish(std::unique_ptr<auth::ImmediateEffectState>) override {
                        return eawr::core::Result<void>::failure(refusal());
                    }
                };
                fresh.initialize_freestore = [initialize = providers.initialize_freestore, &refusal_scripts, instance_bound]
                    (auto player, auto& actions, const auto& snapshot, const auto& admitted) {
                    if (auto initialized = initialize(player, actions, snapshot, admitted); !initialized) return initialized;
                    if (!refusal_scripts) throw std::runtime_error("refusal fixture requires its borrowed scheduler");
                    expect(refusal_scripts->instances().empty(), "empty-owner fixture pumps no player Lua before the outer service");
                    // Replace the host after Manager's normal bind. With no
                    // instances, its first capture is the bridge's outer service.
                    return refusal_scripts->set_immediate_effect_host(std::make_shared<RefusedImmediateHost>(), instance_bound);
                };
            }
            return eawr::core::Result<cp::GalacticAiRuntimeProviders>::success(std::move(fresh));
        }));
        auto game = must(cp::GameSession::create(layout)); must(configuration->configure_native_ai(game));
        expect(births == 1, "fresh actor facts are prepared exactly once at live birth");
        expect(worlds.back()->evaluate_read_only(*engines.back(), "Timeline", 1, nullptr) == eawr::script::foc::ai::real(0),
            "fresh runtime publishes explicit global constructor timeline before manager initialization");
        const auto before_manager = must(game.save_galactic_ai());
        auto uninitialized = before_manager;
        auto constructor = must(ga::decode_funding_state(uninitialized.state->components.at("funding")));
        expect(constructor.empty(), "pre-manager funding snapshot has no initialized ledger");
        constructor.timeline = 3;
        uninitialized.state->components.at("funding") = must(ga::encode_funding_state(constructor));
        must(game.restore_galactic_ai(uninitialized));
        expect(worlds.back()->evaluate_read_only(*engines.back(), "Timeline", 1, nullptr) == eawr::script::foc::ai::real(0),
            "funding restore cannot replace the actual unserviced mode timeline");
        must(game.restore_galactic_ai(before_manager));
        for (unsigned frame = 0; frame < 3; ++frame) must(game.step(*executor));
        const auto ordinary_budget = must(game.save_galactic_ai());
        auto revised_budget = ordinary_budget;
        auto funding = must(ga::decode_funding_state(revised_budget.state->components.at("funding")));
        funding.timeline = 4;
        funding.at(1).last_service_income = eawr::sim::math::Fixed::from_raw(50 * eawr::sim::math::Fixed::scale);
        funding.at(1).categories.at(1).income = eawr::sim::math::Fixed::from_raw(10 * eawr::sim::math::Fixed::scale);
        funding.at(1).categories.at(1).cash = eawr::sim::math::Fixed::from_raw(25 * eawr::sim::math::Fixed::scale);
        revised_budget.state->components.at("funding") = must(ga::encode_funding_state(funding));
        must(game.restore_galactic_ai(revised_budget));
        const auto budget_native = must(game.save_galactic_ai());
        expect(worlds.back()->evaluate_read_only(*engines.back(), "Timeline", 1, nullptr) == eawr::script::foc::ai::real(1)
            && worlds.back()->evaluate_read_only(*engines.back(), "Budget", 1, nullptr) == eawr::script::foc::ai::parse_real("0.5").value()
            && worlds.back()->evaluate_read_only(*engines.back(), "Category", 1, nullptr) == eawr::script::foc::ai::real(100) / eawr::script::foc::ai::real(45),
            "restored budgets consume decoded funding while Timeline reads the serviced mode owner");
        expect(must(game.save_galactic_ai()).state->components == budget_native.state->components,
            "read-only economy publication and nested getters leave native state unchanged");
        must(game.restore_galactic_ai(ordinary_budget));
        auto expired = worlds.back()->state();
        expect(expired.cache.size() == layout.planets.size() && std::all_of(expired.cache.begin(), expired.cache.end(), [&](const auto& row) {
            return row.interval == eawr::script::foc::ai::real(0) && row.frame < worlds.back()->synchronized_frame();
        }), "serviced fixture saves expired raw enemy-space caches before the execution deadline");
        expired.cache.pop_back(); must(worlds.back()->restore(expired));
        expect(worlds.back()->state().cache.size() == 1,
            "saved perception has an expired archived row and a missing row requiring an unpublished getter");
        const auto captured = must(game.save_galactic_ai());
        expect(captured.state && captured.state->components.size() == 8, "serviced runtime captures six real native owners and two qualified external fixtures");
        const auto composed = states.back().lock(); expect(static_cast<bool>(composed), "real composed state is retained");
        const auto warm_hash = must(composed->state_hash());
        external.back()->set(2); tails.back()->refuse_capture();
        expect(!composed->state_hash(), "later external capture refusal follows changed payload without stale digest success");
        const auto live_hash = must(composed->state_hash());
        const auto live_capture = must(game.save_galactic_ai());
        expect(live_hash != warm_hash && live_capture.state->components.at("TEST_EXTERNAL") == std::vector<std::uint8_t>{2}
            && live_hash == must(ga::native_state_hash(*live_capture.state)), "live external epoch mutation and refusal retry match independently captured full oracle");
        external.back()->set(1);
        expect(must(composed->state_hash()) == warm_hash, "owned external setter restores original hash without composed restore invalidation");
        auto changed = captured; changed.state->components.at("TEST_EXTERNAL").front() = 2;
        const auto original_hash = game.state_sha256();
        external.back()->fail_once = true;
        expect(!game.restore_galactic_ai(changed) && external.back()->value == 1 && game.state_sha256() == original_hash,
            "external partial failure rolls all native owners back with identical digest");
        copied_inputs.back()->fail_once = true;
        expect(!game.restore_galactic_ai(changed) && external.back()->value == 1 && copied_inputs.back()->value == 1
            && game.state_sha256() == original_hash
            && must(game.save_galactic_ai()).state->components == captured.state->components,
            "partial derived publication refusal rolls every native owner back and republishes previous input");
        must(game.restore_galactic_ai(changed));
        expect(external.back()->value == 2 && copied_inputs.back()->value == 2 && game.state_sha256() != original_hash,
            "successful publication observes restored external state after final production owner");
        must(game.restore_galactic_ai(captured));
        expect(game.state_sha256() == original_hash, "external restoration reproduces canonical native hash");
        ga::ExecutionStateContributor execution_codec([&](const ga::ExecutionRetainedState& state) {
            const auto snapshot = game.galaxy().snapshot();
            const auto invalid = [] {
                eawr::core::Diagnostic error; error.code = "TEST-EXECUTION-REFERENCE";
                error.message = "execution inspection names an unresolved reconstructed native reference";
                return eawr::core::Result<void>::failure(std::move(error));
            };
            for (const auto& [player, row] : state) {
                if (std::none_of(snapshot->players().begin(), snapshot->players().end(),
                    [player](const auto& who) { return who.player_id == player; })) return invalid();
                for (const auto node : row.node_order)
                    if (std::none_of(snapshot->planets().begin(), snapshot->planets().end(),
                        [node](const auto& planet) { return planet.id == node; })) return invalid();
                for (const auto& [object, plan] : row.reservations)
                    if (!engines.back()->galactic_reservation_valid(player, object, plan)) return invalid();
            }
            return eawr::core::Result<void>::success();
        });
        must(execution_codec.restore(*captured.state));
        const auto route_state = execution_codec.state().at(1);
        expect(route_state.initialized && route_state.finder_present && route_state.node_order.size() == layout.planets.size()
            && route_state.maximum_distance == eawr::script::foc::ai::real(125), "factory retains initialized nodes and movement maximum across refresh inputs");
        const auto route_work = worlds.back()->work();
        const auto first_planet = layout.planets.front().id;
        expect(!worlds.back()->publish_execution_routes(true, 999, {{1, first_planet, 999999, true, {}, {}, {}}})
            && worlds.back()->execution_route(1, first_planet, first_planet)
            && worlds.back()->work().prepared_objects == route_work.prepared_objects,
            "unresolved route publication refuses atomically without recopying completed objects");
        const auto identities = ck::game_identities(layout); const auto saved = must(ck::save_game_checkpoint(game, identities));
        if (configured_cadence) setup->lifecycle.service_periods = ga::retail_service_periods;
        auto restored = must(ck::load_game_checkpoint(saved, identities, configuration->restore, *executor));
        if (configured_cadence) {
            expect(must(ck::parse_checkpoint(saved)).find(ck::SectionId::galactic_owner) && restored.work.resimulated_ticks == 0,
                "configured AI periods restore through direct owners without replaying ticks");
            expect(managers.front().lock()->service_periods() == configured_periods
                && managers.back().lock()->service_periods() == configured_periods,
                "native lifecycle section imports saved periods despite fresh retail defaults");
        }
        expect(must(ck::save_game_checkpoint(restored.session, identities)) == saved, "factory restore canonically recaptures serviced native state");
        expect(births == 1, "checkpoint reconstruction and native rollback never apply fresh actor defaults");
        const auto reads_after_restore = *route_reads.back();
        std::string continuation;
        for (unsigned frame = 0; frame < 5; ++frame) {
            const auto expected = must(game.step(*executor)).state_hash.get();
            expect(must(restored.session.step(*executor)).state_hash.get() == expected, "real runtime first resumed deadlines/accounts/RNG equal control");
            continuation += expected;
        }
        expect(*route_reads.back() == reads_after_restore, "resumed execution does not refresh routes before the saved deadline");
        const auto reads_before_deadline = *route_reads.front();
        bool refreshed{};
        for (std::uint64_t frame = 0; frame <= route_state.next_refresh + 100; ++frame) {
            const auto expected = must(game.step(*executor)).state_hash.get();
            expect(must(restored.session.step(*executor)).state_hash.get() == expected,
                "expired-cache restored continuation equals control through first admitted execution deadline");
            continuation += expected;
            if (*route_reads.front() != reads_before_deadline) {
                expect(*route_reads.back() == reads_after_restore + 1,
                    "control and restored owners refresh once on the same first admitted deadline");
                const auto live = worlds.back()->state();
                expect(std::all_of(live.cache.begin(), live.cache.end(), [&](const auto& row) {
                    return row.frame > expired.cache.front().frame;
                }), "first normal execution admission refreshes expired caches after read-only reconstruction");
                refreshed = true; break;
            }
        }
        expect(refreshed, "continuation reaches a real saved execution deadline");
        if (configured_cadence) for (unsigned frame = 0; frame < 90; ++frame) {
            const auto expected = must(game.step(*executor)).state_hash.get();
            expect(must(restored.session.step(*executor)).state_hash.get() == expected,
                "non-default direct restore matches 90 further serviced ticks");
            continuation += expected;
        }
        if (baseline.empty()) baseline = continuation; else expect(baseline == continuation, "real serviced runtime equals on 1/2/4/8");
        if (!configured_cadence) {
            refuse_outer_scheduler = true;
            auto faulted = must(cp::GameSession::create(layout)); must(configuration->configure_native_ai(faulted));
            const auto refused = faulted.step(*executor);
            expect(!refused && refused.error().message.find("S6 fixture outer capture refused") != std::string::npos,
                "outer scheduler refuses after the runtime before_service hook completed");
            refuse_outer_scheduler = false;
            auto independent = must(galactic::GalacticSession::create(layout));
            const std::vector<galactic::PlayerId> players{1};
            auto opened = independent.begin_native_barrier(players, 1);
            expect(opened && opened.value(), "a refused outer scheduler releases its barrier before the failed game is destroyed");
            if (opened && opened.value()) {
                must(opened.value()->begin_tail()); must(opened.value()->commit());
            }
        }
    }
}
void prepared_restore_contracts() {
    std::string baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto layout = galactic::synthetic_galaxy({.seed = 42, .planets = 2, .players = 2, .fleets_per_player = 0});
        auto game = must(cp::GameSession::create(layout));
        auto scripts = schedulers();
        must(game.install_galactic_scripts(std::move(scripts.first), std::move(scripts.second)));
        for (unsigned frame = 0; frame < 3; ++frame) must(game.step(executor));
        const auto identities = ck::game_identities(layout);
        const auto bytes = must(ck::save_game_checkpoint(game, identities));
        auto setup = std::make_shared<cp::GalacticAiRuntimeSetup>();
        setup->executor = std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers);
        setup->content = std::make_shared<eawr::script::foc::ai::GalacticSelectionContent>();
        const auto missing = cp::prepare_galactic_ai_runtime(setup, {});
        expect(!missing && missing.error().code == "EAWR-GCAI-RUNTIME-0001"
            && missing.error().message.find("B live connectivity") != std::string::npos,
            "factory refuses absent live execution instead of prepared stand-ins");
        unsigned prepared{}, installed{}, fallback{};
        std::vector<std::shared_ptr<unsigned>> owners;
        ck::GameLoadOptions options;
        options.configure_session = [&](auto&) {
            expect(prepared == installed + 1, "fresh parent bindings are prepared before reconstruction adapters");
            return eawr::core::Result<void>::success();
        };
        options.prepare_galactic = [&]() {
            auto owner = std::make_shared<unsigned>(++prepared); owners.push_back(owner);
            return eawr::core::Result<ck::GameLoadOptions::PreparedGalacticScripts>::success({schedulers(),
                [&, owner](cp::GameSession& restored) {
                    expect(restored.galactic_scripts() && restored.completed_tick() == 3 && *owner != 0,
                        "matching native closure runs after restored Lua/world boundary");
                    ++installed; return eawr::core::Result<void>::success();
                }, {}, {}});
        };
        options.configure_native_ai = [&](auto&) { ++fallback; return eawr::core::Result<void>::success(); };
        auto first = must(ck::load_game_checkpoint(bytes, identities, options, executor));
        auto second = must(ck::load_game_checkpoint(bytes, identities, options, executor));
        expect(prepared == 2 && installed == 2 && fallback == 0 && owners[0] != owners[1],
            "each load constructs isolated binding owners and invokes only its native installer");
        expect(must(ck::save_game_checkpoint(first.session, identities)) == bytes,
            "runtime preparation preserves exact saved barrier bytes");
        std::string continuation;
        for (unsigned frame = 0; frame < 4; ++frame) {
            const auto expected = must(game.step(executor)).state_hash.get();
            expect(must(first.session.step(executor)).state_hash.get() == expected
                && must(second.session.step(executor)).state_hash.get() == expected,
                "first and later resumed services equal uninterrupted state");
            continuation += expected;
        }
        if (baseline.empty()) baseline = continuation;
        else expect(baseline == continuation, "prepared native load continuation equals on 1/2/4/8");
        options.prepare_galactic = [] {
            return eawr::core::Result<ck::GameLoadOptions::PreparedGalacticScripts>::success({schedulers(), {}, {}, {}});
        };
        expect(!ck::load_game_checkpoint(bytes, identities, options, executor), "prepared runtime missing native closure refuses before publication");
    }
}
std::filesystem::path game_root() {
#ifdef _MSC_VER
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") || !value) throw std::runtime_error("installed campaign gate requires EAWR_EAW_GAME_ROOT");
    std::filesystem::path root(value); std::free(value); return root;
#else
    const auto* value = std::getenv("EAWR_EAW_GAME_ROOT");
    if (!value) throw std::runtime_error("installed campaign gate requires EAWR_EAW_GAME_ROOT");
    return value;
#endif
}
// Installed structural/lifecycle proof, separate from opponent progression.
// Both constructors are actual stock native force writers; no AI barrier is
// active when the first birth delivers through the configured worker fallback.
void installed_world_phase_births(const cp::CampaignSetupPlan& plan, const cp::CampaignSetupServices& services,
    const cp::GalacticAiRuntimeSetup& setup, galactic::PlayerId player) {
    const auto ship = std::find_if(setup.content->types.begin(), setup.content->types.end(), [](const auto& row) {
        return row.second.name == "ACCLAMATOR_ASSAULT_SHIP";
    });
    expect(ship != setup.content->types.end(), "installed ordinary Acclamator constructor is qualified");
    if (ship == setup.content->types.end()) return;
    const auto members = cp::freestore_member_qualifier(setup.content);
    std::string baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        auto configured = std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers);
        const eawr::sim::PartitionExecutor* admitted{};
        auto root = must(cp::CampaignRoot::create(plan, services));
        auto world = must(galactic::GalacticSession::create(root.game()->galaxy().setup()));
        const auto planet = std::find_if(world.snapshot()->planets().begin(), world.snapshot()->planets().end(),
            [player](const auto& row) { return row.owner == player; });
        expect(planet != world.snapshot()->planets().end(), "installed constructor has a real owned source planet");
        if (planet == world.snapshot()->planets().end()) return;
        const auto location = planet->id;
        ga::NativeActions actions(world, std::make_shared<eawr::script::foc::ai::GalacticFunding>(),
            [inputs = services.stock_starting_forces](const auto& request, const auto& snapshot, auto first) {
                return cp::stock_native_force_builder(inputs, {request.player, request.target, request.type, {}})(snapshot, first);
            }, {}, {}, configured.get());
        const cp::GalacticAiSnapshotActorFactsReader reader = [content = setup.content, members](const auto& snapshot, auto actor) {
            return cp::read_stock_galactic_ai_actor_facts(*content, members, snapshot, actor);
        };
        cp::bind_galactic_ai_actor_changes(actions, reader, [&admitted, configured] {
            return admitted ? admitted : configured.get();
        });
        actions.bind_freestore_births([content = setup.content, members](auto& retained, const auto& snapshot,
            const auto& shadow, auto changes, const auto& executor) {
            return cp::apply_galactic_ai_actor_changes(retained, changes, [&snapshot, &shadow, content, members](auto actor) {
                return cp::read_stock_galactic_ai_actor_facts(*content, members, snapshot, actor, &shadow);
            }, executor);
        });
        using Change = galactic::GalacticSession::NativeActorChange;
        std::vector<Change> initial;
        for (const auto& actor : world.snapshot()->actor_records())
            initial.push_back({Change::Kind::created, actor.id, actor.generated_source, actor.dead, actor.delete_pending, {}});
        must(cp::apply_galactic_ai_actor_changes(actions.qualified_freestore(), initial,
            [&](auto actor) { return reader(*world.snapshot(), actor); }, *configured));
        const auto offers = must(ga::initial_freestore_offers(*world.snapshot(), actions.qualified_freestore()));
        must(actions.qualified_freestore().initialize(player, offers));
        actions.bind_handlers();
        const auto before = actions.qualified_freestore().object_count(player);
        ga::ActionRequest request; request.kind = ga::ActionKind::magic;
        request.player = player; request.target = location; request.type = ship->first;
        const auto born = must(actions.admit(request));
        expect(born.admitted && actions.created().size() == 1 && !admitted
            && actions.qualified_freestore().contains(player, born.request)
            && actions.qualified_freestore().object_count(player) == before + 1,
            "actual world-phase stock birth offers through configured workers without an active AI barrier");
        const auto* born_facts = actions.qualified_freestore().facts(born.request);
        const auto* current = born_facts ? setup.content->types.at(born_facts->current_type).original.get() : nullptr;
        bool loaded_unit_ai{};
        if (current) for (const auto* behavior : {&current->behavior, &current->space_behavior, &current->land_behavior})
            loaded_unit_ai = loaded_unit_ai || std::find(behavior->begin(), behavior->end(), "UNIT_AI") != behavior->end();
        expect(current && born_facts->unit_ai == loaded_unit_ai && born_facts->prefer_ground,
            "stock current behavior supplies unit-AI presence and ordinary birth initializes preference true");
        if (born_facts && loaded_unit_ai) {
            must(actions.qualified_freestore().set_prefer_ground(born.request, false));
            const auto reread = must(reader(*world.snapshot(), born.request));
            expect(reread.unit_ai && !actions.qualified_freestore().facts(born.request)->prefer_ground,
                "stock type reader does not replace the retained actor preference with a fresh default");
        }
        auto host = actions.immediate_host([inputs = services.stock_starting_forces](const auto& input, const auto& view, auto first) {
            return cp::stock_native_force_shadow_builder(inputs, {input.player, input.target, input.type, {}})(view, first);
        }, 4);
        auto shadow = must(host->capture());
        const auth::ValueList args{auth::Value{auth::Handle{eawr::script::foc::handle_player, player}},
            auth::Value{auth::Handle{eawr::script::foc::handle_game_object, location}},
            auth::Value{auth::ValueList{auth::Value{auth::Handle{eawr::script::foc::handle_type, ship->first}}}}};
        const auto made = must(shadow->apply("galactic.magic_force", args));
        const auto& actors = std::get<auth::ValueList>(made.at(0).data);
        const auto private_actor = std::get<auth::Handle>(actors.at(0).data).id;
        const auth::ValueList count_args{auth::Value{auth::Handle{ga::handle_galactic_freestore, player}}};
        const auto count = [&] { return std::get<ga::Number>(must(shadow->query("galactic.freestore_count", count_args)).at(0).data); };
        expect(count() == ga::Number(static_cast<std::int64_t>(before + 2)), "same-pass count sees actual private birth");
        must(shadow->apply("galactic.prevent_ai_usage", {auth::Value{auth::Handle{eawr::script::foc::handle_game_object, private_actor}}, auth::Value{true}}));
        expect(count() == ga::Number(static_cast<std::int64_t>(before + 1)), "same-pass prevent removes the new private membership");
        must(host->publish(std::move(shadow)));
        expect(world.snapshot()->current_type(private_actor) && actions.qualified_freestore().facts(private_actor)
            && !actions.qualified_freestore().contains(player, private_actor)
            && actions.qualified_freestore().object_count(player) == before + 1,
            "private publication preserves prevent and suppresses duplicate world birth offers");
        const auto result = world.state_sha256() + must(actions.state_hash());
        if (baseline.empty()) baseline = result;
        expect(result == baseline, "actual world/private constructor and pool bytes are equal on1/2/4/8 workers");
    }
}
// WGA-309/436..439 installed evidence: units of the stock free store actually move. The movement counter is the
// number of distinct free-store movement requests the native owner retained; the hop witness is a free-store
// fleet observed in transit. Fixed frame count, so every worker count compares the same state.
struct MovementProbe {
    std::size_t workers{};
    std::uint64_t frames{}, requests{}, first_request_frame{}, hop_frame{}, refusals{};
    std::vector<std::pair<std::uint64_t, std::string>> hashes;
    std::string final_hash, failure;
};
std::vector<MovementProbe> movement_probes;
void freestore_movement_probe(cp::GameSession& game, const cp::GalacticAiRuntimeSetup& setup,
    const std::shared_ptr<galactic_campaign_gate::Observations>& observed, std::uint64_t frames, std::size_t workers) {
    MovementProbe out; out.workers = workers;
    try {
        if (observed->runtimes.empty()) throw std::runtime_error("campaign factory did not publish native runtime owners");
        const auto view = observed->runtimes.front();
        std::set<std::uint64_t> seen;
        while (out.frames < frames) {
            const auto tick = must(game.step(*setup.executor));
            ++out.frames;
            const auto actions = view.actions.lock();
            if (!actions) throw std::runtime_error("native action owner lifetime ended during the probe");
            out.refusals = actions->freestore_move_refusals();
            for (const auto& [request, movement] : actions->movements()) {
                if (!movement.freestore_actor || !movement.actor || !seen.insert(request).second) continue;
                if (seen.size() == 1) out.first_request_frame = out.frames;
            }
            if (!out.hop_frame) for (const auto& entry : actions->movements()) {
                const auto& movement = entry.second;
                if (!movement.freestore_actor || !movement.registered || movement.finished) continue;
                const auto fleets = tick.galaxy->fleets();
                const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == movement.actor; });
                if (fleet != fleets.end() && fleet->motion.transit) { out.hop_frame = out.frames; break; }
            }
            if (out.frames % 300 == 0) out.hashes.emplace_back(out.frames, game.state_sha256());
        }
        out.requests = seen.size();
        out.final_hash = game.state_sha256();
    } catch (const std::exception& error) { out.failure = error.what(); }
    movement_probes.push_back(std::move(out));
}
int installed(bool release, const std::filesystem::path& directory, bool benchmark, std::uint64_t probe_frames = 0,
    std::size_t probe_workers = 1) {
    const auto root = game_root();
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto files = must(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    // Stock single-player authoring supplies Human to eager perception and Lua
    // equations (WGA-417/418/422). Keep both observed AI players and use the
    // actual Equal Footing Underworld campaign's authored human seat.
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
    const auto* source = data.campaign(campaign);
    expect(source && source->starting_active_player == "Underworld", "stock release campaign authors the Underworld human seat");
    cp::CampaignSetupOptions options; options.seed = 42;
    galactic::PlayerId human{};
    std::vector<ga::PlayerSelection> selected;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<galactic::PlayerId>(options.players.size() + 1);
        const bool selected_faction = faction.name == "Empire" || faction.name == "Rebel" || faction.name == source->starting_active_player;
        options.players.push_back({{id, id, 0}, faction.name, selected_faction || !faction.is_playable.value_or(false)});
        if (faction.name == "Empire" || faction.name == "Rebel") {
            options.seats.push_back({galactic::SeatId{id}, id, galactic::SeatKind::ai, id - 1, true, 0});
            selected.push_back({id, faction.name == "Empire" ? "BASICEMPIRE" : "BASICREBEL", "NORMAL", {}, {}});
        }
        if (faction.name == source->starting_active_player) {
            human = id; options.local_observer = id;
            options.seats.push_back({galactic::SeatId{id}, id, galactic::SeatKind::human, id - 1, true, 1});
        }
    }
    expect(selected.size() == 2, "frozen Empire/Rebel AI seats exist in installed data");
    expect(human != 0, "frozen authored human exists in installed data");
    selected[0].enemy = selected[1].player; selected[1].enemy = selected[0].player;
    for (auto& player : selected) player.human = human;
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    const auto setup = must(cp::prepare_stock_galactic_ai_runtime(files, loaded.catalog, data, types, plan, selected,
        std::make_shared<eawr::platform::ThreadWorkerAdapter>(probe_workers), false, 4096, 4096));
    const auto human_facts = std::find_if(setup->perception.players.begin(), setup->perception.players.end(),
        [human](const auto& row) { return row.player == human; });
    expect(human_facts != setup->perception.players.end() && human_facts->human && !human_facts->ai
        && human_facts->faction == source->starting_active_player, "release Human comes from the actual prepared active human seat");
    expect(!setup->plans.plans.empty() && !setup->modules.empty() && !setup->perception.force_types.empty()
        && setup->content_identity.size() == 64, "installed selected plan/Lua/type closure prepared from real providers");
    if (release) {
        std::filesystem::create_directories(directory);
        auto observed = std::make_shared<galactic_campaign_gate::Observations>();
        for (const auto& player : selected) observed->players.emplace(player.player, galactic_campaign_gate::PlayerProgress{});
        std::ofstream frozen(directory / "case.json", std::ios::trunc);
        frozen << "{\"campaign\":\"" << campaign << "\",\"seed\":42,\"difficulty\":\"NORMAL\",\"closure\":\"" << setup->content_identity
            << "\",\"uses_fog\":false,\"mode_fps\":30,\"game_seconds_limit\":900,\"immediate_instances\":4096,\"immediate_requests\":4096,\"human\":{\"id\":" << human
            << ",\"faction\":\"Underworld\"},\"players\":[";
        bool first = true;
        for (const auto& player : selected) {
            if (!first) { frozen << ','; }
            first = false;
            frozen << "{\"id\":" << player.player << ",\"type\":"; galactic_campaign_gate::json_string(frozen, player.player_type); frozen << '}';
        }
        frozen << "]}\n"; frozen.close();
        expect(static_cast<bool>(frozen), "frozen installed gate identity report is durable");
        bool initialized{};
        try {
            const auto services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
            auto root_owner = must(cp::CampaignRoot::create(plan, services));
            for (const auto& actor : root_owner.game()->galaxy().snapshot()->actor_records()) {
                const auto original = setup->content->types.find(actor.original_type);
                const auto current = setup->content->types.find(actor.current_type);
                expect(original != setup->content->types.end() && original->second.original
                    && current != setup->content->types.end() && current->second.original,
                    "installed constructor actor identities resolve in immutable AI content");
            }
            const auto space = must(cp::prepare_stock_space_entry(files, loaded.catalog, data, types, plan));
            const auto usability = must(cp::prepare_stock_galactic_ai_usability(data, plan, setup->perception));
            cp::GalacticAiStockQueryFactory queries = [usability] {
                auto provider = std::make_shared<cp::GalacticAiUsabilityProvider>(usability);
                cp::GalacticAiStockQueries out;
                out.usability = [provider](auto player, const auto& snapshot, auto planet) { return provider->usability(player, snapshot, planet); };
                out.connectivity_publication = [provider](const auto& parent, auto& world, auto& engine, const auto& workers) { return provider->connectivity_publication(parent, world, engine, workers); };
                out.movement = [provider](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto& workers) { return provider->movement(player, snapshot, world, engine, workers); };
                out.external.emplace("GALACTIC_USABILITY", std::move(provider));
                return eawr::core::Result<cp::GalacticAiStockQueries>::success(std::move(out));
            };
            auto factory = must(cp::stock_galactic_ai_providers(setup, services.stock_starting_forces, space, std::move(queries),
                [observed](auto view) { observed->runtimes.push_back(std::move(view)); },
                [observed](const auto& notice) { observed->battle(notice); }));
            eawr::platform::CampaignServiceConfiguration configuration;
            configuration.configure = [&](auto& game) { return cp::configure_live_campaign_conflicts(game, files, loaded.catalog, data, types, plan, space); };
            configuration.restore.configure_session = configuration.configure;
            configuration.restore.content = [space](const auto& conflict) { return space->content(conflict); };
            must(cp::configure_galactic_ai_runtime(configuration, setup, std::move(factory)));
            must(configuration.configure(*root_owner.game()));
            must(configuration.configure_native_ai(*root_owner.game())); initialized = true;
            if (probe_frames) {
                freestore_movement_probe(*root_owner.game(), *setup, observed, probe_frames, probe_workers);
                return movement_probes.back().failure.empty() ? 0 : 2;
            }
            const auto report = galactic_campaign_gate::run(*root_owner.game(), *setup, configuration, observed, directory,
                galactic_learning_gate::probe(directory / "learning.json"));
            report.write(std::cout);
            return (benchmark ? report.passed : report.correctness) && !failures ? 0 : 2;
        } catch (const std::exception& error) {
            std::ofstream refused(directory / "result.json", std::ios::trunc);
            refused << "{\"release_gate_passed\":false,\"run_started\":" << (initialized ? "true" : "false") << ",\"failure\":";
            galactic_campaign_gate::json_string(refused, error.what()); refused << "}\n"; refused.close();
            std::cerr << error.what() << '\n'; return 2;
        }
    }
    const auto birth_services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
    installed_world_phase_births(plan, birth_services, *setup, selected.front().player);
    eawr::platform::CampaignServiceConfiguration configuration;
    const auto configured = cp::configure_galactic_ai_runtime(configuration, setup, {});
    expect(!configured && configured.error().code == "EAWR-GCAI-RUNTIME-0001" && !configuration.configure_native_ai,
        "unavailable runtime refuses without publishing a half configuration");
    std::cout << "{\"campaign\":\"" << campaign << "\",\"seed\":42,\"difficulty\":\"NORMAL\","
        "\"players\":[\"BASICEMPIRE\",\"BASICREBEL\"],\"mode_fps\":30,\"game_seconds_limit\":900,"
        "\"immediate_instances\":4096,\"immediate_requests\":4096,\"closure\":\"" << setup->content_identity
        << "\",\"release_gate_passed\":false,\"run_started\":false,\"builds\":0,\"expansions\":0,"
        "\"battles\":0,\"heroes\":0,\"terminal_result\":null,\"live_save_boundaries_verified\":0}"
        << '\n' << configured.error().message << '\n';
    // No tick limit, forced victory or empty campaign can pass the release gate.
    return failures ? 1 : 0;
}
void authored_selection_contracts() {
    namespace ai = eawr::script::foc::ai;
    eawr::data::galactic::GalacticData data;
    for (const auto& name : {"Azure", "Copper", "Jade", "Rogue"}) {
        eawr::data::galactic::Faction faction; faction.name = name;
        faction.is_neutral = std::string_view(name) == "Rogue";
        faction.enemies = {"Azure", "Copper", "Jade", "Rogue"};
        data.factions.push_back(std::move(faction));
    }
    eawr::data::galactic::Planet planet; planet.name = "Home"; planet.behaviors = {"PLANET"};
    planet.galactic_position = eawr::data::galactic::Position{};
    data.planets.push_back(planet);
    data.constants = {{"MaximumFleetMovementDistance", "10", eawr::sim::math::Fixed::from_integer(10).value()},
        {"Fiscal_Cycle_Time_In_Secs", "45", eawr::sim::math::Fixed::from_integer(45).value()}};
    eawr::data::galactic::Campaign campaign; campaign.name = "Renamed"; campaign.locations = {"Home"}; campaign.planets = {"Home"};
    campaign.ai_player_control = {{"azure", "QuietPlayer"}, {"copper", "BuilderPlayer"}, {"jade", "OtherPlayer"}, {"rogue", "NoPlans"}};
    data.campaigns.push_back(campaign);
    cp::CampaignSetupOptions options; options.seed = 42;
    for (galactic::PlayerId id = 1; id <= 4; ++id) {
        options.players.push_back({{id, id, 0}, data.factions[id - 1].name, true});
        options.seats.push_back({galactic::SeatId{id}, id, id == 1 ? galactic::SeatKind::human : galactic::SeatKind::ai,
            id - 1, true, id == 1 ? 1U : 0U});
    }
    ai::AiData ai_data;
    for (const auto& assignment : campaign.ai_player_control) {
        ai::PlayerType type; type.name = assignment.name;
        type.difficulty.emplace("HARD", "MOD_CHALLENGE");
        ai_data.players.emplace(ai::upper_case(type.name), std::move(type));
    }
    const auto select = [&] {
        auto plan = must(cp::plan_campaign_setup(data, {}, campaign.name, options));
        return must(cp::select_live_galactic_ai_players(data, plan, ai_data, "mod_challenge"));
    };
    auto selected = select();
    expect(selected.size() == 2 && selected[0].player == galactic::PlayerId{2} && selected[0].player_type == "BuilderPlayer"
        && selected[1].player_type == "OtherPlayer" && selected[0].difficulty == "HARD",
        "renamed campaign/types select actual AI seats, excluding Neutral controller metadata");
    expect(selected[0].human == galactic::PlayerId{1} && selected[0].enemy == galactic::PlayerId{1} && selected[1].enemy == galactic::PlayerId{1},
        "galactic context prefers the active human enemy over later AI and rogue players");
    ai::PlayerType unrelated; unrelated.name = "UnrelatedModPlayer";
    unrelated.difficulty.emplace("MEDIUM", "MOD_CHALLENGE");
    unrelated.difficulty.emplace("NORMAL", "MOD_CHALLENGE");
    ai_data.players.emplace(ai::upper_case(unrelated.name), std::move(unrelated));
    ai_data.players.at("NOPLANS").difficulty.emplace("NORMAL", "MOD_CHALLENGE");
    ai_data.players.at("QUIETPLAYER").difficulty.emplace("NORMAL", "MOD_CHALLENGE");
    selected = select();
    expect(selected.size() == 2 && selected[0].difficulty == "HARD" && selected[1].difficulty == "HARD",
        "unrelated ambiguous mod, human and Neutral player types cannot block installed AI seats");
    ai_data.players.at("NOPLANS").difficulty.erase("NORMAL");
    ai_data.players.at("QUIETPLAYER").difficulty.erase("NORMAL");
    ai_data.players.at("BUILDERPLAYER").difficulty.emplace("NORMAL", "MOD_CHALLENGE");
    auto ambiguous_plan = must(cp::plan_campaign_setup(data, {}, campaign.name, options));
    expect(!cp::select_live_galactic_ai_players(data, ambiguous_plan, ai_data, "mod_challenge"),
        "ambiguity in an installed AI player type still refuses");
    ai_data.players.at("BUILDERPLAYER").difficulty.erase("NORMAL");
    ai_data.players.at("OTHERPLAYER").difficulty.erase("HARD");
    ai_data.players.at("OTHERPLAYER").difficulty.emplace("NORMAL", "MOD_CHALLENGE");
    expect(!cp::select_live_galactic_ai_players(data, ambiguous_plan, ai_data, "mod_challenge"),
        "conflicting difficulty levels between installed AI player types still refuse");
    ai_data.players.at("OTHERPLAYER").difficulty.erase("NORMAL");
    ai_data.players.at("OTHERPLAYER").difficulty.emplace("HARD", "MOD_CHALLENGE");
    options.seats[0].kind = galactic::SeatKind::ai; options.seats[0].human_side_id = 0;
    selected = select();
    expect(selected.size() == 3 && !selected[0].human && selected[0].enemy == galactic::PlayerId{3} && selected[1].enemy == galactic::PlayerId{3},
        "no human context uses last non-rogue enemy");
    options.seats[2].active = false; options.players[2].active = false;
    ai_data.players.at("OTHERPLAYER").difficulty.emplace("NORMAL", "MOD_CHALLENGE");
    selected = select();
    expect(selected.size() == 2 && selected[0].enemy == galactic::PlayerId{2} && selected[1].enemy == galactic::PlayerId{1},
        "inactive players/controllers cannot acquire AI or enemy contexts");
    const auto plan = must(cp::plan_campaign_setup(data, {}, campaign.name, options));
    expect(!cp::select_live_galactic_ai_players(data, plan, ai_data, "Missing_Adjustment"),
        "missing difficulty mapping refuses instead of assuming Normal");
}
struct ProfileEvidence final {
    std::vector<std::vector<std::string>> hashes{};
    std::vector<std::string> galaxy_sha{};
};
int live_production(const std::filesystem::path& directory, std::uint64_t frames, bool performance_only,
    bool cadence_window = false, ga::ServicePeriods periods = ga::retail_service_periods,
    ProfileEvidence* evidence = nullptr, bool short_record_contract = false) {
    using Live = eawr::platform::LiveGameSession;
    if (!frames) throw std::runtime_error("live production probe requires nonzero frames");
    if (cadence_window && frames < (short_record_contract ? 32U : 600U))
        throw std::runtime_error("cadence proof requires at least two perception deadlines");
    const bool profiling = eawr::gc_profile::enabled.exchange(false, std::memory_order_relaxed);
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, game_root() / folder / "Data")).mount);
    auto files = std::make_shared<const eawr::vfs::Vfs>(must(eawr::vfs::Vfs::mount(mounts)));
    const auto loaded = must(eawr::data::load_catalog(*files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(*files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
    const auto* source = data.campaign(campaign);
    if (!source) throw std::runtime_error("installed live AI campaign missing");
    cp::CampaignSetupOptions setup; setup.seed = 42;
    galactic::SeatId human{};
    std::set<galactic::PlayerId> ai_players;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<galactic::PlayerId>(setup.players.size() + 1);
        const bool local = faction.name == source->starting_active_player;
        const bool playable = faction.is_playable.value_or(false);
        setup.players.push_back({{id, id, 0}, faction.name, true});
        // Match the viewer's full seat layout, including its neutral/nonplayable
        // AI metadata. Only playable opponents supply the progression witnesses.
        setup.seats.push_back({galactic::SeatId{id}, id, local ? galactic::SeatKind::human : galactic::SeatKind::ai, id - 1, true, local ? id : 0});
        if (local) { setup.local_observer = id; human = {id}; }
        else if (playable) ai_players.insert(id);
    }
    expect(human.value != 0 && !ai_players.empty(), "installed production case has actual human and AI controllers");
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) setup.types.push_back({setup.types.size() + 1, name});
    const auto prepare = [&](bool install_native_ai) {
        return must(eawr::platform::PreparedLiveCampaign::create(files, loaded.catalog, data, types, campaign, setup, "Normal_Default", install_native_ai, periods));
    };
    const auto advance = [](Live& live, std::uint64_t tick) {
        live.advance_to(tick);
        if (!live.wait_for(tick, std::chrono::minutes(30))) {
            const auto error = live.failure();
            throw std::runtime_error(error ? eawr::core::format_diagnostic(*error) : "live AI frame timed out");
        }
    };
    // Native AI capabilities must not constrain ordinary (default-OFF) mod starts.
    for (const bool missing_fog : {true, false}) {
        auto modified = data;
        if (missing_fog) {
            std::erase_if(modified.constants, [](const auto& row) { return row.tag == "AIUsesFogOfWarGalactic"; });
            expect(!modified.constant("AIUsesFogOfWarGalactic"), "mod fixture removes native AI fog policy");
        } else {
            bool changed{};
            for (auto& authored : modified.campaigns) {
                if (authored.name != campaign) continue;
                for (auto& assignment : authored.ai_player_control) {
                    const auto player = std::find_if(setup.players.begin(), setup.players.end(), [&](const auto& row) {
                        return eawr::script::foc::ai::upper_case(row.faction) == eawr::script::foc::ai::upper_case(assignment.faction)
                            && ai_players.contains(row.player.player_id);
                    });
                    if (player == setup.players.end()) continue;
                    assignment.name = "EAWR_UNMAPPED_NATIVE_PLAYER"; changed = true; break;
                }
            }
            expect(changed, "mod fixture removes one active native AI player mapping");
        }
        auto off = must(eawr::platform::PreparedLiveCampaign::create(files, loaded.catalog, modified, types,
            campaign, setup, "Normal_Default"));
        Live::Options off_options; off_options.pacing = Live::Pacing::driven;
        auto running = must(Live::start(std::move(off), off_options));
        advance(*running, 1);
        expect(!running->failure(), "default-OFF campaign starts and steps without native AI inputs");
        running->stop();
        auto on = eawr::platform::PreparedLiveCampaign::create(files, loaded.catalog, modified, types,
            campaign, setup, "Normal_Default", true, periods);
        expect(!on && on.error().message.find(missing_fog ? "fog policy" : "authored AI player type") != std::string::npos,
            "explicit native AI opt-in clearly refuses its missing authored input");
        std::cout << "native AI off/on missing " << (missing_fog ? "fog policy" : "AI player mapping") << " checked\n";
    }
    const auto measure = [&](Live& live, std::vector<eawr::gc_profile::Snapshot>& phases,
        const std::function<void()>& midpoint = {}, const std::function<void()>& after_frame = {}) {
        std::vector<double> samples;
        for (std::uint64_t index = 0; index < frames; ++index) {
            const auto phase_begin = profiling ? eawr::gc_profile::snapshot() : eawr::gc_profile::Snapshot{};
            eawr::gc_profile::enabled.store(profiling, std::memory_order_relaxed);
            const auto begin = std::chrono::steady_clock::now();
            advance(live, live.completed_tick() + 1);
            samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
            eawr::gc_profile::enabled.store(false, std::memory_order_relaxed);
            if (profiling) phases.push_back(eawr::gc_profile::difference(phase_begin, eawr::gc_profile::snapshot()));
            if (after_frame) after_frame();
            if (midpoint && index + 1 == frames / 2) midpoint();
        }
        return samples;
    };
    const auto mean = [](const auto& samples) { return std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size()); };
    // Nearest-rank percentiles summarize each independent run without pairing frames.
    const auto percentile = [](auto samples, std::size_t percent) {
        std::sort(samples.begin(), samples.end());
        return samples[(samples.size() * percent + 99) / 100 - 1];
    };
    std::filesystem::create_directories(directory);
    std::ofstream report(directory / "live-production.json", std::ios::trunc);
    report << "{\"frames\":" << frames << ",\"cadence_window\":" << (cadence_window ? "true" : "false") << ",\"periods\":[";
    for (std::size_t i = 0; i < periods.size(); ++i) report << (i ? "," : "") << periods[i];
    report << "],\"runs\":[";
    std::vector<std::string> reference;
    std::vector<galactic::GalacticEvent> reference_events;
    for (const std::size_t workers : std::vector<std::size_t>{1U, 2U, 4U, 8U}) {
        Live::Options options; options.workers = workers; options.pacing = Live::Pacing::driven;
        options.saves.directory = (directory / ("w" + std::to_string(workers))).string();
        // Same initialized galaxy and ordinary campaign services; native AI
        // preparation and installation are off. Delta includes AI-caused sim work.
        auto baseline = must(Live::start(prepare(false), options));
        advance(*baseline, 1);
        std::vector<eawr::gc_profile::Snapshot> baseline_phases;
        const auto baseline_samples = measure(*baseline, baseline_phases, cadence_window
            ? std::function<void()>{[&] { advance(*baseline, baseline->completed_tick() + 1); }} : std::function<void()>{});
        const auto baseline_ms = mean(baseline_samples);
        const auto baseline_p50 = percentile(baseline_samples, 50);
        const auto baseline_p95 = percentile(baseline_samples, 95);
        baseline->stop();
        auto prepared_live = prepare(true);
        const auto live_layout = prepared_live->layout();
        const auto prepared_snapshot = prepared_live->snapshot();
        auto live = must(Live::start(std::move(prepared_live), options));
        expect(live->frame().latest == prepared_snapshot,
            "production AI is installed before prepared snapshot publication and owner transfer");
        std::vector<galactic::GalacticEvent> events;
        std::uint64_t event_cursor{};
        std::uint64_t last_save_token{};
        std::string boundary_galaxy_sha;
        const auto collect = [&] {
            const auto rows = live->galactic_events_after(event_cursor);
            expect(!rows.incomplete, "cadence proof drains each committed event before bounded retention expires");
            for (const auto& row : rows.events) events.push_back(row.event);
            event_cursor = rows.through;
        };
        const auto capture = [&](std::string name) {
            eawr::platform::LiveSaveRequest request; request.seat = human; request.name = name; request.overwrite = true;
            const auto tick = live->completed_tick() + 1;
            const auto token = live->request_save(std::move(request));
            last_save_token = token;
            advance(*live, tick);
            if (!live->wait_save(token, std::chrono::minutes(5))) throw std::runtime_error("live AI save timed out");
            const auto outcome = live->terminal_save_outcome(token);
            if (!outcome || outcome->phase != eawr::platform::LiveSavePhase::completed) throw std::runtime_error("live AI save failed");
            collect();
            auto saved = must(ck::parse_checkpoint(must(eawr::platform::save_detail::read(options.saves.directory, name))));
            const auto* section = saved.find(ck::SectionId::galactic_ai);
            if (!section) throw std::runtime_error("production save missing native AI section");
            const auto ai = must(cp::decode_galactic_ai_save(section->bytes));
            if (!ai.state) throw std::runtime_error("production startup did not install native galactic AI");
            expect(ai.state->components.contains(std::string(ga::lifecycle_component)), "production native owner includes galactic services");
            if (name == "boundary" && evidence) {
                const auto* owner = saved.find(ck::SectionId::galactic_owner);
                if (!owner) throw std::runtime_error("profile equality needs the direct owner checkpoint");
                // Reconstruct only the existing owner payload, outside the timed
                // window, to read canonical galaxy SHA without a new live API.
                auto imported = must(cp::GameSession::create(live_layout));
                must(imported.load_checkpoint_state(owner->bytes));
                boundary_galaxy_sha = imported.galaxy().state_sha256();
            }
            return *ai.state;
        };
        const auto initial_ai = capture("initial");
        if (workers == 1) {
            auto off = must(Live::start(prepare(false), options));
            const auto refused = off->prepare_load("initial", false);
            expect(!refused && refused.error().message.find("native AI section needs a fresh state contributor") != std::string::npos,
                "native AI checkpoint clearly requires opt-in when loading through an OFF owner");
            off->stop();
        }
        std::unique_ptr<eawr::platform::PreparedLiveRestore> midpoint_restore;
        std::vector<eawr::gc_profile::Snapshot> live_phases;
        const auto live_samples = measure(*live, live_phases, cadence_window ? std::function<void()>{[&] {
            capture("midwindow");
            midpoint_restore = must(live->prepare_load("midwindow", false));
        }} : std::function<void()>{}, collect);
        const auto live_ms = mean(live_samples);
        const auto live_p50 = percentile(live_samples, 50);
        const auto live_p95 = percentile(live_samples, 95);
        std::cout << "live production workers=" << workers << " frames=" << frames << " baseline_ms_per_frame=" << baseline_ms
            << " live_ms_per_frame=" << live_ms << " added_ai_ms_per_frame=" << live_ms - baseline_ms
            << " baseline_p50_ms=" << baseline_p50 << " baseline_p95_ms=" << baseline_p95
            << " live_p50_ms=" << live_p50 << " live_p95_ms=" << live_p95
            << " added_ai_p50_ms=" << live_p50 - baseline_p50 << " added_ai_p95_ms=" << live_p95 - baseline_p95
            << " fps=" << 1000.0 / live_ms << std::endl;
        std::size_t builds{}, movements{}, conflicts{};
        const auto snapshot = live->frame().latest;
        const auto owner_of = [&](const galactic::GalacticEvent& event) {
            if (event.kind == galactic::GalacticEventKind::production_started) {
                for (const auto& planet : snapshot->planets()) if (planet.id == event.subject) return planet.owner;
            } else {
                for (const auto& fleet : snapshot->fleets()) if (fleet.id == event.subject) return fleet.owner;
            }
            return galactic::PlayerId{};
        };
        for (const auto& row : live->galactic_events_after(0).events) {
            if (row.event.kind == galactic::GalacticEventKind::conflict_pending) ++conflicts;
            if (ai_players.contains(owner_of(row.event))) {
                if (row.event.kind == galactic::GalacticEventKind::production_started) ++builds;
                if (row.event.kind == galactic::GalacticEventKind::movement_begin) ++movements;
            }
        }
        if (!performance_only) {
            expect(builds > 0, "authored AI seat issues real builds through production startup within N frames");
            expect(movements > 0, "authored AI seat issues free movement through production startup within N frames");
        }
        const auto saved_ai = capture("boundary");
        const auto final_save_tick = live->completed_tick();
        const auto final_save_token = last_save_token;
        expect(initial_ai.components.at(std::string(ga::lifecycle_component)) != saved_ai.components.at(std::string(ga::lifecycle_component)),
            "production galactic service clocks/deadlines advance");
        auto restored = cadence_window ? std::move(midpoint_restore) : must(live->prepare_load("boundary", false));
        const auto boundary = restored->completed_tick();
        const auto end = live->completed_tick() + 12;
        advance(*live, end);
        collect();
        live->stop();
        const auto hashes = live->tick_hashes();
        if (evidence) { evidence->hashes.push_back(hashes); evidence->galaxy_sha.push_back(boundary_galaxy_sha); }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "production live AI agrees at every frame on 1/2/4/8 workers");
        if (workers == 1) reference_events = events;
        else expect(events == reference_events, "production live AI commits identical events on 1/2/4/8 workers");
        auto restored_options = options;
        if (cadence_window) restored_options.workers = 8 / workers;
        auto resumed = must(Live::start(std::move(restored), restored_options));
        std::vector<galactic::GalacticEvent> resumed_events;
        std::uint64_t resumed_cursor{};
        for (auto tick = boundary + 1; tick <= end; ++tick) {
            // Save requests are recorded orders: continue with identical input
            // on both branches, including the control's final proof checkpoint.
            std::optional<std::uint64_t> paired_save;
            if (cadence_window && tick == final_save_tick) {
                eawr::platform::LiveSaveRequest request; request.seat = human; request.name = "boundary"; request.overwrite = true;
                paired_save = resumed->request_save(std::move(request));
                expect(*paired_save == final_save_token, "restored proof uses the same recorded save token");
            }
            advance(*resumed, tick);
            if (paired_save) {
                if (!resumed->wait_save(*paired_save, std::chrono::minutes(5))) throw std::runtime_error("paired continuation save timed out");
                const auto outcome = resumed->terminal_save_outcome(*paired_save);
                if (!outcome || outcome->phase != eawr::platform::LiveSavePhase::completed) throw std::runtime_error("paired continuation save failed");
            }
            const auto rows = resumed->galactic_events_after(resumed_cursor);
            expect(!rows.incomplete, "restored cadence proof drains each continuation event");
            for (const auto& row : rows.events) if (row.event.tick > boundary) resumed_events.push_back(row.event);
            resumed_cursor = rows.through;
        }
        resumed->stop();
        const auto tail = resumed->tick_hashes();
        expect(tail.size() == end - boundary && hashes.size() >= tail.size()
            && std::equal(tail.begin(), tail.end(), hashes.end() - static_cast<std::ptrdiff_t>(tail.size())),
            "production save/load reconstructs fresh AI and preserves uninterrupted continuation");
        if (cadence_window) {
            std::vector<galactic::GalacticEvent> control_events;
            for (const auto& event : events) if (event.tick > boundary) control_events.push_back(event);
            expect(resumed_events == control_events, "midwindow restore on different workers preserves committed orders/events across perception deadlines");
        }
        report << (workers == 1 ? "" : ",") << "{\"workers\":" << workers << ",\"baseline_ms_per_frame\":" << baseline_ms
            << ",\"live_ms_per_frame\":" << live_ms << ",\"added_ai_ms_per_frame\":" << live_ms - baseline_ms
            << ",\"baseline_p50_ms\":" << baseline_p50 << ",\"baseline_p95_ms\":" << baseline_p95
            << ",\"live_p50_ms\":" << live_p50 << ",\"live_p95_ms\":" << live_p95
            << ",\"added_ai_p50_ms\":" << live_p50 - baseline_p50 << ",\"added_ai_p95_ms\":" << live_p95 - baseline_p95
            << ",\"fps\":" << 1000.0 / live_ms
            << ",\"restored_workers\":" << restored_options.workers << ",\"restore_boundary\":" << boundary << ",\"restore_end\":" << end
            << ",\"builds\":" << builds << ",\"movement_requests\":" << movements << ",\"conflicts\":" << conflicts;
        if (profiling) {
            report << ",\"ai_compute_budget\":";
            eawr::gc_profile::write_budget(report, live_phases);
            std::cout << "GC-AI-BUDGET workers=" << workers << ' ';
            eawr::gc_profile::write_budget(std::cout, live_phases);
            std::cout << std::endl;
            report << ",\"profile_frames\":[";
            for (std::size_t index = 0; index < live_phases.size(); ++index) {
                // Cadence sampling skips initial and midpoint checkpoint frames.
                const auto tick = index + 2 + (cadence_window && index >= frames / 2 ? 1 : 0);
                report << (index ? "," : "");
                eawr::gc_profile::write_frame(report, live_phases[index], tick);
                std::cout << "GC-AI-PROFILE-FRAME workers=" << workers << ' ';
                eawr::gc_profile::write_frame(std::cout, live_phases[index], tick);
                std::cout << '\n';
            }
            report << ']';
        }
        report << ",\"hashes\":[";
        for (std::size_t index = 0; index < hashes.size(); ++index)
            report << (index ? "," : "") << '"' << hashes[index] << '"';
        report << "],\"canonical_galaxy_sha_at_boundary\":\"" << boundary_galaxy_sha << "\"}";
        std::cout << "GC-AI-PROFILE-GALAXY workers=" << workers << " sha=" << boundary_galaxy_sha << std::endl;
        report.flush();
    }
    report << "],\"passed\":" << (failures ? "false" : "true") << "}\n";
    return failures ? 1 : 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--live-record-contracts") {
            // Exercise the real composed owners and cross-worker midpoint restore in a short
            // window. Accelerated service periods cross several deadlines; this is not timing evidence.
            ProfileEvidence evidence;
            return live_production(argc > 2 ? argv[2] : "out/galactic-ai-live-record-contracts",
                32, true, true, ga::ServicePeriods{8, 3, 2, 2, 8, 5}, &evidence, true);
        }
        if (argc > 1 && std::string_view(argv[1]) == "--live-profile-comparison") {
            const std::filesystem::path directory = argc > 2 ? argv[2] : "out/galactic-ai-profile";
            const auto frames = argc > 3 ? std::stoull(argv[3]) : 600;
            ProfileEvidence off, on;
            eawr::gc_profile::enabled.store(false, std::memory_order_relaxed);
            live_production(directory / "off", frames, true, true, ga::retail_service_periods, &off);
            eawr::gc_profile::enabled.store(true, std::memory_order_relaxed);
            live_production(directory / "on", frames, true, true, ga::retail_service_periods, &on);
            expect(off.hashes == on.hashes && !off.hashes.empty(), "profile off/on preserves every live tick hash at 1/2/4/8 workers");
            expect(off.galaxy_sha == on.galaxy_sha && !off.galaxy_sha.empty(), "profile off/on preserves canonical galaxy SHA at 1/2/4/8 workers");
            std::cout << "GC-AI-PROFILE-EQUALITY " << (failures ? "FAIL" : "PASS") << std::endl;
            return failures ? 1 : 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--live-cadence") {
            ga::ServicePeriods periods = ga::retail_service_periods;
            if (argc > 4) periods[2] = periods[3] = std::stoull(argv[4]);
            if (argc > 5) periods[1] = periods[5] = std::stoull(argv[5]);
            return live_production(argc > 2 ? argv[2] : "out/galactic-ai-live-cadence", argc > 3 ? std::stoull(argv[3]) : 600,
                true, true, periods);
        }
        if (argc > 1 && (std::string_view(argv[1]) == "--live-production" || std::string_view(argv[1]) == "--live-performance"))
            return live_production(argc > 2 ? argv[2] : "out/galactic-ai-live-production", argc > 3 ? std::stoull(argv[3]) : 1800,
                std::string_view(argv[1]) == "--live-performance");
        if (argc > 1 && std::string_view(argv[1]) == "--freestore-movement") {
            const std::filesystem::path directory = argc > 2 ? argv[2] : "out/galactic-ai-freestore-movement";
            const std::uint64_t frames = argc > 3 ? std::stoull(argv[3]) : 1800;
            std::filesystem::create_directories(directory);
            for (const std::size_t workers : eawr::platform::determinism_worker_counts())
                installed(true, directory / ("w" + std::to_string(workers)), false, frames, workers);
            bool ok = movement_probes.size() == eawr::platform::determinism_worker_counts().size();
            std::ofstream report(directory / "freestore-movement.json", std::ios::trunc);
            report << "{\"frames\":" << frames << ",\"runs\":[";
            for (std::size_t index = 0; index < movement_probes.size(); ++index) {
                const auto& run = movement_probes[index];
                ok = ok && run.failure.empty() && run.requests > 0 && run.hop_frame > 0 && run.refusals == 0
                    && run.final_hash == movement_probes.front().final_hash && run.hashes == movement_probes.front().hashes;
                report << (index ? "," : "") << "{\"workers\":" << run.workers << ",\"frames\":" << run.frames
                    << ",\"freestore_movement_requests\":" << run.requests << ",\"first_request_frame\":" << run.first_request_frame
                    << ",\"hop_frame\":" << run.hop_frame << ",\"unbound_refusals\":" << run.refusals
                    << ",\"final_hash\":\"" << run.final_hash << "\",\"failure\":\"" << run.failure << "\",\"hashes\":[";
                for (std::size_t at = 0; at < run.hashes.size(); ++at)
                    report << (at ? "," : "") << "{\"frame\":" << run.hashes[at].first << ",\"sha256\":\"" << run.hashes[at].second << "\"}";
                report << "]}";
            }
            report << "],\"passed\":" << (ok && !failures ? "true" : "false") << "}\n"; report.close();
            std::cout << "freestore movement evidence " << (ok && !failures ? "PASS" : "FAIL") << ": "
                << (directory / "freestore-movement.json").string() << '\n';
            return ok && !failures ? 0 : 2;
        }
        if (argc > 1) {
            const auto mode = std::string_view(argv[1]);
            return installed(mode == "--release" || mode == "--release-benchmark",
                argc > 2 ? argv[2] : "out/galactic-ai-campaign-gate", mode == "--release-benchmark");
        }
        prepared_restore_contracts();
        authored_selection_contracts();
        worker_verdict_contracts();
        qualified_safety_contracts();
        no_source_transit_contracts();
        nested_actor_facts_contracts();
        actor_parent_depth_contracts();
        sparse_actor_change_contracts();
        free_fleet_signal_contracts();
        real_factory_contracts();
        real_factory_contracts(true);
    } catch (const std::exception& error) {
        if (argc > 1 && (std::string_view(argv[1]) == "--release" || std::string_view(argv[1]) == "--release-benchmark")) {
            const std::filesystem::path directory = argc > 2 ? argv[2] : "out/galactic-ai-campaign-gate";
            std::error_code ignored; std::filesystem::create_directories(directory, ignored);
            std::ofstream refused(directory / "result.json", std::ios::trunc);
            refused << "{\"release_gate_passed\":false,\"run_started\":false,\"failure\":";
            galactic_campaign_gate::json_string(refused, error.what()); refused << "}\n";
        }
        std::cerr << error.what() << '\n'; return 1;
    }
    return failures ? 1 : 0;
}
