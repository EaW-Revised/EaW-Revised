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
    static std::uint64_t movement_fixture(Engine& e, std::uint64_t goal) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_replaced(); e.mark_native_state_changed();
        running(e, goal);
        auto& block = e.players_.front().blocks.begin()->second;
        block.kind = Block::Kind::move; block.galactic_request = 77; block.galactic_actor = 50;
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

void goal_state_contracts() {
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
