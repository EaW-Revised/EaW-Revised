#include "../support/result.hpp"
#include "ai_campaign_stock_private.hpp"
#include "galactic_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/campaign/galactic_ai_runtime.hpp"
#include "galactic_ai_battle_listener.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/checkpoint/game.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galactic_campaign_stock_test {
using namespace galactic_test;
namespace cp = eawr::campaign;
namespace ck = eawr::checkpoint;
namespace auth = eawr::script::authoritative;
namespace ga = eawr::script::foc::galactic_ai;
namespace {
using eawr::testing::result_code_space_value::must;

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
} // namespace

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
void real_factory_contracts(bool configured_cadence) {
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
            pair.parent_queries = [](const auto&) { return cp::GalacticParentQuery{}; };
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
} // namespace galactic_campaign_stock_test
