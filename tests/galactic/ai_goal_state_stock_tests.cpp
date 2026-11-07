#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include "../support/check.hpp"
#include "../support/result.hpp"
#include "ai_goal_state_stock.hpp"
#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_world.hpp"
#include "galactic_ai.hpp"
#include "galactic_ai_funding.hpp"
#include "galactic_ai_battle_listener.hpp"
#include "eawr/campaign/game_session.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/sim/galactic/synthetic.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
using eawr::testing::must;

namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
namespace lifecycle = foc::galactic_ai;
namespace cp = eawr::campaign;
namespace {
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool ok, std::string_view message) {
    if (!ok) local_checker.check_before_print(ok, message, std::cerr, "FAIL: ");
}

struct Owner {
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    std::shared_ptr<ai::Engine> engine;
    auth::ScriptScheduler scripts = must(auth::ScriptScheduler::create({}, {}));
    Owner(ai::GalacticPerceptionSetup setup, ai::AiData data, std::vector<ai::PlanDef> plans,
        std::vector<lifecycle::PlayerSelection> selected)
        : world(std::make_shared<ai::GalacticWorld>(setup)), engine(std::make_shared<ai::Engine>(host, std::move(data), std::move(plans))) {
        host->setup.seed = setup.seed; host->ai_world = world; host->engine = engine.get();
        host->setup.content.types = setup.types;
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(ai::upper_case(type.name), &type);
        }
        for (const auto& player : setup.players) {
            const auto found = std::find_if(selected.begin(), selected.end(), [&](const auto& row) { return row.player == player.player; });
            host->setup.players.push_back({player.player, player.faction, player.neutral, found != selected.end(),
                found != selected.end() ? found->player_type : "", player.human});
        }
        for (const auto& row : host->setup.players) host->players.emplace(row.player, &row);
    }
    void begin(std::shared_ptr<const gc::GalacticSnapshot> snapshot, const eawr::sim::PartitionExecutor& executor,
        std::span<const lifecycle::PlayerSelection> selected, std::uint64_t frame = 0, eawr::sim::EntityId next = 1000) {
        must(world->bind(snapshot, {}, executor, frame, next));
        std::uint64_t sequence{};
        must(engine->begin_galactic_barrier(*world, {frame, snapshot->clock().galactic_frame}, scripts, sequence, selected, &executor));
    }
};
ai::AiData fixture_data() {
    ai::AiData data;
    data.equations = must(ai::EquationSet::parse({{"fixture.xml", "<Equations><Positive>1</Positive><Zero>0</Zero></Equations>"}}, {}));
    ai::GoalType goal; goal.name = "Goal"; goal.mode = "GALACTIC"; goal.category = "OFFENSIVE"; goal.application = {"FRIENDLY"};
    goal.tracking_duration = ai::real(2); goal.activation_tracking_duration = ai::real(1);
    data.goals["GOAL"] = goal;
    data.function_sets["FUNCTIONS"] = {{"GOAL", "Positive"}};
    ai::PlayerType type; type.name = "Fixture"; type.function_sets = {"FUNCTIONS"}; type.difficulty["NORMAL"] = "NORMAL";
    data.players["FIXTURE"] = type; data.difficulties["NORMAL"] = {};
    return data;
}
std::vector<ai::PlanDef> fixture_plans() {
    ai::PlanDef first; first.name = "FIRST"; first.module = "first.lua"; first.goals = {"GOAL"};
    first.taskforces.push_back({"Main", {{1, 1, 0, false, {100}}}, false, true, {}, 1});
    auto second = first; second.name = "SECOND"; second.module = "second.lua";
    return {first, second};
}
ai::GalacticPerceptionSetup fixture_setup() {
    ai::GalacticPerceptionSetup setup; setup.seed = 42;
    setup.players = {{1, 1, "Empire", "Normal", false, true, false, 10}, {2, 2, "Rebel", "Normal", false, true, false, 20}};
    setup.planets = {{10, "ALPHA", 1000, {}, {}, {}, {}, {}}, {20, "BETA", 2000, {}, {}, {}, {}, {}}};
    foc::AiType type; type.type_id = 100; type.name = "UNIT"; setup.types.push_back(type);
    return setup;
}
std::shared_ptr<const gc::GalacticSnapshot> fixture_snapshot() {
    gc::Fleet fleet; fleet.id = 50; fleet.owner = 1; fleet.orbit = 10; fleet.members = {{51, 100}};
    gc::Fleet other; other.id = 60; other.owner = 2; other.orbit = 20; other.members = {{61, 100}};
    return std::make_shared<gc::GalacticSnapshot>(0, gc::GalacticClock{}, std::vector<gc::GalacticPlayer>{{1, 1, 0}, {2, 2, 0}},
        std::vector<gc::Planet>{{10, {}, 1}, {20, {}, 2}}, std::vector<gc::Route>{}, std::vector<gc::Fleet>{fleet, other},
        std::vector<gc::GalacticEvent>{}, std::nullopt, std::nullopt, nullptr);
}
void policy(Owner& owner, std::uint32_t player) {
    lifecycle::TemplatePolicy policy; policy.goal_types["GOAL"] = {true, 1};
    policy.plan_names["FIRST"] = policy.plan_names["SECOND"] = {true, 1};
    must(owner.engine->apply_galactic_policy(player, policy));
}
std::string fixture_identity() { return eawr::core::sha256_hex(std::span<const std::uint8_t>{}); }
}

namespace eawr::script::foc::ai {
// Supporting synthetic continuation inventory, not installed AI behavior.
struct EngineContractAccess {
    static void phase_global(Engine& e, int value) {
        const auto write = e.native_record_scope();
        e.native_records_changed(0, {27, 31}); e.mark_native_state_changed();
        e.globals_["PHASE"] = real(value);
        e.host_->credit_changes[1] = math::Fixed::from_raw(value);
        e.publish_record({0, 1, "phase", "GOAL", "target", "event", "report"});
    }
    static void running(Engine& e, std::uint64_t goal) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_replaced(); e.mark_native_state_changed();
        auto& p = e.players_.front();
        auto found = std::find_if(p.proposed.begin(), p.proposed.end(), [&](const auto& row) { return row.id == goal; });
        Goal retained = *found; p.proposed.erase(found); retained.active = true;
        auto& potential = *retained.potential; potential.valid = potential.reserved = true;
        potential.units = {100}; potential.freestore = {51}; potential.sources = {0}; potential.producers = {0}; potential.pool_tokens = {0};
        potential.taskforce_of_unit = potential.team_of_unit = {0}; potential.cost = Real::from_repr(0x3ff0000000000001ULL);
        potential.current_contrast = potential.target_contrast = {{7, real(2)}}; potential.threshold = real(3);
        Plan plan; plan.id = *e.allocate_plan_id(p); plan.instance = *e.allocate_instance_id(p); plan.definition = potential.plan;
        plan.goal = retained.id; plan.player = p.player; plan.target = retained.target; plan.start_tick = 0;
        TaskForce tf; tf.id = *e.allocate_taskforce_id(p); tf.plan = plan.id; tf.name = "Main"; tf.units = {51};
        tf.types = {100}; tf.sources = {0}; tf.producers = {0}; tf.pool_tokens = {0}; tf.had_units = true; tf.last_target[51] = 61;
        plan.taskforces = {tf.id}; retained.plan = plan.id; p.assigned[51] = tf.id;
        const auto block = block_id(plan.instance, 17);
        Block b; b.id = block; b.instance = plan.instance; b.taskforce = tf.id; b.kind = Block::Kind::produce;
        b.destination_object = 61; b.movers = {51}; b.ordered_tick[51] = 99; b.distance = real(5); b.waiting_on = 0;
        e.players_.front().blocks[block] = b;
        BuildTask task; task.taskforce = tf.id; task.block = block; task.type = 100; task.object = 51;
        task.source = 2; task.producer = 10; task.issued = 41; task.generic_cost = real(2); p.tasks = {task};
        p.active.clear(); p.active.push_back(retained); p.activations[{"GOAL", retained.target}].entries = {{true, -1}, {false, 600}};
        p.goal_outcomes = p.activations; p.plan_outcomes[{plan.definition, retained.target}].entries = {{false, 700}};
        p.plan_success[plan.definition] = {3, 5};
        p.goal_success["GOAL"] = {1, 4}; p.target_goal_success[{"GOAL", retained.target}] = {1, 4};
        p.target_plan_success[{plan.definition, retained.target}] = {2, 3}; p.unit_survival["UNIT"] = {2, 5}; p.next_function = 1; p.next_target = 1; p.sleep_frames = 2;
        p.nontrivial = 7; p.maintenance_due = true; p.exclusion_counts["ZERO"] = 0; p.reserved_credits[retained.id] = real(8);
        p.reserved_pool[100] = 1; p.reserved_pool_tokens[222] = retained.id;
        p.reserved[999999] = retained.id; // known pending loss/cancel retains identity
        e.players_.front().taskforces[tf.id] = tf; e.players_.front().running[plan.id] = plan; e.players_.front().plan_of_instance[plan.instance] = plan.id;
        e.pad_reservations_[10] = {p.player, retained.id, 100, true, 47};
        e.pending_attach_.push_back({p.player, retained.id}); e.globals_["MARKER"] = Real::from_repr(0x8000000000000000ULL);
        e.threats_[51][61] = real(4); e.last_hits_[51] = {61, true};
        authoritative::ScriptEvent event; event.key = {1, 16, plan.instance, 4}; event.target = plan.instance;
        event.kind = authoritative::ScriptEvent::Kind::thread_signal; event.name = "Signal"; event.thread = 2;
        event.parameter = authoritative::Value{authoritative::Handle{handle_taskforce, tf.id}};
        event.arguments = {authoritative::Value{std::vector<authoritative::Value>{authoritative::Value::text("nested"), authoritative::Value::number(real(2))}}};
        e.events_ = {event}; e.orders_ = {{1, plan.instance, 3, "fixture", {authoritative::Value::number(real(11))}}};
        e.host_->credit_changes[p.player] = math::Fixed::from_raw(12345);

        e.rebuild_runtime_index();
    }
    static std::uint64_t movement_fixture(Engine& e, std::uint64_t goal, std::uint64_t request = 77) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_replaced(); e.mark_native_state_changed();
        running(e, goal);
        auto& block = e.players_.front().blocks.begin()->second;
        block.kind = Block::Kind::move; block.galactic_request = request; block.galactic_actor = 50;
        auto& force = e.players_.front().taskforces.at(block.taskforce); force.thread = 0; force.galactic_fleet = 50;
        e.players_.front().tasks.clear(); e.events_.clear(); e.orders_.clear(); e.pending_attach_.clear(); e.pad_reservations_.clear();
        return block.id;
    }
    static std::size_t pending_events(const Engine& e) { return e.events_.size(); }
    static bool completed_learning(const Engine& e, std::uint64_t target) {
        const auto& p = *e.player_ai(1);
        return p.plan_success.size() == 1 && p.plan_success.begin()->second == std::pair<std::int64_t, std::int64_t>{3, 6}
            && p.goal_success.at("GOAL") == std::pair<std::int64_t, std::int64_t>{1, 5}
            && p.target_goal_success.at({"GOAL", target}) == std::pair<std::int64_t, std::int64_t>{1, 5}
            && p.target_plan_success.begin()->second == std::pair<std::int64_t, std::int64_t>{2, 4}
            && p.goal_outcomes.at({"GOAL", target}).entries.back() == std::pair<bool, std::int64_t>{false, 61}
            && p.plan_outcomes.begin()->second.entries.back() == std::pair<bool, std::int64_t>{false, 61};
    }
    static std::size_t function_count(const Engine& e, tactical::PlayerId player) { return e.player_ai(player)->functions.size(); }
    static std::size_t goal_function(const Engine& e, tactical::PlayerId player, std::uint64_t id) {
        const auto* p = e.player_ai(player);
        for (const auto* goals : {&p->active, &p->proposed}) for (const auto& goal : *goals) if (goal.id == id) return goal.function;
        throw std::runtime_error("goal function identity absent");
    }
};
}

namespace {
void galactic_budget_and_desire_stub_contracts() {
    const std::vector<lifecycle::PlayerSelection> selected{{1, "FIXTURE", "NORMAL", {}, {}}};
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto data = fixture_data(); data.goal_categories["MAJORITEM"] = 1; data.goal_categories["UNFUNDED"] = 2;
        Owner control(fixture_setup(), std::move(data), fixture_plans(), selected);
        control.begin(fixture_snapshot(), executor, selected); policy(control, 1);
        const auto goal = must(control.engine->allocate_galactic_goal(1, 0, control.engine->target_of_object(10)->id));
        ai::EngineContractAccess::movement_fixture(*control.engine, goal->goal);
        const auto plan_id = control.engine->galactic_goal(1, goal->goal)->running_plan;
        const auto* plan = control.engine->plan(plan_id);
        const auto instance = plan->instance;
        const auto fixed = [](std::int64_t value) { return must(eawr::sim::math::Fixed::from_integer(value)); };
        auto funding = std::make_shared<ai::GalacticFunding>();
        ai::EvaluatedFundingTemplate allocation; allocation.equations = {{1, fixed(1)}};
        must(funding->initialize(1, std::span<const ai::EvaluatedFundingTemplate>(&allocation, 1), fixed(100)));
        control.engine->set_galactic_funding(funding);
        const auto before = *funding->ledger(1);
        expect(before.categories.at(1).available_cash != fixed(0), "budget binding fixture begins with available category cash");
        auth::ModuleManifest modules;
        const std::string source = R"LUA(
function Base_Definitions() Budget = nil end
function BadCategory() Budget.Flush_Category("Unregistered_Category") end
function UnfundedOnly() Budget.Flush_Category("Unfunded") end
Count = 0
function Worker()
    while Count < 2 do
        Bonus = GiveDesireBonus(PlayerObject, "Goal", Target, 15, 5)
        Budget.Flush_Category("MajorItem")
        Count = Count + 1
        coroutine.yield(true)
    end
end
Create_Thread("Worker")
)LUA";
        must(modules.add("first.lua", source)); must(modules.add("second.lua", source));
        auth::SessionConfig config; config.galactic_clock = true;
        control.scripts = must(auth::ScriptScheduler::create(config, modules));
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(control.scripts, control.host, errors);
        expect(errors.empty(), "budget method and desire stub register without protecting the Budget variable");
        must(control.scripts.create_instance(instance, control.engine->plans().at(plan->definition).module));
        std::uint64_t sequence{};
        const auto post = [&](auth::ScriptEvent::Kind kind, std::string name, std::optional<auth::Value> value = {}) {
            auth::ScriptEvent event; event.key = {1, 16, instance, sequence++}; event.target = instance;
            event.kind = kind; event.name = std::move(name); event.parameter = std::move(value);
            must(control.scripts.submit_event(std::move(event)));
        };
        post(auth::ScriptEvent::Kind::call, "Base_Definitions");
        post(auth::ScriptEvent::Kind::assign, "Budget", auth::Value{auth::Handle{ai::handle_galactic_budget, plan_id}});
        post(auth::ScriptEvent::Kind::assign, "PlayerObject", auth::Value{auth::Handle{foc::handle_player, 1}});
        post(auth::ScriptEvent::Kind::assign, "Target", auth::Value{auth::Handle{foc::handle_game_object, 10}});
        post(auth::ScriptEvent::Kind::call, "UnfundedOnly");
        auth::ServiceOptions callback_only; callback_only.host_paced = [](std::uint64_t) { return true; };
        const auto unfunded_before = funding->record_sync_hash(true);
        auto unfunded = must(control.scripts.service(executor, callback_only));
        expect(unfunded.diagnostics.empty() && unfunded.commands.size() == 1 && control.scripts.retired_instances().empty(),
            "a registered category without a ledger account stages an ordinary flush without retiring the plan");
        must(control.engine->after_service(unfunded));
        expect(funding->record_sync_hash(true) == unfunded_before && *funding->ledger(1) == before
            && !funding->ledger(1)->categories.contains(2),
            "flushing a registered unfunded category is a no-op on exact ledger state and hash, with no new account");
        eawr::core::LoadNoteCollector notes;
        for (unsigned tick = 0; tick < 2; ++tick) {
            auto report = must(control.scripts.service(executor));
            expect(report.diagnostics.empty() && control.scripts.retired_instances().empty(),
                "shared TaskForce desire and flush calls continue without retiring the plan");
            if (tick == 0) expect(*funding->ledger(1) == before, "Lua stages budget writes until the ordered native commit");
            must(control.engine->after_service(report));
            for (auto& note : report.load_notes) notes.append(std::move(note));
        }
        const auto value = must(control.scripts.read_global(instance, "Count"));
        const auto bonus = must(control.scripts.read_global(instance, "Bonus"));
        expect(value && std::get<ai::Real>(value->data) == ai::real(2) && bonus
            && std::holds_alternative<std::monostate>(bonus->data), "warned nil bonus lets the plan continue past both calls");
        const auto after = funding->ledger(1);
        expect(after->categories.at(1).available_cash == fixed(0) && after->categories.at(1).cash == before.categories.at(1).cash,
            "WGA-24 native flush zeros available category cash without a credit refund");
        const auto emitted = notes.snapshot();
        expect(emitted.size() == 1 && emitted.front().tag == "GiveDesireBonus"
            && emitted.front().code == "EAWR-LOAD-1400", "unsupported timed bonus emits one LoadNote per plan type");
        const auto hash = must(control.scripts.state_hash()) + funding->record_sync_hash(true);
        if (workers == 1) reference = hash; else expect(hash == reference, "budget flush and warned bonus stub agree on 1/2/4/8");
        const auto funding_hash = funding->record_sync_hash(true);
        auth::ScriptEvent bad; bad.key = {4, 16, instance, sequence++}; bad.target = instance;
        bad.kind = auth::ScriptEvent::Kind::call; bad.name = "BadCategory";
        must(control.scripts.submit_event(std::move(bad)));
        auto rejected = must(control.scripts.service(executor));
        expect(!rejected.diagnostics.empty() && control.scripts.retired_instances() == std::vector<std::uint64_t>{instance},
            "WGA-24 unknown category remains an authored script error, not a silent flush");
        must(control.engine->after_service(rejected));
        expect(funding->record_sync_hash(true) == funding_hash, "rejected budget input cannot change the native ledger");
    }
}
void degraded_battle_entry_contracts() {
    namespace ga = foc::galactic_ai;
    std::vector<std::string> reference;
    const std::vector<lifecycle::PlayerSelection> selected{{1, "FIXTURE", "NORMAL", {}, {}}, {2, "FIXTURE", "NORMAL", {}, {}}};
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> trace;
        // Missing scheduler, retired Lua instance, missing engine, live control.
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
            const auto run = [&](const cp::GameRecording* recorded) {
                Owner control(fixture_setup(), fixture_data(), fixture_plans(), selected);
                control.begin(fixture_snapshot(), executor, selected); policy(control, 1);
                const auto goal = must(control.engine->allocate_galactic_goal(1, 0, control.engine->target_of_object(10)->id));
                expect(bool(goal), "battle degradation fixture allocates its real native goal");
                const auto block = ai::EngineContractAccess::movement_fixture(*control.engine, goal->goal, 1);
                const auto plan_id = control.engine->galactic_goal(1, goal->goal)->running_plan;
                const auto* plan = control.engine->plan(plan_id);
                const auto taskforce = plan->taskforces.front(), instance = plan->instance;
                auth::ModuleManifest modules;
                const std::string source = scenario == 1
                    ? "function Worker() error('injected provider retirement') end Create_Thread('Worker')"
                    : "function Main_Space_Conflict_Begin(tf) end function Worker() coroutine.yield(true) end Create_Thread('Worker')";
                must(modules.add("first.lua", source)); must(modules.add("second.lua", source));
                auth::SessionConfig config; config.galactic_clock = true;
                control.scripts = must(auth::ScriptScheduler::create(config, modules));
                must(control.scripts.create_instance(instance, control.engine->plans().at(plan->definition).module));
                if (scenario == 1) {
                    must(control.scripts.service(executor));
                    expect(control.scripts.retired_instances() == std::vector<std::uint64_t>{instance},
                        "uncaught Lua failure retires the actual TaskForce instance before battle");
                }
                gc::GalacticSetup setup; setup.seed = 42; setup.players = {{1, 1, 0}, {2, 2, 0}};
                const auto snapshot = fixture_snapshot();
                setup.planets.assign(snapshot->planets().begin(), snapshot->planets().end());
                setup.fleets.assign(snapshot->fleets().begin(), snapshot->fleets().end());
                // Clock-only setups omit the native movement graph. Admission
                // needs its explicit opt-in even when conflict defers travel.
                if (!recorded) {
                    auto graphless = must(gc::GalacticSession::create(setup));
                    ga::NativeActions unavailable(graphless, std::make_shared<ai::GalacticFunding>(), {}, {});
                    unavailable.bind_admission_gate([] { return false; });
                    unavailable.bind_movement([](auto, const auto&) {
                        return eawr::core::Result<ga::NativeActions::MovementInput>::success({});
                    }, [] { return true; }, [](auto, auto) { return eawr::core::Result<void>::success(); });
                    ga::ActionRequest invalid; invalid.kind = ga::ActionKind::move;
                    invalid.player = 1; invalid.object = 50; invalid.target = 20;
                    const auto rejected = unavailable.admit(invalid);
                    expect(!graphless.graph() && !rejected && rejected.error().code == auth::codes::invalid_request
                        && rejected.error().message == "movement destination, policy or request identity is invalid",
                        "graph-less fixture rejects at movement admission before any battle callback");
                }
                setup.rules.maximum_fleet_movement_distance = eawr::sim::math::Fixed{};
                cp::ConflictRecord conflict; conflict.id = 1000; conflict.planet = 10;
                conflict.invader = 1; conflict.defender = 2;
                conflict.forces = {{51, 100, 1, 50}, {61, 100, 2, 60}};
                conflict.tactical.first_free_entity_id = 2000;
                conflict.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
                for (const auto& force : conflict.forces) {
                    eawr::sim::tactical::UnitState unit;
                    unit.entity_id = force.id; unit.type_id = force.type; unit.owner = force.owner;
                    conflict.tactical.units.push_back(unit);
                }
                eawr::sim::tactical::TacticalContent content;
                eawr::sim::tactical::DurabilityProfile profile; profile.type_id = 100;
                profile.max_hull = must(eawr::sim::math::Fixed::from_integer(100));
                content.durability.profiles.push_back(profile);
                auto game = recorded ? must(cp::GameSession::from_recording(*recorded, [content](const auto&) {
                    return eawr::core::Result<eawr::sim::tactical::TacticalContent>::success(content);
                }, {}, cp::ScriptRequirement::none)) : must(cp::GameSession::create(setup));
                std::shared_ptr<ga::NativeActions> actions;
                must(game.configure_galactic_ai_services([&](gc::GalacticSession& world) {
                    expect(world.graph() && world.graph()->node(20),
                        "battle fixture enables its native movement graph and destination before request admission");
                    actions = std::make_shared<ga::NativeActions>(world, std::make_shared<ai::GalacticFunding>(),
                        ga::NativeActions::Placement{}, std::map<gc::PlayerId, eawr::sim::math::Fixed>{});
                    actions->bind_admission_gate([] { return false; });
                    actions->bind_movement([](auto, const auto&) {
                        return eawr::core::Result<ga::NativeActions::MovementInput>::success({});
                    }, [] { return true; }, [](auto, auto) { return eawr::core::Result<void>::success(); });
                    ga::ActionRequest request; request.kind = ga::ActionKind::move;
                    request.player = 1; request.object = 50; request.target = 20; request.members = {51};
                    request.plan = plan_id; request.taskforce = taskforce; request.block = block;
                    const auto admitted = must(actions->admit(request));
                    expect(admitted.admitted && admitted.request == 1, "registered native TaskForce movement precedes battle entry");
                    cp::GalacticAiServices services;
                    services.state = std::make_shared<ai::GoalStateContributor>(control.engine, fixture_identity());
                    services.battle = ga::retained_movement_battle_listener(actions,
                        scenario == 2 ? std::shared_ptr<ai::Engine>{} : control.engine,
                        std::function<auth::ScriptScheduler*()>{[&] { return scenario == 0 ? nullptr : &control.scripts; }});
                    return eawr::core::Result<cp::GalacticAiServices>::success(std::move(services));
                }));
                if (!recorded) must(game.stage_battle(conflict, content, cp::ScriptRequirement::none));
                std::vector<std::string> hashes;
                for (unsigned tick = 0; tick < 3; ++tick) {
                    hashes.push_back(must(game.step(executor)).state_hash.get() + must(actions->state_hash())
                        + must(control.scripts.state_hash()));
                    expect(game.mode() == cp::SessionMode::battle && game.battle(),
                        "retired or missing callback owner never aborts campaign and the tactical child keeps running");
                }
                const auto notes = game.galaxy().load_notes();
                expect(notes.size() == (scenario == 3 ? 0U : 1U), "only unavailable callback owners emit a degradation note");
                if (!notes.empty()) expect(notes.front().code == "EAWR-LOAD-1236"
                    && notes.front().phase == eawr::core::LoadPhase::runtime && notes.front().object == "1",
                    "degraded battle notification has its reserved runtime LoadNote");
                expect(actions->movements().at(1).conflict_location == 10,
                    "skipping script delivery preserves native movement conflict bookkeeping");
                expect(ai::EngineContractAccess::pending_events(*control.engine) == (scenario == 3 ? 1U : 0U),
                    "only live owners receive an authored callback; degraded paths do not invent events");
                return std::pair{game.recording(), hashes};
            };
            const auto [recording, hashes] = run(nullptr);
            const auto [replayed, replay_hashes] = run(&recording);
            expect(hashes == replay_hashes && must(gc::write_replay(recording.commands)) == must(gc::write_replay(replayed.commands)),
                "degraded entry reconstructs identical game/native/script hashes and replay bytes");
            trace.insert(trace.end(), hashes.begin(), hashes.end());
        }
        if (workers == 1) reference = trace;
        else expect(trace == reference, "degraded battle entry and replay hashes agree on 1/2/4/8 workers");
    }
}
void movement_delivery_contracts() {
    std::string reference;
    const std::vector<lifecycle::PlayerSelection> selected{{1, "FIXTURE", "NORMAL", {}, {}}, {2, "FIXTURE", "NORMAL", {}, {}}};
    auth::ModuleManifest modules;
    const std::string source = "Begins=0; Ends=0; Pumps=0; function Main_Space_Conflict_Begin(tf) Begins=Begins+1 end; function Default_Space_Conflict_End(tf) Ends=Ends+1 end; function Worker() Pumps=Pumps+1 end; Create_Thread('Worker')";
    must(modules.add("first.lua", source)); must(modules.add("second.lua", source));
    for (auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        Owner control(fixture_setup(), fixture_data(), fixture_plans(), selected);
        control.scripts = must(auth::ScriptScheduler::create({}, modules));
        control.begin(fixture_snapshot(), executor, selected); policy(control, 1);
        const auto goal = must(control.engine->allocate_galactic_goal(1, 0, control.engine->target_of_object(10)->id));
        if (!goal) throw std::runtime_error("movement delivery supporting goal absent");
        const auto block = ai::EngineContractAccess::movement_fixture(*control.engine, goal->goal);
        const auto plan_id = control.engine->galactic_goal(1, goal->goal)->running_plan;
        const auto* plan = control.engine->plan(plan_id); const auto taskforce = plan->taskforces.front(), instance = plan->instance;
        must(control.scripts.create_instance(instance, control.engine->plans().at(plan->definition).module));
        const auto frame = control.engine->frame();
        expect(!control.engine->deliver_galactic_space_conflict(78, plan_id, taskforce, block, true, control.scripts), "wrong saved request association refuses");
        must(control.engine->deliver_galactic_space_conflict(77, plan_id, taskforce, block, true, control.scripts));
        must(control.engine->deliver_galactic_space_conflict(77, plan_id, taskforce, block, false, control.scripts));
        expect(ai::EngineContractAccess::pending_events(*control.engine) == 2 && control.engine->frame() == frame
            && control.scripts.completed_tick() == 0, "retained override/default callbacks queue without parent service");
        for (const auto name : {"Begins", "Ends", "Pumps"}) {
            const auto value = must(control.scripts.read_global(instance, name));
            expect(value && std::get<ai::Real>(value->data) == ai::real(0), "frozen parent has neither delivered handlers nor pumped Worker");
        }
        ai::GoalStateContributor saved(control.engine, fixture_identity()); const auto native = must(saved.capture());
        const auto lua = must(control.scripts.save());
        Owner restored(fixture_setup(), fixture_data(), fixture_plans(), selected);
        restored.scripts = must(auth::ScriptScheduler::create({}, modules)); must(restored.scripts.load(lua));
        restored.begin(fixture_snapshot(), executor, selected);
        ai::GoalStateContributor resumed(restored.engine, fixture_identity()); must(resumed.restore(native));
        expect(must(saved.state_hash()) == must(resumed.state_hash()), "native pending conflict callbacks retain exact IDs/order across load");
        std::uint64_t control_sequence{}, restored_sequence{};
        must(control.engine->commit_galactic_events(control.scripts, control_sequence));
        must(restored.engine->commit_galactic_events(restored.scripts, restored_sequence));
        auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
        expect(must(control.scripts.service(executor, options)).diagnostics.empty()
            && must(restored.scripts.service(executor, options)).diagnostics.empty(), "retained callbacks execute without a coroutine pump");
        for (const auto name : {"Begins", "Ends", "Pumps"}) {
            const auto a = must(control.scripts.read_global(instance, name)), b = must(restored.scripts.read_global(instance, name));
            const auto expected = ai::real(std::string_view(name) == "Pumps" ? 0 : 1);
            expect(a && b && std::get<ai::Real>(a->data) == expected && std::get<ai::Real>(b->data) == expected,
                "override/default handlers execute once and leave Worker unpumped");
        }
        const auto hash = must(control.scripts.state_hash());
        expect(hash == must(restored.scripts.state_hash()) && must(saved.state_hash()) == must(resumed.state_hash()), "actual native delivery continuation is exact after load");
        if (workers == 1) reference = hash; else expect(reference == hash, "actual native delivery equals on 1/2/4/8");
        must(control.scripts.remove_instance(instance));
        must(control.engine->deliver_galactic_space_conflict(77, plan_id, taskforce, block, true, control.scripts));
        expect(ai::EngineContractAccess::pending_events(*control.engine) == 0, "missing/finished TaskForce script receives no callback");
    }
}
}

namespace {
void shared_global_value_contracts() {
    std::string reference;
    const std::vector<lifecycle::PlayerSelection> selected{{1, "FIXTURE", "NORMAL", {}, {}}, {2, "FIXTURE", "NORMAL", {}, {}}};
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        Owner owner(fixture_setup(), fixture_data(), fixture_plans(), selected);
        owner.begin(fixture_snapshot(), executor, selected);
        ai::GoalStateContributor state(owner.engine, fixture_identity());
        const auto empty = must(state.capture());
        const auto empty_hash = must(state.record_sync_hash());
        const auto reader = [engine = owner.engine](const std::string& key) { return engine->global_value(key); };
        const auto make_scripts = [&](bool story) {
            auth::SessionConfig config; config.galactic_clock = true;
            auth::ModuleManifest manifest;
            const std::string text = story ? R"LUA(
Continued = false; Rejected = false
function main()
    while true do
        GlobalValue.Set('shared', 'serialized\000payload')
        GlobalValue.Set('flag', false)
        GlobalValue.Set('number', 42.5)
        GlobalValue.Set('empty', '')
        GlobalValue.Set('collision', 'story')
        coroutine.yield(true)
    end
end
function Unsupported()
    GlobalValue.Set('shared', {1, 2})
    Continued = true
end
function Oversized()
    GlobalValue.Set('shared', string.rep('x', 1048577))
    Continued = true
end
function Reject(wrapper)
    Rejected = not pcall(GlobalValue.Set, 'bad-wrapper', wrapper)
end
Create_Thread('main')
)LUA" : R"LUA(
Seen = nil; Flag = nil; Number = nil; Empty = nil
function main()
    while true do
        Seen = GlobalValue.Get('shared')
        Flag = GlobalValue.Get('flag')
        Number = GlobalValue.Get('number')
        Empty = GlobalValue.Get('empty')
        GlobalValue.Set('collision', 'ai')
        coroutine.yield(true)
    end
end
Create_Thread('main')
)LUA";
            must(manifest.add("global.lua", text));
            auto scripts = must(auth::ScriptScheduler::create(config, std::move(manifest)));
            if (story) must(auth::register_shared_global_values(scripts, reader));
            else {
                std::vector<eawr::core::Diagnostic> errors;
                foc::tactical_ai_detail::register_globals(scripts, owner.host, errors);
                expect(errors.empty(), "AI/object GlobalValue bindings install without collision");
            }
            must(scripts.create_instance(story ? 2 : 1, "global.lua"));
            if (!story) must(scripts.create_instance(foc::first_object_instance, "global.lua"));
            return scripts;
        };
        auto setup = gc::synthetic_galaxy({});
        auto session = must(auth::ScriptedGalacticSession::create(must(gc::GalacticSession::create(setup)),
            make_scripts(false), make_scripts(true)));
        // The bridge's story tail and the AI after-service tail share exactly one owner.
        class AiTail final : public auth::GalacticScriptEngine {
        public:
            explicit AiTail(std::shared_ptr<ai::Engine> value) : engine(std::move(value)) {}
            eawr::core::Result<auth::ServiceOptions> before_service(const eawr::sim::PartitionExecutor&,
                const gc::GalacticSession&, const gc::GalacticTick&, auth::ScriptScheduler&) override {
                return eawr::core::Result<auth::ServiceOptions>::success({});
            }
            eawr::core::Result<void> after_service(auth::ServiceReport& report) override { return engine->after_service(report); }
            std::shared_ptr<ai::Engine> engine;
        };
        must(session.set_ai_engine(std::make_shared<AiTail>(owner.engine)));
        session.set_global_value_committer([engine = owner.engine](auth::ServiceReport& report) { engine->commit_global_values(report); });
        const auto first = must(session.step(executor));
        expect(first.ai && first.ai->diagnostics.empty() && first.story.diagnostics.empty(), "story Set and AI/object Get service without a fault");
        const auto missing = must(session.ai_scheduler().read_global(1, "Seen"));
        expect(missing && std::holds_alternative<std::monostate>(missing->data), "AI reads missing key as nil before story commit");
        const auto second = must(session.step(executor));
        expect(second.ai && second.ai->commands.empty() && second.story.commands.empty(), "shared writes are consumed instead of routed as world orders");
        for (const auto id : {std::uint64_t{1}, foc::first_object_instance}) {
            const auto seen = must(session.ai_scheduler().read_global(id, "Seen"));
            const auto flag = must(session.ai_scheduler().read_global(id, "Flag"));
            const auto number = must(session.ai_scheduler().read_global(id, "Number"));
            const auto blank = must(session.ai_scheduler().read_global(id, "Empty"));
            expect(seen && std::get<std::string>(seen->data) == std::string("serialized\0payload", 18), "story string reaches AI and object instances with embedded NUL intact");
            expect(flag && !std::get<bool>(flag->data), "stored false differs from missing key");
            expect(number && std::get<ai::Real>(number->data) == ai::Real::from_repr(0x4045400000000000ULL), "story number reaches both host kinds");
            expect(blank && std::get<std::string>(blank->data).empty(), "empty string differs from missing key");
        }
        expect(std::get<std::string>(owner.engine->global_value("collision")->data) == "story", "AI then story collision resolves in stable service order");
        const auto capture = must(state.capture());
        const auto hash = must(state.state_hash());
        const auto sync = must(state.record_sync_hash());
        expect(sync == must(state.record_sync_hash(true)) && sync != empty_hash, "typed globals enter both incremental and cold native hashes");
        Owner restored(fixture_setup(), fixture_data(), fixture_plans(), selected);
        must(restored.world->bind(fixture_snapshot(), {}, executor, 0, 1000));
        ai::GoalStateContributor resumed(restored.engine, fixture_identity());
        must(resumed.restore(capture));
        expect(must(resumed.state_hash()) == hash && must(resumed.record_sync_hash()) == sync,
            "optional scalar checkpoint tail restores exact state and sync hashes");
        expect(std::get<std::string>(restored.engine->global_value("shared")->data) == std::string("serialized\0payload", 18), "load restores serialized string bytes");
        for (const bool truncate : {false, true}) {
            auto corrupt = capture;
            auto& bytes = corrupt.components.at(std::string(ai::GoalStateContributor::component));
            if (truncate) bytes.pop_back(); else bytes.push_back(0);
            expect(!resumed.restore(corrupt) && must(resumed.state_hash()) == hash,
                "truncated or trailing scalar tail refuses without mutating the live registry");
        }
        const auto write = [&](std::string key, auth::Value value) {
            auth::ServiceReport report;
            auth::ScriptCommand command;
            command.issuer = 2; command.verb = std::string(auth::global_value_command);
            command.arguments = {auth::Value::text(std::move(key)), std::move(value)};
            report.commands.push_back(std::move(command));
            owner.engine->commit_global_values(report);
        };
        auto phase = must(state.begin_phase());
        write("shared", auth::Value{true}); write("number", auth::Value{}); write("new", auth::Value::text("new"));
        expect(must(state.record_sync_hash()) != sync, "type changes and deletion change native hash");
        must(phase->rollback());
        expect(must(state.state_hash()) == hash && must(state.record_sync_hash(true)) == sync,
            "native phase rollback restores strings booleans numbers and removed/added keys");
        write("shared", auth::Value{}); write("flag", auth::Value{}); write("number", auth::Value{});
        write("empty", auth::Value{}); write("collision", auth::Value{});
        expect(!owner.engine->global_value("shared"), "nil deletes a registry key");
        expect(must(state.record_sync_hash()) == must(state.record_sync_hash(true)), "deletion keeps incremental and cold hashes equal");
        expect(must(state.capture()).components == empty.components, "empty optional tail emits exactly the original empty-registry checkpoint bytes");
        const auto combined = hash + sync + must(session.script_state_hash());
        if (workers == 1) reference = combined;
        else expect(combined == reference, "story/AI/object shared globals are deterministic on 1/2/4/8 workers");
        // A separate game gets an empty registry, independently of the first game.
        Owner fresh(fixture_setup(), fixture_data(), fixture_plans(), selected);
        expect(!fresh.engine->global_value("shared"), "GlobalValue has game scope");
        auth::ScriptEvent event;
        event.key = {session.story_scheduler().completed_tick() + 1, auth::first_simulation_producer, 2, 0};
        event.target = 2; event.kind = auth::ScriptEvent::Kind::call; event.name = "Unsupported";
        must(session.story_scheduler().submit_event(event));
        event.key.sequence = 1; event.name = "Reject";
        event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 51}}};
        must(session.story_scheduler().submit_event(event));
        const auto degraded = must(session.step(executor));
        const auto continued = must(session.story_scheduler().read_global(2, "Continued"));
        const auto rejected = must(session.story_scheduler().read_global(2, "Rejected"));
        expect(continued && std::get<bool>(continued->data), "unsupported sequential table continues the invocation");
        expect(rejected && std::get<bool>(rejected->data), "engine wrapper is rejected with a catchable script error");
        expect(degraded.story.load_notes.size() == 1 && !owner.engine->global_value("bad-wrapper"),
            "unsupported table gets one LoadNote and rejected wrapper never reaches the registry");
        event.key.tick = session.story_scheduler().completed_tick() + 1;
        event.key.sequence = 0; event.name = "Oversized"; event.arguments.clear();
        must(session.story_scheduler().submit_event(event));
        const auto oversized = must(session.step(executor));
        expect(oversized.story.load_notes.size() == 1 && bool(state.record_sync_hash()),
            "Lua oversized string safely refuses with a LoadNote and preserves healthy native hashing");
        expect(std::get<std::string>(owner.engine->global_value("shared")->data) == std::string("serialized\0payload", 18)
            && bool(session.step(executor)), "oversized Lua Set preserves the prior value and the game keeps ticking");

    }
}

void global_value_admission_contracts() {
    const eawr::sim::InlineExecutor executor;
    const std::vector<lifecycle::PlayerSelection> selected{{1, "FIXTURE", "NORMAL", {}, {}}, {2, "FIXTURE", "NORMAL", {}, {}}};
    const auto command = [](std::string key, auth::Value value) {
        auth::ScriptCommand out;
        out.issuer = 2; out.verb = std::string(auth::global_value_command);
        out.arguments = {auth::Value::text(std::move(key)), std::move(value)};
        return out;
    };
    const auto write = [&](Owner& owner, std::string key, auth::Value value) {
        auth::ServiceReport report; report.commands.push_back(command(std::move(key), std::move(value)));
        owner.engine->commit_global_values(report); return report;
    };
    constexpr auto text_limit = lifecycle::native_state_max_text_bytes;
    {
        Owner owner(fixture_setup(), fixture_data(), fixture_plans(), selected);
        owner.begin(fixture_snapshot(), executor, selected);
        ai::GoalStateContributor state(owner.engine, fixture_identity());
        const auto initial = must(state.record_sync_hash());
        const auto refused = write(owner, "large", auth::Value::text(std::string(text_limit + 1, 'x')));
        expect(refused.load_notes.size() == 1 && !owner.engine->global_value("large")
            && must(state.record_sync_hash()) == initial, "review repro: oversize value refuses before native mutation");
        auto phase = must(state.begin_phase()); must(phase->commit());
        expect(write(owner, "large", auth::Value::text(std::string(text_limit, 'x'))).load_notes.empty(),
            "string value at the archive length limit is admitted");
        const auto at_limit = must(state.record_sync_hash());
        expect(write(owner, "large", auth::Value::text(std::string(text_limit + 1, 'y'))).load_notes.size() == 1
            && must(state.record_sync_hash()) == at_limit,
            "one-past string refusal retains the prior value and native hash");
        const auto before_replace = state.record_byte_upper_bound();
        expect(write(owner, "large", auth::Value::text("short")).load_notes.empty()
            && state.record_byte_upper_bound() < before_replace, "replacement credits the displaced string's retained bytes");
        const std::string boundary_key(text_limit, 'k');
        expect(write(owner, boundary_key, auth::Value{false}).load_notes.empty(), "key at the archive length limit is admitted");
        const auto key_hash = must(state.record_sync_hash());
        expect(write(owner, std::string(text_limit + 1, 'k'), auth::Value{true}).load_notes.size() == 1
            && must(state.record_sync_hash()) == key_hash, "one-past key is refused before publication");
        write(owner, boundary_key, auth::Value{}); write(owner, "large", auth::Value{});
        expect(must(state.record_sync_hash()) == initial, "deletion restores the registry's exact original native state");
    }
    {
        Owner owner(fixture_setup(), fixture_data(), fixture_plans(), selected);
        owner.begin(fixture_snapshot(), executor, selected);
        ai::GoalStateContributor state(owner.engine, fixture_identity());
        auth::ServiceReport batch;
        constexpr auto limit = lifecycle::native_state_max_rows;
        batch.commands.reserve(limit + 1);
        for (std::size_t index = 0; index <= limit; ++index)
            batch.commands.push_back(command(std::to_string(index), auth::Value::number(ai::real(1))));
        owner.engine->commit_global_values(batch);
        expect(batch.load_notes.size() == 1 && owner.engine->global_value(std::to_string(limit - 1))
            && !owner.engine->global_value(std::to_string(limit)), "entry count admits the limit and refuses limit plus one");
        expect(write(owner, "0", auth::Value{false}).load_notes.empty(), "replacement at the entry cap is admitted");
        write(owner, "0", auth::Value{});
        expect(write(owner, std::to_string(limit), auth::Value{true}).load_notes.empty(), "deletion frees one entry for a later write");
        expect(bool(state.record_sync_hash()), "entry-cap refusals leave native hashing usable");
        auto phase = must(state.begin_phase()); must(phase->commit());
    }
    {
        Owner owner(fixture_setup(), fixture_data(), fixture_plans(), selected);
        owner.begin(fixture_snapshot(), executor, selected);
        ai::GoalStateContributor state(owner.engine, fixture_identity());
        auth::ServiceReport batch;
        // One bounded batch crosses the aggregate budget without quadratic rehashes.
        const auto chunks = foc::galactic_ai::native_state_max_bytes / text_limit;
        for (std::size_t index = 0; index < chunks; ++index)
            batch.commands.push_back(command("chunk_" + std::to_string(index), auth::Value::text(std::string(text_limit, 'x'))));
        owner.engine->commit_global_values(batch);
        expect(!batch.load_notes.empty() && bool(state.record_sync_hash()), "aggregate retained-byte refusal is safe");
        const auto remaining = foc::galactic_ai::native_state_max_bytes - state.record_byte_upper_bound();
        const std::string key = "fill";
        const auto framing = 2 * (8 + key.size()) + 9;
        expect(remaining > framing + 4 && remaining - framing <= text_limit, "fixture leaves a bounded scalar-sized remainder");
        if (remaining > framing + 4 && remaining - framing <= text_limit) {
            const auto length = remaining - framing;
            expect(write(owner, key, auth::Value::text(std::string(length, 'f'))).load_notes.empty()
                && state.record_byte_upper_bound() == foc::galactic_ai::native_state_max_bytes,
                "aggregate admission accepts exactly the native record budget");
            const auto full = must(state.record_sync_hash());
            expect(write(owner, key, auth::Value::text(std::string(length + 1, 'g'))).load_notes.size() == 1
                && must(state.record_sync_hash()) == full, "one-past aggregate replacement preserves the prior bytes and hash");
            expect(bool(state.capture()), "registry admitted at the aggregate bound still checkpoints");
            write(owner, key, auth::Value{});
            expect(write(owner, "refill", auth::Value::text(std::string(length - 4, 'r'))).load_notes.empty(),
                "delete credits aggregate bytes before the next admission");
        }
        auto phase = must(state.begin_phase()); must(phase->commit());
    }
    {
        Owner owner(fixture_setup(), fixture_data(), fixture_plans(), selected);
        owner.begin(fixture_snapshot(), executor, selected);
        ai::GoalStateContributor state(owner.engine, fixture_identity());
        lifecycle::TemplatePolicy populated;
        constexpr auto cap = lifecycle::native_state_max_rows;
        for (std::size_t index = 0; index < cap; ++index) {
            const auto key = std::to_string(index);
            populated.goal_categories[key] = {true, 1};
            populated.plan_categories[key] = {true, 1};
            populated.plan_types[key] = {true, 1};
        }
        must(owner.engine->apply_galactic_policy(1, populated));
        must(state.capture());
        auth::ServiceReport batch;
        batch.commands.reserve(cap);
        for (std::size_t index = 0; index < cap; ++index)
            batch.commands.push_back(command(std::to_string(index), auth::Value::number(ai::real(1))));
        owner.engine->commit_global_values(batch);
        expect(!batch.load_notes.empty() && batch.load_notes.size() < cap,
            "shared archive node exhaustion refuses only writes beyond the remaining capacity");
        bool checkpoint_ok{};
        try { checkpoint_ok = bool(state.capture()); }
        catch (const std::exception&) { }
        expect(checkpoint_ok, "admitted globals preserve canonical checkpoint serialization");
        bool cold_hash_ok{};
        try { cold_hash_ok = bool(state.record_sync_hash(true)); }
        catch (const std::exception&) { }
        expect(cold_hash_ok, "admitted globals preserve the independent native hash");
        const auto full = must(state.record_sync_hash());
        const auto next_key = std::to_string(cap - batch.load_notes.size());
        expect(write(owner, next_key, auth::Value{true}).load_notes.size() == 1
            && must(state.record_sync_hash()) == full,
            "one-past shared node quota retains the prior registry and hash");
        expect(write(owner, "0", auth::Value::text("replacement")).load_notes.empty(),
            "scalar replacement does not consume another shared archive node");
        auto phase = must(state.begin_phase());
        write(owner, "0", auth::Value{});
        expect(write(owner, next_key, auth::Value{false}).load_notes.empty() && bool(state.capture()),
            "deletion frees exactly one shared node for a later typed scalar");
        must(phase->rollback());
        expect(write(owner, next_key, auth::Value{false}).load_notes.size() == 1
            && bool(state.capture()) && bool(state.record_sync_hash(true)),
            "rollback restores shared node admission and canonical/cold serialization");
    }
}
} // namespace

void goal_state_contracts() {
    shared_global_value_contracts();
    global_value_admission_contracts();
    std::string reference;
    const std::vector<lifecycle::PlayerSelection> selections{{1, "FIXTURE", "NORMAL", {}, {}}, {2, "FIXTURE", "NORMAL", {}, {}}};
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        Owner control(fixture_setup(), fixture_data(), fixture_plans(), selections);
        control.begin(fixture_snapshot(), executor, selections); policy(control, 1); policy(control, 2);
        expect(!control.engine->allocate_galactic_goal(1, 99, 1), "unknown function refuses");
        expect(!control.engine->allocate_galactic_goal(1, 0, 999), "unknown target refuses");
        const auto first = must(control.engine->allocate_galactic_goal(1, 0, control.engine->target_of_object(10)->id));
        const auto other = must(control.engine->allocate_galactic_goal(2, 0, control.engine->target_of_object(20)->id));
        expect(first && other && first->goal == other->goal && first->potential_plan != other->potential_plan, "player-local goals and engine-wide plan identities distinct");
        must(control.engine->reserve_galactic_object(1, first->goal, 51));
        must(control.engine->reserve_galactic_object(1, first->goal, 51));
        expect(!control.engine->reserve_galactic_object(2, other->goal, 51), "foreign owned object refuses");
        expect(control.engine->galactic_reservation_valid(1, 51, first->potential_plan), "E reservation validator uses actual retained mapping");
        expect(!control.engine->galactic_reservation_valid(2, 51, first->potential_plan), "potential-plan owner cannot change");
        ai::EngineContractAccess::running(*control.engine, first->goal);
        ai::GoalStateContributor saved(control.engine, fixture_identity());
        const auto bytes = must(saved.capture()); const auto hash = must(saved.state_hash());
        const auto report_count = control.engine->records().size();
        for (const bool throws : {false, true}) for (const bool after_mutation : {false, true}) {
            auto phase = must(saved.begin_phase());
            expect(phase->work().retained_records == 0 && phase->work().retained_bytes == 0,
                "native goal phase begins without a retained-record scan, copy or archive");
            expect(!saved.begin_phase(), "native goal phase rejects nesting");
            bool caught{};
            try {
                const auto failed = [&]() -> eawr::core::Result<void> {
                    if (after_mutation) {
                        must(control.engine->release_galactic_goal(2, other->goal));
                        expect(bool(must(control.engine->allocate_galactic_goal(2, 0, other->target))),
                            "fault fixture changes actual goal identities and synchronized RNG");
                        ai::EngineContractAccess::phase_global(*control.engine, 7);
                        const auto retained = phase->work().retained_records;
                        ai::EngineContractAccess::phase_global(*control.engine, 9);
                        expect(phase->work().retained_records == retained && phase->work().retained_bytes > 0,
                            "repeated native writes retain only the first displaced record");
                        expect(control.engine->records().size() == report_count,
                            "native reports stay private during a speculative phase");
                        expect(must(saved.state_hash()) != hash, "native fault fixture really changes canonical fields");
                    }
                    if (throws) throw std::runtime_error("native phase fixture");
                    eawr::core::Diagnostic error; error.code = "native-phase-fixture";
                    return eawr::core::Result<void>::failure(std::move(error));
                }();
                expect(!failed && failed.error().code == "native-phase-fixture", "native Result failure keeps its diagnostic");
            } catch (const std::runtime_error& error) { caught = std::string_view(error.what()) == "native phase fixture"; }
            expect(caught == throws, "native deterministic exception identity preserved");
            must(phase->rollback());
            expect(must(saved.capture()).components == bytes.components && must(saved.state_hash()) == hash,
                "native rollback restores exact goal/RNG/policy/history/runtime/event/order/credit archive on 1/2/4/8");
            expect(control.engine->records().size() == report_count, "native rollback discards reports without truncating old history");
            expect(!phase->rollback(), "native goal phase settles once");
        }
        {
            auto phase = must(saved.begin_phase());
            ai::EngineContractAccess::phase_global(*control.engine, 11);
            must(phase->commit());
            expect(control.engine->records().size() == report_count + 1, "native retry publishes its report once");
            expect(!phase->commit(), "native retry cannot publish twice");
            // Restore the fixture baseline for the pre-existing continuation checks.
            must(saved.restore(bytes));
        }
        const auto observations = control.engine->galactic_goal_observations(1);
        expect(observations.size() == 1 && observations.front().goal == first->goal
            && observations.front().target == first->target && observations.front().function == "GOAL"
            && observations.front().desire == "1" && observations.front().active && observations.front().planned
            && observations.front().running_plan == control.engine->galactic_goal(1, first->goal)->running_plan
            && observations.front().plan == first->definition, "goal observer reports target, desire and selected running plan");
        expect(control.engine->galactic_goal_observations(99).empty(), "goal observer has no rows for an unknown player");
        expect(must(saved.capture()).components == bytes.components && must(saved.state_hash()) == hash,
            "goal observation leaves native state and digest unchanged");
        if (workers == 1) reference = hash; else expect(hash == reference, "full native state hash equals across 1/2/4/8");
        Owner resumed(fixture_setup(), fixture_data(), fixture_plans(), selections);
        must(resumed.world->bind(fixture_snapshot(), {}, executor, 0, 1000));
        ai::GoalStateContributor restored(resumed.engine, fixture_identity()); must(restored.restore(bytes));
        expect(must(restored.capture()).components == bytes.components && must(restored.state_hash()) == hash, "all retained fields re-encode byte-identically into fresh owner");
        expect(resumed.engine->galactic_reservation_valid(1, 999999, first->potential_plan), "pending absent actor reservation survives load");
        const auto running = resumed.engine->galactic_goal(1, first->goal);
        expect(running && running->running_plan && resumed.engine->galactic_goal_for_plan(1, running->running_plan) == first->goal, "running plan resolves exact funding goal");
        expect(running && resumed.engine->galactic_goal_for_plan(1, running->potential_plan) == first->goal, "potential plan resolves same retained goal");
        expect(running && !resumed.engine->galactic_reservation_valid(1, 999999, running->running_plan), "running plan ID cannot stand in for potential reservation ID");
        const auto assert_rejected = [&](lifecycle::NativeState bad) {
            expect(!restored.restore(bad), "corrupt native state refuses restore");
            expect(must(restored.state_hash()) == hash, "restore refusal leaves replacement owner unchanged");
        };
        auto bad = bytes; bad.version = 2; assert_rejected(bad);
        bad = bytes; bad.components.clear(); assert_rejected(bad);
        bad = bytes; bad.components.at("galactic-goals").push_back(0); assert_rejected(bad);
        const auto& payload = bytes.components.at("galactic-goals");
        for (const auto length : {std::size_t{0}, std::size_t{8}, payload.size() / 2, payload.size() - 1}) {
            bad = bytes; bad.components.at("galactic-goals").resize(length); assert_rejected(bad);
        }
        constexpr std::size_t header = 8 + std::string_view("eawr-galactic-goals-v3").size() + 4 + 8 + 64 + 8 + 64;
        bad = bytes; bad.components.at("galactic-goals")[header] = 2; assert_rejected(bad);
        bad = bytes;
        std::fill_n(bad.components.at("galactic-goals").begin() + header + 1 + 4 + 6 * 8, 8, std::uint8_t{0});
        assert_rejected(bad); // next plan allocator cannot overlap retained IDs
        ai::GoalStateContributor wrong_closure(resumed.engine, std::string(64, 'a'));
        expect(!wrong_closure.restore(bytes), "different exact stock closure refuses");
        auto changed_plans = fixture_plans(); changed_plans[0].name = "DIFFERENT";
        Owner changed(fixture_setup(), fixture_data(), changed_plans, selections);
        must(changed.world->bind(fixture_snapshot(), {}, executor, 0, 1000));
        ai::GoalStateContributor changed_config(changed.engine, fixture_identity());
        expect(!changed_config.restore(bytes), "different plan order/definition refuses even with caller digest unchanged");
        // Consume the next synchronized draw in both owners after releasing a
        // provisional goal; exact allocators never recycle the released ID.
        must(control.engine->release_galactic_goal(2, other->goal)); must(resumed.engine->release_galactic_goal(2, other->goal));
        const auto next_control = must(control.engine->allocate_galactic_goal(2, 0, other->target));
        const auto next_resumed = must(resumed.engine->allocate_galactic_goal(2, 0, other->target));
        expect(next_control && next_resumed && *next_control == *next_resumed && next_control->goal > other->goal, "continued draws and IDs match uninterrupted owner");
        expect(must(saved.state_hash()) == must(restored.state_hash()), "continued full engine state matches");
        std::uint64_t sequence{};
        must(resumed.engine->begin_galactic_barrier(*resumed.world, {1, 1}, resumed.scripts, sequence, selections, &executor));
        expect(resumed.engine->galactic_goal(1, first->goal) == running, "first resumed barrier does not reinitialize saved goals");
        expect(!resumed.engine->service_galactic_system(1, lifecycle::Service::goals, resumed.scripts, sequence), "composed selector refusal remains explicit");
        // Supporting native-service proof: the saved synthetic plan has no live
        // Lua threads, so admitted planning finishes it using the actual writer.
        control.engine->set_galactic_funding(std::make_shared<ai::GalacticFunding>());
        resumed.engine->set_galactic_funding(std::make_shared<ai::GalacticFunding>());
        std::uint64_t control_sequence{};
        must(control.engine->begin_galactic_barrier(*control.world, {1, 1}, control.scripts, control_sequence, selections, &executor));
        must(control.engine->service_galactic_system(1, lifecycle::Service::planning, control.scripts, control_sequence));
        must(resumed.engine->service_galactic_system(1, lifecycle::Service::planning, resumed.scripts, sequence));
        expect(!control.engine->plan(running->running_plan) && !resumed.engine->plan(running->running_plan), "first resumed admitted planning completes retained plan");
        expect(must(saved.state_hash()) == must(restored.state_hash()), "actual finish writer continuation matches restored owner");
        const auto completed = must(saved.capture());
        must(control.engine->service_galactic_system(1, lifecycle::Service::planning, control.scripts, control_sequence));
        expect(must(saved.capture()).components == completed.components, "repeated plan service cannot register completed plan twice");
        must(control.engine->service_galactic_system(1, lifecycle::Service::learning, control.scripts, control_sequence));
        must(resumed.engine->service_galactic_system(1, lifecycle::Service::learning, resumed.scripts, sequence));
        expect(must(saved.state_hash()) == must(restored.state_hash()), "first resumed sweep preserves exact histories and cumulative counters");
        expect(ai::EngineContractAccess::completed_learning(*control.engine, first->target), "actual plan finish increments all four lifetime tables and appends retained false/expiry");
        Owner after_completion(fixture_setup(), fixture_data(), fixture_plans(), selections);
        must(after_completion.world->bind(fixture_snapshot(), {}, executor, 1, 1000));
        ai::GoalStateContributor completed_restore(after_completion.engine, fixture_identity());
        must(completed_restore.restore(must(saved.capture())));
        expect(must(completed_restore.state_hash()) == must(saved.state_hash()), "histories produced by actual finish writer survive a fresh save/load");
        const auto learned_draw = must(control.engine->allocate_galactic_goal(1, 0, first->target));
        const auto restored_draw = must(after_completion.engine->allocate_galactic_goal(1, 0, first->target));
        expect(learned_draw && learned_draw == restored_draw, "next synchronized weighted draw consumes restored completed-plan statistics");
        expect(must(completed_restore.state_hash()) == must(saved.state_hash()), "learned weighted draw continuation equals fresh restored owner");
    }
    movement_delivery_contracts();
    degraded_battle_entry_contracts();
    galactic_budget_and_desire_stub_contracts();
}

void installed_goal_state(const eawr::vfs::Vfs& filesystem, ai::AiData ai_data, std::vector<ai::PlanDef> plans, std::string identity) {
    const auto loaded = must(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(filesystem, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Empire";
    const auto* source = data.campaign(campaign_name); if (!source) throw std::runtime_error("stock campaign absent");
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source->starting_active_player;
        if (human) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
    const auto planned = must(cp::plan_live_campaign_setup(filesystem, data, types, campaign_name, options, "Normal_Default"));
    const auto services = must(cp::prepare_live_campaign_services(filesystem, loaded.catalog, data, types, planned, "Normal_Default"));
    auto campaign = must(cp::CampaignRoot::create(planned, services));
    ai::GalacticPerceptionSetup setup; setup.seed = 42;
    gc::PlayerId empire{};
    for (const auto& binding : planned.player_bindings()) {
        const auto* faction = data.faction(binding.faction);
        ai::GalacticPlayerFacts row{binding.player.player_id, binding.player.player_id, binding.faction, "Normal", faction->is_neutral.value_or(false), false, false, 0};
        if (binding.faction == "Empire") { empire = row.player; row.ai = true; }
        for (const auto& home : planned.homes()) if (home.player == row.player && home.planet) row.home = *home.planet;
        setup.players.push_back(row);
    }
    if (!empire) throw std::runtime_error("stock Empire player absent");
    for (const auto& location : planned.locations()) if (location.planet)
        setup.planets.push_back({location.planet->id, location.original_type, ai::crc32(ai::upper_case(location.original_type)), {}, {}, {}, {}, {}});
    for (const auto& binding : planned.type_bindings()) {
        const auto* type = planned.original_type(binding.id); if (!type) continue;
        foc::AiType facts; facts.type_id = binding.id; facts.name = ai::upper_case(type->name); setup.types.push_back(std::move(facts));
    }
    const std::vector<lifecycle::PlayerSelection> selected{{empire, "BASICEMPIRE", "NORMAL", {}, {}}};
    std::vector<lifecycle::PlanInput> registrations;
    for (const auto& plan : plans) registrations.push_back({plan.name, plan.goals});
    const auto& player_type = ai_data.players.at("BASICEMPIRE");
    const auto policy = lifecycle::apply_template(ai_data, registrations, {}, ai_data.templates.at(player_type.galactic_templates.front()));
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        Owner control(setup, ai_data, plans, selected);
        auto& galaxy = campaign.game()->galaxy();
        control.begin(galaxy.snapshot(), executor, selected, 0, galaxy.next_entity_id());
        must(control.engine->apply_galactic_policy(empire, policy));
        for (std::size_t index = 0; index < ai::EngineContractAccess::function_count(*control.engine, empire); ++index) {
            // GLOBAL versus target-local application is taken from the actual
            // selected function entry, never from a hand-picked test equation.
            const auto function = [&]() {
                std::vector<ai::GoalFunction> entries;
                for (const auto& set : player_type.function_sets) if (ai_data.function_sets.contains(set))
                    for (const auto& f : ai_data.function_sets.at(set))
                        if (ai_data.goals.contains(f.goal) && ai_data.goals.at(f.goal).mode == "GALACTIC") entries.push_back(f);
                return entries.at(index);
            }();
            const auto& goal = ai_data.goals.at(function.goal);
            if (goal.application.contains("GLOBAL")) must(control.engine->allocate_galactic_goal(empire, index, 0));
            else for (const auto target : *control.engine->target_list(empire)) {
                if (!control.engine->target_matches(empire, goal.application, control.engine->target(target))) continue;
                must(control.engine->allocate_galactic_goal(empire, index, target));
            }
        }
        const auto goals = control.engine->galactic_goals(empire);
        expect(!goals.empty(), "Normal Empire stock turn allocates positive authored goals with production provider");
        if (goals.empty()) throw std::runtime_error("stock allocation produced no goals");
        // Identity-only reservation proof on a real owned campaign unit. This
        // is explicitly not a completed mixed-force selection/paid purchase.
        const auto view = control.engine->completed_view();
        const auto unit = std::find_if(view->units.begin(), view->units.end(), [&](const auto& u) { return u.owner == empire; });
        expect(unit != view->units.end(), "stock campaign supplies real owned reservation object");
        if (unit == view->units.end()) throw std::runtime_error("stock reservation object absent");
        must(control.engine->reserve_galactic_object(empire, goals.front().goal, unit->id));
        ai::GoalStateContributor saved(control.engine, identity); const auto native = must(saved.capture());
        const auto encoded = must(lifecycle::encode_native_state(native));
        const auto decoded = must(lifecycle::decode_native_state(encoded));
        Owner resumed(setup, ai_data, plans, selected);
        must(resumed.world->bind(galaxy.snapshot(), {}, executor, 0, galaxy.next_entity_id()));
        must(resumed.world->restore(control.world->state()));
        ai::GoalStateContributor restored(resumed.engine, identity); must(restored.restore(decoded));
        expect(resumed.engine->galactic_goals(empire) == goals && resumed.engine->galactic_reservation_valid(empire, unit->id, goals.front().potential_plan), "stock IDs and reservation mapping restore exact");
        expect(must(restored.state_hash()) == must(saved.state_hash()), "installed NativeState envelope round-trip matches goal/RNG/plan hash");
        const auto hash = must(saved.state_hash());
        if (workers == 1) reference = hash; else expect(hash == reference, "Normal Empire allocation/reservation state equals on 1/2/4/8");
        // Follow allocation after load with the next synchronized plan draw.
        const auto victim = goals.back();
        const auto function = ai::EngineContractAccess::goal_function(*control.engine, empire, victim.goal);
        must(control.engine->release_galactic_goal(empire, victim.goal)); must(resumed.engine->release_galactic_goal(empire, victim.goal));
        const auto continued = must(control.engine->allocate_galactic_goal(empire, function, victim.target));
        const auto continued_restore = must(resumed.engine->allocate_galactic_goal(empire, function, victim.target));
        expect(continued && continued_restore && continued == continued_restore && continued->goal > victim.goal,
            "installed next synchronized plan draw and goal allocator match uninterrupted owner");
        expect(must(saved.state_hash()) == must(restored.state_hash()), "installed continuation Engine hash equals uninterrupted owner");
        std::uint64_t sequence{};
        must(resumed.engine->begin_galactic_barrier(*resumed.world, {1, 1}, resumed.scripts, sequence, selected, &executor));
        expect(resumed.engine->galactic_goals(empire) == control.engine->galactic_goals(empire), "first installed resumed service preserves allocation identities");
        std::cout << "Normal Empire allocation: " << goals.size() << " real goals, reservation " << unit->id << ", workers=" << workers << '\n';
    }
}
int galactic_goal_state_failures() { return failures; }
