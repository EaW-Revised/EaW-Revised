#include <algorithm>
#include <exception>
#include "../support/check.hpp"
#include "../support/result.hpp"
#include "ai_learning.hpp"
#include "galactic_ai_battle_listener.hpp"
#include "galactic_ai_parent_context_runtime.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/session.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using eawr::testing::must;

namespace {

struct MovementFixture {
    struct Movement { std::uint32_t player{}; std::uint64_t actor{}, conflict_location{}; bool finished{}; std::uint64_t freestore_actor{}; };
    std::map<std::uint64_t, Movement> rows;
    const auto& movements() const { return rows; }
    eawr::core::Result<void> movement_conflict(std::uint64_t request, std::uint64_t location, bool begin) {
        rows.at(request).conflict_location = begin ? location : 0;
        if (begin) rows.at(request).finished = false;
        return eawr::core::Result<void>::success();
    }
};
// Supporting child binding proof; installed gameplay uses the runtime factory.
void parent_context_contracts(bool fog, bool visible) {
    namespace ai = eawr::script::foc::ai;
    namespace foc = eawr::script::foc;
    namespace ga = foc::galactic_ai;
    namespace auth = eawr::script::authoritative;
    namespace gc = eawr::sim::galactic;
    eawr::platform::ThreadWorkerAdapter executor(2);
    auto host = std::make_shared<foc::detail::Host>();
    ai::GalacticPerceptionSetup setup; setup.seed = 42; setup.uses_fog = fog;
    setup.players = {{1, 1, "Empire", "Normal", false, true, false, 10}};
    setup.planets = {{10, "ALPHA", 1000, ai::real(4), {}, {}, {}, {}}};
    setup.cache_intervals = {{"GROUNDBASELEVELUNNORMALIZED", ai::real(-1)}, {"BASEINCOME", ai::real(-1)},
        {"STARBASELEVELUNNORMALIZED", ai::real(1)}};
    auto world = std::make_shared<ai::GalacticWorld>(setup);
    ai::AiData data; data.players["QUERY_COMPONENT"] = ai::PlayerType{};
    data.equations = must(ai::EquationSet::parse({{"parent.xml", "<Equations><Ground>Variable_Target.GroundbaseLevelUnnormalized</Ground><Income>Variable_Target.BaseIncome</Income><Expired>Variable_Target.StarbaseLevelUnnormalized</Expired><Age>Variable_Target.FriendlyForce.SpaceTotal.TimeLastSeen</Age></Equations>"}},
        [](std::string_view, std::string_view) -> std::optional<ai::Real> { return {}; }));
    auto engine = std::make_shared<ai::Engine>(host, std::move(data), std::vector<ai::PlanDef>{});
    host->ai_world = world; host->engine = engine.get();
    host->setup.players = {{1, "Empire", false, true, "QUERY_COMPONENT", false}};
    for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
    gc::GalacticSetup galaxy_setup; galaxy_setup.players = {{1, 1, 0}}; galaxy_setup.planets = {{10, {}, 1}};
    const auto whole = [](std::int64_t value) { return must(eawr::sim::math::Fixed::from_integer(value)); };
    galaxy_setup.player_economy = gc::PlayerEconomySetup{{{1, {}, whole(1000), 1, 5}}};
    galaxy_setup.fiscal = gc::FiscalSetup{whole(1000), {{1, true, whole(1)}}, {{10, whole(4)}}};
    galaxy_setup.rules.fiscal_cycle_frames = 10000;
    galaxy_setup.rules.maximum_fleet_movement_distance = whole(0);
    galaxy_setup.fleet_control.emplace();
    gc::PlanetProduction planet; planet.planet = 10; planet.target_owner = planet.ground_occupier = 1; planet.ground_level = 3;
    galaxy_setup.production = gc::ProductionSetup{5, {{1, false, true, false, {}, {}, {}, {}, {}}}, {planet}};
    auto galaxy = must(gc::GalacticSession::create(galaxy_setup));
    must(world->bind(galaxy.snapshot(), {}, executor, 0, 100));
    auto parent_scripts = must(auth::ScriptScheduler::create({}, {})); std::uint64_t sequence{};
    must(engine->before_service(*world, parent_scripts, sequence, &executor));
    world->set_synchronized_frame(7);
    const auto* target = engine->target_of_object(10);
    if (!target || world->evaluate(*engine, "Ground", 1, target) != ai::real(3))
        throw std::runtime_error("child context fixture requires a retained cache hit and a distinct cache miss");
    if (world->evaluate(*engine, "Expired", 1, target) != ai::real(0))
        throw std::runtime_error("child context fixture requires a qualified fresh starbase getter");
    auto seeded = world->state();
    for (auto& cached : seeded.cache) if (cached.key.tokens == std::vector<std::string>{"VARIABLE_TARGET", "STARBASELEVELUNNORMALIZED"}) {
        cached.value = ai::real(9); cached.frame = 1;
    }
    for (auto& row : seeded.visibility) { row.visible = visible; row.last_observed = 7; }
    must(world->restore(std::move(seeded))); world->set_synchronized_frame(62);
    const auto parent_frame = engine->frame(); const auto parent_revision = engine->native_state_revision();
    const auto world_revision = world->state_revision(); const auto movement_revision = world->movement_input_revision();
    const auto world_hash = must(world->state_hash()); const auto world_bytes = must(world->capture()).components;
    const auto perception = world->state();
    auto child = std::make_shared<foc::detail::Host>();
    child->setup.evaluate_in_galactic_context = ga::galactic_parent_evaluator(engine, world, 10);
    auth::ModuleManifest modules;
    must(modules.add("child.lua", "function Read() Value=Evaluate_In_Galactic_Context('Ground', Player); Income=Evaluate_In_Galactic_Context('Income', Player); Expired=Evaluate_In_Galactic_Context('Expired', Player); Age=Evaluate_In_Galactic_Context('Age', Player); Missing=Evaluate_In_Galactic_Context('Unknown', Player) end"));
    auto scripts = must(auth::ScriptScheduler::create({}, modules)); std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_globals(scripts, child, errors);
    if (!errors.empty()) throw std::runtime_error(errors.front().message);
    must(scripts.create_instance(1, "child.lua"));
    auth::ScriptEvent player; player.key = {1, 16, 1, 0}; player.target = 1; player.kind = auth::ScriptEvent::Kind::assign;
    player.name = "Player"; player.parameter = auth::Value{auth::Handle{foc::handle_player, 1}}; must(scripts.submit_event(std::move(player)));
    auth::ScriptEvent read; read.key = {1, 16, 1, 1}; read.target = 1; read.kind = auth::ScriptEvent::Kind::call; read.name = "Read";
    must(scripts.submit_event(std::move(read))); const auto report = must(scripts.service(executor));
    const auto value = must(scripts.read_global(1, "Value")), missing = must(scripts.read_global(1, "Missing"));
    const auto income = must(scripts.read_global(1, "Income")), expired = must(scripts.read_global(1, "Expired"));
    const auto age = must(scripts.read_global(1, "Age"));
    if (!value || std::get<ai::Real>(value->data) != ai::real(3)
        || !income || std::get<ai::Real>(income->data) != ai::real(4)
        || !expired || std::get<ai::Real>(expired->data) != ai::real(fog && !visible ? 9 : 0)
        || !age || std::get<ai::Real>(age->data) != (fog && !visible ? ai::to_single(ai::real(55) / ai::real(62)) : ai::real(0))
        || !missing || !std::holds_alternative<std::monostate>(missing->data)
        || report.diagnostics.size() != 1 || engine->frame() != parent_frame || engine->native_state_revision() != parent_revision)
        throw std::runtime_error("child binding reads real retained parent immediately; missing context has no numeric result and parent remains frozen");
    const auto after = world->state();
    if (world->state_revision() != world_revision || world->movement_input_revision() != movement_revision
        || must(world->state_hash()) != world_hash || must(world->capture()).components != world_bytes
        || after.cache.size() != perception.cache.size() || after.visibility.size() != perception.visibility.size())
        throw std::runtime_error("child context cache hit/miss must leave parent perception rows, bytes, hash and revisions unchanged");
    // Native graph restore precedes binding the reconstructed parent World.
    // It must restore recorded Lua values without executing host queries.
    const auto saved_child = must(scripts.save());
    auto unavailable = std::make_shared<foc::detail::Host>(); std::uint64_t reconstruction_reads{};
    unavailable->setup.evaluate_in_galactic_context = [&](std::string_view, gc::PlayerId) -> std::optional<ai::Real> {
        ++reconstruction_reads; return {};
    };
    auto restored = must(auth::ScriptScheduler::create({}, modules)); errors.clear();
    foc::tactical_ai_detail::register_globals(restored, unavailable, errors);
    if (!errors.empty()) throw std::runtime_error(errors.front().message);
    must(restored.load(saved_child));
    if (reconstruction_reads != 0 || must(restored.state_hash()) != must(scripts.state_hash()))
        throw std::runtime_error("native child graph restore must remain inert before reconstructed parent binding");
}
void battle_contracts() {
    namespace cp = eawr::campaign;
    namespace gc = eawr::sim::galactic;
    namespace auth = eawr::script::authoritative;
    namespace ga = eawr::script::foc::galactic_ai;
    using eawr::testing::require;
    gc::GalacticSetup setup; setup.players = {{1, 1, 0}, {2, 2, 0}}; setup.planets = {{10, {}, 1}};
    auto galaxy = must(gc::GalacticSession::create(setup));
    const auto snapshot = galaxy.snapshot();
    std::string reference;
    for (auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auth::ModuleManifest modules;
        must(modules.add("listener.lua", "Callbacks=0; Pumps=0; function OnBattle(winner) Callbacks=Callbacks+1; Winner=winner end; function Worker() Pumps=Pumps+1 end; Create_Thread('Worker')"));
        auto scripts = must(auth::ScriptScheduler::create({}, modules));
        must(scripts.create_instance(100000, "listener.lua"));
        auto movements = std::make_shared<MovementFixture>();
        movements->rows = {{1, {1, 50, 0, false}}, {2, {1, 60, 0, false}}, {3, {2, 50, 0, false}}, {4, {1, 50, 0, true}}, {5, {1, 50, 0, true}}};
        std::uint64_t delivered{}, sequence{};
        cp::GalacticAiServices services;
        services.battle = ga::movement_battle_listener(movements, [](std::uint64_t request) { return request != 5; }, [&](std::uint64_t request, bool begin, const cp::GalacticAiBattleNotice& notice) {
            require((request == 1 || request == 4) && &notice.world == snapshot.get(), "battle delivery keeps exact retained request and current world");
            ++delivered;
            if (begin) return eawr::core::Result<void>::success();
            auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, 16, 100000, sequence++};
            event.target = 100000; event.kind = auth::ScriptEvent::Kind::call; event.name = "OnBattle";
            event.arguments = {auth::Value::number(eawr::script::foc::ai::real(notice.result->winner))};
            return scripts.submit_event(std::move(event));
        });
        cp::ConflictRecord conflict; conflict.id = 100; conflict.planet = 10; conflict.layer = cp::BattleLayer::space;
        cp::ConflictForce force; force.id = 51; force.owner = 1; force.parent = 50; conflict.forces = {force};
        must(services.battle({cp::GalacticAiBattleKind::entered, 900, conflict, *snapshot}));
        require(delivered == 2 && movements->rows.at(1).conflict_location == 10 && !movements->rows.at(2).conflict_location,
            "battle entry requires actor participation rather than destination or another player's actor");
        require(movements->rows.at(4).conflict_location == 10 && !movements->rows.at(4).finished
            && !movements->rows.at(5).conflict_location && movements->rows.at(5).finished,
            "active listener can reopen a finished movement; unsubscribed old requests remain untouched");
        const auto before = must(scripts.state_hash());
        cp::BattleResult result; result.conflict = 100; result.winner = 2;
        auto wrong = result; wrong.conflict = 101;
        require(!services.battle({cp::GalacticAiBattleKind::returned, 1200, conflict, *snapshot, &wrong}), "previous conflict result refuses before listener mutation");
        require(!services.battle({cp::GalacticAiBattleKind::returned, 1200, conflict, *snapshot}), "missing finalized return result refuses");
        must(services.battle({cp::GalacticAiBattleKind::returned, 1200, conflict, *snapshot, &result}));
        require(delivered == 4 && !movements->rows.at(1).conflict_location && !movements->rows.at(4).conflict_location && scripts.completed_tick() == 0,
            "battle listener clears matching end subscription without a parent service tick");
        const auto callbacks = must(scripts.read_global(100000, "Callbacks"));
        require(callbacks && std::get<eawr::script::numeric::LuaNumber>(callbacks->data) == eawr::script::foc::ai::real(0),
            "WGA-204 seam is explicitly deferred until next admitted scheduler service");
        require(must(scripts.state_hash()) != before, "pending deferred callback participates in native script state");
        const auto saved = must(scripts.save());
        auto restored = must(auth::ScriptScheduler::create({}, modules)); must(restored.load(saved));
        auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
        require(must(scripts.service(executor, options)).diagnostics.empty() && must(restored.service(executor, options)).diagnostics.empty(), "deferred handler executes without diagnostics");
        const auto winner = must(scripts.read_global(100000, "Winner"));
        const auto restored_winner = must(restored.read_global(100000, "Winner"));
        const auto pumps = must(scripts.read_global(100000, "Pumps"));
        const auto restored_pumps = must(restored.read_global(100000, "Pumps"));
        require(winner && std::get<eawr::script::numeric::LuaNumber>(winner->data) == eawr::script::foc::ai::real(2)
            && restored_winner && std::get<eawr::script::numeric::LuaNumber>(restored_winner->data) == eawr::script::foc::ai::real(2)
            && restored_pumps && std::get<eawr::script::numeric::LuaNumber>(restored_pumps->data) == eawr::script::foc::ai::real(0)
            && pumps && std::get<eawr::script::numeric::LuaNumber>(pumps->data) == eawr::script::foc::ai::real(0),
            "callback uses current finalized winner and does not pump parent coroutine");
        const auto hash = must(scripts.state_hash());
        if (hash != must(restored.state_hash())) {
            const auto control_bytes = must(scripts.save()), restored_bytes = must(restored.save());
            const auto mismatch = std::mismatch(control_bytes.begin(), control_bytes.end(), restored_bytes.begin(), restored_bytes.end());
            throw std::runtime_error("deferred callback save/load continuation differs at byte "
                + std::to_string(mismatch.first - control_bytes.begin()) + "; sizes "
                + std::to_string(control_bytes.size()) + "/" + std::to_string(restored_bytes.size()));
        }
        if (workers == 1) reference = hash; else require(reference == hash, "battle callback continuation equals on 1/2/4/8");
        must(services.battle({cp::GalacticAiBattleKind::returned, 1200, conflict, *snapshot, &result, true}));
        require(delivered == 4, "auto-resolve return does not invent an entry subscription or repeat completed end");
    }
}
}

int galactic_learning_contracts() {
    namespace ai = eawr::script::foc::ai;
    namespace learning = ai::learning;
    using eawr::testing::require;
    try {
        ai::GoalType type; type.tracking_duration = ai::real(2); type.activation_tracking_duration = ai::real(1);
        ai::PlayerAi player; player.functions.push_back({&type, "GOAL", nullptr, {}});
        ai::Goal goal; goal.id = 1; goal.target = 9;
        ai::Plan plan; plan.id = 2; plan.goal = 1; plan.definition = 3;
        learning::activation(player, goal, true, 100, 30);
        require(player.plan_success.empty(), "activation admission must not register completed outcome");
        learning::outcome(player, goal, plan, 100, 30);
        require(player.goal_success.at("GOAL") == std::pair<std::int64_t, std::int64_t>{0, 1}, "unwritten result is failure");
        plan.result = true;
        learning::outcome(player, goal, plan, 101, 30);
        require(player.target_plan_success.at({3, 9}) == std::pair<std::int64_t, std::int64_t>{1, 2}, "targeted attempts accumulate success without erasing failure");
        require(player.goal_outcomes.at({"GOAL", 9}).entries == player.plan_outcomes.at({3, 9}).entries, "goal and plan histories share retained result and expiry");
        require(player.goal_outcomes.at({"GOAL", 9}).entries.front().second == 160, "expiry uses supplied synchronization frame and logical FPS");
        type.tracking_duration = ai::Real{}; type.activation_tracking_duration = ai::Real{};
        goal.target = 10;
        learning::activation(player, goal, false, 110, 30);
        learning::outcome(player, goal, plan, 110, 30);
        require(!player.goal_outcomes.contains({"GOAL", 10}) && !player.plan_outcomes.contains({3, 10})
            && !player.activations.contains({"GOAL", 10}), "zero durations omit recent keys but retain cumulative attempts");
        require(player.goal_success.at("GOAL").second == 3, "zero-duration completed outcome increments lifetime counts");
        type.tracking_duration = ai::real(-1);
        learning::outcome(player, goal, plan, 110, 30);
        require(player.goal_outcomes.at({"GOAL", 10}).entries.front().second == -1, "negative duration stores negative expiry");
        player.activations[{"GOAL", 9}].entries = {{false, -1}, {false, 120}, {true, 121}, {false, -1}, {false, 115}};
        learning::sweep(player, 120);
        require(player.activations.at({"GOAL", 9}).entries == std::vector<std::pair<bool, std::int64_t>>{{true, 121}, {false, -1}, {false, 115}},
            "positive future stops prefix scan and preserves every suffix row");
        require(player.goal_outcomes.at({"GOAL", 10}).entries.empty(), "negative-only vector clears at admitted sweep");
        require(player.plan_success.at(3).second == 4, "sweep leaves cumulative attempts unchanged");
        goal.target = 0; learning::outcome(player, goal, plan, 120, 30);
        learning::erase_target(player, 9);
        require(!player.target_goal_success.contains({"GOAL", 9}) && !player.target_plan_success.contains({3, 9})
            && !player.activations.contains({"GOAL", 9}) && !player.goal_outcomes.contains({"GOAL", 9}) && !player.plan_outcomes.contains({3, 9}),
            "delete cleans all five targeted tables");
        require(player.target_goal_success.contains({"GOAL", 0}) && player.plan_success.at(3).second == 5,
            "target delete preserves global cumulative and null-target rows");
        require(learning::survival(player, "UNKNOWN") == ai::real(1), "unobserved unit survival is one");
        learning::increment(player.unit_survival["UNIT"], false);
        learning::increment(player.unit_survival["UNIT"], true);
        require(learning::survival(player, "UNIT") == ai::to_single(ai::real(1) / ai::real(2)), "survival is independent of plan result");
        battle_contracts();
        parent_context_contracts(false, true);
        parent_context_contracts(true, true);
        parent_context_contracts(true, false);
    } catch (const std::exception& error) { std::cerr << "learning: " << error.what() << '\n'; return 1; }
    return 0;
}
