// Qualified synthetic provider proof. Installed content/stock Lua integration
// remains a separate acceptance test; these facts do not stand in for retail.
#include "galactic_support.hpp"
#include "galactic_ai_world.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_execution.hpp"
#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_selection_content.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <stdexcept>

// Test-only allocation probe, armed solely around immutable publication hits.
namespace build_alloc_probe {
std::atomic<bool> armed{false};
std::atomic<std::size_t> calls{0};
}
void* operator new(std::size_t size) {
    if (build_alloc_probe::armed.load(std::memory_order_relaxed))
        build_alloc_probe::calls.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace ga = foc::galactic_ai;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
using eawr::sim::math::Fixed;
namespace {
void expect(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
template<class T> T must(eawr::core::Result<T> r) { if (!r) throw std::runtime_error(r.error().message); return std::move(r).value(); }
void must(eawr::core::Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
Fixed f(std::int64_t n) { return must(Fixed::from_integer(n)); }
std::string garrison_fact_contracts() {
    ai::GalacticSelectionContent content;
    ai::GalacticSelectionContentType carrier; carrier.name = "CARRIER";
    auto original = std::make_shared<eawr::units::GalacticType>(); original->name = "CARRIER";
    original->starting_spawned_units[1] = {{"Alpha", 2}, {"BETA", 1}, {"ALPHA", 9}};
    carrier.original = original; carrier.space_escorts = {"BETA", "Alpha"}; content.types.emplace(100, carrier);
    for (const auto& [id, name, channel, power, movement] : {
        std::tuple{101ULL, "ALPHA", gc::ProductionDomain::ground, 100, "WALK"},
        std::tuple{102ULL, "BETA", gc::ProductionDomain::space, 20, "FLY"}}) {
        ai::GalacticSelectionContentType type; type.name = name; type.value.channel = channel; type.movement_class = movement;
        content.types.emplace(id, type);
        foc::AiType traits; traits.type_id = id; traits.name = name; traits.combat_power = ai::real(power); content.ai.types.push_back(traits);
    }
    content.planets.emplace(10, ai::GalacticSelectionContentPlanet{"MUD", 0, {}, {}});
    content.terrain["MUD"]["WALK"] = ai::real(1) / ai::real(2);
    const auto rows = must(ai::galactic_proposal_garrisons(content, 100, 3, 10));
    const std::array<gc::TypeId, 7> expected{101, 101, 102, 101, 101, 102, 101};
    expect(rows.size() == expected.size(), "proposal garrisons use first resolved-type count for duplicate entries, then authored escorts");
    std::string trace;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        expect(row.type == expected.at(index) && row.all_main_domains,
            "proposal garrison type order is retained and scoring admits either main domain");
        expect(row.power == ai::real(row.type == 101 ? 100 : 20)
            && row.terrain_effectiveness == (row.type == 101 ? ai::real(1) / ai::real(2) : ai::real(1)),
            "garrison power and target terrain come from the spawned type rather than its carrier");
        trace += std::to_string(row.type) + ":" + std::to_string(row.power.repr) + ":" + std::to_string(row.terrain_effectiveness.repr) + ";";
    }
    expect(must(ai::galactic_proposal_garrisons(content, 100, 0, 10)).size() == 2, "empty lowest tech row retains only escorts");
    expect(!ai::galactic_proposal_garrisons(content, 100, 6, 10), "out-of-table tech refuses before indexing");
    expect(!ai::galactic_proposal_garrisons(content, 999, 3, 10), "missing original proposal type refuses");
    expect(!ai::galactic_proposal_garrisons(content, 100, 3, 999), "missing target terrain refuses");
    content.types.at(100).space_escorts.push_back("UNBOUND");
    expect(!ai::galactic_proposal_garrisons(content, 100, 3, 10), "unbound authored escort refuses the whole fact result");
    return trace;
}
void staging_contracts() {
    ai::PlanDef plan; plan.taskforces.resize(3);
    std::vector<ai::galactic_goals::GalacticForceConstraints> constraints(3);
    constraints[1].unique_stage = constraints[2].unique_stage = true;
    const std::array<gc::PlanetId, 2> sites{10, 20};
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "ANY", sites)) == std::vector<gc::PlanetId>{10, 20, 30},
        "unique staging excludes prior locations then falls back to the goal");
    constraints[1].unique_stage = false; constraints[1].matching_stage = true;
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "ANY", sites)) == std::vector<gc::PlanetId>{10, 10, 20},
        "matching staging includes a prior location in authored order");
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "SINGLE_HOP_DISCONNECTED", sites)) == std::vector<gc::PlanetId>(3, 30),
        "single-hop-disconnected stages directly at the goal");
    expect(must(ai::galactic_force_stages(plan, constraints, 0, "ANY", sites)) == std::vector<gc::PlanetId>(3), "global target does not invent staging");
    plan.magic = true;
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "ANY", sites)) == std::vector<gc::PlanetId>(3), "magic plan leaves staging absent");
    plan.magic = false; plan.allow_free_store = false;
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "ANY", sites)) == std::vector<gc::PlanetId>(3), "freestore-disabled plan leaves staging absent");
    expect(!ai::galactic_force_stages(plan, {}, 30, "ANY", sites), "missing authored stage constraints refuse");
}
gc::GalacticSetup setup() {
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
void producer_admission_contracts() {
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
            auto configured = setup(); configured.player_economy->players.front().credits = {};
            if (scenario == 1) configured.production->players.front().locked = {100};
            if (scenario == 2) configured.production->planets.front().enabled = false;
            if (scenario == 3) {
                configured.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}};
                configured.fiscal->players.front() = {1, false, f(1)};
                configured.production->players.front().human = true;
                configured.production->players.front().pay_as_you_go = false;
            }
            auto session = must(gc::GalacticSession::create(configured));
            ai::GalacticPerceptionSetup perception;
            perception.players = {{1, 1, "Empire", "Normal", false, true, false, 10}};
            perception.planets = {{10, "ALPHA", 1000, {}, {}, {}, {}, {}}};
            foc::AiType type; type.type_id = 100; type.name = "UNIT"; type.tech_level = 2;
            perception.types = {type};
            ai::GalacticWorld world(perception);
            must(world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
            const auto input = world.copy_input();
            expect(input->producers.size() == 1, "producer admission cases retain the actual offer");
            const auto expected = scenario == 0 ? gc::ProductionAdmission::allowed
                : scenario == 1 ? gc::ProductionAdmission::locked
                : scenario == 2 ? gc::ProductionAdmission::unavailable : gc::ProductionAdmission::credits;
            expect(input->producers.front().admission == expected,
                "zero-credit AI retains eligible producer while locks, disabled sites and human credits still gate");
        }
    }
}
ai::AiData data() {
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
ai::PlanDef definition() {
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
    Owner(gc::GalacticSession& session, const eawr::sim::PartitionExecutor& executor) : scripts(must(auth::ScriptScheduler::create({}, {}))) {
        ai::GalacticPerceptionSetup perception; perception.seed = 42;
        perception.players = {{1, 1, "Empire", "Normal", false, true, false, 10}};
        perception.planets = {{10, "ALPHA", 1000, {}, {}, {}, {}, {}}};
        for (const auto& planet : session.setup().planets) if (planet.id != 10)
            perception.planets.push_back({planet.id, "PLANET" + std::to_string(planet.id), 1000, {}, {}, {}, {}, {}});
        foc::AiType type; type.type_id = 100; type.name = "UNIT"; type.tech_level = 2;
        perception.types = {type}; host->setup.content.types = {type}; host->setup.seed = 42;
        host->setup.players = {{1, "Empire", false, true, "FIXTURE", false}};
        host->players.emplace(1, &host->setup.players.front());
        host->types.emplace(100, &host->setup.content.types.front());
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
}
namespace eawr::script::foc::ai {
struct EngineContractAccess {
    static const void* build_cache(const Engine& e) { return e.galactic_build_cache_.get(); }
    static PlayerAi& player(Engine& e) { return e.players_.front(); }
    static bool selected_reservation(Engine& e) {
        auto& p = player(e); auto& g = p.proposed.front();
        if (!e.select_units(p, g, *g.potential) || !e.production_time_allowed(p, g) || !e.galactic_fundable(p, g, false)) return false;
        e.reserve(p, g); return true;
    }
    static void add_tasks(Engine& e) {
        auto& p = player(e); const auto& plan = e.running_.at(p.active.front().plan);
        const auto tf = plan.taskforces.front(); const auto block = block_id(plan.instance, 17);
        Block b; b.id = block; b.instance = plan.instance; b.taskforce = tf; b.kind = Block::Kind::produce; e.blocks_[block] = b;
        BuildTask task; task.taskforce = tf; task.block = block; task.type = 100; task.source = 2; task.producer = 10; p.tasks = {task};
    }
    static void release_twice(Engine& e) { auto& p = player(e); e.release(p, p.active.front()); e.release(p, p.active.front()); }
    static void structure_fixture(Engine& e) { e.plans_.front().taskforces.front().teams.front().max_count = 2; }
    static void staging_fixture(Engine& e) { e.plans_.front().allow_free_store = true; }
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
                proposed.potential->id = e.next_plan_id_++; proposed.potential->valid = true;
                proposed.potential->reserved = proposed.potential->funded = false;
            }
            p.proposed.push_back(std::move(proposed));
            if (scenario == 2) e.running_.at(p.active.front().plan).removable = true;
        }
        e.begin_galactic_maintenance(p);
        std::vector<Goal> kept; e.maintain_category(p, "OFFENSIVE", kept);
        p.active = std::move(kept); p.proposed.clear();
    }
    static TaskForce& force(Engine& e) { return e.taskforces_.at(e.running_.at(player(e).active.front().plan).taskforces.front()); }
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
};
}
int main() {
    try {
        staging_contracts();
        const auto garrison_trace = garrison_fact_contracts();
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::array<std::string, 4> traces;
            must(executor.execute_phase("proposal-garrison-facts", traces.size(), [&](std::size_t index) { traces[index] = garrison_fact_contracts(); }));
            for (const auto& trace : traces) expect(trace == garrison_trace, "proposal garrison facts equal at 1/2/4/8 workers");
        }
        producer_admission_contracts();
        std::string baseline;
        std::array<std::string, 5> running_baselines;
        for (const auto count : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(count);
            {
                auto refused_session = must(gc::GalacticSession::create(setup())); Owner refused(refused_session, executor);
                must(refused.engine->allocate_galactic_goal(1, 0, refused.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*refused.engine), "fresh estimate refusal fixture first selects a real roster");
                const auto before = refused.funding->state(); refused.facts->reachable = false;
                expect(ai::EngineContractAccess::fresh_estimate_refused(*refused.engine) && refused.funding->state() == before,
                    "fresh rejected estimate refuses with a contextual diagnostic before Q24 conversion or any funding write");
            }
            for (unsigned scenario = 0; scenario < 5; ++scenario) {
                auto configured = setup();
                if (scenario == 3) {
                    configured.planets.push_back({20, {f(3), f(4), {}}, 1});
                    configured.fiscal->planets.push_back({20, {}});
                    auto producer = configured.production->planets.front(); producer.planet = 20;
                    configured.production->planets.push_back(std::move(producer));
                }
                auto running_session = must(gc::GalacticSession::create(configured)); Owner running(running_session, executor);
                const auto identity = must(running.engine->allocate_galactic_goal(1, 0, running.engine->target_of_object(10)->id));
                expect(identity && ai::EngineContractAccess::selected_reservation(*running.engine), "running budget fixture selects a paid roster");
                auto& p = ai::EngineContractAccess::player(*running.engine); p.next_function = p.functions.size();
                must(running.engine->service_galactic_system(1, ga::Service::goals, running.scripts, running.sequence));
                const auto plan = p.active.front().plan;
                ai::EngineContractAccess::add_tasks(*running.engine);
                must(running.engine->service_galactic_system(1, ga::Service::execution, running.scripts, running.sequence));
                const auto build = p.tasks.front().completion;
                if (scenario == 3) {
                    auto state = running.funding->state(); state.at(1).categories.at(1).cash = f(150);
                    state.at(1).goals.at(identity->goal).unallocated = {};
                    must(running.funding->restore(std::move(state)));
                }
                if (scenario == 4) running.facts->reachable = false;
                ai::EngineContractAccess::running_maintenance(*running.engine, scenario);
                const auto ledger = running.funding->ledger(1);
                expect(p.active.size() == 1 && p.active.front().id == identity->goal && p.active.front().plan == plan,
                    "all category paths retain the same running plan without attaching or selecting again");
                expect(ledger->goals.at(identity->goal).reassessed && ledger->goals.at(identity->goal).capital == f(100)
                    && ledger->categories.at(1).available_cash == f(scenario == 3 ? 50 : 100),
                    "maintenance restores running paid capital once, with or without competing proposals");
                expect(p.exclusion_counts.at("EXCLUDED") == 1 && running.actions->purchases().size() == 1,
                    "running reassessment adds no exclusions or duplicate purchase");
                for (unsigned frame = 0; frame < 35; ++frame) (void)must(running_session.step(executor));
                const auto& completed = running.actions->purchases().at(build);
                expect(completed.completed && !completed.failed && completed.object && completed.billed > Fixed{},
                    "accepted running build pays and completes after a later goal maintenance");
                // WGEP-43/71: completion does not add a final installment.
                // Reconcile actual bills, including the retained residual mark.
                const auto outstanding = must(eawr::sim::math::subtract(completed.price, completed.billed));
                const auto settled = running.funding->ledger(1);
                expect(settled->goals.at(identity->goal).cost == outstanding && settled->goals.at(identity->goal).marked == outstanding
                    && settled->goals.at(identity->goal).capital == outstanding
                    && settled->categories.at(1).cash == must(eawr::sim::math::subtract(f(scenario == 3 ? 150 : 200), completed.billed))
                    && running_session.snapshot()->player_economy()->players.front().credits == must(eawr::sim::math::subtract(f(200), completed.billed)),
                    "actual producer bills reconcile retained goal/category cash and the world account without a final debt flush");
                ai::GoalStateContributor retained(running.engine, eawr::core::sha256_hex(std::span<const std::uint8_t>{}));
                const auto trace = running_session.state_sha256() + must(running.actions->state_hash()) + must(retained.state_hash())
                    + eawr::core::sha256_hex(must(ga::encode_funding_state(running.funding->state())));
                if (running_baselines[scenario].empty()) running_baselines[scenario] = trace;
                else expect(trace == running_baselines[scenario], "running maintenance, billing and completion hashes match on 1/2/4/8 workers");
                if (scenario == 0) {
                    // Fixture: remove a completed-work budget while its plan is retained.
                    const auto object = completed.object;
                    const auto credits = running_session.snapshot()->player_economy()->players.front().credits;
                    must(running.funding->remove_goal(1, identity->goal));
                    ai::EngineContractAccess::running_maintenance(*running.engine, 0);
                    expect(p.active.empty() && p.tasks.empty() && !running.engine->galactic_goal(1, identity->goal)
                        && p.exclusion_counts.at("EXCLUDED") == 0,
                        "running plan with no retained budget finishes through ordinary teardown");
                    expect(running.actions->purchases().size() == 1 && running.actions->purchases().at(build).object == object
                        && running_session.snapshot()->player_economy()->players.front().credits == credits,
                        "missing-budget teardown preserves completed production and grants no credits");
                }
            }
            for (const auto capital : {100, 30}) {
                auto paid_session = must(gc::GalacticSession::create(setup())); Owner paid(paid_session, executor);
                const auto identity = must(paid.engine->allocate_galactic_goal(1, 0, paid.engine->target_of_object(10)->id));
                expect(identity && ai::EngineContractAccess::selected_reservation(*paid.engine), "salvage fixture selects and funds actual paid roster");
                auto& p = ai::EngineContractAccess::player(*paid.engine); p.next_function = p.functions.size();
                must(paid.engine->service_galactic_system(1, ga::Service::goals, paid.scripts, paid.sequence));
                ai::EngineContractAccess::add_tasks(*paid.engine);
                must(paid.engine->service_galactic_system(1, ga::Service::execution, paid.scripts, paid.sequence));
                const auto build = p.tasks.front().completion;
                (void)must(paid_session.step(executor));
                const auto sampled = must(paid.actions->build_head(1, build));
                expect(sampled.progress == ai::Real{} && sampled.remaining_seconds < ai::real(1)
                    && must(paid.actions->build_remaining_cost(1, build)) == f(100)
                    && paid.funding->state().at(1).goals.at(identity->goal).remaining == f(1),
                    "clock-only advancement changes head countdown but not sampled paid progress, remaining price or fiscal budget time");
                auto state = paid.funding->state(); auto& ledger = state.at(1); auto& budget = ledger.goals.at(identity->goal);
                budget.capital = f(capital); ledger.categories.at(1).available_cash = f(200 - capital);
                must(paid.funding->restore(std::move(state)));
                ai::EngineContractAccess::pending_reservation(*paid.engine, identity->goal);
                p.active.front().finished = true;
                expect(!paid.engine->galactic_goal(1, identity->goal) && paid.engine->galactic_potential_for_goal(1, identity->goal) == identity->potential_plan
                    && paid.engine->galactic_reservation_valid(1, 999999, identity->potential_plan), "finished goal retains Potential and exact actor reservation lifetime");
                ai::EngineContractAccess::finish(*paid.engine, paid.scripts);
                const auto& purchase = paid.actions->purchases().at(build);
                const bool salvage = capital == 100;
                expect(purchase.task_finished && purchase.orphan && purchase.salvage == salvage
                    && paid_session.snapshot()->production()->planets.front().queues[0].empty() != salvage,
                    "actual plan teardown keeps selected salvage entry or cancels nonfitting entry after budget selection");
                expect(paid.funding->state().at(1).goals.empty() && paid.funding->state().at(1).dead_goals.empty()
                    && paid.funding->state().at(1).dead_budget == f(salvage ? 100 : 0)
                    && paid_session.snapshot()->player_economy()->players.front().credits == f(200), "salvage changes category/dead budget without granting player credits");
                const auto saved = must(paid.actions->capture()); must(paid.actions->restore(saved));
                expect(must(paid.actions->capture()).components == saved.components, "finished/salvage flags and independently retained actual entry round trip");
                for (unsigned frame = 0; frame < 35; ++frame) (void)must(paid_session.step(executor));
                const auto& settled = paid.actions->purchases().at(build);
                // WGA-159/160: subtract actual orphan installments, independently
                // of completion. Completion does not invent a final debt flush.
                const auto residual = must(eawr::sim::math::subtract(f(salvage ? 100 : 0), settled.billed));
                expect(paid.funding->state().at(1).dead_budget == residual
                    && paid_session.snapshot()->player_economy()->players.front().credits == must(eawr::sim::math::subtract(f(200), settled.billed))
                    && (salvage ? settled.completed && settled.object && settled.billed > Fixed{} : settled.failed && settled.billed == Fixed{}),
                    "actual orphan installments debit aggregate dead budget and credits; completion adds no unsourced residual settlement");
            }
            {
                auto orbital = setup(); orbital.production->planets.front().offers.front().domain = gc::ProductionDomain::space;
                orbital.seats = {{{1}, 1, gc::SeatKind::ai, 0, true, 0}}; // actual orbital completion requires its controller
                auto parent = must(gc::GalacticSession::create(orbital)); Owner built(parent, executor);
                built.facts->types.at(100).channel = gc::ProductionDomain::space;
                built.facts->producers.front().domain = gc::ProductionDomain::space;
                must(built.engine->allocate_galactic_goal(1, 0, built.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*built.engine), "orbital fixture selects actual paid space production");
                auto& player = ai::EngineContractAccess::player(*built.engine); player.next_function = player.functions.size();
                must(built.engine->service_galactic_system(1, ga::Service::goals, built.scripts, built.sequence));
                ai::EngineContractAccess::add_tasks(*built.engine);
                must(built.engine->service_galactic_system(1, ga::Service::execution, built.scripts, built.sequence));
                const auto build = player.tasks.front().completion;
                for (unsigned frame = 0; frame < 35; ++frame) (void)must(parent.step(executor));
                must(built.world->bind(parent.snapshot(), {}, executor, parent.snapshot()->completed_tick(), parent.next_entity_id()));
                const std::array selected{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
                must(built.engine->begin_galactic_barrier(*built.world,
                    {parent.snapshot()->completed_tick(), parent.snapshot()->clock().galactic_frame}, built.scripts, built.sequence, selected, &executor));
                must(built.engine->service_galactic_system(1, ga::Service::execution, built.scripts, built.sequence));
                const auto& fleet = parent.snapshot()->fleets().front(); const auto& task = player.tasks.front();
                expect(task.finished && task.joined && task.object == fleet.members.front().id && task.object != fleet.id
                    && ai::EngineContractAccess::force(*built.engine).galactic_fleet == fleet.id
                    && must(built.actions->build_actor(1, build)) == task.object,
                    "actual orbital completion keeps outer fleet identity separate from assigned first direct member");
            }
            {
                auto configured = setup(); configured.planets.push_back({20, {f(3), f(4), {}}, 1});
                configured.fiscal->planets.push_back({20, {}}); // fiscal source tables cover every actual planet
                gc::PlanetProduction stage; stage.planet = 20;
                configured.production->planets.push_back(stage);
                configured.fleets = {{30, 1, 10, {{31, 100}}}};
                auto stage_session = must(gc::GalacticSession::create(configured)); Owner staged(stage_session, executor);
                ai::EngineContractAccess::stage_reader_fixture(*staged.engine);
                auto catalog = std::make_shared<ai::galactic_goals::MaterializedPlanCatalog>();
                catalog->plans = staged.engine->plans(); catalog->constraints["FIXTURE"].forces = {{}, {false, false, true, false, {}}};
                staged.engine->set_galactic_stage_catalog(catalog);
                ai::GalacticConnectivity paths; paths.execution_available = true;
                paths.execution_routes = {{1, 10, 20, false, ai::real(7), {}, {}}};
                paths.goal_reach = {{1, 20, "ANY", {10}}};
                must(staged.world->bind(stage_session.snapshot(), paths, executor, 0, stage_session.next_entity_id()));
                const auto input = staged.world->copy_input();
                expect(std::any_of(input->producers.begin(), input->producers.end(), [](const auto& producer) {
                    return producer.player == 1 && producer.planet == 10 && producer.type == 100
                        && producer.admission == gc::ProductionAdmission::allowed;
                }), "AI stage producer uses content eligibility independently of player payment admission");
                const std::array selected{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
                must(staged.engine->begin_galactic_barrier(*staged.world, {}, staged.scripts, staged.sequence, selected, &executor));
                must(staged.engine->allocate_galactic_goal(1, 0, staged.engine->target_of_object(20)->id));
                ga::FreestoreFacts actor; actor.object = {31, 100, 1, 10, ga::FreestoreKind::unit, false, false, false, {}}; actor.current_type = 100; actor.parent = 30;
                actor.ai_usable = actor.qualified = true;
                must(staged.actions->qualified_freestore().update(actor));
                expect(!staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front()), "native reader refuses missing per-object flag instead of inferring garrison from type/parent");
                bool usable = true, garrison = true, combat_skip = false;
                staged.engine->set_galactic_stage_garrison([&](auto, const auto&, auto) { return eawr::core::Result<bool>::success(garrison); });
                staged.engine->set_galactic_stage_combat_skip([&](auto, const auto&, auto) { return eawr::core::Result<bool>::success(combat_skip); });
                staged.engine->set_galactic_stage_usability([&](auto, const auto&, auto) { return eawr::core::Result<bool>::success(usable); });
                must(staged.actions->qualified_freestore().update(actor));
                const auto pair = must(staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front()));
                expect(pair.first == ai::real(37) && pair.second, "native stage reader averages helper squared plus actor-stage XY squared over all authored forces");
                paths.execution_routes.front().threat = ai::real(5);
                must(staged.world->bind(stage_session.snapshot(), paths, executor, 0, stage_session.next_entity_id()));
                expect(must(staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front())).first == ai::parse_real("12.5").value(), "failed helper still contributes XY when another stage helper succeeds");
                usable = false; must(staged.actions->qualified_freestore().update(actor));
                expect(must(staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front())).first == ai::galactic_goals::rejected_estimate(), "unusable source rejects actor without a reachable helper");
                combat_skip = true; must(staged.actions->qualified_freestore().update(actor));
                expect(must(staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front())).first == ai::Real{}, "native stealth and global proposals have zero stage cost");
                expect(must(staged.engine->galactic_freestore_selection_facts(1, 0, 31, staged.engine->plans().front())).first == ai::Real{}, "global proposal does not invent stage locations");
            }
            {
                auto layout = setup();
                gc::Fleet fleet; fleet.id = 30; fleet.owner = 1; fleet.orbit = 10; fleet.members = {{20, 100}, {21, 100}};
                layout.fleets.push_back(fleet);
                auto session = must(gc::GalacticSession::create(std::move(layout))); Owner owner(session, executor);
                const auto allocated = must(owner.engine->allocate_galactic_goal(1, 0, owner.engine->target_of_object(10)->id));
                expect(allocated.has_value(), "real galactic Engine allocates reservation identity for the native pool witness");
                auto& store = owner.actions->qualified_freestore();
                for (const auto id : {20ULL, 21ULL}) {
                    ga::FreestoreFacts facts; facts.object.id = id; facts.object.type = facts.current_type = 100;
                    facts.object.owner = 1; facts.object.planet = 10; facts.parent = 30;
                    facts.object.kind = ga::FreestoreKind::unit;
                    must(store.update(ga::ordinary_freestore_birth(facts)));
                }
                const std::array<eawr::sim::EntityId, 2> offers{20, 21}; must(store.initialize(1, offers));
                owner.actions->bind_freestore_validation([&](const auto& decoded) {
                    for (const auto& [id, facts] : decoded.inventory())
                        expect((id == 20 || id == 21) && facts.object.owner == 1 && facts.parent == 30,
                            "synthetic native pool uses the actual commissioned Engine actor domain");
                    return eawr::core::Result<void>::success();
                });
                must(owner.engine->reserve_galactic_object(1, allocated->goal, 20));
                must(owner.engine->reserve_galactic_object(1, allocated->goal, 21));
                expect(store.reservations(1) && store.reservations(1)->at(20) == allocated->potential_plan
                    && store.reservations(1)->at(21) == allocated->potential_plan
                    && must(owner.engine->galactic_execution_reservations(1)) == *store.reservations(1),
                    "actual Engine writes publish the same potential identity to retained pool reservations");
                const auto engine_before = owner.engine->galactic_execution_reservations(1).value();
                expect(!owner.engine->reserve_galactic_object(1, allocated->goal, 999999)
                    && must(owner.engine->galactic_execution_reservations(1)) == engine_before
                    && *store.reservations(1) == engine_before, "unknown actor refusal leaves both reservation owners unchanged");
                must(owner.engine->release_galactic_goal(1, allocated->goal));
                expect(store.reservations(1)->empty() && store.object_count(1) == 2
                    && must(owner.engine->galactic_execution_reservations(1)).empty(),
                    "actual Engine goal release clears its own holds without removing native pool actors");
            }
            {
                auto contrast_session = must(gc::GalacticSession::create(setup())); Owner contrast(contrast_session, executor);
                ai::EngineContractAccess::contrast_fixture(*contrast.engine, false);
                contrast.facts->contrast = {{gc::ProductionDomain::space, 1, ai::real(10), false}, {gc::ProductionDomain::space, 0, ai::real(10), false}};
                must(contrast.engine->allocate_galactic_goal(1, 0, contrast.engine->target_of_object(10)->id));
                expect(!ai::EngineContractAccess::selected_reservation(*contrast.engine), "zero-power parent cannot satisfy classified contrast alone");
                contrast.facts->types.at(100).garrisons = {{200, gc::ProductionDomain::space, ai::real(10), ai::real(1)}};
                expect(ai::EngineContractAccess::selected_reservation(*contrast.engine), "qualified garrison preference and chosen deficit reduction satisfy contrast");
                const auto& selected = *ai::EngineContractAccess::player(*contrast.engine).proposed.front().potential;
                expect(selected.current_contrast[0].force == ai::Real{} && selected.current_contrast[1].force == ai::Real{}, "garrison decreases best category and same-domain total exactly once");
            }
            {
                auto zero_session = must(gc::GalacticSession::create(setup())); Owner zero(zero_session, executor);
                ai::EngineContractAccess::contrast_fixture(*zero.engine, true);
                zero.facts->contrast = {{gc::ProductionDomain::ground, 0, ai::real(10), false}};
                must(zero.engine->allocate_galactic_goal(1, 0, zero.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*zero.engine), "eligible unclassified deficit admits zero contrast without an unmet team minimum");
                expect(ai::EngineContractAccess::player(*zero.engine).proposed.front().potential->units == std::vector<gc::TypeId>{100}, "unclassified fallback selects candidate with no minimum-roster requirement");
            }
            {
                auto limit_session = must(gc::GalacticSession::create(setup())); Owner limit(limit_session, executor);
                ai::EngineContractAccess::structure_fixture(*limit.engine);
                limit.facts->types.at(100).structure = true; limit.facts->types.at(100).instance_remaining = 1;
                limit.facts->remaining_structure_slots_by_channel[0] = 2;
                must(limit.engine->allocate_galactic_goal(1, 0, limit.engine->target_of_object(10)->id));
                expect(!ai::EngineContractAccess::selected_reservation(*limit.engine), "unqualified target quote refuses a targeted structure");
                limit.facts->types.at(100).target_cost = ai::real(250); limit.facts->types.at(100).target_build_seconds = ai::real(1);
                expect(!ai::EngineContractAccess::selected_reservation(*limit.engine), "target quote controls admission independently of raw roster cost");
                limit.facts->types.at(100).target_cost = ai::real(100);
                expect(ai::EngineContractAccess::selected_reservation(*limit.engine), "one allowed structure remains a valid selected roster");
                expect(ai::EngineContractAccess::player(*limit.engine).proposed.front().potential->units.size() == 1,
                    "per-type allowance one prevents a second structure despite two free channel slots");
            }
            {
                auto income_session = must(gc::GalacticSession::create(setup())); Owner income(income_session, executor);
                auto state = income.funding->state(); auto& category = state.at(1).categories.at(1);
                category.cash = category.available_cash = {}; category.income = category.available_income = f(200);
                must(income.funding->restore(std::move(state)));
                ai::EngineContractAccess::player(*income.engine).maintenance_due = false;
                must(income.engine->allocate_galactic_goal(1, 0, income.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*income.engine),
                    "zero estimate supplements half-time income independently of maintenance eligibility");
                expect(income.funding->ledger(1)->goals.begin()->second.capital == Fixed{},
                    "half-time admission uses income without invented capital");
            }
            auto combined = setup(); combined.planets.push_back({20, {f(12), {}, {}}, 1});
            combined.fiscal->planets.push_back({20, {}});
            combined.seats = {{{1}, 1, gc::SeatKind::ai, 0, true, 0}}; // actual native fleet commands require an active issuer
            auto second = combined.production->planets.front(); second.planet = 20; combined.production->planets.push_back(second);
            combined.rules.maximum_fleet_movement_distance = f(12);
            combined.fleets = {{30, 1, 10, {{31, 101}, {32, 101}}}};
            combined.fleet_control->types.push_back({101, false, true, false});
            combined.movement.emplace(); auto& movement = *combined.movement;
            movement.mode_modifier = f(1); movement.types = {{100, f(1), f(100), 0, true}, {101, f(1), f(10), 0, true}};
            movement.players = {{1, f(1), f(1), false}}; movement.planets = {{10, f(1), false}, {20, f(1), false}};
            movement.access = {{1, 10, true, false}, {1, 20, true, false}};
            // The real move creates another fleet record, so the actor table must
            // already qualify the existing fleet and both literal members.
            combined.census.emplace(); combined.census->types = {{100, {}}, {101, {}}};
            combined.census->players = {{1, 0, false, {}}};
            combined.census->objects = {
                {10, 100, 1, 10, 0, false, false, 100, 0, gc::ActorCombatRole::ordinary},
                {20, 100, 1, 20, 0, false, false, 100, 0, gc::ActorCombatRole::ordinary},
                {30, 100, 1, 10, 10, false, false, 100, 0, gc::ActorCombatRole::fleet},
                {31, 101, 1, 10, 30, false, false, 101, 0, gc::ActorCombatRole::ordinary},
                {32, 101, 1, 10, 30, false, false, 101, 0, gc::ActorCombatRole::ordinary}};
            auto session = must(gc::GalacticSession::create(combined)); Owner owner(session, executor);
            ai::EngineContractAccess::staging_fixture(*owner.engine); owner.facts->force_stages = {10};
            const auto identity = must(owner.engine->allocate_galactic_goal(1, 0, owner.engine->target_of_object(10)->id));
            expect(identity.has_value(), "authored positive fixture allocates a retained goal");
            owner.facts->complete = false;
            expect(!ai::EngineContractAccess::selected_reservation(*owner.engine), "incomplete provider refuses selection and reservation");
            owner.facts->complete = true;
            expect(ai::EngineContractAccess::selected_reservation(*owner.engine), "live selection uses D estimator and C feasibility/reservation");
            auto& player = ai::EngineContractAccess::player(*owner.engine);
            expect(player.reserved_credits.empty() && session.snapshot()->player_economy()->players.front().credits == f(200), "reservation never prepays a tactical wallet");
            expect(owner.funding->ledger(1)->goals.at(identity->goal).capital == f(100), "C owns the actual reservation");
            ai::EngineContractAccess::pending_reservation(*owner.engine, identity->goal);
            const auto reservations = must(owner.engine->galactic_execution_reservations(1));
            expect(reservations == std::map<eawr::sim::EntityId, std::uint64_t>{{999999, identity->potential_plan}},
                "execution copy preserves pending-absent actor to exact potential plan identity");
            const auto digest = eawr::core::sha256_hex(std::span<const std::uint8_t>{});
            ai::GoalStateContributor codec(owner.engine, digest); const auto saved = must(codec.capture());
            must(codec.restore(saved)); owner.engine->set_galactic_funding(owner.funding);
            expect(must(codec.capture()).components == saved.components, "mid-reservation native restore is byte identical");
            expect(must(owner.engine->galactic_execution_reservations(1)) == reservations,
                "execution reservation copy survives native restore without actor remapping");
            const std::array restored_selection{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
            must(owner.engine->begin_galactic_barrier(*owner.world, {}, owner.scripts, owner.sequence, restored_selection, &executor));
            auto& restored = ai::EngineContractAccess::player(*owner.engine); restored.next_function = restored.functions.size();
            expect(restored.exclusion_counts.at("EXCLUDED") == 1, "global exclusion registered once");
            must(owner.engine->service_galactic_system(1, ga::Service::goals, owner.scripts, owner.sequence));
            expect(restored.active.size() == 1 && restored.active.front().plan != 0, "live completed pass attaches after successful reserve");
            expect(ai::EngineContractAccess::force(*owner.engine).galactic_stage == 10, "attachment retains the qualified force staging planet");
            const auto staged = must(codec.capture()); must(codec.restore(staged));
            expect(ai::EngineContractAccess::force(*owner.engine).galactic_stage == 10, "force staging planet round trips in the native codec");
            ai::EngineContractAccess::force(*owner.engine).galactic_stage = 999998;
            expect(!codec.capture(), "missing staging planet refuses canonical native capture");
            ai::EngineContractAccess::force(*owner.engine).galactic_stage = 10;
            must(owner.engine->begin_galactic_barrier(*owner.world, {}, owner.scripts, owner.sequence, restored_selection, &executor));
            ai::EngineContractAccess::add_tasks(*owner.engine);
            must(owner.engine->service_galactic_system(1, ga::Service::execution, owner.scripts, owner.sequence));
            auto& resumed = ai::EngineContractAccess::player(*owner.engine);
            const auto build = resumed.tasks.front().completion;
            expect(build != 0 && !resumed.tasks.front().finished && session.snapshot()->production()->planets.front().queues[0].front().build_id == build,
                "execution waits on real paid admission build ID");
            const auto head = must(owner.actions->build_head(1, build));
            expect(head.entry && head.entry->build_id == build && head.progress == ai::Real{} && head.remaining_seconds == ai::real(1), "actual head retains build, sampled progress and independent countdown");
            const auto rows = must(owner.engine->galactic_build_budgets(*owner.actions, *owner.funding));
            expect(rows->size() == 1 && rows->front().builder == 1 && rows->front().planet == 10 && rows->front().build == build
                && rows->front().type == 100 && rows->front().cost_to_completion == ai::real(100) && rows->front().spendable == ai::real(100), "live publication carries exact task/head builder and retained goal values");
            expect(must(owner.engine->galactic_build_remaining_cost(1, resumed.tasks.front())) == f(100), "task remaining price differs from goal-budget quote");
            const auto production_revision = owner.actions->build_state_revision();
            const auto cached_builds = ai::EngineContractAccess::build_cache(*owner.engine);
            ga::FreestoreFacts moving; moving.object = {31, 101, 1, 10, ga::FreestoreKind::unit, false, false, false, {}}; moving.current_type = 101;
            moving.parent = 30; moving.ai_usable = moving.qualified = true;
            must(owner.actions->qualified_freestore().update(moving));
            const std::array<eawr::sim::EntityId, 1> offers{31}; must(owner.actions->qualified_freestore().initialize(1, offers));
            owner.actions->bind_admission_gate([] { return false; });
            owner.actions->bind_movement([](auto, const gc::GalacticSnapshot& snapshot) {
                ga::NativeActions::MovementInput input; input.ai_player = true;
                for (const auto& planet : snapshot.planets()) { gc::PathPlanet access; access.id = planet.id;
                    access.relation = gc::PathRelation::allied; input.access.push_back(access); input.orbital_occupants[planet.id] = 0; }
                return eawr::core::Result<ga::NativeActions::MovementInput>::success(std::move(input));
            }, [] { return false; }, [&session](auto player, auto fleet) { return session.recheck_native_orbit(player, fleet); });
            owner.actions->bind_freestore_movement([](auto, const auto& facts, const auto&) {
                ga::NativeActions::FreestoreMoveInput input; input.actor_valid = input.source_ai_usable = true;
                input.grandparent = facts.object.planet;
                return eawr::core::Result<ga::NativeActions::FreestoreMoveInput>::success(input);
            });
            // Publish the real packed execution route before direct freestore
            // admission, as in the movement fixture (WGA-385..389).
            ga::ExecutionRouteInput route; route.player = 1; route.maximum_distance = ga::Number(12);
            for (const auto& planet : session.snapshot()->planets())
                route.planets.push_back({planet.id, planet.position, {}, {}, {}, true});
            auto route_build = must(ga::build_execution_routes(route, executor));
            auto route_cache = std::make_shared<ga::ExecutionRoutes>(); route_cache->available = true;
            route_cache->rows = std::move(route_build.rows); route_cache->paths = std::move(route_build.paths);
            owner.actions->bind_freestore_path([route_cache](gc::PlayerId player_id, const gc::GalacticSnapshot& snapshot,
                gc::PlanetId source, gc::PlanetId destination) {
                using Result = eawr::core::Result<ga::NativeActions::FreestorePathInput>;
                auto found = ga::cached_execution_path(*route_cache, player_id, source, destination);
                if (!found) return Result::failure(found.error());
                ga::NativeActions::FreestorePathInput input;
                input.planets = std::move(found.value().planets); input.threat = found.value().threat;
                input.next_occupant = gc::PlayerId{0};
                if (input.planets.size() > 1) for (const auto& fleet : snapshot.fleets())
                    if (fleet.orbit == input.planets[1] && !fleet.motion.transit) { input.next_occupant = fleet.owner; break; }
                return Result::success(std::move(input));
            }, [] { return false; });
            owner.actions->bind_freestore_validation([](const auto&) { return eawr::core::Result<void>::success(); });
            expect(must(owner.actions->admit_freestore_move(1, 31, 20)), "actual paid head and freestore move coexist before union codec restore");
            build_alloc_probe::calls.store(0, std::memory_order_relaxed);
            build_alloc_probe::armed.store(true, std::memory_order_relaxed);
            const auto unchanged = owner.engine->galactic_build_budgets(*owner.actions, *owner.funding);
            build_alloc_probe::armed.store(false, std::memory_order_relaxed);
            expect(unchanged && unchanged.value().get() == rows.get()
                && unchanged.value()->front().build == build
                && owner.actions->build_state_revision() == production_revision
                && ai::EngineContractAccess::build_cache(*owner.engine) == cached_builds
                && build_alloc_probe::calls.load(std::memory_order_relaxed) == 0,
                "freestore-only writes reuse immutable task storage with no rescan or allocation");
            const auto movement_request = owner.actions->movements().begin()->first;
            const auto associated = must(codec.capture()); const auto producer_saved = must(owner.actions->capture());
            must(codec.restore(associated)); owner.engine->set_galactic_funding(owner.funding); must(owner.actions->restore(producer_saved));
            expect(must(codec.capture()).components == associated.components && must(owner.actions->capture()).components == producer_saved.components,
                "actual associated task, head and producer references round trip before continuation");
            must(owner.engine->begin_galactic_barrier(*owner.world, {}, owner.scripts, owner.sequence, restored_selection, &executor));
            auto& continued = ai::EngineContractAccess::player(*owner.engine);
            expect(must(owner.engine->galactic_build_budgets(*owner.actions, *owner.funding))->front().build == build
                && owner.actions->movements().at(movement_request).freestore_actor == 31
                && owner.actions->qualified_freestore().facts(31)->tracked_movement,
                "union restore republishes actual paid head and retained freestore movement association together");
            for (unsigned frame = 0; frame < 35; ++frame) (void)must(session.step(executor));
            must(owner.world->bind(session.snapshot(), {}, executor, session.snapshot()->completed_tick(), session.next_entity_id()));
            must(owner.engine->begin_galactic_barrier(*owner.world, {session.snapshot()->completed_tick(), session.snapshot()->clock().galactic_frame},
                owner.scripts, owner.sequence, restored_selection, &executor));
            must(owner.engine->service_galactic_system(1, ga::Service::execution, owner.scripts, owner.sequence));
            const auto& task = continued.tasks.front();
            expect(task.finished && !task.failed && task.object != 0 && task.object != build, "exact completion actor differs from admitted build identity");
            expect(session.snapshot()->production()->planets.front().objects.front().id == task.object, "build observer resolves actual producer-created actor");
            ai::EngineContractAccess::force(*owner.engine).galactic_stage = task.object;
            expect(!codec.capture(), "live nonplanet actor cannot stand in for a staging planet");
            ai::EngineContractAccess::force(*owner.engine).galactic_stage = 10;
            ai::EngineContractAccess::release_twice(*owner.engine);
            expect(continued.exclusion_counts.at("EXCLUDED") == 0, "repeated release unregisters exclusions once and retains zero row");
            expect(must(owner.engine->galactic_execution_reservations(1)).empty(), "release clears pending-absent execution reservation copy");
            const auto trace = session.state_sha256() + must(owner.actions->state_hash()) + must(codec.state_hash());
            if (baseline.empty()) baseline = trace; else expect(trace == baseline, "live selection/reservation/paid execution equality at 1/2/4/8");
        }
    } catch (const std::exception& error) { galactic_test::expect(false, error.what()); }
    return galactic_test::failures == 0 ? 0 : 1;
}
