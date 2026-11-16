// Contracts of the three tactical super weapon script calls (docs/behaviour/space-tactical-superweapon.md TSW-07,
// docs/behaviour/foc-tactical-ai.md): the global Find_First_Object, the object methods Is_Tactical_Superweapon_Ready and
// Fire_Tactical_Superweapon. The world is the synthetic contract fixture; the production bindings and the production
// order translation run on it.
#include "../../support/env.hpp"
#include "foc_ai_contract_fixtures.hpp"
#include "tactical_ai_world.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/tsw.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace foc_contract {

namespace {

constexpr tactical::TypeId tsw_type = 2;
constexpr tactical::TypeId catalog_only_type = 3;

constexpr eawr::sim::EntityId ship = 10;
constexpr eawr::sim::EntityId first_tsw = 20;  // owner 1
constexpr eawr::sim::EntityId second_tsw = 30; // owner 2

struct TswWorld {
    World world;
    std::unique_ptr<auth::ScriptScheduler> scripts;
    std::uint64_t sequence{};

    TswWorld() {
        foc::AiType type;
        type.type_id = tsw_type;
        type.name = "DEATH_STAR"; // upper case, as the host keys its names
        world.host->setup.content.types.push_back(type);
        // The vector may have moved: rebuild the lookups the fixture built.
        world.host->types.clear();
        world.host->types_by_name.clear();
        for (const foc::AiType& entry : world.host->setup.content.types) {
            world.host->types.emplace(entry.type_id, &entry);
            world.host->types_by_name.emplace(entry.name, &entry);
        }
        using Names = std::vector<std::pair<std::string, tactical::TypeId>>;
        world.host->setup.content.catalog_types = std::make_shared<const Names>(Names{{"CATALOG_ONLY", catalog_only_type}});
        add_tsw(first_tsw, 1);
        add_tsw(second_tsw, 2);
    }
    void add_tsw(const eawr::sim::EntityId id, const tactical::PlayerId owner) {
        ai::ViewUnit row;
        row.id = id;
        row.type = tsw_type;
        row.owner = owner;
        row.visible_to = 3;
        row.tsw = foc::detail::TswView{true, tactical::TswPhase::idle, true};
        world.view->units.push_back(row);
    }
    foc::detail::ViewUnit& unit(const eawr::sim::EntityId id) {
        return *std::find_if(world.view->units.begin(), world.view->units.end(),
            [id](const foc::detail::ViewUnit& row) { return row.id == id; });
    }
    bool start() {
        auth::ModuleManifest manifest;
        const auto added = manifest.add("TSW.LUA", R"LUA(
            function First(name) found = Find_First_Object(name) end
            function FirstExtra(name) found = Find_First_Object(name, 5, nil) end
            function FirstNone() Find_First_Object() end
            function FirstNumber() Find_First_Object(5) end
            function Ready(object) ready = object.Is_Tactical_Superweapon_Ready() end
            function Fire(object) fired = object.Fire_Tactical_Superweapon() end
            function FireTwice(object)
                first = object.Fire_Tactical_Superweapon()
                second = object.Fire_Tactical_Superweapon()
            end
            function FireBoth(first_object, second_object)
                first = first_object.Fire_Tactical_Superweapon()
                second = second_object.Fire_Tactical_Superweapon()
            end
            function FindAndFire(name)
                local object = Find_First_Object(name)
                while not object.Is_Tactical_Superweapon_Ready() do end
                fired = object.Fire_Tactical_Superweapon()
            end
        )LUA");
        expect(added.has_value(), "TSW-07: script fixture loads");
        auto created = auth::ScriptScheduler::create({}, std::move(manifest));
        expect(created.has_value(), "TSW-07: scheduler starts");
        if (!added || !created) return false;
        scripts = std::make_unique<auth::ScriptScheduler>(std::move(created.value()));
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(*scripts, world.host, errors);
        foc::tactical_ai_detail::register_methods(*scripts, world.host, errors);
        expect(errors.empty(), "TSW-07: production bindings register");
        expect(scripts->create_instance(1, "TSW.LUA").has_value() && scripts->create_instance(2, "TSW.LUA").has_value(),
            "TSW-07: two plan instances");
        return errors.empty();
    }
    // Runs one call on `instance`; the report carries the staged commands. `tick` 0 is the next scheduler tick.
    std::optional<auth::ServiceReport> call(const std::uint64_t instance, const std::string& name, auth::ValueList arguments,
        const std::uint64_t tick = 0) {
        auth::ScriptEvent event;
        event.key = {tick != 0 ? tick : scripts->completed_tick() + 1, foc::producer_foc_engine, instance, sequence++};
        event.target = instance;
        event.kind = auth::ScriptEvent::Kind::call;
        event.name = name;
        event.arguments = std::move(arguments);
        expect(scripts->submit_event(std::move(event)).has_value(), "TSW-07: call queues");
        const eawr::sim::InlineExecutor executor;
        auto report = scripts->service(executor);
        expect(report.has_value(), "TSW-07: call services");
        if (!report) return std::nullopt;
        return std::move(report.value());
    }
    std::optional<auth::Value> global(const std::uint64_t instance, const std::string& name) {
        const auto value = scripts->read_global(instance, name);
        return value && value.value() ? std::optional<auth::Value>(*value.value()) : std::nullopt;
    }
};

auth::Value object_handle(const eawr::sim::EntityId id) { return auth::Value{auth::Handle{foc::handle_game_object, id}}; }
auth::Value text(const std::string& value) { return auth::Value::text(value); }

bool is_nil(const std::optional<auth::Value>& value) { return value && std::holds_alternative<std::monostate>(value->data); }
bool is_object(const std::optional<auth::Value>& value, const eawr::sim::EntityId id) {
    if (!value) return false;
    const auto* handle = std::get_if<auth::Handle>(&value->data);
    return handle != nullptr && handle->kind == foc::handle_game_object && handle->id == id;
}
bool is_bool(const std::optional<auth::Value>& value, const bool expected) {
    if (!value) return false;
    const auto* flag = std::get_if<bool>(&value->data);
    return flag != nullptr && *flag == expected;
}

void find_first_object_cases() {
    TswWorld fixture;
    if (!fixture.start()) return;
    // The first match by identity, whoever owns it.
    auto report = fixture.call(1, "First", {text("Death_Star")});
    expect(report && report->diagnostics.empty() && is_object(fixture.global(1, "found"), first_tsw),
        "#1713: Find_First_Object returns the lowest-identity object of the type, any owner, matching the name case-insensitively");
    report = fixture.call(1, "First", {text("SHIP")});
    expect(report && is_object(fixture.global(1, "found"), ship), "#1713: Find_First_Object finds an ordinary type");
    // A type with no live object, a catalog-only type and an unknown name all answer nil.
    report = fixture.call(1, "First", {text("CATALOG_ONLY")});
    expect(report && report->diagnostics.empty() && is_nil(fixture.global(1, "found")), "#1713: a type with no live object answers nil");
    report = fixture.call(1, "First", {text("NO_SUCH_TYPE")});
    expect(report && report->diagnostics.empty() && is_nil(fixture.global(1, "found")), "#1713: an unknown type name answers nil");
    // Extra arguments are ignored.
    report = fixture.call(1, "FirstExtra", {text("Death_Star")});
    expect(report && report->diagnostics.empty() && is_object(fixture.global(1, "found"), first_tsw), "#1713: extra arguments are ignored");
    // A missing or non-string first argument is a script error.
    report = fixture.call(1, "FirstNone", {});
    expect(report && report->diagnostics.size() == 1, "#1713: no argument is a script error");
    report = fixture.call(1, "FirstNumber", {});
    expect(report && report->diagnostics.size() == 1, "#1713: a non-string argument is a script error");
    // The lowest identity moves on when the first object is gone.
    auto& units = fixture.world.view->units;
    units.erase(std::remove_if(units.begin(), units.end(),
        [](const foc::detail::ViewUnit& row) { return row.id == first_tsw; }), units.end());
    report = fixture.call(1, "First", {text("Death_Star")});
    expect(report && is_object(fixture.global(1, "found"), second_tsw), "#1713: the lookup follows the live objects");
    units.erase(std::remove_if(units.begin(), units.end(),
        [](const foc::detail::ViewUnit& row) { return row.id == second_tsw; }), units.end());
    report = fixture.call(1, "First", {text("Death_Star")});
    expect(report && is_nil(fixture.global(1, "found")), "#1713: no live object answers nil");
}

void readiness_cases() {
    TswWorld fixture;
    if (!fixture.start()) return;
    // True exactly in the ready phase; it looks at neither the gate nor the owner.
    for (std::uint8_t phase = 0; phase <= static_cast<std::uint8_t>(tactical::TswPhase::complete); ++phase) {
        for (const bool gate : {true, false}) {
            fixture.unit(first_tsw).tsw = foc::detail::TswView{true, static_cast<tactical::TswPhase>(phase), gate};
            const auto report = fixture.call(1, "Ready", {object_handle(first_tsw)});
            expect(report && report->diagnostics.empty()
                    && is_bool(fixture.global(1, "ready"), phase == static_cast<std::uint8_t>(tactical::TswPhase::ready)),
                "TSW-07: Is_Tactical_Superweapon_Ready is true exactly in the ready phase (phase " + std::to_string(phase) + ")");
        }
    }
    // An inert behaviour (the battle has no setup for it) is never ready.
    fixture.unit(first_tsw).tsw = foc::detail::TswView{false, tactical::TswPhase::ready, true};
    auto report = fixture.call(1, "Ready", {object_handle(first_tsw)});
    expect(report && is_bool(fixture.global(1, "ready"), false), "TSW-07: an inert behaviour is never ready");
    // An object without the behaviour is a script error.
    report = fixture.call(1, "Ready", {object_handle(ship)});
    expect(report && report->diagnostics.size() == 1, "TSW-07: readiness on a non-super-weapon object is a script error");
}

void fire_cases() {
    TswWorld fixture;
    if (!fixture.start()) return;
    // Fire from every phase and both gate states: only ready with an open gate stages the command and answers true.
    for (std::uint8_t phase = 0; phase <= static_cast<std::uint8_t>(tactical::TswPhase::complete); ++phase) {
        for (const bool gate : {true, false}) {
            fixture.unit(second_tsw).tsw = foc::detail::TswView{true, static_cast<tactical::TswPhase>(phase), gate};
            const auto report = fixture.call(2, "Fire", {object_handle(second_tsw)});
            const bool accepted = phase == static_cast<std::uint8_t>(tactical::TswPhase::ready) && gate;
            const std::string label = " (phase " + std::to_string(phase) + (gate ? ", gate open)" : ", gate shut)");
            expect(report && report->diagnostics.empty() && is_bool(fixture.global(2, "fired"), accepted),
                "TSW-07: Fire_Tactical_Superweapon answers whether the power-up request was accepted" + label);
            expect(report && report->commands.size() == (accepted ? 1U : 0U), "TSW-07: a request is staged only when accepted" + label);
            if (!report || !accepted || report->commands.empty()) continue;
            auto order = foc::tactical_ai_detail::translate_order(report->commands.front());
            expect(order.has_value(), "TSW-07: the staged request translates");
            if (!order) continue;
            const auto* payload = std::get_if<tactical::TacticalSuperweaponPayload>(&order.value().payload);
            expect(payload != nullptr && payload->stage == tactical::TswStage::start_power_up
                    && payload->admission == tactical::TswAdmission::ai_object,
                "TSW-07: the shared switch command, power-up stage, AI admission");
            expect(order.value().issuer == 2 && order.value().units == std::vector<eawr::sim::EntityId>{second_tsw},
                "TSW-07: issuer is the object's owner and the one listed unit is the object");
        }
    }
    // An inert behaviour does not fire.
    fixture.unit(second_tsw).tsw = foc::detail::TswView{false, tactical::TswPhase::ready, true};
    auto report = fixture.call(2, "Fire", {object_handle(second_tsw)});
    expect(report && report->commands.empty() && is_bool(fixture.global(2, "fired"), false), "TSW-07: an inert behaviour does not fire");
    // An object without the behaviour is a script error and stages nothing.
    report = fixture.call(2, "Fire", {object_handle(ship)});
    expect(report && report->diagnostics.size() == 1 && report->commands.empty(),
        "TSW-07: firing a non-super-weapon object is a script error");
}

void double_fire_cases() {
    TswWorld fixture;
    if (!fixture.start()) return;
    fixture.unit(second_tsw).tsw = foc::detail::TswView{true, tactical::TswPhase::ready, true};
    fixture.unit(first_tsw).tsw = foc::detail::TswView{true, tactical::TswPhase::ready, true};
    // The same instance asking twice in one tick: the first is accepted, the second is not, one command is staged.
    auto report = fixture.call(2, "FireTwice", {object_handle(second_tsw)});
    expect(report && report->diagnostics.empty() && is_bool(fixture.global(2, "first"), true)
            && is_bool(fixture.global(2, "second"), false) && report->commands.size() == 1,
        "TSW-07: a second call in the same tick answers false and stages nothing more");
    // The next tick is a new request (the completed view still says ready).
    report = fixture.call(2, "Fire", {object_handle(second_tsw)});
    expect(report && is_bool(fixture.global(2, "fired"), true) && report->commands.size() == 1,
        "TSW-07: the next tick may ask again");
    // Two different objects in one tick are two requests.
    report = fixture.call(1, "FireBoth", {object_handle(first_tsw), object_handle(second_tsw)});
    expect(report && is_bool(fixture.global(1, "first"), true) && is_bool(fixture.global(1, "second"), true)
            && report->commands.size() == 2, "TSW-07: each object has its own request in a tick");
}

void stock_plan_shape() {
    // The stock plan's shape: find the object, wait for readiness, then fire once.
    TswWorld fixture;
    if (!fixture.start()) return;
    fixture.unit(first_tsw).tsw = foc::detail::TswView{true, tactical::TswPhase::ready, true};
    const auto report = fixture.call(1, "FindAndFire", {text("Death_Star")});
    expect(report && report->diagnostics.empty() && is_bool(fixture.global(1, "fired"), true) && report->commands.size() == 1,
        "#1713: find, ready, fire in one plan pass");
}

} // namespace

// ---- The stock plan on installed data ------------------------------------------------------------------------------

namespace {

constexpr tactical::TypeId stock_tsw_type = 10;
constexpr tactical::TypeId stock_enemy_type = 11;
constexpr eawr::sim::EntityId stock_tsw_id = 1;
constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr std::uint32_t fixture_taskforce_kind = 900;

eawr::sim::math::Fixed units(const std::int64_t value) { return eawr::sim::math::Fixed::from_raw(value * one); }
eawr::sim::math::Vec3 at(const std::int64_t x, const std::int64_t y = 0, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

// The stock numbers (docs/behaviour/space-tactical-superweapon.md TSW-05, TSW-08): attack distance 5800, Max_Speed 0.75 and
// Max_Thrust 0.02 per frame, 270 / 75 / 33 / 350 frames.
tactical::TswProfile stock_profile() {
    tactical::TswProfile profile;
    profile.attack_distance = units(5800);
    profile.max_speed = eawr::sim::math::Fixed::from_raw(one * 3 / 4);
    profile.max_thrust = eawr::sim::math::Fixed::from_raw(one / 50);
    profile.power_up_frames = 270;
    profile.attack_anim_frames = 75;
    profile.debris_delay_frames = 33;
    profile.post_destruction_wait_frames = 350;
    return profile;
}

// Player 1 (faction 1) owns a Death Star 8000 units from a target prop of faction 2 and attacks a destroyable planet.
tactical::TacticalSession make_attack_session() {
    tactical::TacticalSetup setup;
    setup.seed = 7;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    const auto unit = [](const eawr::sim::EntityId id, const tactical::TypeId type, const tactical::PlayerId owner,
                          const eawr::sim::math::Vec3 position) {
        tactical::UnitState state;
        state.entity_id = id;
        state.type_id = type;
        state.owner = owner;
        state.position = position;
        return state;
    };
    setup.units = {unit(stock_tsw_id, stock_tsw_type, 1, at(8000)), unit(2, stock_enemy_type, 2, at(0, 4000, 0))};
    tactical::TswSetup tsw;
    tsw.targets = {{1, 2, 0, at(0), {}}};
    tsw.conflict = tactical::TswConflictSetup{2, true, false}; // player 2 defends the planet
    setup.tsw = tsw;
    tactical::CombatTable table;
    tactical::CombatProfile weapon;
    weapon.type_id = stock_tsw_type;
    weapon.category_bits = 1U;
    weapon.tactical_super_weapon = stock_profile();
    table.profiles = {weapon, tactical::CombatProfile{stock_enemy_type, 2U, std::nullopt, std::nullopt, {}, {}, {}}};
    const std::vector<tactical::SensorProfile> sensors{{stock_tsw_type, units(2000)}, {stock_enemy_type, units(2000)}};
    auto created = tactical::TacticalSession::create(setup, sensors, {}, {}, std::nullopt, table);
    expect(created.has_value(), "TSW plan: the synthetic battle is created");
    if (!created) throw std::runtime_error("synthetic battle");
    return std::move(created).value();
}

// The engine side of the plan: before each script service it rebinds the host's view of the completed world, as the
// production AI manager does, and on the first tick it hands the plan its two engine globals and starts its thread.
class PlanEngine final : public auth::ScriptEngine {
public:
    std::shared_ptr<ai::Host> host;
    std::vector<std::string> equations;
    [[nodiscard]] eawr::core::Result<auth::ServiceOptions> before_service(const tactical::TacticalSession& world,
        const tactical::TacticalTick&, auth::ScriptScheduler& scripts) override {
        host->world = &world;
        host->snapshot = world.snapshot();
        host->view = foc::detail::build_view(world, *host->snapshot);
        if (!started_) {
            started_ = true;
            std::uint64_t order = 0;
            const auto submit = [&](auth::ScriptEvent event) {
                event.key = {scripts.completed_tick() + 1, foc::producer_foc_engine, 1, order++};
                event.target = 1;
                return scripts.submit_event(std::move(event));
            };
            // The engine's own start of a plan: the base definitions, the target globals, then the thread.
            auth::ScriptEvent base;
            base.kind = auth::ScriptEvent::Kind::call;
            base.name = "Base_Definitions";
            auth::ScriptEvent target;
            target.kind = auth::ScriptEvent::Kind::assign;
            target.name = "Target"; // the plan is untargeted: no value
            auth::ScriptEvent player;
            player.kind = auth::ScriptEvent::Kind::assign;
            player.name = "PlayerObject";
            player.parameter = auth::Value{auth::Handle{foc::handle_player, 1}};
            auth::ScriptEvent force;
            force.kind = auth::ScriptEvent::Kind::assign;
            force.name = "MainForce";
            force.parameter = auth::Value{auth::Handle{fixture_taskforce_kind, 1}};
            auth::ScriptEvent thread;
            thread.kind = auth::ScriptEvent::Kind::start_thread;
            thread.name = "MainForce_Thread";
            for (auto* event : {&base, &target, &player, &force, &thread}) {
                if (auto queued = submit(std::move(*event)); !queued)
                    return eawr::core::Result<auth::ServiceOptions>::failure(queued.error());
            }
        }
        return eawr::core::Result<auth::ServiceOptions>::success(auth::ServiceOptions{});
    }

private:
    bool started_{};
};

struct PlanRun {
    std::string final_hash;
    std::vector<std::uint64_t> power_up_ticks, obliterated_ticks, complete_ticks;
    std::size_t tsw_commands{};
    std::size_t script_diagnostics{};
    bool plan_exited{};
    bool planet_destroyed{};
    tactical::TswPhase final_phase{tactical::TswPhase::idle};
    std::uint32_t final_hold{};
    std::vector<std::string> equations;
};

std::optional<PlanRun> run_stock_plan(const std::map<std::string, std::string>& stock, const std::size_t workers) {
    auto host = ai::make_tactical_host();
    foc::AiType type;
    type.type_id = stock_tsw_type;
    type.name = "DEATH_STAR";
    host->setup.content.types.push_back(type);
    host->setup.players = {foc::AiPlayer{1, "EMPIRE", false, true, ""}, foc::AiPlayer{2, "REBEL", false, true, ""}};
    for (const foc::AiType& entry : host->setup.content.types) {
        host->types.emplace(entry.type_id, &entry);
        host->types_by_name.emplace(entry.name, &entry);
    }
    for (const foc::AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
    auto engine = std::make_shared<PlanEngine>();
    engine->host = host;
    auto* asked = &engine->equations;
    host->setup.evaluate_in_galactic_context = [asked](std::string_view name, tactical::PlayerId) -> std::optional<eawr::script::numeric::LuaNumber> {
        asked->emplace_back(name);
        return eawr::script::numeric::LuaNumber(1);
    };
    auth::ModuleManifest modules;
    for (const auto& [path, bytes] : stock)
        expect(modules.add(path, bytes).has_value(), "TSW plan: installed module registers: " + path);
    auth::SessionConfig config;
    config.tick_duration = {1, 30};
    auto scheduler = auth::ScriptScheduler::create(config, std::move(modules));
    if (!scheduler) { expect(false, "TSW plan: scheduler starts"); return std::nullopt; }
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_globals(scheduler.value(), host, errors);
    foc::tactical_ai_detail::register_methods(scheduler.value(), host, errors);
    // The plan's TaskForce: Produce_Force answers no block, so the stock BlockOnCommand returns at once.
    expect(scheduler.value().register_method(fixture_taskforce_kind, "Produce_Force", [](auth::BindingContext&, const auth::ValueList&)
        -> eawr::core::Result<auth::ValueList> { return eawr::core::Result<auth::ValueList>::success({}); }).has_value(),
        "TSW plan: fixture TaskForce registers");
    expect(errors.empty(), "TSW plan: production bindings register");
    expect(scheduler.value().create_instance(1, "FIREDEATHSTAR.LUA").has_value(), "TSW plan: the stock plan instance starts");
    auto scripted = auth::ScriptedTacticalSession::create(make_attack_session(), std::move(scheduler.value()));
    if (!scripted) { expect(false, "TSW plan: the scripted battle is created"); return std::nullopt; }
    expect(scripted.value().register_verb(foc::verb_tactical_superweapon, foc::tactical_ai_detail::translate_order).has_value(),
        "TSW plan: the switch verb registers");
    expect(scripted.value().set_engine(engine).has_value(), "TSW plan: the plan engine installs");
    scripted.value().set_authoritative_hash(false); // the world's own hash is the comparison; saving the Lua heaps every tick adds nothing
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    PlanRun run;
    for (std::uint64_t tick = 0; tick < 4600; ++tick) {
        auto stepped = scripted.value().step(executor);
        if (!stepped) { expect(false, "TSW plan: the scripted tick steps: " + stepped.error().message); return std::nullopt; }
        for (const auto& event : stepped.value().world.snapshot->events()) {
            if (event.kind == tactical::EventKind::tsw_power_up_started) run.power_up_ticks.push_back(tick);
            if (event.kind == tactical::EventKind::planet_obliterated) run.obliterated_ticks.push_back(tick);
            if (event.kind == tactical::EventKind::tsw_sequence_complete) run.complete_ticks.push_back(tick);
        }
        run.script_diagnostics += stepped.value().scripts.diagnostics.size();
        for (const auto& diagnostic : stepped.value().scripts.diagnostics)
            if (run.script_diagnostics <= 5) std::cerr << "TSW plan: tick " << tick << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
        for (const auto& command : stepped.value().scripts.commands)
            if (command.verb == foc::verb_tactical_superweapon) ++run.tsw_commands;
        for (const auto removed : stepped.value().scripts.removed_instances) if (removed == 1) run.plan_exited = true;
        run.final_hash = stepped.value().world.state_sha256;
    }
    const auto record = scripted.value().world().tsw_state(stock_tsw_id);
    run.final_phase = record ? record->phase : tactical::TswPhase::idle;
    run.final_hold = scripted.value().world().tsw_world().victory_hold;
    run.planet_destroyed = scripted.value().world().tsw_planet_destroyed();
    run.equations = engine->equations;
    std::cerr << "TSW plan: workers " << workers << ": diagnostics " << run.script_diagnostics << ", commands " << run.tsw_commands
              << ", power-ups " << run.power_up_ticks.size() << ", hand-offs " << run.obliterated_ticks.size()
              << ", completes " << run.complete_ticks.size() << ", exited " << run.plan_exited
              << ", equation asks " << run.equations.size() << '\n';
    return run;
}

} // namespace

// The stock FireDeathStar plan, unmodified from the installed data, on a synthetic battle with a parent conflict: the
// object flies in by itself (TSW-03), the plan waits for readiness and the galactic equation, fires once, the sequence
// runs (TSW-08..TSW-11), the planet hand-off is published once (TSW-10) and the plan ends (its sleep after the fire).
void installed_stock_fire_death_star() {
    const auto game_root = eawr::testing::game_root().value_or("");
    if (game_root.empty()) { std::cout << "SKIPPED: EAWR_EAW_GAME_ROOT is not set\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [layer, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mounted = eawr::vfs::resolve_manifest_mount(layer, std::filesystem::path(game_root) / folder / "Data");
        expect(mounted.has_value(), "TSW plan: installed data layer resolves");
        if (!mounted) return;
        mounts.push_back(std::move(mounted).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(filesystem.has_value(), "TSW plan: installed data mounts");
    if (!filesystem) return;
    auto libraries = filesystem.value().enumerate("data/scripts/library", ".lua");
    expect(libraries.has_value(), "TSW plan: installed libraries enumerate");
    if (!libraries) return;
    std::map<std::string, std::string> stock;
    const auto load = [&](const std::string& path) {
        const auto bytes = filesystem.value().open(path);
        expect(bytes.has_value(), "TSW plan: installed Lua opens: " + path);
        if (bytes) stock.emplace(std::filesystem::path(path).filename().string(),
            std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
    };
    for (const auto& library : libraries.value()) load(library.canonical_path);
    load("data/scripts/ai/spacemode/firedeathstar.lua");
    if (failures != 0) return;
    std::optional<PlanRun> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const auto run = run_stock_plan(stock, workers);
        if (!run) return;
        const std::string suffix = " (workers " + std::to_string(workers) + ")";
        expect(run->script_diagnostics == 0, "TSW plan: the stock plan runs without a Lua fault" + suffix);
        expect(run->tsw_commands == 1, "TSW plan: the plan fires exactly once" + suffix);
        expect(run->power_up_ticks.size() == 1 && run->obliterated_ticks.size() == 1 && run->complete_ticks.size() == 1,
            "TSW plan: one power-up, one planet hand-off and one completed sequence" + suffix);
        // The Death Star flies in by itself: the fire cannot come before the approach (about 2900 frames) has ended.
        expect(!run->power_up_ticks.empty() && run->power_up_ticks.front() > 2800,
            "TSW plan: the plan fires only after the approach ends" + suffix);
        expect(run->planet_destroyed && run->final_phase == tactical::TswPhase::idle && run->final_hold == 0,
            "TSW plan: the planet is destroyed, the object is idle again and the victory hold is released" + suffix);
        expect(run->plan_exited, "TSW plan: the plan ends after its sleep following the fire" + suffix);
        expect(!run->equations.empty() && run->equations.front() == "Want_To_Fire_DS",
            "TSW plan: the plan asks the parent for Want_To_Fire_DS" + suffix);
        if (!baseline) baseline = run;
        else expect(run->final_hash == baseline->final_hash && run->power_up_ticks == baseline->power_up_ticks,
            "TSW plan: the run is identical on 1, 2, 4 and 8 workers" + suffix);
    }
}

void tactical_superweapon_bindings() {
    find_first_object_cases();
    readiness_cases();
    fire_cases();
    double_fire_cases();
    stock_plan_shape();
}

} // namespace foc_contract
