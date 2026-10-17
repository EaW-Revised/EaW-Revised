#pragma once
// Qualified synthetic provider proof. Installed content/stock Lua integration
// remains a separate acceptance test; these facts do not stand in for retail.
#include "galactic_support.hpp"
#include "galactic_ai_world.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_execution.hpp"
#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_movement_runtime.hpp"
#include "galactic_ai_selection_content.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <set>
#include <stdexcept>

namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace ga = foc::galactic_ai;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
using eawr::sim::math::Fixed;
namespace selector_test {
inline void expect(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
template<class T> inline T must(eawr::core::Result<T> r) { if (!r) throw std::runtime_error(r.error().message); return std::move(r).value(); }
inline void must(eawr::core::Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
inline Fixed f(std::int64_t n) { return must(Fixed::from_integer(n)); }
inline void record_digest_contract(const ga::StateContributor& owner, std::string_view message) {
    const auto warm = must(owner.record_sync_hash());
    expect(!warm.empty() && warm == must(owner.record_sync_hash(true)), message);
}
inline gc::GalacticSetup setup() {
    gc::GalacticSetup out;
    out.players = {{1, 1, 0}}; out.planets = {{10, {}, 1}};
    out.player_economy = gc::PlayerEconomySetup{{{1, f(200), f(1000), 2, 5}}};
    out.fiscal = gc::FiscalSetup{f(2500), {{1, true, f(1)}}, {{10, {}}}};
    out.rules.fiscal_cycle_frames = 10000;
    out.rules.maximum_fleet_movement_distance = Fixed{};
    out.fleet_control = gc::FleetControlSetup{{}, {{100, false, true, false}}, {}};
    out.production = gc::ProductionSetup{5, {{1, false, true, true, f(50), {}, {}, {}, {}}}, {}};
    gc::PlanetProduction p; p.planet = 10; p.target_owner = p.starbase_owner = p.ground_occupier = 1;
    p.ground_slots = p.space_slots = 3; p.maximum_ground_forces = 10;
    p.enabled = p.surface_accessible = true;
    gc::ProductionOffer offer; offer.type = 100; offer.price = f(100); offer.seconds = f(1);
    offer.tech = 2; offer.affiliated = offer.direct_build_allowed = true;
    p.offers = {offer}; out.production->planets = {p};
    return out;
}
inline ai::AiData data() {
    ai::AiData out;
    out.equations = must(ai::EquationSet::parse({{"fixture.xml", "<Equations><Positive>1</Positive></Equations>"}}, {}));
    ai::GoalType goal; goal.name = "GOAL"; goal.mode = "GALACTIC"; goal.category = "OFFENSIVE";
    goal.reachability = "ANY";
    goal.application = {"FRIENDLY"}; goal.global_exclusions = {"EXCLUDED"};
    out.goals["GOAL"] = goal; out.goal_categories["OFFENSIVE"] = 1;
    out.function_sets["FUNCTIONS"] = {{"GOAL", "Positive"}};
    ai::PlayerType player; player.name = "FIXTURE"; player.function_sets = {"FUNCTIONS"}; player.difficulty["NORMAL"] = "NORMAL";
    out.players["FIXTURE"] = player; out.difficulties["NORMAL"] = {};
    return out;
}
inline ai::PlanDef definition() {
    ai::PlanDef plan; plan.name = "FIXTURE"; plan.module = "fixture.lua"; plan.goals = {"GOAL"};
    plan.ignore_target = true; plan.allow_free_store = false;
    plan.taskforces = {{"Main", {{1, 1, 0, false, {100}}}, false, true, {}, 1}};
    return plan;
}
struct Owner {
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    std::shared_ptr<ai::Engine> engine;
    std::shared_ptr<ai::GalacticFunding> funding = std::make_shared<ai::GalacticFunding>();
    std::shared_ptr<ai::GalacticSelectionInput> facts = std::make_shared<ai::GalacticSelectionInput>();
    auth::ScriptScheduler scripts;
    std::unique_ptr<ga::NativeActions> actions;
    std::uint64_t sequence{};
    Owner(gc::GalacticSession& session, const eawr::sim::PartitionExecutor& executor, bool ordinary_enemy = false) : scripts(must(auth::ScriptScheduler::create({}, {}))) {
        ai::GalacticPerceptionSetup perception; perception.seed = 42;
        perception.players = {{1, 1, "Empire", "Normal", false, true, false, 10}};
        perception.planets = {{10, "ALPHA", 1000, {}, {}, {}, {}, {}}};
        for (const auto& planet : session.setup().planets) if (planet.id != 10)
            perception.planets.push_back({planet.id, "PLANET" + std::to_string(planet.id), 1000, {}, {}, {}, {}, {}});
        foc::AiType type; type.type_id = 100; type.name = "UNIT"; type.tech_level = 2;
        perception.types = {type};
        if (ordinary_enemy) {
            auto enemy = type; enemy.type_id = 101; enemy.name = "ENEMY_SHIP"; enemy.combat_power = ai::real(10);
            perception.types.push_back(enemy);
        }
        host->setup.content.types = perception.types; host->setup.seed = 42;
        host->setup.players = {{1, "Empire", false, true, "FIXTURE", false}};
        host->players.emplace(1, &host->setup.players.front());
        for (const auto& row : host->setup.content.types) host->types.emplace(row.type_id, &row);
        world = std::make_shared<ai::GalacticWorld>(perception); host->ai_world = world;
        engine = std::make_shared<ai::Engine>(host, data(), std::vector{definition()}); host->engine = engine.get();
        auth::ModuleManifest modules;
        must(modules.add("fixture.lua", "function Base_Definitions() end\nfunction Main_Thread() end\n"));
        scripts = must(auth::ScriptScheduler::create({}, std::move(modules)));
        must(world->bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
        const std::array selected{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
        must(engine->begin_galactic_barrier(*world, {}, scripts, sequence, selected, &executor));
        must(funding->initialize(1, std::array{ai::EvaluatedFundingTemplate{1, {{1, f(1)}}, {}}}, f(200)));
        engine->set_galactic_funding(funding);
        ga::TemplatePolicy policy; policy.goal_types["GOAL"] = {true, 1}; policy.plan_names["FIXTURE"] = {true, 1};
        policy.equations["OFFENSIVE"] = {"Positive", 1}; must(engine->apply_galactic_policy(1, policy));
        facts->complete = facts->reachable = true; facts->player_tech = 2;
        ai::GalacticSelectionType info; info.cost = ai::real(100); info.build_seconds = ai::real(1); info.tech = 2;
        info.terrain_allowed = true; info.instance_allowed = true; facts->types.emplace(100, info);
        ga::ProducerInput producer; producer.producer = producer.planet = 10; producer.player = 1; producer.type = 100;
        producer.admission = gc::ProductionAdmission::allowed; producer.reachable = true;
        producer.cost = f(100); producer.build_seconds = f(1); facts->producers = {producer};
        engine->set_galactic_selection([source = facts](auto, auto, const auto&, const auto&) {
            return eawr::core::Result<std::shared_ptr<const ai::GalacticSelectionInput>>::success(source);
        });
        actions = std::make_unique<ga::NativeActions>(session, funding, ga::NativeActions::Placement{}, std::map<gc::PlayerId, Fixed>{{1, f(1)}},
            [manager = engine](auto player, auto plan) { return manager->galactic_goal_for_plan(player, plan); }, &executor,
            [](auto) { return eawr::core::Result<Fixed>::success(f(100)); });
        actions->bind_handlers(); host->galactic_execution = std::shared_ptr<ga::Actions>(actions.get(), [](auto*) {});
        engine->set_galactic_build_observer([this](auto player, auto build) { return actions->build_status(player, build); });
    }
};
} // namespace selector_test
using namespace selector_test;
namespace eawr::script::foc::ai {
struct EngineContractAccess {
    static const void* build_cache(const Engine& e) { return e.galactic_build_cache_.get(); }
    static PlayerAi& player(Engine& e) { return e.players_.front(); }
    static gc::funding::GoalRequest funding_request(Engine& e, bool running = false) {
        auto& p = player(e);
        return e.galactic_funding_request(p, running ? p.active.front() : p.proposed.front());
    }
    static bool selected_reservation(Engine& e) {
        auto& p = player(e); auto& g = p.proposed.front();
        if (!e.select_units(p, g, *g.potential) || !e.production_time_allowed(p, g) || !e.galactic_fundable(p, g, false)) return false;
        e.reserve(p, g); return true;
    }
    static PotentialPlan indexed_selection(Engine& e) {
        auto& p = player(e); auto& g = p.proposed.front();
        auto& definition = e.plans_.front(); definition.allow_free_store = true;
        definition.taskforces.front().teams.front().min_count = 4;
        definition.taskforces.front().teams.front().max_count = 4;
        definition.taskforces.front().minimum_size = 4;
        p.reserved[23] = 999; p.assigned[24] = 999;
        if (!e.select_units(p, g, *g.potential)) throw std::runtime_error("typed pool selection failed");
        return *g.potential;
    }
    static void add_tasks(Engine& e) {
        const auto write = e.native_record_scope();
        auto& p = player(e); const auto& plan = player(e).running.at(p.active.front().plan);
        const auto tf = plan.taskforces.front(); const auto block = block_id(plan.instance, 17);
        e.native_records_changed(p.player, {4}); e.native_record_changed(p.player, 23, block);
        e.mark_native_state_changed();
        Block b; b.id = block; b.instance = plan.instance; b.taskforce = tf; b.kind = Block::Kind::produce; player(e).blocks[block] = b;
        BuildTask task; task.taskforce = tf; task.block = block; task.type = 100; task.source = 2; task.producer = 10; p.tasks = {task};
        e.rebuild_runtime_index();
    }
    static void release_twice(Engine& e) { auto& p = player(e); e.release(p, p.active.front()); e.release(p, p.active.front()); }
    static void structure_fixture(Engine& e) { e.plans_.front().taskforces.front().teams.front().max_count = 2; }
    static void base_fixture(Engine& e, bool two_teams) {
        auto& force = e.plans_.front().taskforces.front();
        if (two_teams) { auto team = force.teams.front(); team.types = {101}; force.teams.push_back(team); }
        auto type = e.host_->setup.content.types.front(); type.type_id = 200; type.name = "RESOLVED_BASE"; type.star_base = true;
        e.host_->setup.content.types.push_back(type);
        e.host_->types.clear();
        for (const auto& row : e.host_->setup.content.types) e.host_->types.emplace(row.type_id, &row);
    }
    static void base_preference_fixture(Engine& e) {
        base_fixture(e, false);
        e.plans_.front().taskforces.front().teams.front().types = {200, 100};
    }
    static void staging_fixture(Engine& e) { e.plans_.front().allow_free_store = true; }
    static void assembly_fixture(Engine& e) {
        staging_fixture(e);
        auto& force = e.plans_.front().taskforces.front();
        force.teams.front().min_count = force.teams.front().max_count = 3;
        force.minimum_size = 3;
    }
    static void previous_assembly_member(Engine& e) {
        auto& tf = force(e); tf.units.push_back(101); tf.had_units = true;
        auto& p = player(e); p.assigned[101] = tf.id;
        const auto instance = player(e).running.at(tf.plan).instance;
        Block block; block.id = block_id(instance, 16); block.instance = instance; block.taskforce = tf.id;
        block.kind = Block::Kind::produce; block.finished = true; player(e).blocks.emplace(block.id, block);
        BuildTask old; old.taskforce = tf.id; old.block = block.id; old.type = 100; old.object = 101;
        old.source = 0; old.finished = old.joined = true; old.staging = 10;
        old.galactic_assembly = BuildTask::GalacticAssembly::complete; p.tasks.push_back(old);
        e.rebuild_runtime_index();
    }
    static void pending_previous_assembly(Engine& e) {
        auto& task = player(e).tasks.back();
        task.finished = false; task.galactic_assembly = BuildTask::GalacticAssembly::staged;
        player(e).blocks.at(task.block).finished = false;
    }
    static void stage_reader_fixture(Engine& e) {
        staging_fixture(e); e.plans_.front().taskforces.push_back(e.plans_.front().taskforces.front());
    }
    static void finish(Engine& e, auth::ScriptScheduler& scripts) { auto& p = player(e); e.finish_plan(p, p.active.front().plan, scripts); }
    static void pending_reservation(Engine& e, std::uint64_t goal) { player(e).reserved[999999] = goal; }
    static bool fresh_estimate_refused(const Engine& e) {
        const auto& p = e.players_.front();
        try { (void)e.galactic_funding_request(p, p.proposed.front()); }
        catch (const std::runtime_error& error) {
            return std::string_view(error.what()) == "galactic funding production estimate unavailable";
        }
        return false;
    }
    static void running_maintenance(Engine& e, unsigned scenario) {
        auto& p = player(e);
        p.proposed.clear();
        if (scenario && scenario != 4) {
            auto proposed = p.active.front(); proposed.id = p.next_goal_id++;
            proposed.plan = 0; proposed.active = false; proposed.exclusions_registered = false;
            proposed.potential->valid = false; proposed.desire = real(scenario == 1 ? 0 : 2);
            if (scenario == 3) {
                proposed.target = e.target_of_object(20)->id; proposed.desire = real(1) / real(2);
                proposed.potential->id = *e.allocate_plan_id(p); proposed.potential->valid = true;
                proposed.potential->reserved = proposed.potential->funded = false;
            }
            p.proposed.push_back(std::move(proposed));
            if (scenario == 2) player(e).running.at(p.active.front().plan).removable = true;
        }
        e.begin_galactic_maintenance(p);
        std::vector<Goal> kept; e.maintain_category(p, "OFFENSIVE", kept);
        p.active = std::move(kept); p.proposed.clear();
    }
    static TaskForce& force(Engine& e) { return player(e).taskforces.at(player(e).running.at(player(e).active.front().plan).taskforces.front()); }
    static Block& retained_block(Engine& e, std::uint64_t id) { return player(e).blocks.at(id); }
    // Fixture writes use the same semantic record boundaries as native writers.
    static void drop_goal(Engine& e) {
        const auto write = e.native_record_scope();
        auto& p = player(e); auto& goal = p.active.front();
        e.native_record_changed(p.player, 3, goal.id); e.mark_native_state_changed();
        goal.finished = true;
    }
    static void assembly_request(Engine& e, BuildTask& task, std::uint64_t request) {
        const auto write = e.native_record_scope();
        e.native_records_changed(player(e).player, {4}); e.mark_native_state_changed();
        task.galactic_assembly_request = request;
    }
    static void force_fleet(Engine& e, TaskForce& force, sim::EntityId fleet) {
        const auto write = e.native_record_scope();
        e.native_record_changed(player(e).running.at(force.plan).player, 21, force.id); e.mark_native_state_changed();
        force.galactic_fleet = fleet;
    }
    static void block_actor(Engine& e, Block& block, sim::EntityId actor) {
        const auto write = e.native_record_scope();
        const auto& force = player(e).taskforces.at(block.taskforce);
        e.native_record_changed(player(e).running.at(force.plan).player, 23, block.id); e.mark_native_state_changed();
        block.galactic_actor = actor;
    }
    static void force_stage(Engine& e, gc::PlanetId stage) {
        const auto write = e.native_record_scope();
        auto& tf = force(e);
        e.native_record_changed(player(e).running.at(tf.plan).player, 21, tf.id); e.mark_native_state_changed();
        tf.galactic_stage = stage;
    }
    static void assembly_record_inventory(Engine& e, const GoalStateContributor& codec) {
        auto& tasks = player(e).tasks;
        expect(!tasks.empty(), "assembly record witness has actual native production tasks");
        auto& task = tasks.front();
        const auto phase = task.galactic_assembly;
        const auto request = task.galactic_assembly_request;
        const auto original = must(codec.record_sync_hash(true));
        task.galactic_assembly = phase == BuildTask::GalacticAssembly::staged
            ? BuildTask::GalacticAssembly::assigned : BuildTask::GalacticAssembly::staged;
        const auto changed_phase = must(codec.record_sync_hash(true));
        task.galactic_assembly = phase;
        task.galactic_assembly_request = request ^ 1ULL;
        const auto changed_request = must(codec.record_sync_hash(true));
        task.galactic_assembly_request = request;
        expect(original != changed_phase && original != changed_request,
            "galactic record inventory fingerprints assembly phase and request independently");
        record_digest_contract(codec, "temporary record inventory witnesses restore the original warm digest");
    }
    static void contrast_fixture(Engine& e, bool unclassified) {
        auto& definition = e.plans_.front(); definition.ignore_target = false;
        definition.max_contrast = definition.min_contrast = real(1);
        if (unclassified) { definition.taskforces.front().teams.front().min_count = 0; definition.taskforces.front().minimum_size = 0; }
        auto& types = e.host_->setup.content.types;
        types.front().combat_power = {};
        AiType spawned; spawned.type_id = 200; spawned.name = "GARRISON";
        spawned.combat_power = real(10); spawned.category_bits = 1; types.push_back(spawned);
        e.host_->types.clear(); for (const auto& type : types) e.host_->types.emplace(type.type_id, &type);
        e.host_->contrast[1] = {{0, 200, real(1)}};
    }
    static void plan_contrast_fixture(Engine& e) {
        auto& definition = e.plans_.front();
        definition.galactic_contrast_qualified = true; definition.galactic_contrast_order = {1};
        definition.max_contrast = definition.min_contrast = real(2);
        definition.galactic_contrast[1] = {{1, {}, real(7)}, {0, 200, real(1)}};
        e.host_->contrast[1] = {{0, 200, Real{}}};
    }
};
}
