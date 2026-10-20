#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <vector>
#include <iostream>
#include <cstddef>
#include <string_view>
#include <string>
#include <optional>
#include "../support/env.hpp"
#include "../support/result.hpp"
// WGA-284..299: economy readers consume immutable completed inputs.
#include "galactic_support.hpp"
#include "galactic_ai_world.hpp"
#include "galactic_ai_funding.hpp"
#include "galactic_ai_state.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/core/sha256.hpp"
#include "galactic_ai_selection_content.hpp"
#include "galactic_ai.hpp"
#include "eawr/campaign/ai_perception_content.hpp"
#include "eawr/campaign/ai_selection_content.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/game_session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>
using eawr::testing::must;

namespace {
namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using ai::real;
using eawr::sim::math::Fixed;
Fixed whole(std::int64_t value) { return Fixed::from_integer(value).value(); }

ai::GalacticPerceptionSetup setup() {
    ai::GalacticPerceptionSetup out; out.maximum_ground_level = out.maximum_space_level = real(5);
    out.players = {{1, 1, "Empire", "Normal", false, true, false, 10},
        {2, 2, "Rebel", "Normal", false, false, true, 20}, {3, 3, "Pirates", "Normal", true, false, false, 0}};
    out.planets = {{10, "ALPHA", 0, {}, {}, {}, {}, {}}, {11, "BETA", 0, {}, {}, {}, {}, {}}, {20, "GAMMA", 0, {}, {}, {}, {}, {}}};
    for (const auto id : {100U, 200U, 300U, 400U, 500U, 600U, 700U, 800U, 900U, 1000U}) {
        foc::AiType named; named.type_id = id; named.name = "TYPE" + std::to_string(id); out.types.push_back(named);
        ai::GalacticEconomyType type; type.type = id; type.generic_price = real(60); type.generic_seconds = real(2);
        type.maintenance = ai::parse_real("0.5").value(); type.affiliations = {"EMPIRE"};
        type.space_unit = id == 500 || id == 700; type.ground_unit = id == 800 || id == 900;
        type.ground_structure = id == 100; type.space_structure = id == 200 || id == 1000;
        type.built_on_ground = id == 100 || id == 300 || id == 400 || id == 800 || id == 900;
        if (id == 600) { type.advances_tech = 4; type.tech_level = 3; }
        if (id == 400) { type.initially_locked = true; type.tech_level = 5; }
        if (id == 700) type.maintenance = real(0);
        if (id == 800) { type.maintenance = real(1); type.generic_price = real(120); type.generic_seconds = real(4); }
        ai::GalacticForceType force; force.type = id; force.categories = 1;
        force.power = real(id == 500 ? 10 : id == 700 ? 8 : id == 800 ? 9 : id == 900 ? 11 : id == 200 ? 3 : id == 1000 ? 5 : 0);
        if (type.space_structure) force.space_structure_categories = 4;
        if (id == 500) force.starting_spawns[3] = {{900, 2}};
        out.force_types.push_back(force); out.economy_types.push_back(type);
    }
    out.economy_types_complete = true; out.faction_maintenance = {{1, ai::parse_real("0.25").value()}, {2, real(1)}};
    return out;
}

std::shared_ptr<const gc::GalacticSnapshot> snapshot(std::uint64_t tick,
    std::shared_ptr<const gc::EconomyInputGeneration> generation, bool queued_tech = false, bool cap_block = false, bool own_planets = true,
    const std::function<void(gc::ProductionSetup&)>& adjust_production = {}, bool interpolate_motion = false) {
    gc::GalacticClock clock; clock.session_tick = clock.galactic_frame = tick;
    gc::PlayerEconomySetup accounts; accounts.players = {{1, whole(100), whole(9999), 3, 5}, {2, {}, whole(9999), 1, 5}, {3, {}, whole(9999), 1, 5}};
    gc::ProductionSetup production; production.maximum_human_queue = 1;
    gc::ProductionPlayer builder; builder.player = 1; builder.locked = {500}; builder.unlocked = {400};
    production.players = {builder};
    for (const auto id : {10U, 11U, 20U}) {
        gc::PlanetProduction location; location.planet = id;
        location.target_owner = location.starbase_owner = location.ground_occupier = location.orbit_occupier = id == 20 || !own_planets ? 2 : 1;
        location.ground_level = id == 10 ? 5 : id == 11 ? 6 : 0; location.space_level = id == 10 ? 5 : id == 11 ? 3 : 0;
        location.maximum_space_level = 5; location.ground_slots = location.space_slots = 5;
        location.enabled = location.surface_accessible = !cap_block;
        if (id == 10) {
            location.starbase_type = 1000;
            location.objects = {{70, 100, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure},
                {71, 100, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure},
                {72, 200, 1, {}, gc::ProductionDomain::space, gc::ProductionKind::structure}};
            location.queues[0] = {{300, 1, 80, whole(60), whole(2), whole(1)}, {400, 1, 81, whole(60), whole(2), whole(2)}};
            location.queues[1] = {{200, 1, 82, whole(60), whole(2), whole(1)}};
            for (auto& queue : location.queues) for (auto& entry : queue) { entry.start_frame = 0; entry.duration_frames = 60; }
        } else if (id == 11) location.objects = {{73, 100, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure}};
        for (const auto type : {200U, 300U, 400U, 600U}) {
            gc::ProductionOffer offer; offer.type = type; offer.domain = type == 200 || type == 600 ? gc::ProductionDomain::space : gc::ProductionDomain::ground;
            offer.kind = type == 600 ? gc::ProductionKind::technology : gc::ProductionKind::structure;
            offer.price = whole(60); offer.seconds = whole(2); offer.affiliated = offer.direct_build_allowed = true;
            if (type == 600) { offer.tech = 3; offer.target_tech = 4; }
            location.offers.push_back(offer);
        }
        if (id == 20 && queued_tech) location.queues[1] = {{600, 1, 83, whole(60), whole(2), whole(1)}};
        production.planets.push_back(location);
    }
    if (adjust_production) adjust_production(production);
    gc::Fleet fleet; fleet.id = 50; fleet.owner = 1; fleet.motion.transit.emplace(); fleet.members = {{51, 500}, {52, 700}};
    if (interpolate_motion) fleet.motion.position.x = whole(static_cast<std::int64_t>(tick));
    auto movement = std::make_shared<gc::MovementSetup>();
    gc::MovementType carrier; carrier.type = 700; carrier.transport = true; movement->types = {carrier};
    movement->contents = {{52, {{53, 800}}}, {53, {{54, 900}}}};
    gc::ObserverSetup observer;
    observer.players = {{1, true, false, 0, 0, {}, false}, {2, false, false, 0, 0, {}, false}, {3, false, false, 0, 0, {}, false}};
    for (const auto id : {10U, 11U, 20U}) {
        gc::ObserverPlanet planet; planet.planet = id; planet.allegiance = planet.target_owner = id == 20 || !own_planets ? 2 : 1;
        observer.planets.push_back(planet);
    }
    return std::shared_ptr<const gc::GalacticSnapshot>(new gc::GalacticSnapshot(tick, clock,
        {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}}, {{10, {}, 1}, {11, {}, 1}, {20, {}, 2}}, {}, {fleet}, {}, {}, {}, movement, {}, {}, accounts,
        production, {}, {}, {}, observer, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, 1, std::move(generation)));
}

struct Host {
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    ai::Engine engine;
    eawr::script::authoritative::ScriptScheduler scripts;
    static ai::AiData data() {
        const std::vector<std::pair<std::string, std::string>> expressions{
            {"CONTROL", "Variable_Self.PlanetsControlled"}, {"CONTROLRAW", "Variable_Self.PlanetsControlledUnnormalized"},
            {"ENEMYCONTROL", "Variable_Enemy.PlanetsControlled"}, {"HUMANCONTROL", "Variable_Human.PlanetsControlledUnnormalized"},
            {"MAXG", "Variable_Self.MaxedGroundbases"}, {"MAXS", "Variable_Self.MaxedStarbases"},
            {"NUMG", "Variable_Self.NumGroundbaseOfLevel {Parameter_Level=4.5,Parameter_Count=2}"},
            {"NUMS", "Variable_Self.NumStarbaseOfLevel {Parameter_Level=3}"},
            {"HASG", "Variable_Self.HasGroundbaseOfLevel {Parameter_Level=6}"},
            {"HASS", "Variable_Self.HasStarbaseOfLevel {Parameter_Level=5,Parameter_Count=2}"},
            {"COUNT", "Variable_Self.StructureCount {Parameter_Type=\"TYPE100\",Parameter_Type=\"TYPE200\"}"},
            {"COUNTQ", "Variable_Target.StructureCount {Parameter_Type=\"TYPE200\",Parameter_Type=\"TYPE200\"}"},
            {"COUNTDONE", "Variable_Target.StructureCount {Parameter_Type=\"TYPE200\",Parameter_Only_Consider_Complete=1}"},
            {"COUNTFLAG2", "Variable_Target.StructureCount {Parameter_Type=\"TYPE200\",Parameter_Only_Consider_Complete=2}"},
            {"STRUCT", "Variable_Self.HasStructure {Parameter_Type=\"TYPE100\",Parameter_Count=2}"},
            {"HEAD", "Variable_Target.HasStructure {Parameter_Type=\"TYPE300\"}"},
            {"TAIL", "Variable_Target.HasStructure {Parameter_Type=\"TYPE400\"}"},
            {"DONE", "Variable_Target.HasStructure {Parameter_Type=\"TYPE300\",Parameter_Only_Consider_Complete=1}"},
            {"UNIT", "Variable_Self.HasUnit {Parameter_Type=\"TYPE900\",Parameter_Only_Consider_Complete=1}"},
            {"UNITFRACTION", "Variable_Self.HasUnit {Parameter_Type=\"TYPE900\",Parameter_Count=1.9,Parameter_Only_Consider_Complete=1}"},
            {"UNITQ", "Variable_Self.HasUnit {Parameter_Type=\"TYPE300\"}"},
            {"UNITDONE", "Variable_Self.HasUnit {Parameter_Type=\"TYPE300\",Parameter_Only_Consider_Complete=2}"},
            {"EMPTYCOUNT", "Variable_Self.HasUnit {Parameter_Type=\"MISSING\",Parameter_Count=0}"},
            {"TECH", "Variable_Self.HasTechToProduce {Parameter_Type=\"TYPE500\"}"},
            {"UNLOCK", "Variable_Self.HasTechToProduce {Parameter_Type=\"TYPE400\"}"},
            {"ADVANCE", "Variable_Self.CanAdvanceTech"}, {"MAINT", "Variable_Self.Maintenance"},
            {"RAWMAINT", "Variable_Self.MaintenanceUnnormalized"}, {"GROSS", "Variable_Self.GrossIncomeUnnormalized"},
            {"NET", "Variable_Self.NetIncomeUnnormalized"}, {"SPACE", "Variable_Self.SpaceTotalUnnormalized"},
            {"GROUND", "Variable_Self.GroundTotalUnnormalized"},
            {"MASK", "Variable_Self.SpaceTotalUnnormalized {Parameter_Category=4}"},
            {"BUDGET", "Variable_Self.BudgetFractionToBuild {Parameter_Type=\"TYPE500\"}"},
            {"CATEGORY", "Variable_Self.BudgetFractionToBuild {Parameter_Type=\"TYPE500\",Parameter_Category=7}"},
            {"ACTIVE", "Variable_Self.BudgetFractionToBuild {Parameter_Type=\"TYPE300\"}"},
            {"TIMELINE", "Game.Timeline"}, {"NESTEDTIMELINE", "Function_TIMELINE.Evaluate"}, {"TIMELINEBASES", "Function_TIMELINE.Evaluate + Function_MAXG.Evaluate"}, {"FOREIGN", "Variable_Enemy.StructureCount {Parameter_Type=\"TYPE100\"}"},
            {"UNKNOWN", "Variable_Target.StructureCount"}};
        std::string xml = "<Equations>";
        for (const auto& [name, value] : expressions) xml += "<" + name + ">" + value + "</" + name + ">";
        xml += "</Equations>";
        ai::AiData out; out.equations = must(ai::EquationSet::parse({{"fixture.xml", xml}}, {}));
        out.players["QUERY"] = ai::PlayerType{}; return out;
    }
    Host(ai::GalacticPerceptionSetup input = setup()) : world(std::make_shared<ai::GalacticWorld>(std::move(input))), engine(host, data(), {}),
        scripts(must(eawr::script::authoritative::ScriptScheduler::create({}, {}))) {
        host->ai_world = world; host->engine = &engine;
        for (const auto& row : setup().players) host->setup.players.push_back({row.player, row.faction, row.neutral, row.ai, "QUERY", row.human});
        for (const auto& row : host->setup.players) host->players.emplace(row.player, &row);
    }
    std::optional<ai::Real> query(std::string_view name) { return world->evaluate(engine, name, 1, engine.target_of_object(10)); }
};

// WGEP-149: identity selection precedes admission and never falls through a tie.
std::string next_technology(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto input = setup();
    auto& first = input.economy_types[4];
    first.advances_tech = 4; first.tech_level = 99; first.initially_locked = true;
    first.generic_price = {}; first.generic_seconds = {};
    const auto generation = std::make_shared<gc::EconomyInputGeneration>();
    std::string trace;
    const auto check = [&](const ai::GalacticPerceptionSetup& content,
        std::shared_ptr<const gc::GalacticSnapshot> view, ai::Real expected, std::string_view message) {
        Host host(content); must(host.world->bind(view, {}, executor, 0, 200));
        std::uint64_t sequence{}; must(host.engine.before_service(*host.world, host.scripts, sequence, &executor));
        expect(host.query("ADVANCE") == expected, std::string(message));
        Host restored(content); must(restored.world->bind(view, {}, executor, 0, 200));
        sequence = 0; must(restored.engine.before_service(*restored.world, restored.scripts, sequence, &executor));
        must(restored.world->restore(host.world->state()));
        expect(restored.query("ADVANCE") == expected, "restored first-match identity and target heads");
        trace += std::to_string(expected.repr) + ":" + must(host.world->state_hash()) + ";";
    };
    check(input, snapshot(0, generation), real(0), "unbuildable first match prevents usable later match");
    check(input, snapshot(0, generation, true, true), real(0), "queued later match cannot replace selected identity");
    check(input, snapshot(0, generation, false, true, true, [](auto& production) {
        auto& target = production.planets.back();
        // Retained heads still require bound offer metadata for population
        // reservations, even when locks and ownership prevent a new purchase.
        auto offer = target.offers.back(); offer.type = 500; offer.domain = gc::ProductionDomain::ground;
        target.offers.insert(std::lower_bound(target.offers.begin(), target.offers.end(), offer.type,
            [](const auto& row, auto type) { return row.type < type; }), offer);
        target.queues[0] = {{500, 1, 83, whole(60), whole(2), whole(1)}};
    }), real(1), "exact first match at ground head wins despite lock, price and target eligibility");
    std::swap(input.economy_types[4], input.economy_types[5]);
    check(input, snapshot(0, generation), real(1), "registration order beats ascending bound identity and ambiguity refusal");
    check(input, snapshot(0, generation, true, true), real(1), "exact space head on retained foreign target precedes admission");
    check(input, snapshot(0, generation, true, true, true, [](auto& production) {
        production.planets.back().queues[1].front().builder = 2;
    }), real(0), "another builder's head does not qualify");
    check(input, snapshot(0, generation, true, true, true, [](auto& production) {
        production.planets.back().queues[1].insert(production.planets.back().queues[1].begin(),
            {200, 1, 84, whole(60), whole(2), whole(1)});
    }), real(0), "exact identity in queue tail does not qualify");
    input.economy_types[4].affiliations = {"REBEL"};
    check(input, snapshot(0, generation), real(0), "faction filters identity before choosing the first match");
    input.economy_types[5].advances_tech = 5;
    check(input, snapshot(0, generation), real(0), "no exact next-level match is numeric zero");
    return trace;
}

std::string contracts(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers); Host host;
    auto generation = std::make_shared<gc::EconomyInputGeneration>(); generation->income = {{10, whole(40)}, {11, whole(60)}, {20, whole(900)}};
    must(host.world->bind(snapshot(0, generation), {}, executor, 0, 200));
    std::uint64_t sequence{}; must(host.engine.before_service(*host.world, host.scripts, sequence, &executor));
    const auto frozen_hash = must(host.world->state_hash());
    expect(host.world->evaluate_with_timeline(host.engine, "NESTEDTIMELINE", 1, host.engine.target_of_object(10), 4) == real(4)
        && must(host.world->state_hash()) == frozen_hash, "nested copied child timeline overrides parent without publishing state");
    host.world->bind_timeline_reader([] { return eawr::core::Result<int>::success(3); });
    expect(host.query("NESTEDTIMELINE") == real(3), "live mode reader propagates through nested equation");
    host.world->bind_timeline_reader({});
    expect(host.world->evaluate_read_only(host.engine, "MAXG", 1, host.engine.target_of_object(10)) == ai::parse_real("0.5").value()
        && host.world->evaluate_read_only(host.engine, "COUNT", 1, host.engine.target_of_object(10)) == real(5)
        && must(host.world->state_hash()) == frozen_hash, "read-only nested economy reads preserve parent native state and revision");
    const auto check = [&](std::string_view name, ai::Real expected) { expect(host.query(name) == expected, std::string(name)); };
    check("CONTROL", real(2) / real(3)); check("CONTROLRAW", real(2)); check("ENEMYCONTROL", real(1) / real(3)); check("HUMANCONTROL", real(1));
    check("MAXG", ai::parse_real("0.5").value()); check("MAXS", ai::parse_real("0.5").value());
    check("NUMG", real(1)); check("NUMS", real(2)); check("HASG", real(1)); check("HASS", real(0));
    check("COUNT", real(5)); check("COUNTQ", real(3)); check("COUNTDONE", real(1)); check("STRUCT", real(1));
    check("COUNTFLAG2", real(2)); check("UNITFRACTION", real(1));
    check("HEAD", real(1)); check("TAIL", real(0)); check("DONE", real(0));
    check("UNIT", real(1)); check("UNITQ", real(1)); check("UNITDONE", real(0)); check("EMPTYCOUNT", real(1));
    check("TECH", real(1)); check("UNLOCK", real(1)); check("ADVANCE", real(1));
    const auto maintenance = ai::parse_real("0.375").value();
    check("RAWMAINT", maintenance); check("GROSS", real(100)); check("MAINT", maintenance / real(100)); check("NET", real(100) - maintenance);
    check("SPACE", real(48)); check("GROUND", real(42)); check("MASK", real(8));
    expect(!host.query("BUDGET") && !host.query("TIMELINE") && !host.query("FOREIGN") && !host.query("UNKNOWN"), "named missing-input refusals");
    auto runtime = std::make_shared<ai::GalacticEconomyRuntime>(*ai::fresh_economy_runtime(setup().players));
    must(host.world->bind_economy_runtime(runtime)); check("TIMELINE", real(0)); check("BUDGET", real(60) / real(100)); check("ACTIVE", real(0));
    runtime = std::make_shared<ai::GalacticEconomyRuntime>(*runtime); runtime->budgets[0].income = real(10);
    runtime->budgets[0].categories[7] = {real(5), real(20)};
    runtime->builds = {{10, 300, ai::GalacticBudgetBuild::Status::goal, real(40), real(5), real(1), 1, gc::ProductionDomain::ground, 80}};
    must(host.world->bind_economy_runtime(runtime)); check("BUDGET", real(60) / real(120)); check("CATEGORY", real(2)); check("ACTIVE", real(25) / real(10));
    auto wrong_builder = std::make_shared<ai::GalacticEconomyRuntime>(*runtime); wrong_builder->builds.front().builder = 2;
    must(host.world->bind_economy_runtime(wrong_builder)); check("ACTIVE", real(0));
    auto wrong_build = std::make_shared<ai::GalacticEconomyRuntime>(*runtime); wrong_build->builds.front().build = 81;
    must(host.world->bind_economy_runtime(wrong_build)); check("ACTIVE", real(0));
    must(host.world->bind_economy_runtime(runtime));
    std::uint32_t supplier_calls{};
    host.world->bind_economy_supplier([&]() {
        ++supplier_calls;
        return eawr::core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>>::success(runtime);
    });
    check("BUDGET", real(60) / real(120));
    expect(host.world->evaluate_read_only(host.engine, "CATEGORY", 1, host.engine.target_of_object(10)) == real(2)
        && supplier_calls == 2, "ordinary and read-only refresh copied funding outside cache lock");
    host.world->bind_economy_supplier({});
    const auto state = host.world->state();
    Host restored; must(restored.world->bind(snapshot(0, generation), {}, executor, 0, 200));
    sequence = 0; must(restored.engine.before_service(*restored.world, restored.scripts, sequence, &executor));
    must(restored.world->restore(state)); must(restored.world->bind_economy_runtime(runtime));
    expect(restored.query("ACTIVE") == host.query("ACTIVE") && restored.query("MAXG") == host.query("MAXG"), "restored observer context and runtime copy agree");
    const auto previous_input = host.world->copy_input();
    const auto previous_native = must(host.world->capture());
    must(host.world->bind(snapshot(1, generation), {}, executor, 1, 200));
    expect(host.world->work().economy_rebuilds == 0 && host.world->work().economy_objects == 0, "quiet completed tick reuses census without sweep");
    expect(host.world->work().prepared_planets == 0 && host.world->work().prepared_objects == 0
        && host.world->work().producer_offers == 0,
        "qualified clock-only bind reuses physical rows and producer admission");
    expect(host.world->completed_tick() == 1 && host.world->view()->tick == 1
        && host.world->copy_input()->clocks.synchronization == 1
        && previous_input->clocks.synchronization == 0,
        "reused physical publication advances clocks without changing retained inputs");
    expect(must(host.world->capture()).components == previous_native.components,
        "clock-only binding leaves canonical perception state unchanged");
    auto other_publisher = std::make_shared<gc::EconomyInputGeneration>(*generation);
    must(host.world->bind(snapshot(1, other_publisher), {}, executor, 1, 200));
    expect(host.world->work().prepared_objects != 0,
        "equal generation numbers from a different publisher rebuild physical rows");
    expect(!host.query("BUDGET") && !host.query("TIMELINE"), "stale runtime publication refuses");
    auto changed = std::make_shared<gc::EconomyInputGeneration>(*generation); ++changed->census;
    must(host.world->bind(snapshot(2, changed, false, true), {}, executor, 2, 200)); check("ADVANCE", real(0));
    expect(host.world->work().economy_rebuilds == 1, "dirty census rebuilds once");
    changed = std::make_shared<gc::EconomyInputGeneration>(*changed); ++changed->census;
    must(host.world->bind(snapshot(3, changed, true, true), {}, executor, 3, 200)); check("ADVANCE", real(1));
    changed = std::make_shared<gc::EconomyInputGeneration>(*changed); ++changed->census;
    must(host.world->bind(snapshot(4, changed, false, false, false), {}, executor, 4, 200));
    check("CONTROL", real(0)); check("CONTROLRAW", real(0)); check("ENEMYCONTROL", real(1));
    check("MAXG", real(0)); check("MAXS", real(0)); check("GROSS", real(0)); check("MAINT", real(0));
    ai::GalacticFunding funding; must(funding.initialize(1, {}, whole(100)));
    expect(funding.state().at(1).last_service_income == Fixed{} && funding.state().timeline == 0, "fresh native retained zeros");
    const auto revision = funding.state_revision(); must(funding.set_timeline(3));
    must(funding.notify(gc::GalacticSession::CreditNotification{gc::GalacticSession::CreditNotification::Kind::fiscal, 1, whole(100), whole(40), whole(2)}));
    expect(funding.state_revision() > revision && funding.state().at(1).last_service_income == whole(20), "every native retained write advances revision");
    const auto bytes = must(foc::galactic_ai::encode_funding_state(funding.state()));
    ai::GalacticFunding loaded; must(loaded.restore(must(foc::galactic_ai::decode_funding_state(bytes))));
    expect(loaded.state() == funding.state() && must(foc::galactic_ai::encode_funding_state(loaded.state())) == bytes, "funding v4 exact saved rate and timeline");
    auto invalid = loaded.state(); invalid.timeline = 6; expect(!loaded.restore(invalid), "out of range retained timeline rejected atomically");
    ai::GalacticFunding empty; must(empty.set_timeline(2));
    const auto empty_bytes = must(foc::galactic_ai::encode_funding_state(empty.state()));
    const auto empty_saved = must(foc::galactic_ai::decode_funding_state(empty_bytes));
    expect(empty_saved.empty() && empty_saved.timeline == 2 && empty_saved.constructor_income == Fixed{}, "empty pre-manager save retains explicit scalar and constructor rate");
    auto old_version = bytes;
    // Funding domain is length-prefixed, followed by its explicit four-byte version.
    const auto version_at = std::size_t{4} + std::string_view("eawr-galactic-ai-funding").size(); old_version[version_at] = 2;
    expect(!foc::galactic_ai::decode_funding_state(old_version), "old funding format has no compatibility reader");
    return std::to_string(host.query("SPACE")->repr) + ":" + std::to_string(host.query("RAWMAINT")->repr)
        + ":" + std::to_string(host.query("ADVANCE")->repr) + ":" + eawr::core::sha256_hex(bytes);
}
void generations(unsigned workers) {
    auto galaxy = must(gc::GalacticSession::create(gc::synthetic_galaxy({42, 4, 2, 1, 1, true, false})));
    eawr::platform::ThreadWorkerAdapter executor(workers); must(galaxy.step(executor)); // consume genuine initial dirty publication
    const auto first = galaxy.snapshot()->economy_input_generation();
    must(galaxy.step(executor));
    expect(galaxy.snapshot()->economy_input_generation() == first, "session quiet tick retains derived generation");
    const auto census = first->census;
    const auto command = galactic_test::owner(galaxy.completed_tick(), {1}, 1, galaxy.setup().planets.front().id, 2);
    must(galaxy.submit(command)); must(galaxy.step(executor));
    expect(galaxy.snapshot()->economy_input_generation() != first, "real owner write changes derived generation");
    expect(first->census == census, "old snapshot generation remains immutable");
    Host live;
    live.world->enable_live_deltas(300, std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers));
    auto generation = std::make_shared<gc::EconomyInputGeneration>();
    must(live.world->bind(snapshot(1, generation, false, false, true, {}, true), {}, executor, 1, 200));
    const auto retained_view = live.world->view();
    const auto retained_input = live.world->copy_input();
    const auto retained_native = must(live.world->capture());
    auto changed = std::make_shared<gc::EconomyInputGeneration>(*generation); ++changed->census; ++changed->accounts;
    const auto current = snapshot(2, changed, false, false, true, {}, true);
    must(live.world->bind(current, {}, executor, 2, 200));
    Host atomic; must(atomic.world->bind(current, {}, executor, 2, 200));
    expect(live.world->work().prepared_objects == 0 && live.world->work().prepared_planets == 0,
        "motion-only generation publishes live deltas without rebuilding perception rows");
    for (const auto id : {50U, 51U, 52U, 53U, 54U}) {
        const auto* actual = live.world->view()->find(id); const auto* expected = atomic.world->view()->find(id);
        expect(actual && expected && actual->position == expected->position && actual->position.x == whole(2),
            "fleet, members and nested payload motion match atomic current-world view");
        expect(retained_view->find(id)->position.x == whole(1), "prior motion publication remains immutable");
    }
    expect(retained_input->clocks.synchronization == 1 && live.world->copy_input()->clocks.synchronization == 2
        && must(live.world->capture()).components == retained_native.components,
        "motion deltas advance clocks without altering saved perception caches");
    must(live.world->bind(current, {}, executor, 2, 200, true));
    expect(live.world->work().prepared_objects != 0, "an inclusive perception deadline admits the atomic row refresh");
}
std::string game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("installed economy contracts require game root");
    return std::move(*root);
}
std::string read(const eawr::vfs::Vfs& filesystem, std::string_view path) {
    const auto bytes = must(filesystem.open(path)); std::string text(bytes.size(), '\0');
    if (!text.empty()) { std::memcpy(text.data(), bytes.data(), text.size()); }
    return text;
}
// #2971: a partitioned phase that queries perception (the usability phase, a
// partitioned Lua service) opens partitioned_reads() first. The owner refreshes
// the copied economy input once; no partition calls the supplier, which reads
// owner caches that are not thread-safe.
std::string partitioned_supplier(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers); Host host;
    auto generation = std::make_shared<gc::EconomyInputGeneration>(); generation->income = {{10, whole(40)}, {11, whole(60)}, {20, whole(900)}};
    must(host.world->bind(snapshot(0, generation), {}, executor, 0, 200));
    std::uint64_t sequence{}; must(host.engine.before_service(*host.world, host.scripts, sequence, &executor));
    auto runtime = std::make_shared<ai::GalacticEconomyRuntime>(*ai::fresh_economy_runtime(setup().players));
    runtime->budgets[0].income = real(10); runtime->budgets[0].categories[7] = {real(5), real(20)};
    runtime->builds = {{10, 300, ai::GalacticBudgetBuild::Status::goal, real(40), real(5), real(1), 1, gc::ProductionDomain::ground, 80}};
    const auto owner = std::this_thread::get_id();
    std::atomic<std::uint32_t> calls{}, foreign{};
    host.world->bind_economy_supplier([&] {
        ++calls; if (std::this_thread::get_id() != owner) ++foreign;
        // A fresh publication per call, as the stock selector fixture's supplier makes.
        return eawr::core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>>::success(
            std::make_shared<const ai::GalacticEconomyRuntime>(*runtime));
    });
    std::vector<std::optional<ai::Real>> values(eawr::sim::tick_partition_count);
    {
        const auto reads = host.world->partitioned_reads();
        must(executor.execute_phase("test-partitioned-perception", values.size(), [&](std::size_t partition) {
            values[partition] = host.world->evaluate(host.engine, "CATEGORY", 1, host.engine.target_of_object(10));
        }));
    }
    expect(calls == 1 && foreign == 0, "the owner refreshes the copied economy input once; no partition calls the supplier");
    expect(std::all_of(values.begin(), values.end(), [](const auto& value) { return value == real(2); }),
        "every partition reads the copied category budget");
    expect(host.query("CATEGORY") == real(2) && calls == 2, "a closed scope restores per-query refresh");
    host.world->bind_economy_supplier({});
    return std::to_string(values.front().value_or(ai::Real{}).repr) + ":" + must(host.world->state_hash());
}
// #2971: a fan-out that queries perception without partitioned_reads() (battle
// child scripts querying their galactic parent) still never runs the supplier
// on two threads at once.
std::string unguarded_supplier(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers); Host host;
    auto generation = std::make_shared<gc::EconomyInputGeneration>(); generation->income = {{10, whole(40)}, {11, whole(60)}, {20, whole(900)}};
    must(host.world->bind(snapshot(0, generation), {}, executor, 0, 200));
    std::uint64_t sequence{}; must(host.engine.before_service(*host.world, host.scripts, sequence, &executor));
    auto runtime = std::make_shared<ai::GalacticEconomyRuntime>(*ai::fresh_economy_runtime(setup().players));
    runtime->budgets[0].income = real(10); runtime->budgets[0].categories[7] = {real(5), real(20)};
    runtime->builds = {{10, 300, ai::GalacticBudgetBuild::Status::goal, real(40), real(5), real(1), 1, gc::ProductionDomain::ground, 80}};
    std::atomic<std::uint32_t> inside{}, overlaps{}, calls{};
    host.world->bind_economy_supplier([&] {
        ++calls;
        if (inside.fetch_add(1) != 0) ++overlaps;
        // Hold the call open long enough for an unserialised second worker to enter.
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(500);
        while (overlaps.load() == 0 && std::chrono::steady_clock::now() < until) std::this_thread::yield();
        inside.fetch_sub(1);
        return eawr::core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>>::success(
            std::make_shared<const ai::GalacticEconomyRuntime>(*runtime));
    });
    std::vector<std::optional<ai::Real>> values(eawr::sim::tick_partition_count);
    must(executor.execute_phase("test-unguarded-perception", values.size(), [&](std::size_t partition) {
        values[partition] = host.world->evaluate(host.engine, "CATEGORY", 1, host.engine.target_of_object(10));
    }));
    host.world->bind_economy_supplier({});
    expect(overlaps == 0 && calls == values.size(), "unguarded partitioned queries take turns in the supplier");
    expect(std::all_of(values.begin(), values.end(), [](const auto& value) { return value == real(2); }),
        "unguarded partitioned queries read the supplied category budget");
    return std::to_string(values.front().value_or(ai::Real{}).repr) + ":" + must(host.world->state_hash());
}
std::string aggregate_restore_observations(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto inputs = setup(); inputs.cache_intervals = {{"TARGETPLAYERID", real(0)}};
    const auto generation = std::make_shared<gc::EconomyInputGeneration>();
    Host continuous(inputs);
    must(continuous.world->bind(snapshot(1, generation), {}, executor, 1, 200));
    expect(continuous.query("CONTROLRAW") == real(2), "ordinary aggregate reads the owned planet count");
    const auto saved = must(continuous.world->capture());
    expect(continuous.query("CONTROLRAW") == real(2) && must(continuous.world->capture()).components == saved.components,
        "aggregate memo reuses one frame's already published observations");
    const auto current = snapshot(2, generation);
    must(continuous.world->bind(current, {}, executor, 2, 200));
    Host restored(inputs);
    must(restored.world->bind(current, {}, executor, 2, 200));
    must(restored.world->restore(saved));
    expect(continuous.query("CONTROLRAW") == restored.query("CONTROLRAW"), "warm and restored aggregate values agree");
    const auto expected = must(continuous.world->capture());
    expect(expected.components == must(restored.world->capture()).components,
        "warm aggregate and cold restore publish identical next-frame native observations");
    const auto state = continuous.world->state();
    expect(state.cache.size() == 3 && std::all_of(state.cache.begin(), state.cache.end(), [](const auto& row) {
        return row.key.tokens == std::vector<std::string>{"VARIABLE_TARGET", "TARGETPLAYERID"} && row.frame == 2;
    }), "zero-interval ownership reads publish this frame even on unchanged physical input");
    expect(std::all_of(state.visibility.begin(), state.visibility.end(), [](const auto& row) { return row.last_observed == 2; }),
        "aggregate continuation retains the same visibility observation ages after restore");
    expect(continuous.world->work().economy_rebuilds == 0, "per-frame observations do not rebuild the quiet economy projection");
    expect(continuous.query("CONTROLRAW") == real(2) && must(continuous.world->capture()).components == expected.components,
        "same-frame reuse leaves native observation bytes unchanged");
    return must(continuous.world->state_hash());
}
void cached_bases(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto inputs = setup(); inputs.cache_intervals = {{"GROUNDBASELEVEL", real(1)}, {"MAXGROUNDBASELEVEL", real(-1)}};
    Host host(std::move(inputs)); auto generation = std::make_shared<gc::EconomyInputGeneration>();
    must(host.world->bind(snapshot(1, generation), {}, executor, 1, 200));
    std::uint64_t sequence{}; must(host.engine.before_service(*host.world, host.scripts, sequence, &executor));
    auto retained = host.world->state();
    retained.cache.push_back({{1, 2, 2, 10, {"VARIABLE_TARGET", "GROUNDBASELEVEL"}, {}}, real(0), real(1), 1});
    must(host.world->restore(std::move(retained)));
    expect(host.query("MAXG") == real(0), "maxed aggregate consumes retained target level rather than raw census");
    must(host.world->bind(snapshot(31, generation), {}, executor, 31, 200));
    expect(host.query("MAXG") == real(0), "target cache equality at expiry remains retained through aggregate memo");
    const auto child_timeline = std::make_shared<const eawr::campaign::ModeTimelineSnapshot>(
        eawr::campaign::ModeTimelineSnapshot{{eawr::campaign::SessionMode::battle, 900}, 31, 4});
    const ai::GalacticParentContext child_context{&host.engine, host.world.get(), 10, true, child_timeline, {}};
    const ai::GalacticParentContext value_context{&host.engine, host.world.get(), 10, true, {}, 4};
    const auto positive_bytes = must(host.world->capture());
    expect(ai::evaluate_in_galactic_context(child_context, "TIMELINEBASES", 1) == real(4)
        && ai::evaluate_in_galactic_context(value_context, "TIMELINEBASES", 1) == real(4)
        && must(host.world->capture()).components == positive_bytes.components,
        "positive cache equality under nested child override keeps exact parent bytes");
    must(host.world->bind(snapshot(32, generation), {}, executor, 32, 200));
    const auto parent = must(host.world->state_hash());
    const auto expired_bytes = must(host.world->capture());
    expect(ai::evaluate_in_galactic_context(child_context, "TIMELINEBASES", 1) == ai::parse_real("4.5").value()
        && ai::evaluate_in_galactic_context(value_context, "TIMELINEBASES", 1) == ai::parse_real("4.5").value()
        && must(host.world->capture()).components == expired_bytes.components,
        "expired cache computes under nested child override without publishing parent bytes");
    expect(host.world->evaluate_read_only(host.engine, "MAXG", 1, host.engine.target_of_object(10)) == ai::parse_real("0.5").value()
        && must(host.world->state_hash()) == parent, "expired nested base read computes without parent publication");
    expect(host.query("MAXG") == ai::parse_real("0.5").value(), "positive visible target deadline expires aggregate memo on quiet input");
    expect(host.world->work().economy_rebuilds == 0, "cache deadline refresh never rebuilds quiet census");
}

void installed() {
    namespace cp = eawr::campaign;
    namespace ga = foc::galactic_ai;
    const auto root = std::filesystem::path(game_root()); std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    const auto filesystem = must(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = must(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(filesystem, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1); options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == "Rebel"; if (human) options.local_observer = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> names; for (const auto& type : types.types) names.push_back(type.name); std::sort(names.begin(), names.end());
    for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    const auto planned = must(cp::plan_live_campaign_setup(filesystem, data, types, "Sandbox_Equal_Footing_Empire", options, "Normal_Default"));
    const auto services = must(cp::prepare_live_campaign_services(filesystem, loaded.catalog, data, types, planned, "Normal_Default"));
    const auto content = must(cp::prepare_ai_selection_content(filesystem, loaded.catalog, data, types, planned));
    const auto prepared = must(cp::prepare_ai_perception_content(filesystem, data, planned, *content, "Normal"));
    std::vector<gc::TypeId> registered;
    for (const auto& type : eawr::units::galactic_type_table(loaded.catalog, true).types) {
        const auto bound = std::find_if(content->types.begin(), content->types.end(), [&](const auto& row) {
            return row.second.name == ai::upper_case(type.name);
        });
        if (bound != content->types.end()) registered.push_back(bound->first);
    }
    expect(content->registered_types == registered && registered.size() == content->types.size(),
        "installed catalog winner order survives bound type lookup");
    std::vector<gc::TypeId> category_order;
    for (const auto& type : content->ai.types) category_order.push_back(type.type_id);
    expect(category_order.size() > 2 && !std::is_sorted(registered.begin(), registered.end()),
        "PL-20: the installed fixture distinguishes registration order from binding-id order");
    expect(category_order == registered,
        "PL-20: prepare_ai_selection_content enumerates category candidates in type-table order");
    std::vector<gc::TypeId> perception_order;
    for (const auto& type : prepared->economy_types) perception_order.push_back(type.type);
    expect(perception_order == registered, "installed perception carries complete registration order");
    ga::LifecycleSetup lifecycle; for (const auto& path : ga::required_xml()) lifecycle.xml.emplace(path, read(filesystem, path));
    const auto ai_data = must(ga::load_data(lifecycle));
    gc::PlayerId empire{}; for (const auto& player : prepared->players) if (ai::upper_case(player.faction) == "EMPIRE") empire = player.player;
    expect(empire != 0, "installed Empire player present"); std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto campaign = must(cp::CampaignRoot::create(planned, services)); auto& galaxy = campaign.game()->galaxy();
        auto host = std::make_shared<foc::detail::Host>(); host->setup.seed = prepared->seed; host->setup.content = content->ai;
        for (const auto& type : host->setup.content.types) { host->types.emplace(type.type_id, &type); host->types_by_name.emplace(ai::upper_case(type.name), &type); }
        for (const auto& player : prepared->players) host->setup.players.push_back({player.player, player.faction, player.neutral, player.ai, player.ai ? "BASICEMPIRE" : "", player.human});
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        auto world = std::make_shared<ai::GalacticWorld>(*prepared); host->ai_world = world;
        ai::Engine engine(host, ai_data, {}); host->engine = &engine;
        auto scripts = must(eawr::script::authoritative::ScriptScheduler::create({}, {})); std::uint64_t sequence{};
        const auto view = galaxy.snapshot(); must(world->bind(view, {}, executor, view->completed_tick(), galaxy.next_entity_id()));
        must(world->bind_economy_runtime(ai::fresh_economy_runtime(prepared->players)));
        const std::array selected{ga::PlayerSelection{empire, "BASICEMPIRE", "NORMAL", {}, {}}};
        must(engine.begin_galactic_barrier(*world, {view->completed_tick(), view->clock().galactic_frame}, scripts, sequence, selected, &executor));
        const auto& player_type = ai_data.players.at("BASICEMPIRE");
        const auto policy = ga::apply_template(ai_data, {}, {}, ai_data.templates.at(player_type.galactic_templates.front()));
        must(engine.apply_galactic_policy(empire, policy));
        ai::EvaluatedFundingTemplate evaluated;
        std::string trace;
        for (const auto& [category, setting] : policy.equations) {
            const auto value = engine.evaluate(setting.value, empire, nullptr);
            if (!value) throw std::runtime_error("stock Normal budget unavailable: " + setting.value);
            evaluated.equations.push_back({ai_data.goal_categories.at(category), must(eawr::script::numeric::to_fixed(*value))});
            trace += setting.value + ":" + std::to_string(value->repr) + ";";
        }
        expect(engine.evaluate("BasicEmpireInfrastructureBudgetAllocation", empire, nullptr).has_value(), "actual Normal infrastructure budget closure available");
        ai::GalacticFunding funding;
        for (const auto& account : view->player_economy()->players) if (account.player == empire) must(funding.initialize(empire, std::array{evaluated}, account.credits));
        expect(!funding.state().at(empire).categories.empty(), "actual Normal equations initialize real retained categories");
        const auto probe = [&](std::string expression, const ai::Target* target = nullptr) {
            const auto parsed = must(ai::EquationSet::parse({{"probe.xml", "<Equations><ECONPROBE>" + expression + "</ECONPROBE></Equations>"}}, {}));
            ai::PlayerAi player; player.player = empire; const auto context = world->goal_context(player, target); ai::AiRandom random;
            const auto value = world->perception(engine, *parsed.find("ECONPROBE"), context, random);
            if (!value) throw std::runtime_error("stock token unavailable: " + expression);
            trace += expression + ":" + std::to_string(value->repr) + ";"; return *value;
        };
        for (const auto token : {"PlanetsControlled", "PlanetsControlledUnnormalized", "MaxedStarbases", "MaxedGroundbases", "NumStarbaseOfLevel", "NumGroundbaseOfLevel", "HasStarbaseOfLevel", "HasGroundbaseOfLevel",
            "Maintenance", "MaintenanceUnnormalized", "GrossIncomeUnnormalized", "NetIncomeUnnormalized", "CanAdvanceTech", "SpaceTotalUnnormalized", "GroundTotalUnnormalized"})
            static_cast<void>(probe("Variable_Self." + std::string(token)));
        const auto type = std::find_if(prepared->economy_types.begin(), prepared->economy_types.end(), [&](const auto& row) {
            return std::any_of(row.affiliations.begin(), row.affiliations.end(), [](const auto& faction) { return ai::upper_case(faction) == "EMPIRE"; }) && row.generic_price > real(0) && row.generic_seconds > real(0);
        });
        expect(type != prepared->economy_types.end(), "authored Empire production type for parameter probes");
        if (type == prepared->economy_types.end()) throw std::runtime_error("no stock economy production type");
        const auto named = std::find_if(prepared->types.begin(), prepared->types.end(), [&](const auto& row) { return row.type_id == type->type; });
        if (named == prepared->types.end()) throw std::runtime_error("stock type name unavailable");
        const auto parameter = " {Parameter_Type=\"" + named->name + "\"}";
        for (const auto token : {"HasUnit", "HasTechToProduce", "StructureCount", "HasStructure", "BudgetFractionToBuild"}) static_cast<void>(probe("Variable_Self." + std::string(token) + parameter));
        const auto* target = engine.target_of_object(view->planets().front().id);
        for (const auto token : {"StructureCount", "HasStructure"}) static_cast<void>(probe("Variable_Target." + std::string(token) + parameter, target));
        expect(probe("Game.Timeline") == real(0), "authentic fresh retained timeline zero");
        const auto bytes = must(ga::encode_funding_state(funding.state())); ai::GalacticFunding restored; must(restored.restore(must(ga::decode_funding_state(bytes))));
        expect(restored.state() == funding.state(), "stock retained budgets survive native codec exactly");
        trace += eawr::core::sha256_hex(bytes) + galaxy.state_sha256();
        if (baseline.empty()) baseline = trace; else expect(trace == baseline, "stock economy and actual Normal budget 1/2/4/8 equality");
        std::cout << "stock Normal budget and economy tokens workers=" << workers << '\n';
    }
}
}
int case_galactic_ai_economy_tokens_tests(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") { installed(); return galactic_test::failures ? 1 : 0; }
        if (argc != 1) throw std::runtime_error("economy contracts accept only --installed");
        const auto baseline = contracts(1);
        for (const auto workers : {2U, 4U, 8U}) expect(contracts(workers) == baseline, "1/2/4/8 source token and save equality");
        const auto next_baseline = next_technology(1);
        for (const auto workers : {2U, 4U, 8U}) expect(next_technology(workers) == next_baseline, "next-tech identity and restored state 1/2/4/8 equality");
        const auto supplier_baseline = partitioned_supplier(1);
        for (const auto workers : {2U, 4U, 8U, 16U}) expect(partitioned_supplier(workers) == supplier_baseline, "partitioned supplier 1/2/4/8/16 equality");
        const auto unguarded_baseline = unguarded_supplier(1);
        for (const auto workers : {2U, 4U, 8U, 16U}) expect(unguarded_supplier(workers) == unguarded_baseline, "unguarded supplier 1/2/4/8/16 equality");
        const auto aggregate_baseline = aggregate_restore_observations(1);
        for (const auto workers : {2U, 4U, 8U}) expect(aggregate_restore_observations(workers) == aggregate_baseline,
            "aggregate observation continuation is identical on 1/2/4/8 workers");
        for (const auto workers : {1U, 2U, 4U, 8U}) cached_bases(workers);
        for (const auto workers : {1U, 2U, 4U, 8U}) generations(workers);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
