// Contracts of the hosted FoC goal-system pieces (#449, docs/behaviour/foc-tactical-ai.md): the
// perceptual equations (PE-01 to PE-05), the AI random (PE-11), the target name hash (PE-12),
// the threat grid (PG-01 to PG-07), the TaskForce definitions (PL-10 to PL-12) and the
// scheduler reads the engine uses (thread slots, instance removal, command sequences). The
// M2 battle with game data (tests/skirmish/foc_plan_tests.cpp) covers the plan bindings.

#include "ai_engine.hpp"
#include "tactical_ai_internal.hpp"

#include "eawr/sim/world.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/replay.hpp"

#include <iostream>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>

namespace eawr::script::foc::ai {
// Fixture setup only; execution and admission use the production paths.
struct EngineContractAccess {
    static PlayerAi& collect_fixture(Engine& engine) {
        PlayerAi player;
        player.player = 2;
        player.assigned[14] = 99;
        player.reserved[15] = 7;
        player.reserved[16] = 1;
        engine.players_.push_back(std::move(player));
        Plan plan;
        plan.id = plan.instance = plan.goal = 1;
        plan.player = 2;
        plan.taskforces = {1};
        engine.running_.emplace(1, plan);
        engine.plan_of_instance_[1] = 1;
        TaskForce force;
        force.id = force.plan = 1;
        engine.taskforces_.emplace(1, force);
        return engine.players_.back();
    }
    static void initialize_goals(Engine& engine, PlayerAi& player) { engine.initialize_goals(player); }
    static bool matches(const Engine& engine, tactical::PlayerId player, const std::set<std::string>& flags,
        const Target& target) { return engine.target_matches(player, flags, &target); }
    static void target_fixture(Engine& engine, std::uint64_t id, sim::EntityId object) {
        Target target;
        target.id = id;
        target.object = object;
        engine.targets_.emplace(id, target);
    }
    static void order_fixture(Engine& engine, std::vector<sim::EntityId> members) {
        collect_fixture(engine);
        engine.taskforces_.at(1).units = std::move(members);
        target_fixture(engine, 1, 11);
        target_fixture(engine, 2, 0);
        target_fixture(engine, 3, 999);
    }
    static void propose(Engine& engine, PlayerAi& player) { engine.propose(player); }
    static std::uint32_t evaluated(const Engine& engine) { return engine.work_.goals_evaluated; }
    static void goals(Engine& engine, PlayerAi& player) { engine.service_goals(player); }
    static std::uint32_t maintenances(const Engine& engine) { return engine.work_.maintenances; }
    static bool selection(Engine& engine, PlayerAi& player, PotentialPlan& potential, std::uint32_t seed,
        std::uint64_t goal_id = 1) {
        engine.sync_.set_seed(seed);
        Goal goal;
        goal.id = goal_id;
        return engine.select_units(player, goal, potential);
    }
    static std::uint32_t selection_seed(const Engine& engine) { return engine.sync_.seed(); }
    static core::Result<void> attach(Engine& engine, PlayerAi& player, Goal& goal,
        authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        engine.event_tick_ = 1;
        engine.sequence_ = &sequence;
        engine.scripts_ = &scripts;
        return engine.attach_plan(player, goal, scripts, sequence);
    }
    static const std::vector<authoritative::ScriptEvent>& attachment_events(const Engine& engine) {
        return engine.events_;
    }
    static std::vector<sim::EntityId> eligible(Engine& engine, const PlayerAi& player,
        const Goal& goal, const PlanDef& plan) {
        std::vector<sim::EntityId> units;
        for (const ViewUnit* unit : engine.freestore_list(player, goal, plan)) units.push_back(unit->id);
        return units;
    }
    static std::vector<authoritative::ScriptEvent> hazard_events(Engine& engine,
        authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        engine.scripts_ = &scripts;
        engine.sequence_ = &sequence;
        engine.event_tick_ = 1;
        Plan plan;
        plan.id = plan.instance = 1;
        plan.player = 1;
        plan.taskforces = {1};
        engine.running_.emplace(1, plan);
        TaskForce force;
        force.id = force.plan = 1;
        force.name = "MainForce";
        force.units = {7};
        engine.taskforces_.emplace(1, force);
        engine.service_taskforce_events();
        return engine.events_;
    }
    static void seed(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        engine.scripts_ = &scripts;
        Plan plan;
        plan.id = plan.instance = plan.goal = 1;
        plan.player = player.player;
        plan.taskforces = {1};
        plan.reserved_pads = {1};
        engine.running_.emplace(1, plan);
        TaskForce force;
        force.id = force.plan = 1;
        engine.taskforces_.emplace(1, force);
        Block block;
        block.id = block.taskforce = 1;
        block.kind = Block::Kind::produce;
        engine.blocks_.emplace(1, block);
        BuildTask task;
        task.taskforce = task.block = 1;
        task.type = 30;
        task.source = 2;
        task.producer = 1;
        player.tasks.push_back(task);
        player.reserved_credits[1] = real(100);
    }
    static void service(Engine& engine, PlayerAi& player, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_execution(player);
    }
    static void seed_purchases(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        seed(engine, player, scripts);
        engine.running_.at(1).reserved_pads.clear();
        player.tasks.front().type = 50;
        auto later = player.tasks.front();
        later.type = 51;
        player.tasks.push_back(later);
    }
    static std::size_t orders(const Engine& engine) { return engine.orders_.size(); }
    static void seed_reinforcements(Engine& engine, PlayerAi& player, const std::uint64_t count, const bool large_block = false) {
        for (std::uint64_t id = 1; id <= count; ++id) {
            Plan plan;
            plan.id = plan.instance = id;
            plan.player = player.player;
            plan.taskforces = {id};
            engine.running_.emplace(id, plan);
            TaskForce force;
            force.id = force.plan = id;
            force.pooled = {{1, id * 2 - 1}, {1, id * 2}};
            engine.taskforces_.emplace(id, force);
            Block block;
            block.id = large_block ? block_id(first_plan_instance + id, 17) : id;
            block.taskforce = id;
            block.kind = Block::Kind::reinforce;
            engine.blocks_.emplace(block.id, block);
            player.reserved_pool[1] += 2;
            player.reserved_pool_tokens[id * 2 - 1] = id;
            player.reserved_pool_tokens[id * 2] = id;
        }
    }
    static const std::vector<authoritative::ScriptCommand>& reinforce_service(
        Engine& engine, PlayerAi& player, const std::int64_t frame) {
        engine.frame_ = frame;
        engine.work_ = {};
        engine.orders_.clear();
        engine.service_reinforcements(player);
        return engine.orders_;
    }
    static std::uint32_t reinforcement_candidates(const Engine& engine) {
        return engine.work_.reinforcement_candidates;
    }
    static PlayerAi& duplicate_fixture(Engine& engine) {
        PlayerAi player;
        player.player = 1;
        seed_reinforcements(engine, player, 1);
        engine.blocks_.clear();
        engine.plan_of_instance_[1] = 1;
        engine.players_.push_back(std::move(player));
        return engine.players_.back();
    }
    static PlayerAi& concurrent_fixture(Engine& engine) {
        PlayerAi player;
        player.player = 1;
        seed_reinforcements(engine, player, 2);
        engine.players_.push_back(std::move(player));
        return engine.players_.back();
    }
    static bool in_freestore(const Engine& engine, const ViewUnit& unit) {
        return engine.in_freestore(unit, 1);
    }
    static void abandon(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        engine.finish_plan(player, 1, scripts, true);
    }
    static bool test_valid(Engine& engine, PlayerAi& player, Goal& goal) {
        engine.frame_ = 1;
        return engine.test_valid(player, goal);
    }
    static void reinforce_at(Engine& engine, const math::Vec3& position) {
        engine.blocks_.at(block_id(first_plan_instance + 1, 17)).destination = position;
    }
    static void movement_fixture(Engine& engine, authoritative::ScriptScheduler& scripts,
        std::uint64_t& sequence) {
        engine.scripts_ = &scripts;
        engine.sequence_ = &sequence;
        engine.event_tick_ = 1;
        Plan plan;
        plan.id = plan.instance = 1;
        plan.player = 1;
        plan.taskforces = {1};
        engine.running_.emplace(1, plan);
        TaskForce force;
        force.id = force.plan = 1;
        force.name = "MainForce";
        force.units = {10};
        engine.taskforces_.emplace(1, force);
        for (const auto kind : {Block::Kind::move, Block::Kind::ambush}) {
            Block block;
            block.id = kind == Block::Kind::move ? 1 : 2;
            block.taskforce = 1;
            block.kind = kind;
            block.destination_object = kind == Block::Kind::ambush ? 20 : 0;
            block.movers = {10};
            block.ordered_tick[10] = 0;
            engine.blocks_.emplace(block.id, std::move(block));
        }
    }
    static void movement_service(Engine& engine, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_blocks();
    }
    static bool movement_finished(const Engine& engine, std::uint64_t id) {
        return engine.blocks_.at(id).finished;
    }
    static void guard_fixture(Engine& engine, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        movement_fixture(engine, scripts, sequence);
        auto& guard = engine.blocks_.at(1);
        guard.order = Block::Order::guard;
        guard.destination_object = 20;
        auto& move = engine.blocks_.at(2);
        move.kind = Block::Kind::move;
        move.order = Block::Order::move;
    }
    static void empty_guard_force(Engine& engine) { engine.taskforces_.at(1).units.clear(); }
};
} // namespace eawr::script::foc::ai

namespace {

namespace ai = eawr::script::foc::ai;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
using ai::Real;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Fixed final : ai::LookupResolver {
    std::optional<Real> resolve(const ai::Lookup& lookup, const std::vector<ai::Binding>& bindings) override {
        std::string path;
        for (const std::string& token : lookup.tokens) path += (path.empty() ? "" : ".") + token;
        last_bindings = bindings;
        if (path == "VARIABLE_TARGET.HEALTH") return ai::real(1) / ai::real(2);
        if (path == "GAME.AGE") return ai::real(30);
        return std::nullopt;
    }
    std::vector<ai::Binding> last_bindings;
};

std::optional<Real> evaluate(std::string_view body) {
    const ai::ConverterFunction converters = [](std::string_view converter, std::string_view value) -> std::optional<Real> {
        if (converter == "GameObjectCategoryType" && ai::trimmed(value) == "Fighter | Bomber") return ai::real(3);
        return std::nullopt;
    };
    auto equation = ai::parse_equation("Test", body, converters);
    if (!equation) return std::nullopt;
    Fixed resolver;
    ai::AiRandom random;
    random.set_seed(7);
    return ai::run(equation.value(), resolver, random);
}

void equations() {
    expect(evaluate("1 + 2 * 3") == ai::real(7), "PE-03: * binds tighter than +");
    expect(evaluate("(1 + 2) * 3") == ai::real(9), "PE-03: parentheses");
    expect(evaluate("2 > 1") == ai::real(1) && evaluate("2 < 1") == ai::real(0), "PE-04: comparisons give 1 or 0");
    expect(evaluate("0 / 0") == ai::real(0), "PE-04: 0 / x is 0");
    expect(!evaluate("4 / 0").has_value(), "PE-04: a division by 0 fails the equation");
    expect(evaluate("clamp(5, 0, 3)") == ai::real(3), "PE-04: clamp");
    expect(!evaluate("clamp(1, 3, 0)").has_value(), "PE-04: clamp with high < low fails");
    expect(evaluate("Variable_Target.Health * 4") == ai::real(2), "PE-05: a token chain resolves through the host");
    expect(!evaluate("Variable_Target.Unknown").has_value(), "PE-05: an unknown token fails the equation");
    expect(evaluate("3 # 3") == ai::real(3), "PE-11: a # b draws in [a, b]");
    const auto drawn = evaluate("10 # 20");
    expect(drawn && !(*drawn < ai::real(10)) && !(ai::real(20) < *drawn), "PE-11: the draw stays in range");
    expect(evaluate("Game.Age {Parameter_Category = GameObjectCategoryType[Fighter | Bomber]}") == ai::real(30),
        "PE-01: converter constants bind as parameters");
}

void random_and_hash() {
    // PE-11: seed = seed * 0x41C64E6D + 0xBDF (mod 2^32); the draw is bits 10 to 24.
    ai::AiRandom random;
    random.set_seed(12345);
    std::uint32_t seed = 12345;
    bool same = true;
    for (int step = 0; step < 16; ++step) {
        seed = seed * 0x41C64E6DU + 0xBDFU;
        same = same && random.next() == ((seed >> 10) & 0x7FFFU);
    }
    expect(same, "PE-11: the AI random is the engine's linear congruential generator");
    expect(ai::crc32("123456789") == 0xCBF43926U, "PE-12: the target name hash is CRC-32");
}

// Two players on different teams, one AI unit type of power 1000 and reach 500 on a 2000 x 2000 map.
struct World {
    std::shared_ptr<ai::Host> host = std::make_shared<ai::Host>();
    std::shared_ptr<ai::WorldView> view = std::make_shared<ai::WorldView>();
    World() {
        foc::AiType type;
        type.type_id = 1;
        type.name = "SHIP";
        type.category_bits = 2;
        type.combat_power = ai::real(1000);
        type.max_attack_distance = ai::real(500);
        type.locomotor = true;
        host->setup.content.types.push_back(type);
        host->setup.content.categories.emplace("CORVETTE", 2);
        host->setup.content.categories.emplace("FIGHTER", 1);
        host->setup.players = {foc::AiPlayer{1, "REBEL", false, true, ""}, foc::AiPlayer{2, "EMPIRE", false, true, ""}};
        for (const foc::AiType& entry : host->setup.content.types) {
            host->types.emplace(entry.type_id, &entry);
            host->types_by_name.emplace(entry.name, &entry);
        }
        for (const foc::AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
        view->players = {foc::detail::ViewPlayer{1, 1, 0}, foc::detail::ViewPlayer{2, 2, 1}};
        ai::ViewUnit unit;
        unit.id = 10;
        unit.type = 1;
        unit.owner = 2;
        view->units.push_back(unit);
        host->view = view;
    }
};

void capture_target_application() {
    World world;
    auto nonpad = world.host->setup.content.types.front();
    nonpad.capture_point = true;
    auto pad = nonpad;
    pad.type_id = 2;
    pad.locomotor = false;
    pad.build_pad = true;
    world.host->setup.content.types = {nonpad, pad};
    world.host->types.clear();
    for (const auto& type : world.host->setup.content.types) world.host->types.emplace(type.type_id, &type);
    world.host->setup.players.push_back(foc::AiPlayer{4, "NEUTRAL", true, false, ""});
    world.host->players.clear();
    for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
    world.view->players.push_back(foc::detail::ViewPlayer{4, 4, 0});
    world.view->units.clear();
    for (const auto& [id, type] : {std::pair{10U, 1U}, std::pair{11U, 2U}, std::pair{12U, 1U}}) {
        ai::ViewUnit unit;
        unit.id = id; unit.type = type; unit.owner = 4;
        world.view->units.push_back(unit);
    }
    eawr::sim::tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
        {2, 2, 2, eawr::sim::tactical::player_flag_commandable}, {4, 4, 4, 0}};
    setup.units = {{10, 1, 4}, {11, 2, 4}, {12, 1, 4}};
    eawr::sim::tactical::EconomyRules rules;
    rules.pads.neutral = 4;
    const auto radius = eawr::sim::math::Fixed::from_integer(100).value();
    const auto seconds = eawr::sim::math::Fixed::from_integer(1).value();
    rules.pads.capture = {{1, radius, seconds, {1, 2}, false, true, false},
        {2, radius, seconds, {1, 2}, false, true, true}};
    auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(session), "GS-11: capturable non-pad and build-pad world starts");
    if (!session) return;
    world.host->world = &session.value();
    ai::Engine engine(world.host, {}, {});
    ai::Target ordinary; ordinary.object = 10;
    ai::Target build; build.object = 11;
    const std::set<std::string> enemy{"ENEMY_UNIT", "ENEMY_STRUCTURE"};
    const auto matches = [&](const ai::Target& target, const std::set<std::string>& flags) {
        return ai::EngineContractAccess::matches(engine, 2, flags, target);
    };
    expect(matches(ordinary, enemy) && !matches(ordinary, {"ENEMY_BUILD_PAD"}),
        "GS-11: a non-allied capturable non-pad participates in ordinary target evaluation");
    expect(matches(build, {"ENEMY_BUILD_PAD"}) && !matches(build, enemy),
        "GS-11: a real build pad takes the build-pad application flags");
    world.view->units[0].owner = world.view->units[1].owner = 2;
    expect(matches(ordinary, {"FRIENDLY_UNIT"}) && !matches(ordinary, enemy),
        "GS-11: allied capture ownership uses the friendly ordinary target kind");
    expect(matches(build, {"FRIENDLY_BUILD_PAD"}) && !matches(build, {"ENEMY_BUILD_PAD"}),
        "GS-11: allied build pads use friendly pad flags");
    world.view->units[0].owner = world.view->units[1].owner = 4;
    world.host->setup.content.types[0].locomotor = false;
    expect(matches(ordinary, {"ENEMY_STRUCTURE"}) && !matches(ordinary, {"ENEMY_UNIT"}),
        "GS-11: a stationary capturable non-pad is an ordinary structure, not a unit");
    world.view->units[0].owner = 2;
    expect(matches(ordinary, {"FRIENDLY_STRUCTURE"}) && !matches(ordinary, {"FRIENDLY_UNIT"}),
        "GS-11: allied stationary capturable non-pads keep their structure kind");
    world.view->units[0].owner = 4;
    ai::GoalType goal; goal.application = enemy;
    ai::PlayerAi player; player.player = 2; player.per_frame = 3; player.targets = {1, 2, 3};
    for (std::size_t index = 0; index < 7; ++index) {
        ai::GoalFunctionEntry function; function.goal = &goal;
        function.goal_name = "CAPTURE_TARGET_" + std::to_string(index);
        player.functions.push_back(std::move(function));
    }
    ai::EngineContractAccess::target_fixture(engine, 1, 10);
    ai::EngineContractAccess::target_fixture(engine, 2, 12);
    ai::EngineContractAccess::target_fixture(engine, 3, 11);
    std::size_t frames = 0;
    while (!player.maintenance_due && frames < 10) {
        ++frames; ai::EngineContractAccess::propose(engine, player);
    }
    expect(frames == 5 && player.maintenance_due && player.nontrivial == 14
        && ai::EngineContractAccess::evaluated(engine) == 14,
        "GS-04/11: two non-pad targets count against all seven functions even without a desire equation");
    // GS-11: application kind is object behavior, also in combat-only sessions.
    auto combat = eawr::sim::tactical::TacticalSession::create(setup);
    expect(static_cast<bool>(combat), "GS-11: combat-only world starts without pad economy profiles");
    if (!combat) return;
    world.host->world = &combat.value();
    expect(matches(build, {"ENEMY_BUILD_PAD"}) && !matches(build, enemy),
        "GS-11: a neutral real pad never falls through to enemy structure without economy");
    expect(matches(ordinary, {"ENEMY_STRUCTURE"}) && !matches(ordinary, {"ENEMY_BUILD_PAD"}),
        "GS-11: stationary capturable non-pads remain ordinary targets without economy");
    world.view->units[1].owner = 2;
    expect(matches(build, {"FRIENDLY_BUILD_PAD"}) && !matches(build, {"FRIENDLY_STRUCTURE"}),
        "GS-11: an allied real pad keeps its pad kind without economy");
}

void collect_free_categories() {
    struct Case { std::string arguments; std::vector<eawr::sim::EntityId> members; bool valid; };
    const std::array<Case, 6> cases{{
        {"'Fighter | Corvette'", {10, 12, 16}, true},
        {"", {10, 11, 12, 16}, true},
        {"'None'", {}, true},
        {"123", {}, false},
        {"'Unknown'", {}, false},
        {"'Fighter', 'Bomber'", {}, false},
    }};
    for (const auto& test : cases) for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto base = world.host->setup.content.types.front();
        world.host->setup.content.types.clear();
        world.host->setup.content.categories = {{"FIGHTER", std::uint64_t{1} << 63}, {"BOMBER", 2},
            {"CORVETTE", 4}, {"NONE", 0}};
        for (std::uint32_t id = 1; id <= 5; ++id) {
            auto type = base;
            type.type_id = id;
            type.category_bits = id == 2 ? 2 : id == 3 ? 4 : id == 5 ? 0 : std::uint64_t{1} << 63;
            type.locomotor = id != 4;
            world.host->setup.content.types.push_back(type);
        }
        world.host->types.clear();
        for (const auto& type : world.host->setup.content.types) world.host->types.emplace(type.type_id, &type);
        base = world.host->setup.content.types.front();
        const auto initial = world.view->units.front();
        world.view->units.clear();
        for (eawr::sim::EntityId id = 10; id <= 18; ++id) {
            auto unit = initial;
            unit.id = id;
            unit.type = id == 11 ? 2 : id == 12 ? 3 : id == 17 ? 4 : id == 18 ? 5 : 1;
            unit.owner = id == 13 ? 1 : 2;
            world.view->units.push_back(unit);
        }
        ai::Engine engine(world.host, {}, {});
        world.host->engine = &engine;
        auto& player = ai::EngineContractAccess::collect_fixture(engine);
        auth::ModuleManifest manifest;
        expect(manifest.add("COLLECT.LUA", "function Check(tf) tf.Collect_All_Free_Units(" + test.arguments + ") end\n")
            .has_value(), "EX-12: collect fixture loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "EX-12: collect scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "COLLECT.LUA").has_value(),
            "EX-12: real TaskForce binding registers");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (std::uint64_t repeat = 0; repeat < (test.valid ? 2U : 1U); ++repeat) {
            auth::ScriptEvent event;
            event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, repeat};
            event.target = 1;
            event.kind = auth::ScriptEvent::Kind::call;
            event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{ai::handle_taskforce, 1}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "EX-12: collection call queues");
            auto report = scripts.value().service(executor);
            expect(report.has_value(), "EX-12: collection binding runs");
            if (!report) return;
            expect(report.value().diagnostics.empty() == test.valid, "EX-12: invalid category arguments fail");
            if (!test.valid) expect(report.value().commands.empty(), "EX-12: a rejected call stages no collection");
            expect(engine.after_service(report.value()).has_value(), "EX-12: staged collection applies");
            expect(engine.taskforce(1)->units == test.members,
                "EX-12: union/all/none collect only matching own free units and never duplicate members");
            expect(player.assigned.at(14) == 99 && !player.assigned.contains(13) && !player.assigned.contains(15)
                && !player.assigned.contains(17) && !player.assigned.contains(18),
                "EX-12: ownership, other assignments/reservations, non-movers and category-none stay excluded");
        }
    }
}

void order_destinations() {
    struct Case {
        std::uint32_t receiver;
        std::string method;
        auth::Value target;
        std::vector<eawr::sim::EntityId> members;
        bool valid;
        std::uint64_t object;
        int x, y, z;
        bool command{true};
        bool parent{};
    };
    const auto handle = [](std::uint32_t kind, std::uint64_t id) { return auth::Value{auth::Handle{kind, id}}; };
    const auto point = foc::detail::position_value({eawr::sim::math::Fixed::from_integer(7).value(),
        eawr::sim::math::Fixed::from_integer(8).value(), eawr::sim::math::Fixed::from_integer(9).value()});
    const auto force = handle(ai::handle_taskforce, 1);
    const auto object = handle(foc::handle_game_object, 11);
    const auto target = handle(ai::handle_ai_target, 1);
    const auto region = handle(ai::handle_ai_target, 2);
    const std::vector<Case> cases{
        {foc::handle_game_object, "Attack_Move", force, {10, 11}, true, 0, 20, 30, 40},
        {foc::handle_game_object, "Move_To", force, {10, 11}, true, 0, 20, 30, 40},
        {foc::handle_game_object, "Guard_Target", force, {10, 11}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Guard_Target", force, {10, 11, 12}, true, 12, 0, 0, 0, true, true},
        {foc::handle_game_object, "Guard_Target", object, {10, 11}, true, 0, 30, 40, 50, true, true},
        {foc::handle_game_object, "Guard_Target", force, {10}, true, 0, 10, 20, 30},
        {foc::handle_game_object, "Attack_Move", force, {}, true, 0, 0, 0, 0},
        {foc::handle_game_object, "Guard_Target", force, {}, true, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Move", target, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Guard_Target", target, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Move_To", target, {10}, true, 0, 30, 40, 50},
        {foc::handle_game_object, "Attack_Move", region, {10}, true, 0, 0, 0, 0},
        {foc::handle_game_object, "Guard_Target", point, {10}, true, 0, 7, 8, 9},
        {foc::handle_game_object, "Attack_Move", object, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", object, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", target, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", region, {10}, false, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", force, {10}, false, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", point, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", object, {10}, true, 11, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", target, {10}, true, 11, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", region, {10}, true, 0, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", force, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", point, {10}, false, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Move", handle(foc::handle_game_object, 10), {10}, true, 0, 10, 20, 30},
        {foc::handle_game_object, "Attack_Move", handle(ai::handle_ai_target, 3), {10}, false, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Move", auth::Value{}, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Move", force, {10, 11}, true, 0, 20, 30, 40},
        {ai::handle_taskforce, "Guard_Target", force, {11}, true, 11, 0, 0, 0},
        {ai::handle_taskforce, "Guard_Target", force, {10, 11}, true, 999, 0, 0, 0},
        {ai::handle_taskforce, "Guard_Target", point, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Guard_Target", auth::Value{}, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Guard_Target", handle(ai::handle_ai_target, 99), {10}, true, 0, 0, 0, 0, false},
        {ai::handle_taskforce, "Guard_Target", handle(ai::handle_ai_target, 3), {10}, true, 0, 0, 0, 0, false},
        {ai::handle_taskforce, "Guard_Target", force, {}, true, 0, 0, 0, 0, false},
    };
    for (const auto& test : cases) {
      std::optional<std::uint64_t> random_member;
      for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto& first = world.view->units.front();
        first.position = {eawr::sim::math::Fixed::from_integer(10).value(), eawr::sim::math::Fixed::from_integer(20).value(),
            eawr::sim::math::Fixed::from_integer(30).value()};
        auto second = first;
        if (test.parent) first.squadron = 11;
        second.id = 11;
        second.position = {eawr::sim::math::Fixed::from_integer(30).value(), eawr::sim::math::Fixed::from_integer(40).value(),
            eawr::sim::math::Fixed::from_integer(50).value()};
        world.view->units.push_back(second);
        second.id = 12;
        world.view->units.push_back(second);
        ai::Engine engine(world.host, {}, {});
        world.host->engine = &engine;
        ai::EngineContractAccess::order_fixture(engine, test.members);
        auth::ModuleManifest manifest;
        expect(manifest.add("ORDER.LUA", "function Check(receiver, target) receiver." + test.method + "(target) end\n")
            .has_value(), "FH-41/EX-32: order module loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "FH-41/EX-32: scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "ORDER.LUA").has_value(),
            "FH-41/EX-32: production bindings register");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call;
        event.name = "Check";
        event.arguments = {handle(test.receiver, test.receiver == ai::handle_taskforce ? 1 : 10), test.target};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "FH-41/EX-32: order queues");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto report = scripts.value().service(executor);
        if (!report) { expect(false, "FH-41/EX-32: service succeeds"); return; }
        const auto label = test.method + " receiver=" + std::to_string(test.receiver);
        expect(report.value().diagnostics.empty() == test.valid, "FH-41/EX-32: permitted/rejected conversions: " + label);
        expect(report.value().commands.size() == (test.valid && test.command ? 1U : 0U),
            "FH-41/EX-32: rejected/dead destinations stage no orders: " + label);
        if (report.value().commands.empty()) continue;
        const auto& args = report.value().commands.front().arguments;
        if (test.receiver == ai::handle_taskforce) {
            expect(args.size() == 4, "EX-32: TaskForce stages one movement block");
            if (args.size() != 4) continue;
            if (test.object != 0) {
                const auto member = std::get<auth::Handle>(args[3].data).id;
                if (test.object == 999) {
                    if (!random_member) random_member = member;
                    expect((member == 10 || member == 11) && member == *random_member,
                        "EX-32: random member selection is equal on 1/2/4/8 workers");
                } else expect(member == test.object, "EX-32: guard retains member");
            }
            else {
                const auto& coordinates = std::get<std::vector<auth::Value>>(args[3].data);
                expect(coordinates.size() == 3 && std::get<Real>(coordinates[0].data) == ai::real(test.x)
                    && std::get<Real>(coordinates[1].data) == ai::real(test.y)
                    && std::get<Real>(coordinates[2].data) == ai::real(test.z), "EX-32: attack-move takes centroid");
            }
        } else if (test.object != 0) {
            expect(args.size() == 3 && std::get<auth::Handle>(args[2].data).id == test.object, "FH-41: follow object");
        } else {
            expect(args.size() == 5 && std::get<Real>(args[2].data) == ai::real(test.x)
                && std::get<Real>(args[3].data) == ai::real(test.y) && std::get<Real>(args[4].data) == ai::real(test.z),
                "FH-41: movement/attack-move/guard takes point: " + label);
        }
    }
    }
}

void initial_goal_budget() {
    World world;
    ai::Engine engine(world.host, ai::AiData{}, {});
    ai::GoalType global;
    global.application = {"GLOBAL"};
    ai::PlayerAi player;
    player.functions.resize(151);
    for (auto& function : player.functions) function.goal = &global;
    ai::EngineContractAccess::initialize_goals(engine, player);
    expect(player.per_frame == 2, "GS-05: an empty target list estimates one target and rounds 151 evaluations up");
    ai::EngineContractAccess::goals(engine, player);
    expect(player.next_function == 2 && player.nontrivial == 2
        && ai::EngineContractAccess::maintenances(engine) == 0,
        "GS-05: the first proposal service consumes the initialized budget");
    for (int frame = 1; frame < 76; ++frame) ai::EngineContractAccess::goals(engine, player);
    expect(ai::EngineContractAccess::maintenances(engine) == 1 && player.nontrivial == 0 && player.per_frame == 2,
        "GS-05: first maintenance uses the actual pass count and resets its accumulator");

    ai::PlayerAi sparse;
    sparse.targets.resize(151);
    sparse.functions.resize(1);
    sparse.functions.front().goal = &global;
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 2, "GS-05: initialization estimates targets times functions before applicability");
    ai::EngineContractAccess::goals(engine, sparse);
    expect(sparse.per_frame == 1 && ai::EngineContractAccess::maintenances(engine) == 2,
        "GS-05: later budgets use the actual one global evaluation, not the initial estimate");
    sparse.targets.resize(150);
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 1, "GS-05: an exact five-second evaluation budget does not round up");
    sparse.functions.clear();
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 1, "GS-05: no goal functions still leaves a positive service budget");

    world.host->setup.players.resize(8);
    world.host->setup.players.front() = foc::AiPlayer{1, "REBEL", false, false, "", true};
    for (std::size_t index = 1; index < 8; ++index) {
        world.host->setup.players[index] = foc::AiPlayer{static_cast<eawr::sim::tactical::PlayerId>(index + 1),
            index == 2 ? "UNDERWORLD" : "EMPIRE", index == 7, index <= 2, ""};
    }
    sparse.targets.resize(2000);
    sparse.functions.resize(1);
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 3,
        "GS-05: two AI and five non-playable faction players cap at rounded 20/7 with one human");
    world.host->setup.players.resize(5);
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 5,
        "GS-05: three-faction FFA with Empire/Underworld AI and two faction players counts only the human");
}

void selection_team_starts() {
    World world;
    auto second = world.host->setup.content.types.front();
    second.type_id = 2;
    second.name = "OTHER_SHIP";
    world.host->setup.content.types.push_back(second);
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const auto& type : world.host->setup.content.types) {
        world.host->types.emplace(type.type_id, &type);
        world.host->types_by_name.emplace(type.name, &type);
    }
    auto unit = world.view->units.front();
    unit.id = 11;
    unit.type = 2;
    world.view->units.push_back(unit);
    const auto force = [](eawr::sim::tactical::TypeId type) {
        ai::TaskForceDef result;
        ai::TeamDef team;
        team.max_count = 1;
        team.types = {type};
        result.teams.push_back(team);
        return result;
    };
    ai::PlayerAi player;
    player.player = 2;
    ai::PotentialPlan potential;
    ai::PlanDef plan;
    plan.taskforces = {force(1), force(2)};
    ai::Engine singleton(world.host, {}, {plan});
    expect(ai::EngineContractAccess::selection(singleton, player, potential, 12345),
        "PL-22: types with a single eligible team select both available objects");
    expect(potential.taskforce_of_unit == std::vector<std::size_t>{0, 1}
        && ai::EngineContractAccess::selection_seed(singleton) == 12345,
        "PL-22: distinct single-team types consume no synchronized draw");
    world.view->units.clear();
    ai::Engine empty(world.host, {}, {plan});
    expect(!ai::EngineContractAccess::selection(empty, player, potential, 12345)
        && ai::EngineContractAccess::selection_seed(empty) == 12345,
        "PL-22: a multi-team plan without proposals consumes no synchronized draw");
    world.view->units = {unit};
    world.view->units.front().id = 10;
    world.view->units.front().type = 1;
    world.view->units.push_back(unit);
    plan.taskforces = {force(1), force(2), force(1), force(2)};
    ai::Engine independent(world.host, {}, {plan});
    expect(ai::EngineContractAccess::selection(independent, player, potential, 12345),
        "PL-22: types with multiple eligible teams select both available objects");
    expect(potential.taskforce_of_unit == std::vector<std::size_t>{0, 3}
        && ai::EngineContractAccess::selection_seed(independent) == 0xa4451d33U,
        "PL-22: each type draws independently among its own teams before allocation");
}

void shared_station_perception() {
    World world;
    auto& station = world.host->setup.content.types.front();
    station.star_base = true;
    station.base_level = 3;
    station.tech_level = 7;
    auto enemy = station;
    enemy.type_id = 2;
    enemy.name = "ENEMY_STATION";
    enemy.base_level = 5;
    world.host->setup.content.types.push_back(enemy);
    world.host->types.clear();
    for (const auto& type : world.host->setup.content.types) world.host->types.emplace(type.type_id, &type);
    world.view->players = {{1, 0, 0}, {2, 1, 1}, {3, 0, 2}, {4, 1, 3}};
    eawr::sim::tactical::TacticalSetup setup;
    setup.players = {{1, 0, 1, eawr::sim::tactical::player_flag_commandable},
        {2, 1, 2, eawr::sim::tactical::player_flag_commandable},
        {3, 0, 1, eawr::sim::tactical::player_flag_commandable},
        {4, 1, 2, eawr::sim::tactical::player_flag_commandable}};
    setup.units = {{10, 1, 1, {}}, {20, 2, 2, {}}};
    eawr::sim::tactical::EconomyRules economy;
    economy.players = {{1, {}, 20}, {2, {}, 20}, {3, {}, 20}, {4, {}, 20}};
    auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
    expect(static_cast<bool>(session), "SAE-02: four ledgers share two live stations");
    if (!session) return;
    world.host->world = &session.value();
    world.host->snapshot = session.value().snapshot();
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
        return std::nullopt;
    };
    auto equations = ai::EquationSet::parse({{"team.xml", "<Root><Level>Variable_Self.BaseLevel</Level></Root>"}}, converters);
    expect(static_cast<bool>(equations), "SAE-02: shared station perception equation parses");
    if (!equations) return;
    ai::AiData data;
    data.equations = std::move(equations).value();
    ai::Engine engine(world.host, std::move(data), {});
    for (const auto player : {1U, 3U})
        expect(engine.evaluate("Level", player, nullptr) == ai::real(3),
            "SAE-02: each teammate perceives its allied station's base level, excluding higher enemy tech");
    for (const auto player : {2U, 4U})
        expect(engine.evaluate("Level", player, nullptr) == ai::real(5),
            "SAE-02: the opposing team's second player also sees its shared station");
    expect(static_cast<bool>(session.value().stage_remove(10)), "SAE-02: remove shared station");
    for (const auto player : {1U, 3U})
        expect(engine.evaluate("Level", player, nullptr) == Real{},
            "SAE-02: station loss clears both teammates' level without borrowing the enemy's");
    expect(engine.evaluate("Level", 4, nullptr) == ai::real(5), "SAE-02: other team's station survives independently");
}

void economy_inputs() {
    World world;
    world.host->setup.perception.campaign_game = false;
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
    auto equations = ai::EquationSet::parse({{"economy.xml", "<Root>"
        "<Campaign>Game.IsCampaignGame</Campaign>"
        "<Credits>Variable_Self.CreditsUnnormalized</Credits>"
        "<Room>Variable_Self.UnitSpaceAvailable</Room>"
        "<Pool>Variable_Self.ReinforcementsUnnormalized</Pool>"
        "<Level>Variable_Self.BaseLevel</Level>"
        "<Structures>Variable_Self.TacticalBuiltStructureCount{Parameter_Type = \"SHIP\"}</Structures>"
        "<FilteredPool>Variable_Self.ReinforcementsUnnormalized{Parameter_Category = 1}</FilteredPool>"
        "<OpenPads>Variable_Self.OpenBuildPadCount{Parameter_Type = \"SHIP\"}</OpenPads>"
        "<OtherPads>Variable_Self.OpenBuildPadCount{Parameter_Type = \"OTHER\"}</OtherPads>"
        "<Contestable>Variable_Target.IsContestable</Contestable>"
        "<BuildPad>Variable_Target.IsBuildPad</BuildPad>"
        "</Root>"}}, converters);
    expect(static_cast<bool>(equations), "SAE-02: economy equations parse");
    if (!equations) return;
    ai::AiData data;
    data.equations = std::move(equations).value();
    ai::Engine engine(world.host, std::move(data), {});
    expect(engine.evaluate("Campaign", 1, nullptr) == Real{}, "SAE-01: skirmish campaign input is zero");
    expect(engine.evaluate("Credits", 1, nullptr) == Real{}, "SAE-02: absent economy has no spendable credits");
    const auto snapshot = [&](std::uint32_t population, std::uint32_t cap, std::int64_t credits) {
        eawr::sim::tactical::EconomyView account;
        account.player = 1;
        account.credits = eawr::sim::math::Fixed::from_integer(credits).value();
        account.population = population;
        account.population_cap = cap;
        account.pool = {1, 1};
        world.host->snapshot = std::make_shared<eawr::sim::tactical::TacticalSnapshot>(0,
            std::vector<eawr::sim::tactical::SnapshotPlayer>{}, std::vector<eawr::sim::tactical::TacticalInstance>{},
            std::vector<eawr::sim::tactical::Event>{}, std::vector<eawr::sim::tactical::CombatEvent>{},
            std::vector<eawr::sim::tactical::Projectile>{}, std::nullopt,
            std::vector<eawr::sim::tactical::SpinningCraft>{}, std::vector<eawr::sim::tactical::SquadronTarget>{},
            std::vector<eawr::sim::tactical::Squadron>{}, std::vector<eawr::sim::tactical::EconomyView>{account});
    };
    snapshot(7, 20, 1499);
    expect(engine.evaluate("Credits", 1, nullptr) == ai::real(1499), "SAE-02: credits read the completed account");
    expect(engine.evaluate("Room", 1, nullptr) == ai::real(13), "SAE-02: population room is cap minus population");
    expect(engine.evaluate("Pool", 1, nullptr) == ai::real(2000), "SAE-02: reinforcement power includes both pooled units");
    expect(engine.evaluate("FilteredPool", 1, nullptr) == Real{}, "SAE-02: reinforcement power honors the requested category");
    snapshot(23, 20, 1500);
    expect(engine.evaluate("Room", 1, nullptr) == Real{}, "SAE-02: an over-cap account never underflows room");
    expect(engine.evaluate("Credits", 1, nullptr) == ai::real(1500), "SAE-02: the saving threshold uses current credits");
    eawr::sim::tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
                     {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
    setup.units = {{10, 1, 1}, {11, 1, 1}, {12, 1, 2}};
    eawr::sim::tactical::EconomyRules rules;
    rules.players = {{1, eawr::sim::math::Fixed::from_integer(1000).value(), 0},
        {2, eawr::sim::math::Fixed::from_integer(1000).value(), 0}};
    rules.pads.neutral = 2;
    rules.pads.capture = {{1, eawr::sim::math::Fixed::from_integer(100).value(),
        eawr::sim::math::Fixed::from_integer(1).value(), {1, 2}, false, true, true}};
    auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(session), "SAE-02: the synthetic ownership world starts");
    if (session) {
        world.host->world = &session.value();
        world.host->setup.content.types[0].star_base = true;
        world.host->setup.content.types[0].base_level = 3;
        world.host->setup.content.types[0].tech_level = 7;
        expect(engine.evaluate("Level", 1, nullptr) == ai::real(3), "SAE-02: station level uses Base_Level, independently of Tech_Level");
        expect(engine.evaluate("Structures", 1, nullptr) == ai::real(2), "SAE-02: completed structure counts filter type and player");
        expect(session.value().production_counts(1, 1).owned_player == 2, "SAE-03: ownership cache starts from live units");
        expect(session.value().production_counts(99, 1).current_allies == 0,
            "SAE-02: an undeclared AI service slot has no production ownership");
        expect(!session.value().build_allowed(99, 10, 1), "SAE-03: an undeclared buyer cannot use a live producer");
        world.host->setup.content.types[0].capture_point = true;
        world.view->units.clear();
        for (const auto& placed : setup.units) {
            ai::ViewUnit unit;
            unit.id = placed.entity_id;
            unit.type = placed.type_id;
            unit.owner = placed.owner;
            world.view->units.push_back(unit);
        }
        expect(engine.evaluate("OpenPads", 1, nullptr) == ai::real(2), "SAE-02: open pad count excludes the other player's pads");
        expect(engine.evaluate("OtherPads", 1, nullptr) == Real{}, "SAE-02: open pad count honors the requested type");
        ai::Target target;
        target.object = 10;
        expect(engine.evaluate("Contestable", 1, &target) == ai::real(1), "SAE-02: capture targets expose real contestability");
        expect(engine.evaluate("BuildPad", 1, &target) == ai::real(1), "SAE-02: capture targets expose their build capability");
    }
    ai::GoalType goal;
    goal.time_limit = ai::real(30);
    goal.build_time_delay_tolerance = ai::real(2);
    expect(ai::production_time_limit(goal, std::nullopt, ai::real(5)) == ai::real(35), "SAE-05: no potential uses the authored time limit");
    expect(ai::production_time_limit(goal, ai::real(10), ai::real(5)) == ai::real(30), "SAE-05: potential limit multiplies duration plus adjustment");
    expect(ai::production_time_allowed(goal, ai::real(10), ai::real(20)), "SAE-05: equality is admitted");
    expect(!ai::production_time_allowed(goal, ai::real(10), ai::real(21)), "SAE-05: exceeding a positive limit is rejected");
    goal.build_time_delay_tolerance = Real{};
    expect(ai::production_time_allowed(goal, ai::real(10), ai::real(200)), "SAE-05: a zero limit does not reject");
}

void tactical_activation_estimate() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    for (const int scenario : {0, 1, 2}) {
        t::TacticalSetup start;
        start.players = {{1, 1, 100, t::player_flag_commandable}};
        start.units = {{1, 10, 1}};
        t::EconomyRules rules;
        rules.players = {{1, q(scenario == 2 ? 0 : 1000), 20}};
        rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 900, 900, 1, true}}}};
        auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "SAE-09: activation fixture starts");
        if (!made) continue;
        if (scenario == 1) {
            expect(made.value().submit({{0, 1, 0}, {1}, t::BuyPayload{50}}).has_value(),
                "SAE-09: long-running production enters the queue");
            eawr::sim::InlineExecutor executor;
            expect(made.value().step(executor).has_value(), "SAE-09: queued purchase advances");
            expect(!made.value().ledgers().front().queues[0].empty(), "SAE-09: existing delay is present");
        }
        auto host = std::make_shared<ai::Host>();
        host->setup.perception.campaign_game = false;
        foc::AiType factory;
        factory.type_id = 10;
        factory.star_base = true;
        host->types.emplace(10, &factory);
        host->world = &made.value();
        host->snapshot = made.value().snapshot();
        host->view = foc::detail::build_view(made.value(), *host->snapshot);
        ai::PlanDef definition;
        definition.allow_free_store = false;
        definition.ignore_target = true;
        ai::TaskForceDef force;
        ai::TeamDef team;
        team.min_count = team.max_count = 1;
        team.types = {50};
        force.teams = {team};
        definition.taskforces = {force};
        ai::Engine engine(host, {}, {definition});
        expect(engine.producer(1, 50) == 1 && host->economy(1) != nullptr,
            "SAE-09: tactical selection has a producer and a current credit account");
        ai::GoalType type;
        type.time_limit = ai::real(1);
        type.build_time_delay_tolerance = ai::real(1) / ai::real(100);
        ai::PlayerAi player;
        player.player = 1;
        ai::GoalFunctionEntry function;
        function.goal = &type;
        player.functions.push_back(function);
        ai::Goal goal;
        goal.id = 1;
        goal.potential.emplace();
        goal.potential->valid = true;
        const bool allowed = ai::EngineContractAccess::test_valid(engine, player, goal);
        expect(allowed == (scenario != 2),
            "SAE-09/03: production duration and queue delay do not block selection; insufficient credits do");
        if (allowed) expect(goal.potential->units == std::vector<t::TypeId>{50}
            && goal.potential->sources == std::vector<std::uint8_t>{2},
            "SAE-09: selected new production survives a positive delay limit below its duration");
    }
}

void production_lifecycle() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    // Stations use the owner's faction even when an allied buyer has another menu.
    t::TacticalSetup start;
    start.players = {{1, 1, 100, 1}, {2, 1, 200, 1}};
    start.units = {{1, 10, 2}};
    t::EconomyRules rules;
    rules.players = {{1, q(1000), 20}, {2, q(1000), 20}};
    rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(900), 30, 30, 1, true}}},
        {10, 200, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 1, true},
                  {51, t::BuildKind::unit, t::BuildQueue::units, q(200), 30, 30, 1, true}}}};
    auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(made), "SAE-03: allied producer fixture starts");
    if (!made) return;
    auto host = std::make_shared<ai::Host>();
    auto view = std::make_shared<ai::WorldView>();
    ai::ViewUnit station;
    station.id = 1; station.type = 10; station.owner = 2;
    view->units.push_back(station);
    for (eawr::sim::EntityId id = 2; id <= 1001; ++id) {
        ai::ViewUnit unit; unit.id = id; unit.type = 99;
        view->units.push_back(unit);
    }
    foc::AiType station_type;
    station_type.type_id = 10; station_type.star_base = true;
    host->types.emplace(10, &station_type);
    host->view = view; host->world = &made.value();
    ai::Engine engine(host, {}, {});
    expect(made.value().build_allowed(1, 1, 51) && engine.producer(1, 51) == 1,
        "SAE-03: AI discovers a type exclusive to its allied station owner's faction");
    const auto* option = engine.build_option(1, 50, 1);
    expect(option != nullptr && option->price == q(100), "SAE-03: AI uses owner-faction station price");
    for (std::uint32_t i = 0; i < 100; ++i) {
        expect(engine.producer(1, 500 + i) == 0, "SAE-03: missing type has no producer");
        expect(engine.producer(1, 50) == 1, "SAE-03: indexed producer remains deterministic");
    }
    expect(engine.producer_work().entities == 1001 && engine.producer_work().candidates == 101,
        "SAE-03: repeated candidates and misses scan world entities only once per service");
    eawr::sim::InlineExecutor executor;
    expect(made.value().submit({{0, 1, 0}, {1}, t::BuyPayload{50}}).has_value(), "SAE-03: allied purchase submits");
    expect(made.value().step(executor).has_value() && made.value().ledgers().front().credits == q(900),
        "SAE-03: ordinary purchase debits the same owner-faction price");
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
    expect(static_cast<bool>(scripts), "SAE-03: lifecycle scheduler starts");
    if (!scripts) return;
    std::uint64_t sequence = 0;
    const auto snapshot = made.value().snapshot();
    expect(engine.before_service(made.value(), *snapshot, scripts.value(), sequence).has_value(),
        "SAE-03: next AI service resets its derived producer index");
    expect(engine.producer(1, 50) == 1 && engine.producer_work().entities == 1001 && engine.producer_work().candidates == 1,
        "SAE-03: producer work counters reset and the next service rebuilds once");

    // Rejection, constructor removal, pad destruction and failed final replacement all
    // finish the block and release its reservation; the ordinary successful build still waits.
    for (const int scenario : {0, 1, 2, 3, 4}) {
        start.units = {{1, 20, 2}};
        rules.menus = {{20, 100, {{30, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false},
            {20, 200, {{31, t::BuildKind::structure, t::BuildQueue::units, q(900), 30, 30, 0, true}}, 0, false}};
        rules.pads.neutral = 2;
        rules.pads.capture = {{20, q(100), q(1), {100, 200}, false, true, true}};
        rules.pads.construction = {{30, 40, q(100), 1, 1}, {31, 41, q(100), 1, 1}};
        rules.pads.influence = {{20, false}, {30, false}, {40, false}};
        t::DurabilityTable health;
        for (const t::TypeId type : {20U, 30U, 31U, 40U, 41U}) {
            if (scenario == 3 && type == 40) continue;
            t::DurabilityProfile profile; profile.type_id = type; profile.max_hull = q(300);
            health.profiles.push_back(profile);
        }
        auto pad_world = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(pad_world), "SAE-03: pad lifecycle fixture starts");
        if (!pad_world) continue;
        host = std::make_shared<ai::Host>();
        host->world = &pad_world.value(); host->snapshot = pad_world.value().snapshot();
        host->view = foc::detail::build_view(pad_world.value(), *host->snapshot);
        ai::Engine pad_engine(host, {}, {ai::PlanDef{}});
        expect(pad_engine.build_option(1, 30, 1) != nullptr && pad_engine.build_option(1, 31, 1) == nullptr,
            "SAE-03: allied pad preserves buyer-faction menu exception");
        expect(pad_engine.producer(1, 30) == 1, "SAE-03: indexed allied pad is initially available");
        ai::PlayerAi player; player.player = 1;
        ai::EngineContractAccess::seed(pad_engine, player, scripts.value());
        expect(pad_engine.producer(1, 30) == 0, "SAE-03: a reservation made after indexing blocks the next ordered lookup");
        ai::EngineContractAccess::service(pad_engine, player, 1);
        expect(ai::EngineContractAccess::orders(pad_engine) == 1 && !pad_engine.block(1)->finished,
            "SAE-03: pad production emits an ordinary command and waits");
        if (scenario == 0) {
            expect(pad_world.value().submit({{0, 2, 0}, {1}, t::PadBuildPayload{31}}).has_value(), "SAE-03: competing allied build submits");
            expect(pad_world.value().step(executor).has_value(), "SAE-03: competing construction occupies pad");
            expect(pad_world.value().submit({{1, 1, 0}, {1}, t::PadBuildPayload{30}}).has_value(), "SAE-03: stale AI request submits for Core rejection");
        } else {
            expect(pad_world.value().submit({{0, 1, 0}, {1}, t::PadBuildPayload{30}}).has_value(), "SAE-03: pad build submits");
        }
        expect(pad_world.value().step(executor).has_value(), "SAE-03: pad command delivery completes");
        if (scenario == 1 || scenario == 2) {
            const auto victim = scenario == 1 ? pad_world.value().pads().at(1).under_construction : 1;
            expect(victim != 0 && pad_world.value().submit({{1, 1, 1}, {victim}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(),
                "SAE-03: constructor or pad destruction submits");
            expect(pad_world.value().step(executor).has_value(), "SAE-03: destruction commits");
        } else if (scenario >= 3) {
            host->snapshot = pad_world.value().snapshot();
            host->view = foc::detail::build_view(pad_world.value(), *host->snapshot);
            ai::EngineContractAccess::service(pad_engine, player, 4);
            expect(!pad_engine.block(1)->finished, "SAE-03: accepted construction remains pending while its child lives");
            for (int frame = 0; frame < 31; ++frame) expect(pad_world.value().step(executor).has_value(), "SAE-03: construction advances");
        }
        host->snapshot = pad_world.value().snapshot();
        host->view = foc::detail::build_view(pad_world.value(), *host->snapshot);
        ai::EngineContractAccess::service(pad_engine, player, 100);
        expect(player.tasks.empty() && pad_engine.block(1)->finished && pad_engine.block(1)->result == (scenario == 4),
            "SAE-03: pad outcome completes the production block with the correct result");
        expect(pad_engine.plan(1)->reserved_pads.empty() && player.reserved_credits.at(1) == Real{},
            "SAE-03: completed or failed pad task releases pad and credit reservations");
    }
}

void completed_purchases_are_available() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    t::TacticalSetup start;
    start.players = {{1, 1, 100, t::player_flag_commandable}};
    start.units = {{1, 10, 1}};
    t::EconomyRules rules;
    rules.players = {{1, q(1000), 20}};
    rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 3, 3, 1, true},
                            {51, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 1, true}}}};
    auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(made), "SAE-03: shared production pool fixture starts");
    if (!made) return;
    auto host = std::make_shared<ai::Host>();
    host->setup.perception.campaign_game = false;
    host->world = &made.value();
    host->snapshot = made.value().snapshot();
    host->view = foc::detail::build_view(made.value(), *host->snapshot);
    foc::AiType factory;
    factory.type_id = 10; factory.star_base = true;
    host->types.emplace(10, &factory);
    ai::PlanDef definition;
    definition.allow_free_store = true;
    definition.ignore_target = true;
    ai::TaskForceDef force;
    ai::TeamDef team;
    team.min_count = team.max_count = 1;
    team.types = {50};
    force.teams = {team};
    definition.taskforces = {force};
    ai::Engine engine(host, {}, {definition});
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
    expect(static_cast<bool>(scripts), "SAE-03: shared production scheduler starts");
    if (!scripts) return;
    ai::PlayerAi player; player.player = 1;
    ai::EngineContractAccess::seed_purchases(engine, player, scripts.value());
    ai::EngineContractAccess::service(engine, player, 1);
    expect(ai::EngineContractAccess::orders(engine) == 2, "SAE-03: both build tasks issue purchases");
    expect(made.value().submit({{0, 1, 0}, {1}, t::BuyPayload{50}}).has_value()
        && made.value().submit({{0, 1, 1}, {1}, t::BuyPayload{51}}).has_value(),
        "SAE-03: purchases enter the ordinary queue");
    eawr::sim::InlineExecutor executor;
    for (int frame = 0; frame < 5; ++frame)
        expect(made.value().step(executor).has_value(), "SAE-03: first purchase completes before the second");
    host->snapshot = made.value().snapshot();
    host->view = foc::detail::build_view(made.value(), *host->snapshot);
    ai::EngineContractAccess::service(engine, player, 6);
    expect(player.tasks.size() == 1 && player.tasks.front().type == 51 && !engine.block(1)->finished,
        "SAE-03: the producing block still waits for its later purchase");
    expect(engine.taskforce(1)->pooled.empty() && player.reserved_pool_tokens.empty()
        && player.reserved_pool.empty(), "SAE-03: new production does not reserve or attach the completed pool entry");
    ai::PotentialPlan potential;
    expect(ai::EngineContractAccess::selection(engine, player, potential, 67, 2)
        && potential.units == std::vector<t::TypeId>{50} && potential.sources == std::vector<std::uint8_t>{1}
        && potential.pool_tokens.size() == 1 && potential.pool_tokens.front() != 0,
        "SAE-03: another goal selects the completed purchase while production remains blocked");
    if (!potential.pool_tokens.empty()) {
        player.reserved_pool_tokens[potential.pool_tokens.front()] = 3;
        ++player.reserved_pool[50];
        expect(!ai::EngineContractAccess::selection(engine, player, potential, 67, 2),
            "SAE-03/11: a subsequently reserved pool entry remains unavailable to another goal");
    }
}

void reinforcement_work_budget() {
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    const m::Vec3 requested{q(-1000), q(50), q(8)};
    expect(ai::reinforcement_candidate(requested, 0) == requested, "SAE-10: radius zero tests the requested point");
    const auto first = ai::reinforcement_candidate(requested, 1);
    const auto second = ai::reinforcement_candidate(requested, 2);
    const auto next = ai::reinforcement_candidate(requested, 11);
    expect(first && first->x == q(-1500) && first->y == q(50), "SAE-10: first angle is reverse arrival facing at radius 500");
    expect(first && second && second->x > first->x && second->y < first->y, "SAE-10: second angle rotates positive 36 degrees");
    expect(next && next->x == q(-2000) && next->y == q(50), "SAE-10: next ring adds 500 and restarts its angle");
    expect(ai::reinforcement_candidate(requested, 211).has_value(), "SAE-10: search continues beyond the former 10000-unit cutoff");
    for (std::uint32_t attempt = 0; attempt < 221; ++attempt) {
        const auto candidate = ai::reinforcement_candidate(requested, attempt);
        expect(candidate && candidate->z == requested.z, "SAE-10: candidate geometry preserves the requested plane");
    }
    const std::array bounds{q(-2000), q(-2000), q(2000), q(2000)};
    const auto clamped = ai::reinforcement_candidate(requested, 81, {}, bounds);
    expect(clamped && clamped->x == q(-2000), "SAE-10: candidate clamps to playable bounds");
    const auto rotated = ai::reinforcement_candidate({}, 11, q(90));
    expect(rotated && rotated->x == q(0) && rotated->y == q(-1000), "SAE-10: ring origin follows authored arrival yaw");
}

void duplicate_reinforcement_calls() {
    namespace t = eawr::sim::tactical;
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, t::player_flag_commandable}};
    t::EconomyRules economy;
    economy.players = {{1, {}, 20}};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
    expect(static_cast<bool>(made), "SAE-11: duplicate-call fixture starts");
    if (!made) return;
    auto host = std::make_shared<ai::Host>();
    host->world = &made.value();
    host->snapshot = made.value().snapshot();
    host->view = foc::detail::build_view(made.value(), *host->snapshot);
    ai::Engine engine(host, {}, {ai::PlanDef{}});
    auto& player = ai::EngineContractAccess::duplicate_fixture(engine);
    auth::ServiceReport report;
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        auth::ScriptCommand call;
        call.issuer = 1;
        call.sequence = sequence;
        call.verb = std::string(ai::verb_ai);
        call.arguments = {auth::Value{std::string("reinforce")},
            auth::Value{auth::Handle{ai::handle_taskforce, 1}}, foc::detail::position_value({})};
        report.commands.push_back(std::move(call));
    }
    expect(static_cast<bool>(engine.after_service(report)), "SAE-11: repeated Reinforce calls are taken");
    const auto* first = engine.block(ai::block_id(1, 1));
    const auto* second = engine.block(ai::block_id(1, 2));
    expect(first != nullptr && first == second && !first->finished,
        "SAE-11: both Lua handles wait on the same unfinished operation");
    const auto& orders = ai::EngineContractAccess::reinforce_service(engine, player, 1);
    expect(orders.size() == 1, "SAE-11: repeated calls emit exactly one request for the reserved purchase");
    if (orders.size() == 1) {
        const auto* token = std::get_if<auth::Handle>(&orders[0].arguments[4].data);
        expect(token != nullptr && token->id == 1, "SAE-11: request carries the first reservation's identity");
    }
    expect(player.reserved_pool_tokens.size() == 2 && engine.taskforce(1)->pooled.size() == 2,
        "SAE-11: submission keeps both reservations until authoritative admission");
    auto view = std::make_shared<foc::detail::WorldView>(*host->view);
    foc::detail::ViewUnit converted;
    converted.id = 50;
    converted.owner = 1;
    converted.type = 99;
    converted.purchase_type = 1;
    converted.purchase_token = 1;
    view->units.push_back(converted);
    host->view = view;
    const auto& next = ai::EngineContractAccess::reinforce_service(engine, player, 2);
    expect(engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{50}
        && engine.taskforce(1)->pooled.size() == 1 && !player.reserved_pool_tokens.contains(1),
        "SAE-11/WHE-49: a converted hull joins by logical purchase type and releases its exact reservation");
    expect(next.size() == 1 && engine.block(ai::block_id(1, 2)) == first && !first->finished,
        "SAE-11: both callers continue waiting while the second purchase reinforces");
    converted.id = 51;
    converted.purchase_token = 2;
    view->units.push_back(converted);
    const auto& done = ai::EngineContractAccess::reinforce_service(engine, player, 3);
    expect(done.empty() && first->finished && first->result && player.reserved_pool_tokens.empty()
        && engine.taskforce(1)->pooled.empty() && engine.taskforce(1)->units.size() == 2,
        "SAE-11: both handles finish together after each reserved purchase joins once");
}

void taskforce_attachment_waits_for_arrival() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    std::optional<std::uint64_t> baseline;
    for (const bool abandoned : {false, true}) {
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            t::TacticalSetup setup;
            setup.players = {{1, 1, 100, t::player_flag_commandable}};
            setup.units = {{1, 10, 1}};
            t::EconomyRules economy;
            economy.players = {{1, q(1000), 20}};
            economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
            auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
            expect(made.has_value(), "EX-13: arrival attachment fixture starts");
            if (!made) continue;
            auto& world = made.value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "EX-13: buy reserved pool entry");
            expect(world.step(executor).has_value() && world.step(executor).has_value(), "EX-13: purchase reaches pool");
            auto host = std::make_shared<ai::Host>();
            host->world = &world;
            host->snapshot = world.snapshot();
            host->view = foc::detail::build_view(world, *host->snapshot);
            foc::AiType type;
            type.type_id = 1;
            type.locomotor = true;
            host->types.emplace(1, &type);
            ai::Engine engine(host, {}, {ai::PlanDef{}});
            auto& player = ai::EngineContractAccess::duplicate_fixture(engine);
            auth::ServiceReport report;
            auth::ScriptCommand call;
            call.issuer = call.sequence = 1;
            call.verb = std::string(ai::verb_ai);
            call.arguments = {auth::Value{std::string("reinforce")},
                auth::Value{auth::Handle{ai::handle_taskforce, 1}}, foc::detail::position_value({})};
            report.commands.push_back(std::move(call));
            expect(engine.after_service(report).has_value(), "EX-13: stage reinforcement block");
            const auto& requests = ai::EngineContractAccess::reinforce_service(engine, player, 2);
            expect(requests.size() == 1, "EX-13: submit one reserved purchase");
            const auto block_id = ai::block_id(1, 1);
            expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 1}, {}, t::ReinforcePayload{1, {}, 1}},
                {block_id, 0}).has_value(), "EX-13: authoritative reinforcement admission retains the reserved purchase token");
            expect(world.step(executor).has_value() && !world.arrivals().empty(), "EX-13: ship exists during hyperspace");
            const auto arrival_tick = world.completed_tick();
            const auto incoming = world.arrivals().begin()->first;
            if (abandoned) {
                auth::SessionConfig config;
                config.tick_duration = auth::TickDuration{1, 30};
                auto scripts = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
                expect(scripts.has_value(), "EX-13: abandoned-plan scheduler starts");
                if (!scripts) continue;
                ai::EngineContractAccess::abandon(engine, player, scripts.value());
                expect(engine.taskforce(1) == nullptr && player.reserved_pool_tokens.empty(),
                    "EX-13: plan exit releases its reservation while the ship is still incoming");
            }
            while (!world.arrivals().empty() && world.completed_tick() < arrival_tick + t::arrival_frames + 1) {
                host->snapshot = world.snapshot();
                host->view = foc::detail::build_view(world, *host->snapshot);
                const auto& pending = ai::EngineContractAccess::reinforce_service(engine, player,
                    static_cast<std::int64_t>(world.completed_tick()));
                expect(pending.empty(), "EX-13: no duplicate request during hyperspace");
                if (!abandoned) {
                    expect(engine.taskforce(1)->units.empty() && player.reserved_pool_tokens.contains(1),
                        "EX-13: incoming ship stays unattached and reserved");
                    expect(!engine.block(block_id)->finished, "EX-13: block waits while ship is incoming");
                }
                const auto* unit = host->view->find(incoming);
                expect(unit != nullptr && !ai::EngineContractAccess::in_freestore(engine, *unit),
                    "EX-13: incoming ship is unavailable to freestore even after its plan releases the reservation");
                expect(world.step(executor).has_value(), "EX-13: hyperspace advances");
            }
            host->snapshot = world.snapshot();
            host->view = foc::detail::build_view(world, *host->snapshot);
            ai::EngineContractAccess::reinforce_service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
            if (abandoned) {
                const auto* unit = host->view->find(incoming);
                expect(world.arrivals().empty() && unit != nullptr && ai::EngineContractAccess::in_freestore(engine, *unit),
                    "EX-13: unload exposes an abandoned plan's incoming ship to freestore");
            } else {
                expect(world.arrivals().empty() && engine.taskforce(1)->units.size() == 1
                    && !player.reserved_pool_tokens.contains(1) && engine.taskforce(1)->pooled.size() == 1,
                    "EX-13: unload attaches the exact ship once and releases only its reservation");
            }
            if (!baseline) baseline = world.completed_tick();
            else expect(world.completed_tick() == *baseline, "EX-13: attachment tick agrees on 1/2/4/8 workers");
        }
    }
}

void concurrent_reserved_arrivals() {
    namespace t = eawr::sim::tactical;
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, t::player_flag_commandable}};
    t::EconomyRules economy;
    economy.players = {{1, {}, 20}};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
    expect(static_cast<bool>(made), "SAE-11: concurrent-arrival fixture starts");
    if (!made) return;
    auto host = std::make_shared<ai::Host>();
    host->world = &made.value();
    host->snapshot = made.value().snapshot();
    auto view = std::make_shared<ai::WorldView>();
    host->view = view;
    foc::AiType type;
    type.type_id = 1;
    type.locomotor = true;
    host->types.emplace(1, &type);
    ai::Engine engine(host, {}, {ai::PlanDef{}});
    auto& player = ai::EngineContractAccess::concurrent_fixture(engine);
    const auto& requested = ai::EngineContractAccess::reinforce_service(engine, player, 1);
    expect(requested.size() == 2, "SAE-11: distinct TaskForces may request independent purchases together");
    // Admission order differs from block order; identical type and ascending entity IDs
    // cannot identify the reserved TaskForce. The admitted purchase token can.
    ai::ViewUnit second;
    second.id = 50;
    second.owner = 1;
    second.type = second.purchase_type = 1;
    second.purchase_token = 3;
    auto first = second;
    first.id = 51;
    first.purchase_token = 1;
    view->units = {second, first};
    expect(!ai::EngineContractAccess::in_freestore(engine, first)
        && !ai::EngineContractAccess::in_freestore(engine, second),
        "SAE-11: admitted purchases remain reserved before their TaskForce attachment service");
    auto unreserved = first;
    unreserved.purchase_token = 999;
    expect(ai::EngineContractAccess::in_freestore(engine, unreserved),
        "SAE-11: a purchase token without a live reservation does not exclude a unit from freestore");
    ai::EngineContractAccess::reinforce_service(engine, player, 2);
    expect(engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{51}
        && engine.taskforce(2)->units == std::vector<eawr::sim::EntityId>{50},
        "SAE-11: inverted same-type arrivals join their exact reserved TaskForce");
    expect(player.reserved_pool_tokens.size() == 2 && player.reserved_pool_tokens.contains(2)
        && player.reserved_pool_tokens.contains(4),
        "SAE-11: each arrival releases only its own reserved purchase");
}

void reinforcement_service_budget() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    // The blocked-ring and second-angle cases exercise prevention and shared static collision.
    for (const auto radius : {750, 1250}) {
        std::vector<std::string> baseline;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            t::TacticalSetup setup;
            setup.players = {{1, 1, 100, 1}, {2, 2, 200, 1}};
            setup.units = {{1, 10, 1, {q(4000), {}, {}}}, {2, 20, 2},
                {3, 30, 1, {q(-1000), {}, {}}}};
            t::EconomyRules economy;
            economy.players = {{1, q(1000), 20}, {2, {}, 20}};
            economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
            economy.prevention = {{20, q(radius)}};
            t::MotionTable motion;
            motion.avoidance = t::AvoidanceRules{q(24), q(1), q(100), q(1), q(15), q(1), q(1), q(1), q(1), q(1), q(1),
                3500, 6, 90, 45, q(50)};
            motion.footprints = {{1, t::SpaceLayer::corvette, q(10), q(20), q(20), false},
                {30, t::SpaceLayer::static_object, q(100), q(100), q(100), true}};
            auto made = t::TacticalSession::create(setup, {}, {}, motion, {}, {}, {}, {}, economy);
            expect(static_cast<bool>(made), "SAE-10: ring fixture starts");
            if (!made) continue;
            auto& world = made.value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: ring fixture buys pooled unit");
            expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: purchase reaches pool before search");
            expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 1}, {}, t::ReinforcePayload{1, {}}}, {7, 0}).has_value(),
                "SAE-10: submit initial whole-ring search");
            const auto first = world.step(executor);
            expect(static_cast<bool>(first), "SAE-10: whole ring executes in one tick");
            if (!first) continue;
            const auto result = world.reinforcement_search_result(1, 7);
            expect(result.has_value(), "SAE-10: ring result returns to execution service");
            if (!result) continue;
            expect(result->predictions == 0, "SAE-10: ring queries materialize no private prediction samples");
            std::vector<std::string> trace{first.value().state_sha256};
            if (radius == 1250) {
                expect(!result->valid && result->candidates == 10 && result->next_attempt == 21,
                    "SAE-10: blocked radius-1000 ring tests all ten angles in one service and advances to 1500");
                expect(first.value().reinforcement_rejections_by_check[1] >= 10,
                    "SAE-10: every prevention-blocked angle gets a placement verdict");
                expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 2}, {}, t::ReinforcePayload{1, {}}}, {7, result->next_attempt}).has_value(),
                    "SAE-10: next service submits next ring");
                const auto second = world.step(executor);
                expect(second.has_value(), "SAE-10: next radius executes");
                if (!second) continue;
                const auto free = world.reinforcement_search_result(1, 7);
                expect(free && free->valid && free->candidates == 1 && free->position == *ai::reinforcement_candidate({}, 21),
                    "SAE-10: radius 1500 restarts at reverse facing and admits promptly");
                trace.push_back(second.value().state_sha256);
            } else {
                expect(result->valid && result->candidates == 2 && result->position == *ai::reinforcement_candidate({}, 12),
                    "SAE-10: prevention overlap skips requested/500 points and static first-angle blocker selects second angle at 1000");
                expect(first.value().reinforcement_collision_queries >= 2,
                    "SAE-10: shared collision views check the ring and ordinary command admission");
            }
            const auto replay = world.record();
            const auto bytes = t::write_replay(replay);
            expect(bytes.has_value(), "SAE-10: resolved command uses the existing replay format");
            if (bytes) {
                const auto decoded = t::parse_replay(bytes.value());
                expect(decoded && decoded.value() == replay, "SAE-10: resolved ordinary replay round-trips without search metadata");
            }
            expect(std::holds_alternative<t::ReinforcePayload>(replay.commands.back().payload),
                "SAE-10: record contains the ordinary resolved reinforcement");
            auto replayed = t::TacticalSession::create(setup, {}, {}, motion, {}, {}, {}, {}, economy);
            expect(replayed.has_value(), "SAE-10: replay fixture starts");
            if (replayed) {
                for (const auto& command : replay.commands) expect(replayed.value().submit(command).has_value(), "SAE-10: resolved commands replay");
                for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) expect(replayed.value().step(executor).has_value(), "SAE-10: ordinary replay advances");
                expect(replayed.value().snapshot()->canonical_bytes() == world.snapshot()->canonical_bytes(),
                    "SAE-10: ordinary resolved command replay reproduces the searched world");
            }
            if (workers == 1) baseline = trace;
            else expect(trace == baseline, "SAE-10: ring search and world hashes agree on 1/2/4/8 workers");
            expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 10}, {}, t::ReinforcePayload{1, {}}},
                {9, 21}).has_value(), "SAE-10: no-pool search uses ordinary admission");
            const auto empty_pool = world.step(executor);
            const auto held = world.reinforcement_search_result(1, 9);
            expect(empty_pool && held && held->candidates == 0 && held->next_attempt == 21
                && empty_pool.value().snapshot->events().front().reason == t::RejectReason::not_in_pool,
                "SAE-03/10: absent pool entry does no ring work and retains its radius");
        }
    }
}

void reinforcement_gravity_well() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    const auto decimal = [](std::string_view value) { return m::Fixed::from_decimal(value).value(); };
    // SAE-10: two prevention-blocked points from the lone-squadron economy probe.
    // A synthetic neutral well at the origin uses the authored 2200-unit prevention radius.
    const std::array<m::Vec3, 2> points{{{decimal("-795.585"), decimal("-427.722"), {}},
        {decimal("-600.219"), decimal("-887.974"), {}}}};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        std::vector<std::string> trace;
        for (const auto& requested : points) {
            t::TacticalSetup setup;
            setup.players = {{1, 1, 100, 1}, {2, 2, 200, 1}};
            setup.units = {{1, 10, 1, {q(4000), {}, {}}}, {2, 20, 2}};
            t::EconomyRules economy;
            economy.players = {{1, q(1000), 20}, {2, {}, 20}};
            economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
            economy.prevention = {{20, q(2200)}};
            auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
            expect(made.has_value(), "SAE-10: blocked gravity-well fixture starts");
            if (!made) continue;
            auto& world = made.value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: gravity fixture purchases its pool entry");
            expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: gravity purchase reaches pool before search");
            auto host = std::make_shared<ai::Host>();
            host->world = &world;
            host->snapshot = world.snapshot();
            host->view = foc::detail::build_view(world, *host->snapshot);
            ai::Engine engine(host, {}, {});
            ai::PlayerAi player;
            player.player = 1;
            ai::EngineContractAccess::seed_reinforcements(engine, player, 1, true);
            ai::EngineContractAccess::reinforce_at(engine, requested);
            const auto block_token = ai::block_id(ai::first_plan_instance + 1, 17);
            std::uint32_t attempts = 0;
            for (std::int64_t service = 1; service <= 4 && world.arrivals().empty(); ++service) {
                const auto& commands = ai::EngineContractAccess::reinforce_service(engine, player, service);
                expect(commands.size() == 1 && commands.front().arguments.size() == 7,
                    "SAE-10: AI emits one ring request for the blocked pool entry");
                if (commands.empty()) break;
                const auto translated = foc::tactical_ai_detail::translate_order(commands.front());
                expect(translated && translated.value().reinforcement_search
                    && translated.value().reinforcement_search->token == block_token,
                    "SAE-10: command translation preserves the full plan block token");
                if (!translated || !translated.value().reinforcement_search) break;
                const auto tick = world.completed_tick();
                expect(world.submit_reinforcement_search({{tick, 1, static_cast<std::uint64_t>(service)}, {},
                    translated.value().payload}, *translated.value().reinforcement_search).has_value(),
                    "SAE-10: gravity request reaches ordinary command path");
                const auto stepped = world.step(executor);
                expect(stepped.has_value(), "SAE-10: gravity ring advances");
                if (!stepped) break;
                const auto result = world.reinforcement_search_result(1, block_token);
                expect(result && result->candidates <= 10 && result->predictions == 0,
                    "SAE-10: gravity search remains bounded per service with no private predictions");
                if (result) attempts += result->candidates;
                host->snapshot = stepped.value().snapshot;
                host->view = foc::detail::build_view(world, *host->snapshot);
                trace.push_back(stepped.value().state_sha256);
            }
            expect(!world.arrivals().empty() && attempts <= 40,
                "SAE-10: prevention-blocked gravity-well request reinforces within four services");
            for (const auto& command : world.record().commands) {
                if (const auto* reinforce = std::get_if<t::ReinforcePayload>(&command.payload)) {
                    expect(reinforce->position != requested, "SAE-10: blocked gravity request never retries the requested point");
                }
            }
        }
        if (workers == 1) baseline = trace;
        else expect(trace == baseline, "SAE-10: blocked gravity search agrees across 1/2/4/8 workers");
    }
}

void reinforcement_fog_persistence() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, 1}};
    setup.units = {{1, 10, 1}};
    t::EconomyRules economy;
    economy.players = {{1, q(1000), 20}};
    economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
    const t::FogRules fog{q(-12000), q(12000), q(1000), 24, 24};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, fog, {}, {}, {}, economy);
    expect(made.has_value(), "SAE-10: persistent fog fixture starts");
    if (!made) return;
    auto& world = made.value();
    eawr::platform::ThreadWorkerAdapter executor(4);
    expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: fog fixture buys its pool entry");
    expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: fog purchase reaches pool before search");
    std::uint32_t attempt = 0;
    std::uint32_t candidates = 0;
    for (std::uint64_t service = 1; service <= 24; ++service) {
        expect(world.submit_reinforcement_search({{world.completed_tick(), 1, service}, {}, t::ReinforcePayload{1, {}}},
            {8, attempt}).has_value(), "SAE-10: persistent fog submits the next ring");
        expect(world.step(executor).has_value(), "SAE-10: fogged ring executes");
        const auto result = world.reinforcement_search_result(1, 8);
        expect(result && !result->valid && result->candidates == (service == 1 ? 1U : 10U),
            "SAE-10: radius zero checks once; subsequent fogged rings each check ten points");
        if (!result) break;
        attempt = result->next_attempt;
        candidates += result->candidates;
    }
    expect(candidates == 231 && attempt == 231 && world.ledgers().front().pool.size() == 1,
        "SAE-10: failed fog search passes the former distance cutoff and retains the pool entry");
}

void reinforcement_population_wait() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, 1}};
    setup.units = {{1, 10, 1}};
    t::EconomyRules economy;
    economy.players = {{1, q(1000), 1}};
    economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
    expect(made.has_value(), "SAE-10: population wait fixture starts");
    if (!made) return;
    auto& world = made.value();
    eawr::platform::ThreadWorkerAdapter executor(2);
    expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: buy first pool entry");
    expect(world.submit({{0, 1, 1}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: buy second pool entry");
    expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: both pool entries complete");
    expect(world.submit({{2, 1, 2}, {}, t::ReinforcePayload{1, {q(3000), {}, {}}}}).has_value(),
        "SAE-10: reinforce first unit to fill population");
    expect(world.step(executor).has_value(), "SAE-10: population fills");
    expect(world.submit_reinforcement_search({{3, 1, 3}, {}, t::ReinforcePayload{1, {}}}, {10, 21}).has_value(),
        "SAE-10: search waits for population room");
    const auto waiting = world.step(executor);
    const auto result = world.reinforcement_search_result(1, 10);
    expect(waiting && result && result->candidates == 0 && result->next_attempt == 21
        && world.ledgers().front().pool.size() == 1
        && waiting.value().snapshot->events().front().reason == t::RejectReason::no_population_room,
        "SAE-03/10: no population room preserves the pool and current ring without placement work");
}

void threat_grid() {
    World world;
    ai::ThreatGrid grid;
    const foc::AiBounds bounds{ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)};
    grid.partition(bounds, 5, 5, ai::Constants{});
    grid.service(*world.view, *world.host, 0);
    const ai::Rect all{ai::real(-1000), ai::real(-1000), ai::real(2000), ai::real(2000)};
    // PG-02: radius 500, cell 400 x 400: each of the 3 x 3 covered cells holds 1000 * 160000 / 1000000.
    const Real enemy = grid.force(*world.host, *world.view, all, ~std::uint64_t{0}, 1, false, Real{}, 0);
    expect(enemy == ai::real(1440), "PG-05: the enemy threat over the map is 9 cells of 160");
    expect(grid.force(*world.host, *world.view, all, ~std::uint64_t{0}, 1, true, Real{}, 0) == Real{},
        "PG-05: an enemy unit is no friendly threat");
    expect(grid.force(*world.host, *world.view, all, 1, 1, false, Real{}, 0) == Real{}, "PG-05: the category filters");
    expect(grid.total_force(*world.host, *world.view, ~std::uint64_t{0}, 1, false, Real{}) == ai::real(1000),
        "PG-06: the total enemy force is the unit's power");
    expect(grid.force_visibility(*world.host, *world.view, ~std::uint64_t{0}, 1) == ai::real(1),
        "PG-07: an AI player sees every enemy object");
    const ai::Rect corner{ai::real(-1000), ai::real(-1000), ai::real(100), ai::real(100)};
    expect(grid.force(*world.host, *world.view, corner, ~std::uint64_t{0}, 1, false, Real{}, 0) == Real{},
        "PG-02: a zone covers only the cells within its radius");
}

struct QueryExecutor final : eawr::sim::PartitionExecutor {
    explicit QueryExecutor(std::size_t workers) : pool(workers) {}
    std::size_t worker_count() const noexcept override { return pool.worker_count(); }
    eawr::core::Result<void> execute(std::size_t partitions,
        const std::function<void(std::size_t)>& partition) const override {
        return pool.execute(partitions, partition);
    }
    eawr::core::Result<void> execute_phase(std::string_view phase, std::size_t partitions,
        const std::function<void(std::size_t)>& partition) const override {
        expect(partitions == eawr::sim::tick_partition_count, "threat preparation uses the fixed 64 partitions");
        ++phases[std::string(phase)];
        if (fail) {
            eawr::core::Diagnostic error;
            error.code = "EAWR-TEST";
            error.message = "query executor refused the phase";
            return eawr::core::Result<void>::failure(std::move(error));
        }
        return pool.execute_phase(phase, partitions, partition);
    }
    eawr::platform::ThreadWorkerAdapter pool;
    mutable std::map<std::string, std::size_t> phases;
    bool fail{};
};

void threat_preparation() {
    World world;
    auto& types = world.host->setup.content.types;
    types.front().combat_power = ai::real(1);
    auto large = types.front();
    large.type_id = 2;
    large.combat_power = ai::real(16777216);
    types.push_back(large);
    world.host->types.clear();
    for (const auto& type : types) world.host->types.emplace(type.type_id, &type);
    world.view->units.clear();
    for (const auto& [id, type, owner] : std::array<std::array<unsigned, 3>, 3>{{{1, 2, 2}, {2, 1, 1}, {3, 1, 1}}}) {
        ai::ViewUnit unit;
        unit.id = id; unit.type = type; unit.owner = owner;
        world.view->units.push_back(unit);
    }
    ai::ThreatGrid grid;
    const auto configure = [&] {
        grid.partition(foc::AiBounds{ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)}, 5, 5, ai::Constants{});
        grid.service(*world.view, *world.host, 0);
    };
    configure();
    const auto all = ~std::uint64_t{};
    const auto original = grid.total_force(*world.host, *world.view, all, 0, true, Real{});
    expect(original.repr == ai::real(16777218).repr, "PG-06: player-first rounding fixture retains the two small contributions");
    const auto entity_fold = ai::to_single(ai::to_single(ai::real(16777216) + ai::real(1)) + ai::real(1));
    expect(original.repr != entity_fold.repr, "PG-06: fixture detects an entity-order or partition-subtotal fold");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        QueryExecutor executor(workers);
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr,
                "PG-06: staged contributions retain player-first rounding");
            expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr,
                "PG-06: repeated query returns identical bits");
            expect(executor.phases["ai-threat-total"] == 1, "PG-06: identical preparation query reuses its scalar");
        }
        expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr,
            "PG-06: unscoped readers retain the const query path");
        expect(executor.phases["ai-threat-total"] == 1, "PG-06: unscoped reads do not use the borrowed executor");
    }

    types[1].weapons = {{0, ai::real(127), ai::real(431), true, true},
        {1, ai::real(251) / ai::real(7), ai::real(719), true, true}};
    auto other_category = types.front();
    other_category.type_id = 3;
    other_category.category_bits = 1;
    other_category.combat_power = ai::real(73) / ai::real(11);
    types.push_back(other_category);
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const auto& type : types) world.host->types.emplace(type.type_id, &type);
    world.host->setup.players.push_back(foc::AiPlayer{3, "NEUTRAL", true, false, ""});
    world.host->setup.players.push_back(foc::AiPlayer{4, "ALLIED", false, true, ""});
    world.host->players.clear();
    for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
    world.view->players.push_back(foc::detail::ViewPlayer{3, 3, 2});
    world.view->players.push_back(foc::detail::ViewPlayer{4, 1, 3});
    world.view->units.clear();
    for (unsigned index = 0; index < 83; ++index) {
        ai::ViewUnit unit;
        unit.id = index + 1; unit.type = index % 3 + 1; unit.owner = index % 11 == 0 ? 9 : index % 5;
        unit.health = ai::real(1) / ai::real(index % 7 + 1);
        unit.visible_to = index % 16;
        unit.hardpoint_destroyed = {false, index % 2 == 0};
        unit.position.x = eawr::sim::math::Fixed::from_integer(static_cast<int>(index % 5) * 310 - 620).value();
        unit.position.y = eawr::sim::math::Fixed::from_integer(static_cast<int>(index % 7) * 210 - 630).value();
        world.view->units.push_back(unit);
    }
    // A non-AI player uses the authored visibility mask instead of omniscience.
    world.host->setup.players.front().ai = false;
    configure();
    const std::array<ai::Rect, 4> rectangles{{
        {ai::real(-1000), ai::real(-1000), ai::real(2000), ai::real(2000)},
        {ai::real(-413), ai::real(-217), ai::real(713), ai::real(507)},
        {ai::real(3000), ai::real(3000), ai::real(1), ai::real(1)},
        {ai::real(-1001), ai::real(99), ai::real(2), ai::real(803)}}};
    struct Query {
        std::uint64_t category;
        eawr::sim::tactical::PlayerId player;
        bool friendly;
        Real attenuator;
        eawr::sim::EntityId excluded;
    };
    const std::array<Query, 10> queries{{
        {all, 0, true, Real{}, 0}, {2, 1, false, ai::real(1) / ai::real(3), 0},
        {2, 2, true, ai::real(1) / ai::real(7), 13}, {1, 1, true, ai::real(1), 0},
        {all, 2, false, ai::real(1), 17}, {all, 9, false, ai::real(1) / ai::real(2), 0},
        {all, 1, false, Real{}, 0}, {all, 1, false, Real::from_repr(0x8000000000000000ULL), 0},
        {all, 3, false, ai::real(1) / ai::real(2), 0}, {2, 4, true, ai::real(1) / ai::real(5), 7}}};
    std::vector<std::uint64_t> expected;
    const auto read = [&](const Query& query, const auto& append) {
        append(grid.total_force(*world.host, *world.view, query.category, query.player, query.friendly, query.attenuator).repr);
        for (const auto& rect : rectangles) {
            append(grid.force(*world.host, *world.view, rect, query.category, query.player, query.friendly, query.attenuator, query.excluded).repr);
        }
    };
    for (const auto& query : queries) read(query, [&](auto value) { expected.push_back(value); });
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        QueryExecutor executor(workers);
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            for (int repeat = 0; repeat < 2; ++repeat) {
                std::vector<std::uint64_t> actual;
                for (const auto& query : queries) read(query, [&](auto value) { actual.push_back(value); });
                expect(actual == expected, "PG-05/06: cell order, predicates, fractional health and query keys agree across workers");
            }
            expect(executor.phases["ai-threat-total"] == queries.size(), "PG-06: exact query keys include signed-zero attenuation");
            expect(executor.phases["ai-threat-cells"] == queries.size() * 3, "PG-05: repeated rectangles reuse results; empty rectangles do not dispatch");
        }
        std::array<std::vector<std::uint64_t>, eawr::sim::tick_partition_count> concurrent;
        expect(executor.pool.execute(concurrent.size(), [&](const std::size_t partition) {
            for (const auto& query : queries) read(query, [&](auto value) { concurrent[partition].push_back(value); });
        }).has_value(), "PG-05/06: parallel unscoped readers complete");
        for (const auto& values : concurrent) expect(values == expected, "PG-05/06: Lua-style const readers preserve query results");
    }
    QueryExecutor executor(4);
    {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        const auto first = grid.total_force(*world.host, *world.view, all, 0, true, Real{});
        world.view->units.erase(world.view->units.begin() + 1);
        grid.service(*world.view, *world.host, 300);
        const auto changed = grid.total_force(*world.host, *world.view, all, 0, true, Real{});
        expect(changed.repr != first.repr && executor.phases["ai-threat-total"] == 2,
            "PG-03: service invalidates cached totals after a death");
        configure();
        expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == changed.repr,
            "PG-03: repartition invalidates the query cache");
        expect(executor.phases["ai-threat-total"] == 3, "PG-03: repartition recomputes the request");
    }
    executor.fail = true;
    bool refused = false;
    try {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        static_cast<void>(grid.total_force(*world.host, *world.view, all, 0, true, Real{}));
    } catch (const std::runtime_error&) {
        refused = true;
    }
    expect(refused, "PG-06: preparation executor failure cannot fabricate a threat result");
    expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr != 0,
        "PG-06: failure unwinds preparation before later const readers");
}

void taskforce_definitions() {
    World world;
    foc::AiType frigate;
    frigate.type_id = 2;
    frigate.name = "FRIGATE_A";
    frigate.category_bits = 4;
    foc::AiType craft;
    craft.type_id = 3;
    craft.name = "CRAFT_A";
    craft.category_bits = 1;
    craft.craft = true;
    world.host->setup.content.types.push_back(frigate);
    world.host->setup.content.types.push_back(craft);
    world.host->setup.content.categories.emplace("FRIGATE", 4);
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const foc::AiType& entry : world.host->setup.content.types) {
        world.host->types.emplace(entry.type_id, &entry);
        world.host->types_by_name.emplace(entry.name, &entry);
    }
    auth::ValueList globals;
    globals.push_back(auth::Value::text("Destroy_Unit | Destroy_Unit_Minimal"));
    std::vector<auth::Value> taskforce{auth::Value::text("MainForce"), auth::Value::text("Corvette | Frigate = 2, 6"),
        auth::Value::text("Fighter = 1, 4"), auth::Value::text("Frigate_A = 1"), auth::Value::text("Craft_A = 1"),
        auth::Value::text("Frigate_Z = 2"), auth::Value::text("MinimumTotalSize = 3"), auth::Value::text("EscortForce")};
    std::vector<auth::Value> list{auth::Value{taskforce}};
    globals.push_back(auth::Value{list});
    for (int index = 0; index < 8; ++index) globals.push_back(auth::Value{});
    std::vector<std::string> notes;
    auto plan = ai::build_plan(*world.host, "test", "test.lua", globals, notes);
    expect(static_cast<bool>(plan), "PL-10: the plan builds");
    if (!plan) return;
    const ai::PlanDef& definition = plan.value();
    expect(definition.goals == std::vector<std::string>{"DESTROY_UNIT", "DESTROY_UNIT_MINIMAL"}, "PL-10: Category names the goals");
    expect(definition.taskforces.size() == 1, "PL-10: one TaskForce");
    const ai::TaskForceDef& tf = definition.taskforces.front();
    expect(tf.teams.size() == 4, "PL-11: a team naming only a squadron member is dropped");
    if (tf.teams.size() == 4) {
        expect(tf.teams[0].min_count == 2 && tf.teams[0].max_count == 6, "PL-11: a = min, b = max");
        expect(tf.teams[0].types == std::vector<eawr::sim::tactical::TypeId>{1, 2}, "PL-11: a category list takes every type of it");
        expect(tf.teams[1].min_count == 1 && tf.teams[1].max_count == 4 && tf.teams[1].types.empty(),
            "PL-11: a category team no loaded type has stays, and nothing fills it");
        expect(tf.teams[2].min_count == 1 && tf.teams[2].max_count == 1 && tf.teams[2].types.size() == 1,
            "PL-11: a type name with one count");
        expect(tf.teams[3].min_count == 2 && tf.teams[3].types.empty(), "PL-11: a team of an unloaded type stays, and nothing fills it");
    }
    expect(tf.minimum_size == 3 && tf.escort, "PL-11: keywords");
    expect(definition.allow_free_store && definition.allow_engaged && !definition.ignore_target, "PL-12: flag defaults");
}

void required_category_unions() {
    World world;
    world.host->setup.content.categories = {{"FIGHTER", 1}, {"CORVETTE", 2}, {"BOMBER", 4},
        {"HIGH", std::uint64_t{1} << 63}, {"NONE", 0}};
    world.host->setup.content.types.front().category_bits = 1;
    const std::vector<auth::Value> main{auth::Value::text("MainForce"), auth::Value::text("Bomber | Corvette = 0, 2")};
    const std::vector<auth::Value> escort{auth::Value::text("EscortForce"), auth::Value::text("Fighter = 1, 2")};
    auth::ValueList globals{auth::Value::text("Hide"), auth::Value{std::vector<auth::Value>{auth::Value{main}, auth::Value{escort}}}};
    globals.resize(10);
    globals[2] = auth::Value{true};
    const std::vector<std::pair<std::string, std::uint64_t>> rows{
        {"Bomber | Corvette", 6}, {"Bomber,Corvette", 6}, {"Bomber Corvette", 6},
        {"Bomber\tCorvette\nBomber", 6}, {"High | Bomber", (std::uint64_t{1} << 63) | 4},
        {"None", 0}, {"", 0}, {" || , \t\n", 0},
    };
    for (const auto& [text, expected] : rows) {
        globals[9] = auth::Value{std::vector<auth::Value>{auth::Value::text(text)}};
        std::vector<std::string> notes;
        auto plan = ai::build_plan(*world.host, "hide", "hide.lua", globals, notes);
        expect(plan && plan.value().required_categories == std::vector<std::uint64_t>{expected} && notes.empty(),
            "PL-14: category union delimiters preserve full unsigned masks");
    }
    globals[9] = auth::Value{std::vector<auth::Value>{auth::Value::text("Fighter"),
        auth::Value::text("Bomber | Corvette"), auth::Value::text("Bomber | Unknown"),
        auth::Value::text("Bomber & Corvette"), auth::Value{true}}};
    std::vector<std::string> notes;
    auto plan = ai::build_plan(*world.host, "hide", "hide.lua", globals, notes);
    expect(plan && plan.value().required_categories == std::vector<std::uint64_t>{1, 6} && notes.size() == 2,
        "PL-14: separate rows stay separate; malformed unions are reported and omitted in full");
    if (!plan) return;
    ai::Engine engine(world.host, {}, {plan.value()});
    world.host->engine = &engine;
    ai::PlayerAi player;
    player.player = 2;
    ai::PotentialPlan potential;
    expect(!ai::EngineContractAccess::selection(engine, player, potential, 67),
        "PL-14/PL-25: hide admission rejects fighter-only escorts before an absent MainForce can be guarded");
    globals[9] = auth::Value{};
    notes.clear();
    auto unguarded = ai::build_plan(*world.host, "hide", "hide.lua", globals, notes);
    if (!unguarded) { expect(false, "PL-14: unrestricted control parses"); return; }
    ai::Engine control(world.host, {}, {unguarded.value()});
    ai::PotentialPlan without_requirement;
    expect(ai::EngineContractAccess::selection(control, player, without_requirement, 67),
        "PL-14: same fighter-only force is otherwise admissible, reproducing the dropped-union defect");
}

void scheduler_reads() {
    auth::ModuleManifest manifest;
    const bool added = static_cast<bool>(manifest.add("TEST.LUA",
        "function Worker() while true do coroutine.yield(true) end end\nfunction Once() end\n"));
    expect(added, "the test module is added");
    auth::SessionConfig config;
    config.seed = 1;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), std::move(manifest));
    expect(static_cast<bool>(scripts), "the scheduler starts");
    if (!scripts) return;
    std::vector<std::uint64_t> sequences;
    static_cast<void>(scripts.value().register_binding("Probe", [&sequences](auth::BindingContext& context, const auth::ValueList&) {
        sequences.push_back(context.command_sequence());
        context.issue_command("probe", {});
        sequences.push_back(context.command_sequence());
        return eawr::core::Result<auth::ValueList>::success(auth::ValueList{});
    }));
    expect(static_cast<bool>(scripts.value().create_instance(5, "TEST.LUA")), "the instance is created");
    auto submit = [&](auth::ScriptEvent::Kind kind, std::string name, std::uint64_t sequence) {
        auth::ScriptEvent event;
        event.key = auth::EventKey{scripts.value().completed_tick() + 1, auth::first_simulation_producer, 5, sequence};
        event.target = 5;
        event.kind = kind;
        event.name = std::move(name);
        return static_cast<bool>(scripts.value().submit_event(std::move(event)));
    };
    expect(submit(auth::ScriptEvent::Kind::start_thread, "Worker", 0) && submit(auth::ScriptEvent::Kind::start_thread, "Once", 1) &&
            submit(auth::ScriptEvent::Kind::call, "Probe", 2),
        "the events are accepted");
    const eawr::sim::InlineExecutor executor;
    auto report = scripts.value().service(executor);
    expect(static_cast<bool>(report), "the service runs");
    const auto slots = scripts.value().thread_slots(5);
    expect(slots && slots.value() == std::vector<bool>{true, false}, "PL-42: thread slots report which threads live");
    expect(sequences.size() == 2 && sequences[1] == sequences[0] + 1, "EX-20: the command sequence names the next command");
    auto read = scripts.value().read_global(5, "Worker");
    expect(read && !read.value().has_value(), "EX-40: a function global has no value form");
    expect(static_cast<bool>(scripts.value().remove_instance(5)), "PL-43: the instance is removed");
    expect(scripts.value().instances().empty(), "PL-43: no instance remains");
    expect(ai::block_id(5, 7) == ((std::uint64_t{5} << 32) | 7U), "EX-20: a block is named by instance and sequence");
}

void taskforce_team_order() {
    World world;
    ai::PlanDef definition;
    definition.name = "Ordered";
    definition.module = "ORDERED.LUA";
    ai::TaskForceDef force;
    force.name = "MainForce";
    force.teams.resize(2);
    definition.taskforces = {force};
    ai::Engine engine(world.host, {}, {definition});
    auth::ModuleManifest manifest;
    expect(manifest.add(definition.module,
        "function Base_Definitions() end\nfunction MainForce_Thread() coroutine.yield(true) end\n").has_value(),
        "PL-40: team ordering module loads");
    auth::SessionConfig config;
    config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
    expect(scripts.has_value(), "PL-40: team ordering scheduler starts");
    if (!scripts) return;
    ai::PlayerAi player; player.player = 1;
    ai::Goal goal; goal.id = 1; goal.potential.emplace();
    goal.potential->units = {50, 99, 51, 52};
    goal.potential->freestore = {0, 0, 0, 10};
    goal.potential->sources = {2, 2, 1, 0};
    goal.potential->producers = {101, 202, 303, 404};
    goal.potential->pool_tokens = {0, 0, 55, 0};
    goal.potential->taskforce_of_unit = {0, 0, 0, 0};
    goal.potential->team_of_unit = {1, 0, 1, 1};
    std::uint64_t sequence = 0;
    expect(ai::EngineContractAccess::attach(engine, player, goal, scripts.value(), sequence).has_value(),
        "PL-40: out-of-order selected units attach through the plan path");
    const auto* attached = engine.plan(goal.plan);
    expect(attached != nullptr && attached->taskforces.size() == 1, "PL-40: ordered force attaches once");
    if (attached == nullptr || attached->taskforces.size() != 1) return;
    const auto* actual = engine.taskforce(attached->taskforces.front());
    expect(actual != nullptr && actual->types == std::vector<eawr::sim::tactical::TypeId>{99, 50, 51, 52},
        "PL-40: authored teams precede selection order, with stable order within a team");
    if (actual != nullptr) expect(actual->sources == std::vector<std::uint8_t>{2, 2, 1, 0}
        && actual->producers == std::vector<eawr::sim::EntityId>{202, 101, 303, 404}
        && actual->pool_tokens == std::vector<std::uint64_t>{0, 0, 55, 0},
        "PL-40: production and pool identity metadata stay paired with their selected types");
}

void optional_taskforce_startup() {
    // An empty escort's authored exit must not end a selected main force's live plan.
    // Required forces still start when empty, so their authored failure is preserved.
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        for (const int scenario : {0, 1, 2}) {
            for (const auto source : std::array<std::uint8_t, 3>{0, 1, 2}) {
                World world;
                ai::PlanDef definition;
                definition.name = "Startup";
                definition.module = "STARTUP.LUA";
                definition.taskforces = {ai::TaskForceDef{}, ai::TaskForceDef{}};
                definition.taskforces[0].name = "MainForce";
                definition.taskforces[1].name = "EscortForce";
                definition.taskforces[1].required = scenario == 2;
                ai::Engine engine(world.host, {}, {definition});
                auth::ModuleManifest manifest;
                const std::string script = "function Base_Definitions() end\n"
                    "function MainForce_Thread() while true do Probe('main') coroutine.yield(true) end end\n"
                    "function EscortForce_Thread() Probe('escort') "
                    + std::string(scenario == 0 ? "" : "_ScriptExit() coroutine.yield(false) ")
                    + "while true do coroutine.yield(true) end end\n";
                expect(manifest.add(definition.module, script).has_value(), "PL-40: startup fixture module loads");
                auth::SessionConfig config;
                config.tick_duration = {1, 30};
                auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
                expect(scripts.has_value(), "PL-40: startup scheduler starts");
                if (!scripts) continue;
                expect(scripts.value().register_binding("Probe", [](auth::BindingContext& context, const auth::ValueList& args) {
                    context.issue_command("probe", args);
                    return eawr::core::Result<auth::ValueList>::success({});
                }).has_value(), "PL-40: startup observer registers");
                ai::PlayerAi player;
                player.player = 1;
                ai::Goal goal;
                goal.id = 1;
                goal.potential.emplace();
                const std::size_t selected = scenario == 0 ? 2U : 1U;
                for (std::size_t index = 0; index < selected; ++index) {
                    goal.potential->units.push_back(1);
                    goal.potential->freestore.push_back(10 + index);
                    goal.potential->sources.push_back(source);
                    goal.potential->producers.push_back(0);
                    goal.potential->pool_tokens.push_back(source == 1 ? index + 1 : 0);
                    goal.potential->taskforce_of_unit.push_back(index);
                    goal.potential->team_of_unit.push_back(0);
                }
                std::uint64_t sequence = 0;
                expect(ai::EngineContractAccess::attach(engine, player, goal, scripts.value(), sequence).has_value(),
                    "PL-40: selected plan attaches through the production path");
                for (const auto& event : ai::EngineContractAccess::attachment_events(engine)) {
                    expect(scripts.value().submit_event(event).has_value(), "PL-40: ordinary attachment event submits");
                }
                eawr::platform::ThreadWorkerAdapter executor(workers);
                std::vector<std::string> observed;
                std::vector<std::uint64_t> removed;
                for (int tick = 0; tick < 3; ++tick) {
                    auto report = scripts.value().service(executor);
                    expect(report.has_value(), "PL-40: selected plan's coroutine service succeeds");
                    if (!report) break;
                    for (const auto& command : report.value().commands) {
                        if (command.verb == "probe" && command.arguments.size() == 1) {
                            const auto* text = std::get_if<std::string>(&command.arguments.front().data);
                            if (text != nullptr) observed.push_back(*text);
                        }
                    }
                    removed.insert(removed.end(), report.value().removed_instances.begin(), report.value().removed_instances.end());
                    expect(engine.after_service(report.value()).has_value(), "PL-40: completion reaches the plan engine");
                }
                const auto* plan = engine.plan(goal.plan);
                const auto escort = scripts.value().read_global(ai::first_plan_instance, "EscortForce");
                if (scenario == 1) {
                    expect(observed == std::vector<std::string>{"main", "main", "main"} && removed.empty()
                        && plan != nullptr && !plan->exited && escort && escort.value() && std::holds_alternative<std::monostate>(escort.value()->data),
                        "PL-40: empty optional escort has no global or thread and the main plan keeps running");
                } else if (scenario == 0) {
                    expect(observed == std::vector<std::string>{"main", "escort", "main", "main"} && removed.empty()
                        && plan != nullptr && !plan->exited,
                        "PL-40: selected escort starts in definition order and both forces keep their plan");
                } else {
                    expect(observed == std::vector<std::string>{"main", "escort"}
                        && removed == std::vector<std::uint64_t>{ai::first_plan_instance} && plan != nullptr && plan->exited,
                        "PL-40: empty required escort starts and its authored failure ends the plan");
                }
            }
        }
    }
}

void hazard_plan_events() {
    namespace tactical = eawr::sim::tactical;
    World world;
    ai::ViewUnit unit;
    unit.id = 7;
    unit.type = 1;
    unit.owner = 1;
    world.view->units.insert(world.view->units.begin(), unit);
    world.host->snapshot = std::make_shared<const tactical::TacticalSnapshot>(1,
        std::vector<tactical::SnapshotPlayer>{}, std::vector<tactical::TacticalInstance>{},
        std::vector<tactical::Event>{
            {0, tactical::EventKind::ability_cancelled, 1, static_cast<std::uint64_t>(tactical::AbilityKind::turbo), 7},
            {0, tactical::EventKind::ability_ready, 1, static_cast<std::uint64_t>(tactical::AbilityKind::turbo), 7},
            {0, tactical::EventKind::ability_ready, 1, static_cast<std::uint64_t>(tactical::AbilityKind::turbo), 99}});
    auth::ModuleManifest manifest;
    expect(manifest.add("HAZARDS.LUA", "function MainForce_Unit_Ability_Ready(tf, unit, ability) end\n"
        "function Default_Unit_Ability_Cancelled(tf, unit, ability) end\n").has_value(), "WAB-34: handler fixture loads");
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), std::move(manifest));
    expect(scripts.has_value(), "WAB-34: handler scheduler starts");
    if (!scripts) return;
    expect(scripts.value().create_instance(1, "HAZARDS.LUA").has_value(), "WAB-34: plan instance loads");
    ai::Engine engine(world.host, {}, {ai::PlanDef{}});
    std::uint64_t sequence = 0;
    const auto events = ai::EngineContractAccess::hazard_events(engine, scripts.value(), sequence);
    expect(events.size() == 2, "WHZ-23/24: only the TaskForce member's signals reach its plan");
    if (events.size() != 2) return;
    expect(events[0].name == "Default_Unit_Ability_Cancelled" && events[1].name == "MainForce_Unit_Ability_Ready",
        "WAB-34: cancellation and ready use the ordinary fallback/direct plan handlers");
    expect(events[0].arguments.size() == 3 && events[1].arguments.size() == 3,
        "WAB-34: handlers receive TaskForce, unit and ability");
    if (events[1].arguments.size() == 3) {
        const auto* name = std::get_if<std::string>(&events[1].arguments[2].data);
        expect(name && *name == "Turbo", "WAB-34: the stock Turbo recovery comparison receives its expected spelling");
    }
}

// FH-23: the Lua freestore must see formation state even though the container has
// neither a ship locomotor nor weapon state. Exercise the real snapshot and bindings.
void squadron_ability_context() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{10, 100, 1, at(0), math::identity_quat(), {}},
        {11, 101, 1, at(0), math::identity_quat(), {}}, {20, 102, 2, at(2000), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11}}};
    tactical::MotionTable motion;
    tactical::CraftProfile craft;
    craft.type_id = 101;
    craft.max_speed = fixed(5); craft.min_speed = fixed(1); craft.rate_of_turn = fixed(6);
    craft.lift = fixed(6); craft.thrust = fixed(1); craft.roll_rate = fixed(6);
    craft.bank_angle = fixed(70); craft.strafe_distance = fixed(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101}, {at(0)}, fixed(1000), fixed(200), fixed(300), fixed(20)}};
    tactical::CombatTable combat;
    tactical::CombatProfile fighter, enemy;
    fighter.type_id = 101; fighter.max_attack_distance = fixed(450);
    enemy.type_id = 102;
    combat.profiles = {fighter, enemy};
    tactical::AbilityProfile lock;
    lock.kind = tactical::AbilityKind::spoiler_lock;
    tactical::UnitAbilityProfile abilities;
    abilities.type_id = 101;
    abilities.abilities = {lock};
    tactical::AbilityTable table;
    table.profiles = {abilities};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(setup, {}, {}, motion, std::nullopt, combat, {}, table);
        expect(created.has_value(), "FH-23: squadron context world starts");
        if (!created) return;
        auto world = std::move(created).value();
        auto host = std::make_shared<foc::detail::Host>();
        foc::AiType type;
        type.type_id = 100; type.locomotor = true; type.category_bits = 1;
        host->setup.content.types = {type};
        host->setup.content.categories.emplace("FIGHTER", 1);
        host->types.emplace(100, &host->setup.content.types[0]);
        auth::ModuleManifest modules;
        expect(modules.add("CONTEXT.LUA",
            "function Check(unit)\n"
            " target = unit.Get_Attack_Target()\n"
            " busy = unit.Has_Active_Orders()\n"
            " if target then unit.Activate_Ability('SPOILER_LOCK', false)\n"
            " elseif not busy then unit.Activate_Ability('SPOILER_LOCK', true) end\n"
            "end\n").has_value(), "FH-23: query fixture loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        if (!scripts) { expect(false, "FH-23: query scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "CONTEXT.LUA").has_value(),
            "FH-23: real object bindings register");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto query = [&]() {
            host->view = foc::detail::build_view(world, *world.snapshot());
            auth::ScriptEvent event;
            event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 10}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "FH-23: query queues");
            return scripts.value().service(executor);
        };
        const auto step = [&](tactical::CommandPayload payload, std::uint64_t sequence) {
            tactical::PlayerCommand command{{world.completed_tick(), 1, sequence}, {10}, std::move(payload)};
            expect(world.submit(command).has_value(), "FH-23: world order queues");
            auto result = world.step(executor);
            expect(result.has_value(), "FH-23: world steps");
            if (result) hashes.push_back(result.value().state_sha256);
        };
        const auto idle = query();
        expect(idle && idle.value().commands.size() == 1, "FH-23: idle freestore requests travel lock");
        step(tactical::AbilityPayload{tactical::AbilityKind::spoiler_lock, tactical::AbilityAction::activate}, 0);
        step(tactical::MovePayload{at(3000)}, 1);
        const auto travel = query();
        expect(travel && travel.value().commands.empty(), "FH-23: travelling formation is busy, no repeated idle toggle");
        expect(host->view->find(10)->formation_moving, "FH-23: container projects formation travel");
        expect(!host->view->find(10)->moving,
            "FH-23: Lua formation travel does not replace the separate taskforce completion input");
        step(tactical::AttackPayload{20}, 2);
        const auto engaged = query();
        expect(host->view->find(10)->formation_target == 20 && host->view->find(11)->formation_target == 20,
            "FH-23: container and craft project the live formation target");
        expect(host->view->find(10)->attack_target == 0 && host->view->find(11)->attack_target == 0,
            "PL-21: a distant formation target does not mark fighters engaged or fire an in-range event");
        const auto target = scripts.value().read_global(1, "target");
        expect(target && target.value() && std::get<auth::Handle>(target.value()->data).id == 20,
            "FH-23: Lua queries still return the commanded formation target");
        ai::Engine selection(host, {}, {});
        ai::PlayerAi player;
        player.player = 1;
        ai::PlanDef plan;
        plan.allow_engaged = false;
        const ai::Goal goal;
        expect(ai::EngineContractAccess::eligible(selection, player, goal, plan) == std::vector<eawr::sim::EntityId>{10},
            "PL-21: a fighter approaching its commanded target can still join an unengaged-only plan");
        auto combat_view = std::make_shared<foc::detail::WorldView>(*host->view);
        for (auto& unit : combat_view->units) if (unit.id == 11) unit.attack_target = 20;
        host->view = combat_view;
        expect(ai::EngineContractAccess::eligible(selection, player, goal, plan).empty(),
            "PL-21: actual craft combat excludes the squadron from an unengaged-only plan");
        plan.allow_engaged = true;
        expect(ai::EngineContractAccess::eligible(selection, player, goal, plan) == std::vector<eawr::sim::EntityId>{10},
            "PL-21: a plan allowing engaged fighters can take the same squadron");
        expect(engaged && engaged.value().commands.size() == 1,
            "FH-23: engaged freestore requests open foils");
        if (engaged && engaged.value().commands.size() == 1) {
            const auto& request = engaged.value().commands.front();
            expect(request.verb == foc::verb_ability && request.arguments.size() == 4
                && std::get<Real>(request.arguments[3].data) == Real(static_cast<int>(tactical::AbilityAction::deactivate)),
                "FH-23: the engaged request deactivates travel lock");
        }
        const auto busy = scripts.value().read_global(1, "busy");
        expect(busy && busy.value() && std::get<bool>(busy.value()->data), "FH-23: engaged squadron reports active orders");
        step(tactical::AbilityPayload{tactical::AbilityKind::spoiler_lock, tactical::AbilityAction::deactivate}, 3);
        const auto state = world.ability_state(11);
        expect(state && !state->slots[0].active, "FH-23: engaged X-wing foils are open");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "FH-23: squadron context hashes equally across workers");
    }
}

// EX-31: pad-capture plans must keep their travelling squadron reserved until its
// formation arrives. A container has no ship Motion; test the real simulation snapshot.
void squadron_movement_blocks() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{10, 100, 1, at(0), math::identity_quat(), {}},
        {11, 101, 1, at(0), math::identity_quat(), {}}, {20, 102, 2, at(2000), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11}}};
    tactical::MotionTable motion;
    tactical::CraftProfile craft;
    craft.type_id = 101;
    craft.max_speed = fixed(5); craft.min_speed = fixed(1); craft.rate_of_turn = fixed(6);
    craft.lift = fixed(6); craft.thrust = fixed(1); craft.roll_rate = fixed(6);
    craft.bank_angle = fixed(70); craft.strafe_distance = fixed(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101}, {at(0)}, fixed(1000), fixed(200), fixed(300), fixed(20)}};
    std::optional<std::uint64_t> baseline_arrival;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(setup, {}, {}, motion);
        expect(created.has_value(), "EX-31: squadron movement world starts");
        if (!created) return;
        auto world = std::move(created).value();
        auto host = std::make_shared<foc::detail::Host>();
        auth::ModuleManifest modules;
        expect(modules.add("MOVEMENT.LUA", "function MainForce_Unit_Move_Finished(tf, unit) end\n").has_value(),
            "EX-31: movement handler loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        expect(scripts && scripts.value().create_instance(1, "MOVEMENT.LUA").has_value(),
            "EX-31: movement handler instance starts");
        if (!scripts) return;
        ai::PlanDef definition;
        definition.name = "FormationMove";
        ai::Engine engine(host, {}, {definition});
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::movement_fixture(engine, scripts.value(), sequence);
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const tactical::PlayerCommand move{{0, 1, 0}, {10}, tactical::MovePayload{at(300)}};
        expect(world.submit(move).has_value(), "EX-31: ordinary squadron move submits");
        for (int tick = 0; tick < 2; ++tick) expect(world.step(executor).has_value(), "EX-31: squadron moves");
        host->view = foc::detail::build_view(world, *world.snapshot());
        expect(host->view->find(10)->formation_moving && !host->view->find(10)->moving,
            "EX-31: travelling container has formation motion and no ship Motion");
        ai::EngineContractAccess::movement_service(engine, 2);
        expect(!ai::EngineContractAccess::movement_finished(engine, 1) &&
            !ai::EngineContractAccess::movement_finished(engine, 2) && sequence == 0,
            "EX-31: move and ambush blocks do not finish or signal two ticks after departure");
        while (world.completed_tick() < 5000 && world.squadron_state(10)->mode == tactical::SquadronMode::move) {
            if (!world.step(executor)) { expect(false, "EX-31: travelling world steps"); return; }
        }
        expect(world.squadron_state(10)->mode != tactical::SquadronMode::move,
            "EX-31: squadron reaches its destination within the bounded fixture");
        host->view = foc::detail::build_view(world, *world.snapshot());
        ai::EngineContractAccess::movement_service(engine, static_cast<std::int64_t>(world.completed_tick()));
        expect(ai::EngineContractAccess::movement_finished(engine, 1) &&
            ai::EngineContractAccess::movement_finished(engine, 2) && sequence == 2,
            "EX-31: both blocks finish and signal exactly once at actual formation arrival");
        ai::EngineContractAccess::movement_service(engine, static_cast<std::int64_t>(world.completed_tick() + 1));
        expect(sequence == 2, "EX-31: completed formation blocks do not signal again");
        if (!baseline_arrival) baseline_arrival = world.completed_tick();
        else expect(world.completed_tick() == *baseline_arrival, "EX-31: arrival agrees on 1/2/4/8 workers");
    }
}

// EX-36 / WBP-40: arrival is not completion of guarding a live capture point.
void guard_block_lifecycle() {
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
    for (const bool lose_force : {false, true}) {
        World world;
        ai::ViewUnit target;
        target.id = 20;
        world.view->units.push_back(target);
        auth::ModuleManifest modules;
        expect(modules.add("GUARD.LUA", "function MainForce_Unit_Move_Finished(tf, unit) end\n"
            "function MainForce_Current_Target_Destroyed(tf) end\n").has_value(), "EX-36: guard handlers load");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        expect(scripts && scripts.value().create_instance(1, "GUARD.LUA").has_value(), "EX-36: guard instance starts");
        if (!scripts) return;
        ai::PlanDef definition;
        definition.name = "GuardCapturePoint";
        ai::Engine engine(world.host, {}, {definition});
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::guard_fixture(engine, scripts.value(), sequence);
        ai::EngineContractAccess::movement_service(engine, 2);
        expect(!ai::EngineContractAccess::movement_finished(engine, 1)
            && ai::EngineContractAccess::movement_finished(engine, 2) && sequence == 2,
            "EX-36: stopped movers signal arrival once; only the ordinary move completes");
        for (const auto owner : {1U, 2U, 0U}) {
            world.view->units.back().owner = owner;
            ai::EngineContractAccess::movement_service(engine, 1802);
            expect(!ai::EngineContractAccess::movement_finished(engine, 1) && sequence == 2,
                "EX-36: guard survives neutral, friendly and hostile ownership after its mover list empties");
        }
        if (lose_force) ai::EngineContractAccess::empty_guard_force(engine);
        else world.view->units.pop_back();
        ai::EngineContractAccess::movement_service(engine, 1803);
        expect(ai::EngineContractAccess::movement_finished(engine, 1)
            && sequence == (lose_force ? 2U : 3U), "EX-36: force loss or target death releases the guard");
        ai::EngineContractAccess::movement_service(engine, 1804);
        expect(sequence == (lose_force ? 2U : 3U), "EX-36: released guard emits no duplicate events");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(scripts.value().service(executor).has_value(), "EX-36: lifecycle callbacks service on every worker count");
    }
    }
}

void target_choice() {
    int draws = 0;
    const auto choose = [&draws](std::vector<Real> scores, Real fraction, Real unit) {
        return ai::choose_target(scores, fraction, [&draws, unit] {
            ++draws;
            return unit;
        });
    };
    const Real half = ai::real(1) / ai::real(2);
    expect(!choose({}, half, 0), "FT-03: no candidate answers nil");
    expect(!choose({0, 0, 0}, half, 0) && !choose({0, 0, 0}, 0, 0), "FT-03: every score at 0 answers nil");
    expect(!choose({-1, -3}, half, 0) && !choose({-1, -3}, 0, 0) && !choose({-2, 0}, half, 0),
        "FT-03: every score at or below 0 answers nil");
    expect(draws == 0, "FT-03: no draw without a positive weight");
    expect(choose({0, 3, 0, -1}, half, 0) == 1U && draws == 0,
        "FT-03: a best with only zero or negative others is the answer, without a draw");
    expect(choose({0, 4}, half, 0) == 1U, "FT-03: a later positive score replaces the empty best");
    expect(choose({2, 2, 1}, 0, 0) == 0U && draws == 0, "FT-03: equal scores keep the earlier target as the best");
    // {2, 2, 1} at fraction 1/2: the draw weighs [#1: 2, #2: 1, best #0: 3 / (1/2) = 6], total 9.
    expect(choose({2, 2, 1}, half, 0) == 1U, "FT-01: the draw walks the others in list order first");
    expect(choose({2, 2, 1}, half, ai::real(1) / ai::real(4)) == 2U, "FT-01: 2.25 of 9 falls on the third target");
    expect(choose({2, 2, 1}, half, half) == 0U, "FT-01: the best weighs the others' total over the fraction");
    expect(draws == 3, "FT-01: one draw per weighted choice");
}

// DT-02: the decay step is read under its real tag name, and is 1 when the file has none.
void threat_decay_constant() {
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
        return std::nullopt;
    };
    const auto load = [&](const std::string& constants) {
        std::map<std::string, std::string, std::less<>> xml;
        for (const std::string& path : ai::ai_xml_files()) xml.emplace(path, "<Root></Root>");
        xml["data/xml/gameconstants.xml"] = constants;
        return ai::load_ai_data(xml, converters);
    };
    const auto authored = load("<Root><AI_SpaceThreatDecayStep>0.05</AI_SpaceThreatDecayStep></Root>");
    expect(authored && Real{} < authored.value().constants.threat_decay_step
            && authored.value().constants.threat_decay_step < ai::real(1) / ai::real(10),
        "DT-02: AI_SpaceThreatDecayStep is read from gameconstants.xml");
    const auto misspelled = load("<Root><AI_SpaceThreatDecay>0.05</AI_SpaceThreatDecay></Root>");
    expect(misspelled && misspelled.value().constants.threat_decay_step == ai::real(1),
        "DT-02: the engine default is 1 when the tag is absent");
}

void reveal_all_contracts() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, 1}, {2, 2, 200, 1}, {3, 1, 300, 1}, {4, 4, 400, 0}};
    setup.units = {{10, 100, 1, {fixed(50), fixed(-50), {}}},
        {20, 200, 2, {fixed(450), fixed(-450), {}}}};
    t::FogRules rules{{}, {}, fixed(100), 6, 6};
    // Partition failure cannot publish partially assigned rows or the persistent hold.
    struct FailingExecutor final : eawr::sim::PartitionExecutor {
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(std::size_t count, const std::function<void(std::size_t)>& partition) const override {
            for (std::size_t index = 0; index < count / 2; ++index) partition(index);
            return eawr::core::Result<void>::failure(foc::detail::make_error(t::diagnostic_codes::worker_failure, "fixture failure"));
        }
    };
    t::FogCells fog_fixture(rules, setup.players);
    std::vector<std::uint8_t> before, after;
    fog_fixture.append_state(before);
    expect(!fog_fixture.reveal_all(1, FailingExecutor{}), "V-20: reveal reports partition failure");
    fog_fixture.append_state(after);
    expect(before == after, "V-20: failed reveal leaves canonical cells and future holds unchanged");
    expect(fog_fixture.reveal_all(1, eawr::sim::InlineExecutor{}).has_value(), "V-20: reveal succeeds after failure");
    expect(fog_fixture.copied_grid_bytes() == rules.cells_wide, "V-20: whole-map reveal allocates only one row of cell bytes");
    after.clear(); fog_fixture.append_state(after);
    expect(fog_fixture.reveal_all(1, eawr::sim::InlineExecutor{}).has_value(), "V-20: repeated reveal succeeds");
    before.clear(); fog_fixture.append_state(before);
    expect(before == after, "V-20: repeated reveal is idempotent");
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto made = t::TacticalSession::create(setup, {}, {}, {}, rules);
        expect(made.has_value(), "V-20: fog world starts");
        if (!made) return;
        auto host = std::make_shared<foc::detail::Host>();
        host->world = &made.value();
        host->view = foc::detail::build_view(made.value(), *made.value().snapshot());
        auth::ModuleManifest manifest;
        expect(manifest.add("BURN.LUA", "function Firesale(player)\n"
            " local result = FogOfWar.Reveal_All(player, 'extra argument')\n"
            " if result ~= nil then error('expected nil') end\n"
            " continued = true\n"
            "end\n").has_value(), "V-20: narrow firesale continuation module loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "V-20: scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "BURN.LUA").has_value(), "V-20: binding registers");
        for (const auto& [tick, recipient] : {std::pair{1U, 2U}, std::pair{2U, 1U}, std::pair{3U, 4U}}) {
            auth::ScriptEvent event;
            event.key = {tick, auth::first_simulation_producer, 1, tick};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Firesale";
            event.arguments = {auth::Value{auth::Handle{foc::handle_player, recipient}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "V-20: AI/local/neutral reveal events queue");
        }
        auto linked = auth::ScriptedTacticalSession::create(std::move(made).value(), std::move(scripts).value());
        if (!linked) { expect(false, "V-20: scripted world links"); return; }
        auto& session = linked.value();
        expect(session.register_verb(foc::verb_reveal_all, foc::tactical_ai_detail::translate_order).has_value(),
            "V-20: reveal uses production command router");
        const auto previous_rows = session.world().fog_cells()->value_rows(1);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 224; ++tick) {
            host->world = &session.world();
            host->view = foc::detail::build_view(session.world(), *session.world().snapshot());
            const auto stepped = session.step(executor);
            expect(stepped.has_value(), "V-20: firesale world steps");
            if (!stepped) return;
            expect(stepped.value().scripts.diagnostics.empty(), "V-20: supported call does not abort its thread");
            for (const auto& input : stepped.value().script_input)
                expect(input.submitted, "V-20: script reveal reaches replay queue");
            hashes.push_back(stepped.value().world.state_sha256);
            const auto* fog = session.world().fog_cells();
            const auto full = [&](std::size_t player, std::uint8_t value) {
                const auto cells = fog->values(player);
                return std::all_of(cells.begin(), cells.end(), [&](std::uint8_t cell) { return cell == value; });
            };
            expect(full(2, 0), "V-20: reveal does not leak to an allied observer");
            if (tick == 0) expect(full(0, 0) && full(1, 0), "V-20: call changes only the next tick");
            if (tick == 1) {
                expect(full(1, 255) && full(0, 0), "V-20: AI reveal leaves the local observer fogged");
                expect((session.world().snapshot()->instances()[0].visible_to & 2U) != 0,
                    "V-20: revealed enemy is present in recipient visibility");
            }
            if (tick >= 2) expect(full(0, 255) && full(1, 255), "V-20: explicit local reveal persists through refresh");
            if (tick >= 3) expect(full(3, 255), "V-20: non-commandable recipient has its own revealed grid");
        }
        expect((*previous_rows[0])[0] == 0, "V-20: previous fog rows remain immutable");
        const auto recorded = session.record();
        expect(recorded.commands.size() == 3, "V-20: AI/local/neutral reveals each record one command");
        const auto encoded = t::write_replay(recorded);
        expect(encoded.has_value(), "V-20: reveal replay encodes");
        if (!encoded) return;
        const auto parsed = t::parse_replay(encoded.value());
        expect(parsed.has_value() && parsed.value().commands == recorded.commands, "V-20: replay preserves recipient and issuer");
        if (!parsed) return;
        auto replayed = t::TacticalSession::from_replay(parsed.value(), {}, {}, {}, rules);
        expect(replayed.has_value(), "V-20: recorded reveals replay without Lua");
        if (!replayed) return;
        for (std::size_t tick = 0; tick < hashes.size(); ++tick) {
            auto stepped = replayed.value().step(executor);
            expect(stepped && stepped.value().state_sha256 == hashes[tick], "V-20: replay reproduces every world hash");
        }
        auto malformed = encoded.value();
        malformed[malformed.size() - 12] = 1; // last command's reserved payload word
        expect(!t::parse_replay(malformed), "V-20: nonzero reveal reserved field is rejected");
        auto invalid = recorded;
        std::get<t::RevealAllPayload>(invalid.commands[0].payload).player = 99;
        expect(!t::write_replay(invalid), "V-20: undeclared recipient is rejected");
        invalid = recorded; invalid.commands[0].units = {10};
        expect(!t::write_replay(invalid), "V-20: reveal with unit orders is rejected");
        if (workers == 1) baseline = hashes;
        else expect(hashes == baseline, "V-20: canonical reveal agrees on 1/2/4/8 workers");
    }
    // Invalid arguments and a non-tactical host retain script errors, never missing-API errors.
    for (const auto& test : {std::pair{true, std::string{}}, std::pair{true, std::string{"nil"}},
            std::pair{true, std::string{"123"}}, std::pair{false, std::string{}}}) {
        auto host = std::make_shared<foc::detail::Host>();
        if (test.first) host->view = std::make_shared<foc::detail::WorldView>();
        auth::ModuleManifest manifest;
        expect(manifest.add("BAD.LUA", "function Check() FogOfWar.Reveal_All(" + test.second + ") end\n").has_value(),
            "V-20: bad-argument module loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) return;
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "BAD.LUA").has_value(), "V-20: bad-argument binding registers");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0}; event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        expect(scripts.value().submit_event(std::move(event)).has_value(), "V-20: invalid call queues");
        const auto report = scripts.value().service(eawr::sim::InlineExecutor{});
        expect(report && report.value().commands.empty() && !report.value().diagnostics.empty(), "V-20: invalid call reports and issues no reveal");
        if (report) for (const auto& error : report.value().diagnostics) {
            expect(error.code != auth::codes::missing_api, "V-20: genuine argument error stays distinct from unsupported API");
            expect(error.message.find(test.first ? (test.second.empty() ? "requires a player" : "expected a player")
                : "only valid in a tactical game") != std::string::npos, "V-20: argument/mode error explains the rejected call");
        }
    }
}

void faction_controller_inventory() {
    // WSS-35 / GS-02: assigning a faction's controller is insufficient unless
    // its definition and space function set also reach the goal engine.
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
        return std::nullopt;
    };
    std::map<std::string, std::string, std::less<>> xml;
    for (const auto& path : ai::ai_xml_files()) xml.emplace(path, "<Root></Root>");
    const std::string player_path = "data/xml/ai/players/ai_player_underworld.xml";
    const std::string set_path = "data/xml/ai/goalfunctions/ai_goalset_underworld_space.xml";
    expect(xml.contains(player_path) && xml.contains(set_path), "WSS-35: the Underworld player and space set are loaded");
    xml[player_path] = "<AIPlayerType><Name>AI_Player_Underworld</Name>"
        "<GoalProposalFunctionSets>AI_GoalSet_Underworld_Space</GoalProposalFunctionSets>"
        "<Templates><Space>Test_Space</Space></Templates></AIPlayerType>";
    xml[set_path] = "<FunctionSet><Probe><Goal>Probe</Goal><Function>ReallyBig</Function></Probe></FunctionSet>";
    const auto controller = ai::load_ai_data(xml, converters);
    expect(controller && controller.value().players.contains("AI_PLAYER_UNDERWORLD")
        && controller.value().players.at("AI_PLAYER_UNDERWORLD").function_sets == std::vector<std::string>{"AI_GOALSET_UNDERWORLD_SPACE"}
        && controller.value().players.at("AI_PLAYER_UNDERWORLD").space_templates == std::vector<std::string>{"TEST_SPACE"}
        && controller.value().function_sets.at("AI_GOALSET_UNDERWORLD_SPACE").size() == 1,
        "WSS-35 / GS-02: the Underworld goal engine has its authored controller and space functions");
}

} // namespace

int main() {
    equations();
    random_and_hash();
    initial_goal_budget();
    capture_target_application();
    collect_free_categories();
    order_destinations();
    selection_team_starts();
    threat_grid();
    threat_preparation();
    taskforce_definitions();
    required_category_unions();
    scheduler_reads();
    optional_taskforce_startup();
    taskforce_team_order();
    hazard_plan_events();
    squadron_ability_context();
    squadron_movement_blocks();
    guard_block_lifecycle();
    target_choice();
    threat_decay_constant();
    faction_controller_inventory();
    reveal_all_contracts();
    economy_inputs();
    shared_station_perception();
    tactical_activation_estimate();
    production_lifecycle();
    completed_purchases_are_available();
    reinforcement_work_budget();
    reinforcement_service_budget();
    duplicate_reinforcement_calls();
    taskforce_attachment_waits_for_arrival();
    concurrent_reserved_arrivals();
    reinforcement_gravity_well();
    reinforcement_fog_persistence();
    reinforcement_population_wait();
    if (failures != 0) {
        std::cerr << failures << " FoC AI contract(s) failed\n";
        return 1;
    }
    std::cout << "FoC AI contracts passed\n";
    return 0;
}
