#include "../../support/temp_dir.hpp"
#include "../../support/env.hpp"
#include "foc_ai_contract_fixtures.hpp"
#include "tactical_ai_world.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "galactic_ai_execution.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace foc_contract {

namespace skirmish = eawr::skirmish;


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
    {
        auto host = std::make_shared<foc::detail::Host>();
        host->setup.xml["data/xml/enum/aigoalcategorytype.xml"] =
            "<AIGoalCategoryType><Offensive>0</Offensive><Defensive>1</Defensive>"
            "<Information>2</Information><Infrastructure>3</Infrastructure>"
            "<MajorItem>11</MajorItem></AIGoalCategoryType>";
        const auto categories = foc::tactical_ai_detail::converters(host);
        for (const auto& [name, value] : std::map<std::string, int>{{"Offensive", 0}, {"Defensive", 1},
                {"Information", 2}, {"Infrastructure", 3}, {"MajorItem", 11}})
            expect(categories("AIGoalCategoryType", name) == std::optional<ai::Real>{ai::real(value)},
                "PE-01: goal categories use the authored enum ordinals");
        expect(!categories("AIGoalCategoryType", "Unknown"), "PE-01: an unknown goal category stays invalid");
        host->setup.xml["data/xml/enum/aigoalcategorytype.xml"] =
            "<EnumDefinition><Offensive>23</Offensive><ModCategory>37</ModCategory></EnumDefinition>";
        const auto mod = foc::tactical_ai_detail::converters(host);
        expect(mod("aigoalcategorytype", " offensive ") == std::optional<ai::Real>{ai::real(23)}
            && mod("AIGoalCategoryType", "ModCategory") == std::optional<ai::Real>{ai::real(37)},
            "PE-01: enum overrides and additional mod names are data driven");
        const auto parsed = ai::parse_equation("budget", "AIGoalCategoryType[Offensive]", mod);
        expect(static_cast<bool>(parsed), "PE-01: a goal-category budget equation compiles without a startup refusal");
    }
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


void game_random_forms() {
    const std::string source = R"(
function Run()
    Record(GameRandom())
    Record(GameRandom(9))
    Record(GameRandom(7, -3))
    Record(GameRandom(-3.9, 7.9))
    Record(GameRandom(6.9, 6.1))
    Record(GameRandom(0))
    Record(GameRandom.Get_Float())
    Record(GameRandom.Get_Float(4))
    Record(GameRandom.Get_Float(4.5, 2.5))
    Record(GameRandom.Get_Float(3.25, 3.25))
    -- The progressive evaluator needs its supplied range, above the unit interval.
    Record(GameRandom.Get_Float(2, 3))
    Record(GameRandom(32766))
    Record(GameRandom(32767))
    Record(GameRandom(-2147483648, 2147483647))
    Record(GameRandom("bad", 5))
    Record(GameRandom.Get_Float(false, 2))
    local cycle = {}; cycle.self = cycle
    Record(GameRandom(cycle, 2))
    Record(GameRandom(function() end, 2))
    Record(GameRandom(nil))
    Record(GameRandom(Find_Player("REBEL"), 2))
    local ok = pcall(function() GameRandom(1, 2, 3) end)
    assert(not ok)
    ok = pcall(function() GameRandom.Get_Float(1, 2, 3) end)
    assert(not ok)
    -- Inferred host behavior: numeric bounds outside int32 convert to INT32_MIN.
    Record(GameRandom(2147483648))
    Record(GameRandom(-2147483649))
    Record(GameRandom(-2147483649, -2147483648))
    Record(GameRandom(2147483648, -2147483649))
    Record(GameRandom(1, 10))
end
)";
    for (const auto seed : {23U, 2392U, 20833U}) {
        std::vector<ai::Real> baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            World world;
            auth::ModuleManifest manifest;
            expect(manifest.add("RANDOM.LUA", source).has_value(), "GR-01: random fixture loads");
            auth::SessionConfig config;
            config.seed = seed;
            auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
            if (!scripts) { expect(false, "GR-01: scheduler starts"); return; }
            std::vector<ai::Real> results;
            std::uint64_t draws = 0;
            expect(scripts.value().register_binding("Record", [&](auth::BindingContext& context, const auth::ValueList& args) {
                results.push_back(std::get<ai::Real>(args.front().data));
                draws = context.random().draws();
                return eawr::core::Result<auth::ValueList>::success({});
            }).has_value(), "GR-01: recorder registers");
            std::vector<eawr::core::Diagnostic> errors;
            foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
            expect(errors.empty() && scripts.value().create_instance(1, "RANDOM.LUA").has_value(),
                "GR-01: production random bindings register");
            auth::ScriptEvent event;
            event.key = {1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Run";
            expect(scripts.value().submit_event(std::move(event)).has_value(), "GR-01: random calls queue");
            eawr::platform::ThreadWorkerAdapter executor(workers);
            const auto report = scripts.value().service(executor);
            expect(report && report.value().diagnostics.size() == 10,
                "L-54: capped ranges and non-numbers diagnose without suppressing the returned draw");
            auth::RandomStream reference(config.seed, 1, 1);
            std::vector<ai::Real> expected;
            const auto integer = [&](std::int64_t low, std::int64_t high) {
                expected.push_back(ai::real(low + static_cast<std::int64_t>(
                    reference.next_below(static_cast<std::uint64_t>(high - low) + 1))));
            };
            const auto floating = [&](ai::Real low, ai::Real high) {
                expected.push_back(low + (high - low) *
                    ai::Real(reference.next_below(32768)) / ai::real(32767));
            };
            integer(0, 65535); integer(0, 9); integer(-3, 7); integer(-3, 7);
            expected.push_back(ai::real(6));
            expected.push_back(ai::real(0));
            floating(ai::real(0), ai::real(1)); floating(ai::real(0), ai::real(4));
            floating(ai::real(5) / ai::real(2), ai::real(9) / ai::real(2));
            expected.push_back(ai::real(13) / ai::real(4));
            floating(ai::real(2), ai::real(3)); integer(0, 32766); integer(0, 32766);
            integer(-2147483648LL, -2147450882LL);
            integer(0, 5); floating(ai::real(0), ai::real(2));
            integer(0, 2); integer(0, 2); integer(0, 0); integer(0, 2);
            integer(-2147483648LL, -2147450882LL);
            integer(-2147483648LL, -2147450882LL);
            expected.push_back(ai::real(-2147483648LL));
            expected.push_back(ai::real(-2147483648LL));
            integer(1, 10);
            expect(results == expected && draws == reference.draws(),
                "GR-01: inclusive/truncated/swapped forms preserve the instance stream and equal bounds draw nothing");
            if (results.size() > 6 && seed != 23)
                expect(results[6] == ai::real(seed == 2392 ? 1 : 0), "L-54: both default float endpoints are attainable");
            if (workers == 1) baseline = results;
            else expect(results == baseline, "GR-01: forms agree across worker counts");
        }
    }
}


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
        unit.visible_to = 3;
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


void forced_object_fog() {
    const auto handle = [](std::uint32_t kind, std::uint64_t id) { return auth::Value{auth::Handle{kind, id}}; };
    for (const bool requester_ai : {false, true}) for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        world.host->setup.players[0].ai = requester_ai;
        world.host->setup.players.push_back(foc::AiPlayer{7, "HUTTS", false, false, ""});
        world.host->setup.players.push_back(foc::AiPlayer{9, "NEUTRAL", true, false, ""});
        world.host->players.clear();
        for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
        // Player IDs intentionally differ from the raw visibility bit indexes.
        world.view->players = {{1, 1, 0}, {2, 2, 1}, {7, 7, 2}, {9, 9, 3}};
        auto container = world.host->setup.content.types.front();
        container.name = "CONTAINER"; container.category_bits = 4; container.property_bits = 8;
        container.locomotor = false; container.space_evaluator = false;
        auto station = container;
        station.type_id = 2; station.name = "STATION"; station.property_bits = 0;
        station.star_base = true; station.space_evaluator = true;
        world.host->setup.content.types = {container, station};
        world.host->setup.content.categories.emplace("STRUCTURE", 4);
        world.host->setup.content.properties.emplace("NOTOPPORTUNITYTARGET", 8);
        world.host->types.clear(); world.host->types_by_name.clear();
        for (const auto& type : world.host->setup.content.types) {
            world.host->types.emplace(type.type_id, &type);
            world.host->types_by_name.emplace(type.name, &type);
        }
        world.view->units.clear();
        const auto add = [&](sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner, int x, int z) {
            ai::ViewUnit unit; unit.id = id; unit.type = type; unit.owner = owner; unit.visible_to = 1;
            unit.position.x = eawr::sim::math::Fixed::from_integer(x).value();
            unit.position.z = eawr::sim::math::Fixed::from_integer(z).value();
            world.view->units.push_back(unit);
        };
        add(10, 1, 1, 0, 0); add(11, 2, 2, 20, 0); add(12, 2, 2, 20, 0);
        add(13, 1, 7, 10, 0); add(14, 2, 9, 1, 0);
        add(15, 2, 2, 1, 0);
        world.view->units.back().planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
        auth::ModuleManifest manifest;
        expect(manifest.add("FOG.LUA", "function Check(source, mask, player, ally)\n"
            "if player then Record(Find_Nearest(source, mask, player, ally))\n"
            "else Record(Find_Nearest(source, mask)) end end\n").has_value(), "WNO-11: nearest fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "WNO-11: nearest scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), world.host, errors);
        expect(scripts.value().register_binding("Record", [](auth::BindingContext& context, const auth::ValueList& args) {
            context.issue_command("nearest", args);
            return eawr::core::Result<auth::ValueList>::success({});
        }).has_value(), "WNO-11: nearest result recorder registers");
        expect(errors.empty() && scripts.value().create_instance(1, "FOG.LUA").has_value(), "WNO-11: nearest instance starts");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto query = [&](const std::string& mask, std::optional<tactical::PlayerId> player, bool ally,
            sim::EntityId expected, const char* message) {
            auth::ScriptEvent event;
            event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {handle(foc::handle_game_object, 10), auth::Value{mask},
                player ? handle(foc::handle_player, *player) : auth::Value{}, auth::Value{ally}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "WNO-11: query submits");
            const auto report = scripts.value().service(executor);
            expect(report && report.value().diagnostics.empty() && report.value().commands.size() == 1,
                "WNO-11: production nearest query runs");
            if (!report || report.value().commands.size() != 1) return;
            const auto& args = report.value().commands.front().arguments;
            const auto* result = args.empty() ? nullptr : std::get_if<auth::Handle>(&args.front().data);
            expect(expected == 0 ? result == nullptr : result != nullptr && result->id == expected, message);
        };
        query("Structure", 1, false, 13, "WNO-11: visible Hutt container beats enemy structures despite property/evaluator");
        world.view->units[3].visible_to = 2;
        query("Structure", 1, false, 11, "WNO-11: Hutt fog uses requester bits and equal-distance retains first enemy");
        query("NotOpportunityTarget", 1, false, 0, "WNO-11: property filter respects raw fog");
        query("CONTAINER", 1, false, 0, "WNO-11: exact type filter respects raw fog");
        query("Structure", std::nullopt, false, 13, "WNO-11: no player filter ignores fog and still skips Neutral");
        query("NotOpportunityTarget", std::nullopt, false, 13, "WNO-11: property query excludes its source");
        world.view->units[1].visible_to = world.view->units[2].visible_to = 2;
        query("Structure", 1, false, 0, "WNO-11: hidden enemy structures excluded for human and AI");
        query("STATION", std::nullopt, false, 11, "WNO-11: no-filter exact type retains first tie");
        world.view->units[3].visible_to = 1;
        query("CONTAINER", 1, false, 13, "WNO-11: visible exact Hutt type admitted");
        query("NotOpportunityTarget", 1, false, 13, "WNO-11: visible Hutt property admitted");
        query("Corvette", 1, false, 0, "WNO-11: category masks stay intact");
        query("Structure", 1, true, 0, "WNO-11: allied mask and source exclusion stay intact");
        world.view->units[1].visible_to = world.view->units[2].visible_to = 1;
        world.view->units[3].position.z = eawr::sim::math::Fixed::from_integer(30).value();
        query("Structure", 1, false, 11, "WNO-11: nearest uses height as well as planar distance");
        world.view->units[3].position.z = {};
        ai::Engine engine(world.host, {}, {});
        ai::Target target; target.object = 11;
        for (unsigned flags = 0; flags != 4; ++flags) {
            auto& type = world.host->setup.content.types[1];
            type.initial_state_visible_under_fow = (flags & 1U) != 0;
            type.last_state_visible_under_fow = (flags & 2U) != 0;
            for (const std::uint64_t visibility : {0U, 1U, 2U}) {
                world.view->units[1].visible_to = visibility;
                expect(ai::EngineContractAccess::matches(engine, 1, {"ENEMY_STRUCTURE"}, target) ==
                    (visibility == 1 || flags != 0), "WNO-13: raw requester fog or either authored flag admits goal");
                expect(!ai::EngineContractAccess::matches(engine, 1, {"ENEMY_UNIT"}, target),
                    "WNO-13: fog exception preserves category classification");
            }
        }
        world.host->setup.content.types[1].initial_state_visible_under_fow = false;
        world.host->setup.content.types[1].last_state_visible_under_fow = false;
        world.host->setup.content.types[1].star_base = false;
        world.host->setup.content.types[1].capture_point = true;
        world.view->units[1].visible_to = 1; world.view->units[1].owner = 9;
        expect(ai::EngineContractAccess::matches(engine, 1, {"ENEMY_STRUCTURE"}, target),
            "WNO-14: visible Neutral merchant remains a nonallied structure goal");
        world.view->units[1].visible_to = 0;
        expect(!ai::EngineContractAccess::matches(engine, 1, {"ENEMY_STRUCTURE"}, target),
            "WNO-13: hidden Neutral merchant without authored exception is rejected");
        target.object = 0;
        expect(ai::EngineContractAccess::matches(engine, 1, {"TACTICAL_LOCATION"}, target),
            "WNO-13: region goals retain their separate admission");
        ai::ThreatGrid grid;
        grid.partition(foc::AiBounds{ai::real(-100), ai::real(100), ai::real(100), ai::real(-100)}, 5, 5, ai::Constants{});
        world.view->units[1].owner = 2;
        for (auto& unit : world.view->units) unit.visible_to = 0;
        grid.service(*world.view, *world.host, 0);
        const auto tracked = grid.objects();
        expect(std::find(tracked.begin(), tracked.end(), 15) == tracked.end(),
            "WSW-01: planetary source stays outside ordinary threat and distance queries");
        ai::PlayerAi player; player.player = 1;
        ai::EngineContractAccess::build_targets(engine, player);
        expect(engine.target_of_object(15) == nullptr,
            "WSW-01: even an evaluator-marked planetary source is not an ordinary goal target");
        auto source = world.view->units.back(); source.id = 16;
        world.view->units.push_back(source);
        ai::EngineContractAccess::update_targets(engine);
        expect(engine.target_of_object(16) == nullptr,
            "WSW-01: a newly observed planetary source stays outside ordinary goal targets");
        const ai::Rect area{ai::real(-100), ai::real(-100), ai::real(200), ai::real(200)};
        const auto force = grid.force(*world.host, *world.view, area, 4, 1, false, Real{}, 0);
        expect(requester_ai ? force > Real{} : force == Real{},
            "SK-45: unforced planning force keeps its independent AI fog preference");
    }
}


// FH-11, AI-08 (#782): the freestore's unit service skips the units a goal has reserved for its
// plan, as well as those a TaskForce holds, until the plan's Produce_Force takes them.
void freestore_unit_service() {
    World world;
    const auto initial = world.view->units.front();
    world.view->units.clear();
    for (eawr::sim::EntityId id = 10; id <= 16; ++id) {
        auto unit = initial;
        unit.id = id;
        world.view->units.push_back(unit);
    }
    ai::Engine engine(world.host, {}, {});
    world.host->engine = &engine;
    auto& player = ai::EngineContractAccess::collect_fixture(engine); // 14 held; 15 and 16 reserved
    const auto& units = world.view->units;
    expect(engine.unit_service(units[0], 2), "FH-11: a free unit gets the unit service");
    expect(engine.in_freestore(units[5], 2) && !engine.unit_service(units[5], 2),
        "FH-11/AI-08: a unit a goal has reserved stays in the freestore but gets no unit service");
    expect(!engine.unit_service(units[6], 2), "FH-11/AI-08: a unit the running plan's goal reserved gets none");
    expect(!engine.unit_service(units[4], 2), "FH-11: a unit a TaskForce holds gets none");
    expect(!engine.unit_service(units[0], 1), "FH-13: another player's unit gets none");
    // WTA-02 (#783): the service tests run with the player's execution service.
    player.next_execution = 33;
    expect(!engine.execution_due(2, 31) && !engine.execution_due(2, 32) && engine.execution_due(2, 33),
        "WTA-02: the freestore's service tests wait for the player's execution service");
    expect(engine.execution_due(1, 31), "WTA-02: a player the goal system does not run is tested every frame");
}


// WTA-30, WTA-32, DT-04 (#733): Get_Rate_Of_Damage_Taken and Get_Time_Till_Dead answer from the
// unit's damage tracking. An AI Acclamator taking 300 hull and shield a second with 5,000 left over
// its destroyable hardpoints plus its shield answers about 16.7 s, below Default_Unit_Damaged's 20.
void time_till_dead_bindings() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto units = [](std::int64_t n) { return math::Fixed::from_integer(n).value(); };
    tactical::DurabilityProfile profile;
    profile.max_hull = units(9000);
    profile.hardpoints.resize(4);
    profile.hardpoints[0].destroyable = profile.hardpoints[1].destroyable = profile.hardpoints[2].destroyable = true;
    tactical::DurabilityState state;
    state.hull = units(9000);
    state.hardpoints = {units(2000), units(1500), {}, {}}; // the third is destroyed; the fourth is not destroyable
    state.shields = units(1500);
    expect(!tactical::time_to_death(profile, state), "WCC-15: no time to death at a zero rate");
    state.damage_rate = units(300);
    const auto time = tactical::time_to_death(profile, state);
    expect(time && *time == math::divide(units(5000), units(300)).value(),
        "WCC-15: destroyable hardpoints plus shield, over the rate; the hull does not count");
    auto hull_only = profile;
    hull_only.hardpoints.clear();
    auto hull_state = state;
    hull_state.hardpoints.clear();
    hull_state.hull = units(600);
    hull_state.shields = {};
    const auto hull_time = tactical::time_to_death(hull_only, hull_state);
    expect(hull_time && *hull_time == units(2), "WCC-15: without destroyable hardpoints the hull counts");

    World world;
    auto& tracked = world.view->units.front();
    tracked.damage_rate = state.damage_rate;
    tracked.time_till_dead = time;
    auto untracked = tracked;
    untracked.id = 11;
    untracked.damage_rate = {};
    untracked.time_till_dead.reset();
    world.view->units.push_back(untracked);
    auth::ModuleManifest manifest;
    expect(manifest.add("DAMAGE.LUA", "function Check(a, b)\n"
        "  tracked_time = a.Get_Time_Till_Dead(); tracked_rate = a.Get_Rate_Of_Damage_Taken()\n"
        "  idle_time = b.Get_Time_Till_Dead(); idle_rate = b.Get_Rate_Of_Damage_Taken()\n"
        "  flees = tracked_time < 20\n"
        "end\n").has_value(), "DT-04: binding fixture loads");
    auth::SessionConfig config;
    config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
    if (!scripts) { expect(false, "DT-04: scheduler starts"); return; }
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
    expect(errors.empty() && scripts.value().create_instance(1, "DAMAGE.LUA").has_value(), "DT-04: bindings register");
    auth::ScriptEvent event;
    event.key = {1, auth::first_simulation_producer, 1, 0};
    event.target = 1;
    event.kind = auth::ScriptEvent::Kind::call;
    event.name = "Check";
    event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 10}}, auth::Value{auth::Handle{foc::handle_game_object, 11}}};
    expect(scripts.value().submit_event(std::move(event)).has_value(), "DT-04: call queues");
    const eawr::platform::ThreadWorkerAdapter executor(1);
    auto report = scripts.value().service(executor);
    expect(report && report.value().diagnostics.empty(), "DT-04: the queries run");
    const auto number = [&](std::string_view name) -> std::optional<double> {
        const auto value = scripts.value().read_global(1, name);
        if (!value || !value.value()) return std::nullopt;
        const auto* found = std::get_if<Real>(&value.value()->data);
        if (found == nullptr) return std::nullopt;
        return std::bit_cast<double>(found->repr);
    };
    const auto tracked_time = number("tracked_time");
    expect(tracked_time && *tracked_time > 16.6 && *tracked_time < 16.7, "WTA-32: Get_Time_Till_Dead answers about 16.7 s");
    expect(number("tracked_rate") == 300.0, "WTA-32: Get_Rate_Of_Damage_Taken answers the tracked rate");
    expect(number("idle_time") == 1e18 && number("idle_rate") == 0.0, "WTA-32: an untracked or undamaged unit answers 10^18 and 0");
    const auto flees = scripts.value().read_global(1, "flees");
    expect(flees && flees.value() && std::get_if<bool>(&flees.value()->data) != nullptr
        && std::get<bool>(flees.value()->data), "WTA-36: the time test of Default_Unit_Damaged fires");
}


// WTA-36: the damage response's type queries answer the type's authored minimum and maximum
// attack distances and whether it is a hero.
void type_range_and_hero_bindings() {
    for (const bool hero : {false, true}) {
        World world;
        auto& type = world.host->setup.content.types.front();
        type.hero = hero;
        type.min_attack_distance = ai::real(hero ? 150 : 0);
        auth::ModuleManifest manifest;
        expect(manifest.add("TYPE.LUA", "function Check(a)\n"
            "  local t = a.Get_Type()\n"
            "  hero = t.Is_Hero(); min_range = t.Get_Min_Range(); max_range = t.Get_Max_Range()\n"
            "end\n").has_value(), "WTA-36: type fixture loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "WTA-36: scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "TYPE.LUA").has_value(), "WTA-36: bindings register");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call;
        event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 10}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "WTA-36: call queues");
        const eawr::platform::ThreadWorkerAdapter executor(1);
        auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.empty(), "WTA-36: the type queries run");
        const auto number = [&](std::string_view name) -> std::optional<double> {
            const auto value = scripts.value().read_global(1, name);
            if (!value || !value.value()) return std::nullopt;
            const auto* found = std::get_if<Real>(&value.value()->data);
            if (found == nullptr) return std::nullopt;
            return std::bit_cast<double>(found->repr);
        };
        expect(number("min_range") == (hero ? 150.0 : 0.0), "WTA-36: Get_Min_Range answers Targeting_Min_Attack_Distance");
        expect(number("max_range") == 500.0, "WTA-36: Get_Max_Range answers Targeting_Max_Attack_Distance");
        const auto answer = scripts.value().read_global(1, "hero");
        expect(answer && answer.value() && std::get_if<bool>(&answer.value()->data) != nullptr
            && std::get<bool>(answer.value()->data) == hero, "WTA-36: Is_Hero answers the type's hero flag");
    }
}


void collect_free_categories() {
    struct Case { std::string arguments; std::vector<eawr::sim::EntityId> members; bool valid; };
    const std::array<Case, 6> cases{{
        {"'Fighter | Corvette'", {10, 12, 16, 19}, true},
        {"", {10, 11, 12, 16, 19}, true},
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
        auto source = initial; source.id = 19;
        source.planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
        world.view->units.push_back(source);
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
                "EX-12/WSW-47: collection matches free objects independently of firing enablement and never duplicates members");
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


void special_weapon_bindings() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
      for (const int state : {0, 1, 2, 3, 4, 5}) {
        World world;
        auto& type = world.host->setup.content.types.front();
        type.space_special_weapon = true;
        type.locomotor = false;
        auto& source = world.view->units.front();
        source.planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
        source.planetary_weapon->valid_targets = state == 3 ? 1 : 2;
        source.planetary_weapon->readiness = math::Fixed::from_raw(state == 2 ? math::Fixed::scale - 1 : math::Fixed::scale);
        auto enemy = source;
        enemy.id = 11; enemy.owner = state == 4 ? 2 : 1;
        enemy.planetary_weapon.reset(); enemy.visible_to = 0;
        enemy.position.x = math::Fixed::from_integer(100000).value();
        world.view->units.push_back(enemy);
        tactical::EconomyView account; account.player = 2; account.special_weapons_enabled = state != 1;
        world.host->snapshot = std::make_shared<tactical::TacticalSnapshot>(0,
            std::vector<tactical::SnapshotPlayer>{}, std::vector<tactical::TacticalInstance>{},
            std::vector<tactical::Event>{}, std::vector<tactical::CombatEvent>{}, std::vector<tactical::Projectile>{},
            std::nullopt, std::vector<tactical::SpinningCraft>{}, std::vector<tactical::SquadronTarget>{},
            std::vector<tactical::Squadron>{}, std::vector<tactical::EconomyView>{account});
        ai::Engine engine(world.host, {}, {});
        world.host->engine = &engine;
        ai::EngineContractAccess::order_fixture(engine, {10});
        auth::ModuleManifest manifest;
        expect(manifest.add("SPECIAL.LUA", "function Check(tf, object, target, player)\n"
            "tf_result = tf.Fire_Special_Weapon('SHIP', target)\n"
            "object_result = object.Fire_Special_Weapon(target, player)\nend\n").has_value(), "WSW: binding fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "WSW: scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "SPECIAL.LUA").has_value(), "WSW: bindings register");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0}; event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{ai::handle_taskforce, 1}},
            auth::Value{auth::Handle{foc::handle_game_object, 10}},
            auth::Value{auth::Handle{state == 5 ? foc::handle_game_object : ai::handle_ai_target, state == 5 ? 11U : 1U}},
            auth::Value{auth::Handle{foc::handle_player, 2}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "WSW: calls queue");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.empty(), "WSW: both routes service cleanly");
        if (!report) return;
        const bool accepted = state == 0 || state == 5;
        expect(report.value().commands.size() == (accepted ? 2U : 0U), "WSW-14/16: full readiness, permission, category and relationship gates");
        for (const auto& command : report.value().commands) {
            auto order = foc::tactical_ai_detail::translate_order(command);
            expect(order && std::holds_alternative<tactical::SpecialFirePayload>(order.value().payload), "WSW: staged request translates");
            if (!order) continue;
            const auto& payload = std::get<tactical::SpecialFirePayload>(order.value().payload);
            expect(payload.target == 11 && order.value().units == std::vector<eawr::sim::EntityId>{10}, "WSW: source and resolved target retained");
            expect(payload.admission == (command.sequence == 0 ? tactical::SpecialFireAdmission::ai_taskforce : tactical::SpecialFireAdmission::ai_object),
                "WSW: distinct replay admission retained");
        }
        const auto tf_result = scripts.value().read_global(1, "tf_result");
        const auto object_result = scripts.value().read_global(1, "object_result");
        expect(tf_result && tf_result.value() && (accepted ? std::holds_alternative<auth::Handle>(tf_result.value()->data)
            : std::holds_alternative<bool>(tf_result.value()->data) && !std::get<bool>(tf_result.value()->data)), "WSW-41: handle/false contract");
        expect(object_result && (accepted ? object_result.value().has_value() && std::holds_alternative<auth::Handle>(object_result.value()->data)
            : object_result.value().has_value() && std::holds_alternative<std::monostate>(object_result.value()->data)), "WSW-40: handle/nil contract");
        expect(world.view->units.front().planetary_weapon->state == tactical::SpecialWeaponState{}, "WSW: bindings leave frozen state unchanged");
      }
    }
}


void manual_special_weapon_binding() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    for (const int state : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        World world;
        world.host->setup.content.types.front().star_base = true;
        auto enemy = world.view->units.front(); enemy.id = 11; enemy.owner = 1;
        world.view->units.push_back(enemy);
        tactical::TacticalInstance source, target;
        source.entity_id = 10; source.type_id = 1; source.owner = 2; source.team = 2;
        target.entity_id = 11; target.type_id = 1; target.owner = state == 4 ? 2 : 1; target.team = target.owner;
        source.fixed_transform = math::to_matrix(math::identity_quat(), {}).value();
        target.fixed_transform = math::to_matrix(math::identity_quat(), {math::Fixed::from_integer(state == 6 ? 2000 : 500).value(), {}, {}}).value();
        target.category_bits = 2; target.visible_to = state == 5 ? 0 : 3;
        auto profile = std::make_shared<tactical::WeaponProfile>();
        profile->hardpoint = 0; profile->requires_manual_target = true; profile->range = math::Fixed::from_integer(1000).value();
        profile->category_restrictions = state == 7 ? 2 : 0; profile->shot.emplace();
        tactical::ManualWeaponView gun;
        gun.available = state != 3; gun.countdown = state == 1 ? 1U : 0U; gun.target = state == 2 ? 11U : 0U;
        gun.admission_profile = profile;
        if (state != 8) source.manual_weapons.push_back(gun);
        tactical::EconomyView account; account.player = 2; account.special_weapons_enabled = true;
        tactical::ManualPlayerClock clock; clock.last_fired_frame = 599; clock.cooldown_frames = 3600;
        world.host->snapshot = std::make_shared<tactical::TacticalSnapshot>(600,
            std::vector<tactical::SnapshotPlayer>{{1, 1, false}, {2, 2, false}}, std::vector<tactical::TacticalInstance>{source, target},
            std::vector<tactical::Event>{}, std::vector<tactical::CombatEvent>{}, std::vector<tactical::Projectile>{},
            std::nullopt, std::vector<tactical::SpinningCraft>{}, std::vector<tactical::SquadronTarget>{},
            std::vector<tactical::Squadron>{}, std::vector<tactical::EconomyView>{account}, std::vector<tactical::PadView>{},
            std::vector<tactical::PlayerQuit>{}, nullptr, nullptr, std::vector<std::pair<tactical::PlayerId, tactical::ManualPlayerClock>>{{2, clock}});
        auth::ModuleManifest manifest;
        expect(manifest.add("MANUAL.LUA", "function Check(object, target, player) result = object.Fire_Special_Weapon(target, player) end\n").has_value(), "WSW-40: station fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) return;
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "MANUAL.LUA").has_value(), "WSW-40: object binding registers");
        auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, 1, 0}; event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 10}}, auth::Value{auth::Handle{foc::handle_game_object, 11}},
            auth::Value{auth::Handle{foc::handle_player, 2}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "WSW-40: station call queues");
        const eawr::sim::InlineExecutor executor;
        const auto report = scripts.value().service(executor);
        expect(report && (state == 8 ? report.value().diagnostics.size() == 1 : report.value().diagnostics.empty())
            && report.value().commands.size() == (state == 0 ? 1U : 0U),
            "WSW-17/18/40: station uses frozen gun/target gates while human clock recharges");
        if (state == 8 && report) expect(report.value().diagnostics.front().message == "Station has no manual special gun",
            "WSW-40: missing gun warns and returns nil");
        const auto result = scripts.value().read_global(1, "result");
        expect(result && (state == 0 ? result.value().has_value() && std::holds_alternative<auth::Handle>(result.value()->data)
            : result.value().has_value() && std::holds_alternative<std::monostate>(result.value()->data)), "WSW-40: station returns its handle only for an admitted assignment");
    }
}


void special_weapon_attachment() {
    namespace tactical = eawr::sim::tactical;
    World world;
    auto& type = world.host->setup.content.types.front();
    type.locomotor = false; type.space_special_weapon = true;
    world.view->units.front().planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
    world.view->units.front().planetary_weapon->readiness = eawr::sim::math::Fixed::from_raw(eawr::sim::math::Fixed::scale);
    const auto plan_globals = [](bool deny) {
        std::vector<auth::Value> taskforce{auth::Value::text("MainForce")};
        if (deny) taskforce.push_back(auth::Value::text("DenySpecialWeaponAttach"));
        taskforce.push_back(auth::Value::text("SHIP = 1"));
        auth::ValueList globals{auth::Value::text("GROUND_TO_SPACE_DAMAGE"),
            auth::Value{std::vector<auth::Value>{auth::Value{std::move(taskforce)}}}};
        globals.resize(10);
        globals[2] = auth::Value{true};
        return globals;
    };
    const auto publish = [&](bool enabled, std::vector<tactical::Event> events = {}) {
        tactical::EconomyView account; account.player = 2; account.special_weapons_enabled = enabled;
        world.host->snapshot = std::make_shared<tactical::TacticalSnapshot>(1,
            std::vector<tactical::SnapshotPlayer>{}, std::vector<tactical::TacticalInstance>{}, std::move(events),
            std::vector<tactical::CombatEvent>{}, std::vector<tactical::Projectile>{}, std::nullopt,
            std::vector<tactical::SpinningCraft>{}, std::vector<tactical::SquadronTarget>{},
            std::vector<tactical::Squadron>{}, std::vector<tactical::EconomyView>{account});
    };
    // HA-01/WSW-47: DenySpecialWeaponAttach clears the plan's extra-attach permission, but a source
    // its team lists stays admitted (the stock planetary plans deny and list their source); the
    // match's superweapon admission is the gate.
    for (const auto& [deny, admitted] : {std::pair{true, true}, std::pair{false, true}, std::pair{false, false}}) {
        world.host->setup.allow_special_weapons = admitted;
        std::vector<std::string> notes;
        const auto definition = ai::build_plan(*world.host, "ground", "ground.lua", plan_globals(deny), notes);
        expect(definition && definition.value().allow_special_weapons == !deny, "HA-01: DenySpecialWeaponAttach clears the extra attach");
        const bool allow = admitted;
        if (!definition) return;
        ai::Engine engine(world.host, {}, {definition.value()});
        ai::PlayerAi player; player.player = 2;
        ai::PotentialPlan potential;
        publish(false);
        expect(!ai::EngineContractAccess::selection(engine, player, potential, 7),
            "WSW-47: disabled source stays unavailable to proposal selection");
        publish(true);
        potential = {};
        expect(ai::EngineContractAccess::selection(engine, player, potential, 7) == allow,
            "WSW-34/47: unlock makes the nonmoving source eligible for the first shot in a permitting plan");
        if (allow) expect(potential.freestore == std::vector<eawr::sim::EntityId>{10} && potential.sources == std::vector<std::uint8_t>{0},
            "WSW-47: attachment keeps the live source identity without purchase");
        world.view->units.front().planetary_weapon->readiness = eawr::sim::math::Fixed::from_raw(eawr::sim::math::Fixed::scale - 1);
        potential = {};
        expect(!ai::EngineContractAccess::selection(engine, player, potential, 7), "WSW-47: charging source cannot enter a new proposal");
        world.view->units.clear();
        potential = {};
        expect(!ai::EngineContractAccess::selection(engine, player, potential, 7), "WSW-47: absent source is never manufactured");
        ai::ViewUnit source; source.id = 10; source.type = 1; source.owner = 2;
        source.planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
        source.planetary_weapon->readiness = eawr::sim::math::Fixed::from_raw(eawr::sim::math::Fixed::scale);
        world.view->units.push_back(source);
    }
    world.host->setup.allow_special_weapons = true;
    auth::ModuleManifest manifest;
    expect(manifest.add("ONLINE.LUA", "function Default_Special_Weapon_Online(tf, object) online = object end\n").has_value(), "WSW-37: online handler loads");
    auth::SessionConfig config; config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
    if (!scripts) return;
    expect(scripts.value().create_instance(1, "ONLINE.LUA").has_value(), "WSW-37: online instance starts");
    const eawr::sim::InlineExecutor executor;
    expect(scripts.value().service(executor).has_value(), "WSW-37: online handler definition executes");
    ai::Engine engine(world.host, {}, {ai::PlanDef{}});
    ai::EngineContractAccess::event_force(engine, scripts.value(), {10});
    publish(false);
    expect(ai::EngineContractAccess::run_events(engine).empty() && engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{10},
        "WSW-34/37: disabled recharge remains attached without a fabricated online signal");
    tactical::Event event;
    event.kind = tactical::EventKind::special_weapon_ready; event.unit = 10;
    publish(true, {event});
    const auto events = ai::EngineContractAccess::run_events(engine);
    expect(events.size() == 1 && events.front().name == "Default_Special_Weapon_Online"
        && engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{10},
        "WSW-34/37: re-enable online event reaches the retained source's plan");
}


void plan_definition_paths() {
    World world;
    const std::string path = "Data/Scripts/AI/ai_plan_expansiongeneric_generatemagiccashdrop.lua";
    const std::map<std::string, std::string> modules{{path, R"LUA(
function Base_Definitions()
    Category = "Skirmish_Generate_Magic_Cash_Drop_Space"
    TaskForce = { { "CashForce", "MinimumTotalSize = 0" } }
    MagicPlan = true
    IgnoreTarget = true
    AllowFreeStoreUnits = false
end
)LUA"}};
    std::vector<std::string> notes;
    const auto plans = foc::tactical_ai_detail::load_plans(world.host, modules, notes);
    expect(plans && plans.value().size() == 1,
        "L-03a / PL-10: an admitted plan outside SpaceMode loads from its logical path");
    if (!plans || plans.value().size() != 1) return;
    const auto& plan = plans.value().front();
    expect(plan.module == path && plan.magic && plan.ignore_target && !plan.allow_free_store
        && plan.goals == std::vector<std::string>{"SKIRMISH_GENERATE_MAGIC_CASH_DROP_SPACE"},
        "PL-10: root-level cash definition retains its category and flags");
    expect(std::none_of(notes.begin(), notes.end(), [](const auto& note) {
        return note.starts_with("ai_plan_expansiongeneric_generatemagiccashdrop:");
    }), "L-03a: the cash plan has no module-resolution diagnostic");
    const std::set<std::string> unrelated{"ANOTHER_SPACE_GOAL"};
    notes.clear();
    const auto filtered = foc::tactical_ai_detail::load_plans(world.host, modules, notes, &unrelated);
    expect(filtered && filtered.value().empty() && notes.size() == 1
        && notes.front() == "AI selected space plan definitions loaded=0, outside selection=1",
        "AI-16: unselected definition categories are counted before force validation");
}


void mounted_ai_discovery() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("eawr-ai-discovery-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{root};
    const auto write = [&](const std::string& layer, const std::string& path, const std::string& text) {
        const auto file = root / layer / path;
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary); out << text;
        if (!out) throw std::runtime_error("cannot write AI discovery fixture");
    };
    write("parent", "xml/ai/players/controller.xml", "<AIPlayerType><Name>Parent</Name></AIPlayerType>");
    write("leaf", "xml/ai/players/controller.xml", R"XML(<AIPlayerType><Name>Custom</Name>
        <SpaceFreeStoreScript>CustomFree</SpaceFreeStoreScript><GoalSetExtensionSize>7</GoalSetExtensionSize>
        <GoalProposalFunctionSets>CustomSet</GoalProposalFunctionSets>
        <Templates><Space>CustomTemplate</Space></Templates></AIPlayerType>)XML");
    write("parent", "xml/ai/players/another.xml", R"XML(<AIPlayerType><Name>Other</Name>
        <SpaceFreeStoreScript>OtherFree</SpaceFreeStoreScript></AIPlayerType>)XML");
    write("leaf", "xml/ai/templates/custom.xml", "<AITemplates><CustomTemplate><Budget><Offensive>Score</Offensive></Budget></CustomTemplate></AITemplates>");
    write("leaf", "xml/ai/goalfunctions/customset.xml", "<GoalFunctions><Entry><Goal>CustomGoal</Goal><Function>Score</Function></Entry></GoalFunctions>");
    write("leaf", "xml/ai/goals/custom.xml", "<Goals><CustomGoal><GameMode>Space</GameMode><Category>Offensive</Category></CustomGoal></Goals>");
    write("leaf", "xml/ai/perceptualequations/custom.xml", "<Equations><Score>1</Score></Equations>");
    write("leaf", "xml/difficultyadjustments.xml", "<Root/>");
    write("leaf", "xml/gameconstants.xml", "<Root/>");
    write("leaf", "xml/enum/aigoalcategorytype.xml", "<EnumDefinition><Offensive>1</Offensive></EnumDefinition>");
    for (const auto name : {"PGCommands", "PGBaseDefinitions", "PGBase", "PGDebug", "PGEvents", "PGTaskForce"})
        write("leaf", std::string("scripts/library/") + name + ".lua", "");
    write("leaf", "scripts/library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes = {} FriendlyContrastTypes = {} end");
    write("leaf", "scripts/freestore/CustomFree.lua", "require('LeafHelper')\nrequire('Build-Evaluation/Nested')\nselected = leaf_value + parent_value\n");
    write("parent", "scripts/freestore/OtherFree.lua", "selected = 99\n");
    write("parent", "scripts/library/LeafHelper.lua", "error('shadowed module must not execute')");
    write("leaf", "scripts/library/LeafHelper.lua", "require('ParentHelper') leaf_value = 7");
    write("parent", "scripts/library/ParentHelper.lua", "parent_value = 11");
    write("leaf", "scripts/ai/spacemode/Build-Evaluation/Nested.lua", "nested_value = 19");
    write("leaf", "scripts/ai/spacemode/customplan.lua", R"LUA(function Base_Definitions()
        Category = 'CustomGoal'; TaskForce = {{'CustomForce', 'MinimumTotalSize = 0'}}
        MagicPlan = true
    end)LUA");
    const std::array mounts{eawr::vfs::MountSpec{"leaf", root / "leaf", "data", {}}, eawr::vfs::MountSpec{"parent", root / "parent", "data", {}}};
    auto files = eawr::vfs::Vfs::mount(mounts);
    expect(files.has_value(), "AI-16: layered AI fixture mounts");
    if (!files) return;
    foc::AiSetup setup;
    setup.players = {{1, "FIRST", false, true, "Custom"}, {2, "SECOND", false, true, "Other"}};
    expect(eawr::skirmish::enable_goal_system(files.value(), setup).has_value(), "AI-16: mounted AI directories load");
    const auto data = ai::load_ai_data(setup.xml, {});
    expect(data && data.value().players.contains("CUSTOM") && data.value().players.contains("OTHER")
        && !data.value().players.contains("PARENT") && data.value().templates.contains("CUSTOMTEMPLATE")
        && data.value().function_sets.contains("CUSTOMSET") && data.value().goals.contains("CUSTOMGOAL")
        && data.value().equations.find("Score"), "AI-16: VFS winner and every authored AI category enter the parser");
    expect(data && data.value().players.at("CUSTOM").goal_set_extension == 7
        && data.value().players.at("OTHER").goal_set_extension == 2,
        "GS-30: authored goal extension overrides the omitted-tag engine default");
    expect(foc::freestore_module(setup, setup.players[0]) == "Data/Scripts/FreeStore/CustomFree.lua"
        && foc::freestore_module(setup, setup.players[1]) == "Data/Scripts/FreeStore/OtherFree.lua",
        "AI-16: each controller selects its own authored freestore");
    auto modules = eawr::skirmish::ai_modules(files.value(), setup, true);
    expect(modules.has_value(), "ML-34: selected roots and mounted dependencies enter the bounded manifest");
    if (!modules) return;
    const auto plans = foc::selected_plans(modules.value());
    expect(plans == std::vector<std::string>{"data/scripts/ai/spacemode/customplan.lua"},
        "PL-02: a new plan is discovered while a nested helper stays a dependency");
    auto selected = eawr::skirmish::select_ai_modules(modules.value(), setup, true);
    expect(selected && selected.value() == modules.value(), "ML-34: cached sessions preserve the frozen dependency inventory");
    auto aliases = modules.value();
    aliases["DATA/SCRIPTS/LIBRARY/LEAFHELPER.LUA"] = aliases.at("data/scripts/library/leafhelper.lua");
    const auto same_bytes = eawr::skirmish::select_ai_modules(aliases, setup, true);
    expect(same_bytes && same_bytes.value().size() == modules.value().size(),
        "ML-34: cached optional-root additions collapse identical logical aliases");
    aliases["DATA/SCRIPTS/LIBRARY/LEAFHELPER.LUA"] = "error('conflicting bytes')";
    expect(!eawr::skirmish::select_ai_modules(aliases, setup, true),
        "ML-34: conflicting cached aliases are rejected instead of overriding the mounted winner");
    auto prepared = foc::prepare_session(setup, modules.value());
    expect(prepared.has_value(), "ML-34: new library and space-directory dependencies execute through production setup");
    if (prepared) {
        const auto first = prepared.value().scripts.read_global(foc::freestore_instance(1), "selected");
        const auto second = prepared.value().scripts.read_global(foc::freestore_instance(2), "selected");
        const auto nested = prepared.value().scripts.read_global(foc::freestore_instance(1), "nested_value");
        expect(first && first.value() && std::get<Real>(first.value()->data) == ai::real(18)
            && second && second.value() && std::get<Real>(second.value()->data) == ai::real(99)
            && nested && nested.value() && std::get<Real>(nested.value()->data) == ai::real(19),
            "AI-16 / ML-34: isolated freestores use leaf, parent and space-directory modules");
    }
    write("leaf", "scripts/library/ParentHelper.lua", "this is invalid lua !");
    auto invalid = eawr::vfs::Vfs::mount(mounts);
    auto invalid_modules = invalid ? eawr::skirmish::ai_modules(invalid.value(), setup) : decltype(modules)::failure(invalid.error());
    expect(invalid_modules && !foc::prepare_session(setup, invalid_modules.value()),
        "L-03a: a syntax error in the VFS winner cannot fall through to a valid parent");
}


// #3780: the closure is selected by the battle domain from the mounted data; space stays exactly as before.
void mode_selected_closure() {
    using eawr::sim::tactical::BattleDomain;
    const std::map<std::string, std::string> modules{
        {"data/scripts/ai/spacemode/spaceplan.lua", ""}, {"data/scripts/ai/spacemode/Nested/helper.lua", ""},
        {"data/scripts/ai/landmode/landplan.lua", ""}, {"data/scripts/ai/landmode/Nested/helper.lua", ""},
        {"data/scripts/ai/storyarc/story.lua", ""}, {"data/scripts/ai/rootplan.lua", ""},
        {"data/scripts/library/pgcommands.lua", ""}};
    const std::vector<std::string> space_paths{"data/scripts/ai/rootplan.lua", "data/scripts/ai/spacemode/spaceplan.lua",
        "data/scripts/ai/storyarc/story.lua"};
    const std::vector<std::string> land_paths{"data/scripts/ai/landmode/landplan.lua", "data/scripts/ai/rootplan.lua",
        "data/scripts/ai/storyarc/story.lua"};
    auto sorted = [](std::vector<std::string> paths) { std::sort(paths.begin(), paths.end()); return paths; };
    expect(foc::selected_plans(modules) == foc::selected_plans(modules, BattleDomain::space)
        && sorted(foc::selected_plans(modules, BattleDomain::space)) == space_paths,
        "#3780: space plan selection reads the space directory, root plans and story arcs");
    expect(sorted(foc::selected_plans(modules, BattleDomain::land)) == land_paths,
        "#3780: land plan selection reads the land directory, root plans and story arcs");
    const auto space_xml = foc::required_xml();
    expect(space_xml == foc::required_xml(BattleDomain::space) && space_xml.size() == 18
        && space_xml.front() == "data/xml/ai/players/basicempireplayer.xml"
        && space_xml[4] == "data/xml/ai/goalfunctions/basicoffensivespaceset.xml"
        && space_xml.back() == "data/xml/enum/aigoalcategorytype.xml",
        "#3780: the space XML closure keeps today's paths and order");
    expect(foc::required_xml(BattleDomain::land).size() < space_xml.size(),
        "#3780: land does not list space goal sets");

    // A land closure loads from mode data: Templates/Land, land difficulty and constant tags, land plans.
    std::map<std::string, std::string, std::less<>> xml;
    xml["data/xml/ai/players/lander.xml"] = R"XML(<AIPlayerType><Name>Lander</Name>
        <LandFreeStoreScript>LandFree</LandFreeStoreScript><SpaceFreeStoreScript>SpaceFree</SpaceFreeStoreScript>
        <Templates><Space>SpaceT</Space><Land>LandT, LandU</Land></Templates>
        <Difficulty_Adjustments><HARD>Hard</HARD></Difficulty_Adjustments></AIPlayerType>)XML";
    xml["data/xml/difficultyadjustments.xml"] = R"XML(<Root><Adj Name="Hard"><Space_AI_Contrast_Multiplier>2</Space_AI_Contrast_Multiplier>
        <Land_AI_Contrast_Multiplier>3</Land_AI_Contrast_Multiplier><Land_AI_Goal_Cycle_Sleep_Duration>15</Land_AI_Goal_Cycle_Sleep_Duration>
        <Land_Build_Time_Multiplier>0.5</Land_Build_Time_Multiplier></Adj></Root>)XML";
    xml["data/xml/gameconstants.xml"] = "<Root><AI_LandEvaluatorRegionSize>640</AI_LandEvaluatorRegionSize></Root>";
    const auto data = ai::load_ai_data(xml, {});
    expect(data.has_value(), "#3780: a land closure's data loads");
    if (!data) return;
    const auto& player = data.value().players.at("LANDER");
    expect(player.land_templates == std::vector<std::string>{"LANDT", "LANDU"}
        && player.space_templates == std::vector<std::string>{"SPACET"}
        && ai::mode_templates(player, BattleDomain::land) == player.land_templates
        && ai::mode_templates(player, BattleDomain::space) == player.space_templates,
        "#3780: templates are read and selected by mode");
    const auto level = ai::resolve_difficulty(data.value(), &player, "hard");
    expect(level.space_contrast_multiplier == ai::real(2) && level.land_contrast_multiplier == ai::real(3)
        && level.land_goal_cycle_sleep == ai::real(15) && level.land_build_time_multiplier == ai::parse_real("0.5").value()
        && data.value().constants.land_region_size == ai::real(640),
        "#3780: land difficulty and constants are read beside the space ones");
    foc::AiSetup land_setup;
    land_setup.domain = BattleDomain::land;
    land_setup.players = {{1, "FIRST", false, true, "Lander"}};
    land_setup.xml = xml;
    foc::AiSetup space_setup = land_setup;
    space_setup.domain = BattleDomain::space;
    expect(foc::freestore_module(land_setup, land_setup.players[0]) == "Data/Scripts/FreeStore/LandFree.lua"
        && foc::freestore_module(space_setup, space_setup.players[0]) == "Data/Scripts/FreeStore/SpaceFree.lua",
        "#3780: the freestore lookup follows the mode tag");

    // Plan definitions load from the land directory under a land host, and not under a space host.
    const std::string plan_path = "Data/Scripts/AI/LandMode/landplan.lua";
    const std::map<std::string, std::string> plan_modules{{plan_path, R"LUA(
function Base_Definitions()
    Category = "Land_Goal"
    TaskForce = { { "LandForce", "MinimumTotalSize = 0" } }
    MagicPlan = true
    IgnoreTarget = true
end
)LUA"}};
    World land;
    land.host->setup.domain = BattleDomain::land;
    std::vector<std::string> notes;
    const std::set<std::string> land_goals{"LAND_GOAL"};
    const auto land_plans = foc::tactical_ai_detail::load_plans(land.host, plan_modules, notes, &land_goals);
    expect(land_plans && land_plans.value().size() == 1 && land_plans.value().front().module == plan_path
        && notes.size() == 1 && notes.front() == "AI selected land plan definitions loaded=1, outside selection=0",
        "#3780: a land closure loads its LandMode plan in a unit test");
    World space;
    notes.clear();
    const auto space_plans = foc::tactical_ai_detail::load_plans(space.host, plan_modules, notes, &land_goals);
    expect(space_plans && space_plans.value().empty() && notes.size() == 1
        && notes.front() == "AI selected space plan definitions loaded=0, outside selection=0",
        "#3780: a space host does not read the land directory");
}


void authored_goal_extension() {
    for (const auto extension : {0, 2, 7}) {
        World world;
        ai::Engine engine(world.host, {}, {});
        ai::PlayerType controller; controller.goal_set_extension = extension;
        ai::PlayerAi player; player.player = 2; player.type = &controller;
        ai::GoalType kind; kind.category = "HIGH"; kind.application = {"GLOBAL"};
        for (std::size_t index = 0; index < 8; ++index) {
            const auto name = "GOAL_" + std::to_string(index);
            player.functions.push_back({&kind, name, nullptr, {}});
            ai::Goal goal; goal.id = index + 1; goal.function = index; goal.desire = ai::real(1);
            player.proposed.push_back(goal);
        }
        ai::EngineContractAccess::maintain(engine, player);
        expect(player.active.size() == static_cast<std::size_t>(extension),
            "GS-30: actual proposal culling and retention honor the authored extension");
    }
}


void unit_concentration() {
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    const foc::AiBounds bounds{ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)};
    const ai::Rect all{ai::real(-1000), ai::real(-1000), ai::real(2000), ai::real(2000)};
    std::vector<Real> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        World world;
        world.host->setup.content.types.front().weapons = {{0, ai::real(500), ai::real(500), true, true},
            {1, ai::real(500), ai::real(500), true, true}};
        auto small = world.host->setup.content.types.front(); small.type_id = 2; small.name = "SMALL";
        small.combat_power = ai::real(500); small.weapons.clear();
        world.host->setup.content.types.push_back(small);
        world.host->types.clear(); world.host->types_by_name.clear();
        for (const auto& type : world.host->setup.content.types) {
            world.host->types.emplace(type.type_id, &type); world.host->types_by_name.emplace(type.name, &type);
        }
        auto friendly = world.view->units.front(); friendly.id = 20; friendly.type = 2; friendly.owner = 1;
        friendly.position = {q(600), q(600), {}}; friendly.health = ai::real(1) / ai::real(2);
        world.view->units.push_back(friendly);
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto equations = ai::EquationSet::parse({{"concentration.xml", R"xml(<Root>
          <FriendlyRaw>Variable_Target.FriendlyUnitConcentrationUnnormalized</FriendlyRaw>
          <EnemyRaw>Variable_Target.EnemyUnitConcentrationUnnormalized</EnemyRaw>
          <Friendly>Variable_Target.FriendlyUnitConcentration</Friendly>
          <Enemy>Variable_Target.EnemyUnitConcentration</Enemy>
          <FriendlyNbtd>Variable_Target.FriendlyUnitConcentrationNBTD</FriendlyNbtd>
          <EnemyNbtd>Variable_Target.EnemyUnitConcentrationNBTD</EnemyNbtd>
          <Attenuated>Variable_Target.FriendlyUnitConcentration {Parameter_Attenuator=1}</Attenuated>
          <ObjectLocation>Variable_Target.Location.EnemyUnitConcentrationUnnormalized</ObjectLocation>
        </Root>)xml"}}, converters);
        expect(equations.has_value(), "PE-28: concentration token equations parse"); if (!equations) return;
        ai::AiData data; data.equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(data), {});
        auto& grid = ai::EngineContractAccess::grid(engine);
        grid.partition(bounds, 5, 5, ai::Constants{}); grid.service(*world.view, *world.host, 0);
        ai::Target region; region.region = all;
        const auto values = [&] {
            std::vector<Real> result;
            for (const auto* name : {"FriendlyRaw", "EnemyRaw", "Friendly", "Enemy", "FriendlyNbtd", "EnemyNbtd", "Attenuated"}) {
                const auto value = engine.evaluate(name, 1, &region);
                expect(value.has_value(), "PE-28: selected concentration token is hosted");
                result.push_back(value.value_or(Real{}));
            }
            return result;
        };
        const auto cold = values();
        expect(cold == std::vector<Real>{ai::real(500), ai::real(2000), ai::real(1) / ai::real(3), ai::real(1),
            ai::real(1), ai::real(1), ai::real(2) / ai::real(5)},
            "PG-09/PE-28: full company power per base entry; game/side normalizers clamp, and only the denominator attenuates");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            expect(values() == cold, "PE-28: cold prepared queries match the const Lua path");
            grid.prepare_contributions();
            expect(values() == cold, "PE-28: warmed force normalizers preserve concentration answers");
        }
        if (reference.empty()) reference = cold;
        else expect(reference == cold, "PE-28: all token bits agree across 1/2/4/8 workers");
        const ai::Rect spread_only{ai::real(400), ai::real(-100), ai::real(100), ai::real(100)};
        expect(grid.concentration(*world.host, *world.view, spread_only, 2, 1, false) == Real{}
            && grid.force(*world.host, *world.view, spread_only, 2, 1, false, Real{}, 0) > Real{},
            "PG-09: spread threat cells do not also count as the entry's base cell");
        const ai::Rect base_cell{ai::real(200), ai::real(-400), ai::real(10), ai::real(10)};
        expect(grid.concentration(*world.host, *world.view, base_cell, 2, 1, false) == ai::real(2000),
            "PG-09: base-cell coordinates round after adding one half, rather than truncate the object position");
        expect(grid.concentration(*world.host, *world.view, all, 1, 1, false) == Real{}, "PG-09: category mask filters entries");
        ai::Target object; object.object = 10;
        expect(engine.evaluate("ObjectLocation", 1, &object) == ai::real(2000), "PE-28: an object-location concentration includes the object itself");
        world.host->setup.players.push_back(foc::AiPlayer{3, "ALLY", false, true, ""});
        world.host->setup.players.push_back(foc::AiPlayer{4, "NEUTRAL", true, false, ""});
        world.host->players.clear();
        for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
        world.view->players.push_back({3, 1, 2}); world.view->players.push_back({4, 4, 3});
        friendly.id = 21; friendly.owner = 3; world.view->units.push_back(friendly);
        friendly.id = 22; friendly.owner = 4; world.view->units.push_back(friendly);
        grid.service(*world.view, *world.host, 1);
        expect(grid.concentration(*world.host, *world.view, all, 2, 1, true) == ai::real(1000)
            && grid.concentration(*world.host, *world.view, all, 2, 1, false) == ai::real(2000), "PG-09: allies count, neutral owners do not");
        world.view->units.front().position.x = q(400); grid.service(*world.view, *world.host, 2);
        expect(grid.concentration(*world.host, *world.view, base_cell, 2, 1, false) == ai::real(2000), "PG-09: base cell retains the existing grid service cadence");
        grid.service(*world.view, *world.host, 300);
        expect(grid.concentration(*world.host, *world.view, base_cell, 2, 1, false) == Real{}, "PG-09: due movement refresh moves concentration to its new base cell");
        for (auto& weapon : world.host->setup.content.types.front().weapons) weapon.range = ai::real(2000);
        auto edge_constants = ai::Constants{}; edge_constants.threat_range_cap = ai::real(2000);
        world.view->units.front().position = {q(999), {}, {}};
        grid.partition(bounds, 5, 5, edge_constants); grid.service(*world.view, *world.host, 0);
        const ai::Rect wrapped_cell{ai::real(-1000), ai::real(-1000), ai::real(10), ai::real(10)};
        const ai::Rect right_cell{ai::real(990), ai::real(-10), ai::real(10), ai::real(10)};
        expect(grid.concentration(*world.host, *world.view, wrapped_cell, 2, 1, false) == ai::real(2000)
            && grid.concentration(*world.host, *world.view, right_cell, 2, 1, false) == Real{},
            "PG-09: the retail right-edge rounded coordinate wraps into the next row when its zone covers that cell");
        world.view->units.front().position = {{}, q(-999), {}};
        grid.partition(bounds, 5, 5, edge_constants); grid.service(*world.view, *world.host, 0);
        expect(grid.concentration(*world.host, *world.view, all, 2, 1, false) == Real{},
            "PG-09: bottom-edge flattened overflow is safely unmatched, never used as a memory index");
    }
}


void coroutine_module_loading() {
    auth::ModuleManifest modules;
    expect(modules.add("Root.lua", R"LUA(
        function Lazy()
            local outer = require('Outer')
            value = outer.answer
            cached = require('Outer') == outer
            restored_name = _REQUIREDNAME == nil
            coroutine.yield(true)
            local ok, message = pcall(require, 'Broken')
            syntax_failed = not ok and string.find(message, 'BROKEN.LUA') ~= nil
            ok, message = pcall(require, 'RuntimeBad')
            runtime_failed = not ok and string.find(message, 'lazy sentinel') ~= nil
            continued = true
        end
    )LUA").has_value(), "L-03a: lazy coroutine root enters manifest");
    expect(modules.add("Outer.lua", "local inner = require('Inner'); outer_name = _REQUIREDNAME == 'Outer'; return {answer = inner.value + 1}").has_value()
        && modules.add("Inner.lua", "inner_name = _REQUIREDNAME == 'Inner'; inner_runs = (inner_runs or 0) + 1; return {value = 41}").has_value()
        && modules.add("Broken.lua", "local = broken").has_value()
        && modules.add("RuntimeBad.lua", "error('lazy sentinel')").has_value(), "L-03a: nested and failing lazy modules enter manifest");
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto made = auth::ScriptScheduler::create({}, modules);
        expect(made.has_value(), "L-03a: lazy module scheduler creates"); if (!made) return;
        auto scripts = std::move(made).value();
        expect(scripts.create_instance(1, "Root.lua").has_value(), "L-03a: lazy root loads without loading its dependencies");
        auth::ScriptEvent start; start.key = {1, foc::producer_foc_engine, 1, 0}; start.target = 1;
        start.kind = auth::ScriptEvent::Kind::start_thread; start.name = "Lazy";
        expect(scripts.submit_event(start).has_value(), "L-03a: lazy coroutine starts");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto first = scripts.service(executor);
        expect(first && first.value().diagnostics.empty(), "L-03a: first coroutine require uses its own stack and yields");
        const auto saved = scripts.save(executor);
        auto restored = auth::ScriptScheduler::create({}, modules);
        expect(saved && restored && restored.value().load(saved.value()).has_value(), "L-03a: loaded modules and suspended coroutine restore");
        if (!saved || !restored) return;
        auto direct = scripts.service(executor);
        auto resumed = restored.value().service(executor);
        expect(direct && resumed && direct.value().diagnostics.empty() && resumed.value().diagnostics.empty(),
            "L-03a: syntax and runtime require failures reach pcall on the calling coroutine");
        for (const auto* name : {"cached", "restored_name", "outer_name", "inner_name", "syntax_failed", "runtime_failed", "continued"}) {
            const auto value = scripts.read_global(1, name);
            expect(value && value.value() && std::get<bool>(value.value()->data), "L-03a: nested names, cache and failure controls succeed");
        }
        for (const auto& [name, expected] : {std::pair{"value", 42}, {"inner_runs", 1}}) {
            const auto value = scripts.read_global(1, name);
            expect(value && value.value() && std::get<Real>(value.value()->data) == ai::real(expected),
                "L-03a: coroutine receives the returned table and executes a cached dependency once");
        }
        const auto digest = scripts.state_hash(executor), resumed_digest = restored.value().state_hash(executor);
        expect(digest && resumed_digest && digest.value() == resumed_digest.value(), "L-03a: resumed lazy module graph has the uninterrupted bit identity");
        if (!digest) return;
        if (reference.empty()) reference = digest.value();
        else expect(reference == digest.value(), "L-03a: lazy module digest agrees across 1/2/4/8 workers");
    }
}


void movement_query_projection() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    t::TacticalSetup start;
    start.players = {{1, 1, 100, t::player_flag_commandable}};
    start.units = {{1, 100, 1}, {2, 101, 1}};
    t::MotionTable motion;
    motion.rules = {q(30), q(100)};
    motion.profiles = {{100, q(1), q(1), q(1), q(1), q(1)}};
    t::AbilityProfile shield;
    shield.kind = t::AbilityKind::missile_shield; shield.disable_movement = true; shield.effective_radius = q(750);
    auto made = t::TacticalSession::create(start, {}, {}, motion, std::nullopt, {}, {},
        t::AbilityTable{{{100, {shield}}}, {1}});
    expect(made.has_value(), "EX-57: mobile ship and stationary type fixture creates");
    if (!made) { std::cerr << made.error().message << '\n'; return; }
    auto& world = made.value();
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 0; tick < 3; ++tick) {
        if (tick != 0) expect(world.submit({{tick - 1, 1, tick}, {1}, t::AbilityPayload{
            t::AbilityKind::missile_shield, tick == 1 ? t::AbilityAction::activate : t::AbilityAction::deactivate}}).has_value(),
            "EX-57: deployment toggle submits");
        if (tick != 0) expect(world.step(executor).has_value(), "EX-57: deployment toggle commits");
        const auto view = foc::detail::build_view(world, *world.snapshot());
        expect(view->find(1)->can_move == (tick != 1) && !view->find(2)->can_move,
            "EX-57: native ship admission respects active deployment and release while stationary type stays excluded");
    }
}


void mod_ai_calls() {
    World world;
    world.view->players.push_back({3, 2, 2});
    world.host->setup.players.push_back(foc::AiPlayer{3, "ALLY", false, true, ""});
    world.host->players.clear();
    for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
    world.host->setup.content.types.front().combat_power = ai::real(16777216);
    world.host->setup.content.types.front().property_bits = 2;
    world.host->setup.content.properties = {{"FIRST", 1}, {"SECOND", 2}};
    auto small = world.host->setup.content.types.front(); small.type_id = 2; small.name = "SMALL"; small.combat_power = ai::real(1);
    world.host->setup.content.types.push_back(small);
    world.host->types.clear(); world.host->types_by_name.clear();
    for (const auto& type : world.host->setup.content.types) {
        world.host->types.emplace(type.type_id, &type); world.host->types_by_name.emplace(type.name, &type);
    }
    auto extra = world.view->units.front(); extra.id = 11; extra.type = 2;
    world.view->units.push_back(extra); extra.id = 12; world.view->units.push_back(extra);
    extra.id = 13; extra.owner = 1; world.view->units.push_back(extra);
    ai::Engine engine(world.host, {}, {});
    ai::EngineContractAccess::proposal_fixture(engine); world.host->engine = &engine;
    auth::ModuleManifest manifest;
    expect(manifest.add("CALLS.LUA", R"LUA(
        function Block(tf) tf.Block_Goal_Proposal() end
        function Unblock(tf) tf.Unblock_Goal_Proposal() end
        function SkippedOverride(object)
            skipped_no_result = object.Set_Targeting_Priorities('Custom_Set') == nil
            continued = true
        end
        function SkippedMethods(tf, object)
            skipped_tf_priorities = tf.Set_Targeting_Priorities('Custom_Set') == nil
            skipped_tf_lock = tf.Lock_Current_Orders(true) == nil
            skipped_object_lock = object.Lock_Current_Orders(true) == nil
            methods_continued = true
        end
        function Query(tf, object)
            threat = tf.Get_Self_Threat_Sum()
            online = object.Are_Engines_Online()
            mobile = object.Can_Move()
            ally_self = object.Get_Owner().Is_Ally(object.Get_Owner())
            ally_team = object.Get_Owner().Is_Ally(Find_Player('ALLY'))
            ally_enemy = object.Get_Owner().Is_Ally(Find_Player('REBEL'))
            own_count = table.getn(Find_All_Objects_Of_Type(object.Get_Owner(), 'CORVETTE'))
            total_count = table.getn(Find_All_Objects_Of_Type('CORVETTE'))
            category_union = object.Is_Category('Fighter | Corvette')
            category_absent = object.Is_Category('Fighter')
            property_union = object.Has_Property('First | Second')
        end
    )LUA").has_value(), "EX-53/54/55: call fixture enters manifest");
    auto scripts = auth::ScriptScheduler::create({}, std::move(manifest));
    expect(scripts.has_value(), "EX-53/54/55: call scheduler starts"); if (!scripts) return;
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_globals(scripts.value(), world.host, errors);
    foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
    expect(errors.empty(), "EX-53/54/55: production Lua methods register");
    expect(scripts.value().create_instance(1, "CALLS.LUA").has_value()
        && scripts.value().create_instance(2, "CALLS.LUA").has_value(), "EX-55: two independent plan callers");
    const eawr::sim::InlineExecutor executor;
    std::uint64_t sequence{};
    const auto invoke = [&](std::uint64_t instance, const std::string& name, auth::ValueList arguments, std::size_t diagnostics = 0) {
        auth::ScriptEvent event; event.key = {scripts.value().completed_tick() + 1, foc::producer_foc_engine, instance, sequence++};
        event.target = instance; event.kind = auth::ScriptEvent::Kind::call; event.name = name; event.arguments = std::move(arguments);
        expect(scripts.value().submit_event(std::move(event)).has_value(), "EX-55: call event queues");
        auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.size() == diagnostics, "EX-53/54/55: sourced calls report the expected diagnostics");
        if (report && diagnostics != 0)
            expect(std::all_of(report.value().diagnostics.begin(), report.value().diagnostics.end(),
                [](const auto& diagnostic) { return diagnostic.code == auth::codes::unsupported_api; }),
                "EX-59: every skipped override or order lock is explicitly unsupported");
        if (report) expect(engine.after_service(report.value()).has_value(), "EX-55: ordered proposal effect commits");
    };
    const auto force = [](std::uint64_t id) { return auth::Value{auth::Handle{ai::handle_taskforce, id}}; };
    invoke(1, "Block", {force(1)}); invoke(1, "Block", {force(1)}); invoke(2, "Block", {force(2)});
    expect(ai::EngineContractAccess::player(engine, 2)->proposal_blocks == 3, "EX-55: repeated and independent holders are counted");
    invoke(1, "Unblock", {force(1)});
    const auto* player = ai::EngineContractAccess::player(engine, 2);
    expect(player->proposal_blocks == 1 && player->proposal_blockers == std::map<std::uint64_t, std::uint32_t>{{2, 1}},
        "EX-55: unblock removes all of the caller's holders and retains another goal's holder");
    invoke(1, "Unblock", {force(1)});
    expect(player->proposal_blocks == 1, "EX-55: repeated unblock cannot release another goal or underflow");
    invoke(2, "Unblock", {force(2)});
    expect(player->proposal_blocks == 0 && player->proposal_blockers.empty(), "EX-55: final owner releases proposals");
    world.view->units.front().engines_online = false;
    world.view->units.front().can_move = true;
    invoke(1, "Query", {force(1), auth::Value{auth::Handle{foc::handle_game_object, 10}}});
    const auto threat = scripts.value().read_global(1, "threat");
    const auto online = scripts.value().read_global(1, "online");
    const auto own = scripts.value().read_global(1, "own_count");
    const auto total = scripts.value().read_global(1, "total_count");
    const auto category_union = scripts.value().read_global(1, "category_union");
    const auto category_absent = scripts.value().read_global(1, "category_absent");
    const auto property_union = scripts.value().read_global(1, "property_union");
    expect(threat && threat.value() && std::get<Real>(threat.value()->data) == ai::real(16777216),
        "EX-54: threat sum rounds after every member, rather than once after an exact sum");
    expect(online && online.value() && !std::get<bool>(online.value()->data), "EX-53: disabled engines report false independently of combat power");
    for (const auto& [name, expected] : {std::pair{"mobile", true}, {"ally_self", true}, {"ally_team", true}, {"ally_enemy", false}}) {
        const auto value = scripts.value().read_global(1, name);
        expect(value && value.value() && std::get<bool>(value.value()->data) == expected,
            "EX-57/58: completed movement eligibility and player alliances retain independent meanings");
    }
    expect(own && own.value() && std::get<Real>(own.value()->data) == ai::real(3)
        && total && total.value() && std::get<Real>(total.value()->data) == ai::real(4),
        "AI-16: player-first object lookup preserves ownership and category matching");
    expect(category_union && category_union.value() && std::get<bool>(category_union.value()->data)
        && category_absent && category_absent.value() && !std::get<bool>(category_absent.value()->data)
        && property_union && property_union.value() && std::get<bool>(property_union.value()->data),
        "EX-56: authored category and property unions test any matching bit");
    invoke(1, "SkippedOverride", {auth::Value{auth::Handle{foc::handle_game_object, 10}}}, 1);
    for (const auto* name : {"continued", "skipped_no_result"}) {
        const auto value = scripts.value().read_global(1, name);
        expect(value && value.value() && std::get<bool>(value.value()->data),
            "EX-59: counted unsupported setter returns no result and allows the caller to continue");
    }
    invoke(1, "SkippedMethods", {force(1), auth::Value{auth::Handle{foc::handle_game_object, 10}}}, 3);
    for (const auto* name : {"skipped_tf_priorities", "skipped_tf_lock", "skipped_object_lock", "methods_continued"}) {
        const auto value = scripts.value().read_global(1, name);
        expect(value && value.value() && std::get_if<bool>(&value.value()->data) && std::get<bool>(value.value()->data),
            "EX-59: TaskForce targeting and object/TaskForce locks diagnose each skip, return no result and continue");
    }
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


// PL-20: a named type list proposes its types in the order the plan wrote them, so equal weights
// pick the first name written (the original's enumeration lists the satellite plan's laser first).
void named_team_authored_order() {
    World world;
    world.host->setup.content.types.clear();
    // Ids deliberately descend against the authored order: an id sort would reverse it.
    const std::vector<std::pair<eawr::sim::tactical::TypeId, const char*>> named{{9, "UC_SAT_LASER"}, {7, "UC_SAT_MISSILE"}, {5, "UC_SAT_REPAIR"}};
    for (const auto& [id, name] : named) {
        foc::AiType type;
        type.type_id = id;
        type.name = name;
        world.host->setup.content.types.push_back(type);
    }
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const foc::AiType& entry : world.host->setup.content.types) {
        world.host->types.emplace(entry.type_id, &entry);
        world.host->types_by_name.emplace(entry.name, &entry);
    }
    auth::ValueList globals;
    globals.push_back(auth::Value::text("Build_Structure_Space"));
    std::vector<auth::Value> taskforce{auth::Value::text("MainForce"),
        auth::Value::text("Uc_Sat_Laser | Uc_Sat_Missile | Uc_Sat_Repair = 0, 1")};
    std::vector<auth::Value> list{auth::Value{taskforce}};
    globals.push_back(auth::Value{list});
    for (int index = 0; index < 8; ++index) globals.push_back(auth::Value{});
    std::vector<std::string> notes;
    auto plan = ai::build_plan(*world.host, "structure", "structure.lua", globals, notes);
    expect(static_cast<bool>(plan), "PL-20: the named-type plan builds");
    if (!plan) return;
    const ai::TaskForceDef& tf = plan.value().taskforces.front();
    expect(tf.teams.size() == 1 && tf.teams.front().types == std::vector<eawr::sim::tactical::TypeId>{9, 7, 5},
        "PL-20: a named type list keeps the authored order, not the id order");
}


// PL-20 (#3717): a category team lists the type table in load order, filtered; ids (name hashes)
// decide nothing. The engine reads content.types as the table; ai_setup builds it from the tables'
// table_order, so a type-id sort of either reverses the list here.
void category_team_table_order() {
    World world;
    world.host->setup.content.types.clear();
    world.host->setup.content.categories = {{"FIGHTER", 1}, {"CORVETTE", 2}};
    // Ids ascend against the table (the first listed type has the highest id).
    const std::vector<std::pair<eawr::sim::tactical::TypeId, const char*>> table{{90, "UC_FIRST"}, {70, "UC_SECOND"}, {50, "UC_OTHER"}, {30, "UC_THIRD"}};
    for (const auto& [id, name] : table) {
        foc::AiType type;
        type.type_id = id;
        type.name = name;
        type.category_bits = std::string_view(name) == "UC_OTHER" ? 2 : 1;
        world.host->setup.content.types.push_back(type);
    }
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const foc::AiType& entry : world.host->setup.content.types) {
        world.host->types.emplace(entry.type_id, &entry);
        world.host->types_by_name.emplace(entry.name, &entry);
    }
    auth::ValueList globals;
    globals.push_back(auth::Value::text("Hide"));
    std::vector<auth::Value> taskforce{auth::Value::text("MainForce"), auth::Value::text("Fighter = 1, 2")};
    std::vector<auth::Value> list{auth::Value{taskforce}};
    globals.push_back(auth::Value{list});
    for (int index = 0; index < 8; ++index) globals.push_back(auth::Value{});
    std::vector<std::string> notes;
    auto plan = ai::build_plan(*world.host, "category", "category.lua", globals, notes);
    expect(static_cast<bool>(plan), "PL-20: the category plan builds");
    if (!plan) return;
    expect(plan.value().taskforces.front().teams.front().types == std::vector<eawr::sim::tactical::TypeId>{90, 70, 30},
        "PL-20: a category team keeps the type-table order, not the id order");

    // ai_setup: the table order of the loaded tables decides content.types, whatever the ids are.
    const std::vector<std::string> names{"Uc_Alpha", "Uc_Bravo", "Uc_Charlie", "Uc_Delta", "Uc_Echo"};
    std::vector<std::string> by_id = names;
    std::sort(by_id.begin(), by_id.end(), [](const auto& a, const auto& b) { return skirmish::type_id(a) < skirmish::type_id(b); });
    eawr::units::UnitTables tables;
    std::uint64_t order = 100;
    for (auto name = by_id.rbegin(); name != by_id.rend(); ++name) { // table order = descending id
        eawr::units::UnitType unit;
        unit.id = *name;
        unit.table_order = order++;
        tables.units.push_back(std::move(unit));
    }
    std::reverse(tables.units.begin(), tables.units.end()); // roster order must not matter either
    const auto setup = skirmish::ai_setup(skirmish::SkirmishStart{}, skirmish::StartInputs{}, tables);
    std::vector<std::string> listed;
    for (const auto& type : setup.content.types) listed.push_back(type.name);
    std::vector<std::string> expected;
    for (auto name = by_id.rbegin(); name != by_id.rend(); ++name) {
        std::string upper = *name;
        for (char& character : upper) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        expected.push_back(std::move(upper));
    }
    expect(listed == expected, "PL-20: ai_setup lists the types in table order, not type-id order");
    {
        // WTA-47 (#735): a lobby AI carries its slot's level into the AI setup; a human carries none.
        skirmish::SkirmishStart start;
        for (std::uint32_t id = 1; id <= 4; ++id) {
            skirmish::StartPlayer player;
            player.player.player_id = id;
            player.faction = "Empire";
            player.lobby = true;
            player.human = id == 1;
            player.difficulty = id == 2 ? sim::tactical::AiLevel::easy : id == 3 ? sim::tactical::AiLevel::hard
                : id == 1 ? sim::tactical::AiLevel::hard : sim::tactical::AiLevel::normal;
            start.players.push_back(std::move(player));
        }
        skirmish::StartInputs inputs;
        skirmish::StartFaction faction;
        faction.name = "Empire";
        faction.basic_ai = "BasicEmpire";
        inputs.factions.push_back(faction);
        const auto levelled = skirmish::ai_setup(start, inputs, tables);
        expect(levelled.players.size() == 4 && levelled.players[0].difficulty == sim::tactical::AiLevel::normal
                && levelled.players[1].difficulty == sim::tactical::AiLevel::easy
                && levelled.players[2].difficulty == sim::tactical::AiLevel::hard
                && levelled.players[3].difficulty == sim::tactical::AiLevel::normal,
            "WTA-47: ai_setup gives each lobby AI its slot's level, and a human stays Normal");
    }
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
    auto& player = ai::EngineContractAccess::add_player(engine, 1);
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
                auto& player = ai::EngineContractAccess::add_player(engine, 1);
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


class FindTargetFixture final : public ai::FindTargetQueries {
public:
    const ai::Engine* engine{};
    bool galactic_available{true};
    bool current_available{true};
    bool map_source{true};
    std::optional<ai::FindTargetPath> path;
    const ai::Engine* manager(ai::FindTargetManager mode) const override {
        return (mode == ai::FindTargetManager::galactic ? galactic_available : current_available) ? engine : nullptr;
    }
    const ai::Target* source_target(const ai::Engine& selected, sim::EntityId object) const override {
        // FT-05: fixture planet/direct-parent/fleet-parent lookup. Production
        // campaign containment and execution routes are supplied by S4.
        if (!map_source) return nullptr;
        if (object == 10 || object == 12 || object == 13) return selected.target(4);
        return selected.target_of_object(object);
    }
    std::optional<ai::FindTargetPath> execution_path(const ai::Engine&, tactical::PlayerId,
        const ai::Target&, const ai::Target& candidate) const override {
        if (candidate.id == 5 && path) { auto other = *path; other.distance = ai::real(1); return other; }
        return path;
    }
};


void find_target_reach_contract() {
    struct Case {
        std::string name, call;
        bool error{}, answer{true};
        std::optional<ai::FindTargetPath> path{};
        bool galactic{true}, current{true}, map_source{true}, enemy{}, competing{};
        int tolerance{1000};
        std::uint64_t picked_id{1};
        bool queries{true};
    };
    const auto route = [](ai::FindTargetService service, int threat = 0) {
        ai::FindTargetPath value; value.service = service; value.distance = ai::real(900000);
        value.threat = ai::real(threat); return value;
    };
    const auto query = [](std::string policy, std::string tail = ", 1, object", std::string flags = "Friendly_Unit") {
        return "FindTarget.Reachable_Target(player, 'Positive', '" + flags + "', '" + policy + "'" + tail + ")";
    };
    std::vector<Case> cases;
    cases.push_back({"FT-04 omitted probability/source", query("Any", "")});
    cases.push_back({"FT-04 explicit best", query("Any", ", 1")});
    cases.push_back({"FT-04 optional probability", query("Any", ", 0.5")});
    for (const auto* value : {"0", "-0.1", "1.1", "'bad'"})
        cases.push_back({"FT-04 invalid probability", query("Any", std::string(", ") + value), true});
    for (const auto* call : {
        "FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit', 'Any', 1, object, 0, 0)",
        "FindTarget.Reachable_Target(object, 'Positive', 'Friendly_Unit', 'Any')",
        "FindTarget.Reachable_Target(human, 'Positive', 'Friendly_Unit', 'Any')",
        "FindTarget.Reachable_Target(player, 'Unknown', 'Friendly_Unit', 'Any')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Global', 'Any')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Unknown', 'Any')",
        "FindTarget.Reachable_Target(player, 'Positive', '', 'Any')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit', 'Unknown')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit', 'Any|No_Threat')"})
        cases.push_back({"FT-04/13 invalid arguments", call, true});
    for (const auto* policy : {"Friendly_only", "Friendly_Only", "Friendly_Ignore_Threat", "Enemy_destination",
        "Enemy_undefended", "Enemy_Undefended", "Single_Hop_Disconnected", "Any", "No_Threat", "Low_Threat",
        "Medium_Threat", "High_Threat", "Any_Threat", "  aNy_ThReAt  "})
        cases.push_back({"FT-07/13 accepted policy", query(policy), false, true, route(ai::FindTargetService::galactic)});
    auto absent_galactic = Case{"FT-07 Any requires galactic manager", query("Any", ""), true};
    absent_galactic.galactic = false; cases.push_back(absent_galactic);
    absent_galactic.name = "FT-07 Any_Threat selects current manager"; absent_galactic.call = query("Any_Threat", "");
    absent_galactic.error = false; cases.push_back(absent_galactic);
    auto absent_current = Case{"FT-07 current manager absent", query("Any_Threat", ""), true};
    absent_current.current = false; cases.push_back(absent_current);
    for (const auto* source : {"object", "planet_parent", "fleet_parent", "origin"})
        cases.push_back({"FT-05/14 resolved source", query("Friendly_Only", std::string(", 1, ") + source), false, true,
            route(ai::FindTargetService::galactic)});
    cases.push_back({"FT-05 candidate remains equation target",
        "FindTarget.Reachable_Target(player, 'Candidate', 'Friendly_Unit', 'Any', 1, object)", false, true,
        route(ai::FindTargetService::galactic)});
    for (const auto* source : {"nil", "false", "42", "dead_object", "dead_target", "stale_target"})
        cases.push_back({"FT-05 invalid source", query("Any", std::string(", 1, ") + source), true});
    auto blocked = route(ai::FindTargetService::galactic, 1);
    cases.push_back({"FT-05 no source skips cost", query("Friendly_Only", ", 1"), false, true, blocked});
    auto unmapped = Case{"FT-05 live unmapped source skips cost", query("Friendly_Only"), false, true, blocked};
    unmapped.map_source = false; cases.push_back(unmapped);
    cases.push_back({"FT-06 omitted radius", query("Any"), false, true, route(ai::FindTargetService::galactic)});
    cases.push_back({"FT-06 zero radius", query("Any", ", 1, object, 0")});
    cases.push_back({"FT-06 boundary equality in 3D", query("Any", ", 1, object, 13")});
    cases.push_back({"FT-06 negative radius", query("Any", ", 1, object, -13")});
    cases.push_back({"FT-06 just outside radius", query("Any", ", 1, object, 12.999"), false, false});
    cases.push_back({"FT-06 numeric radius required", query("Any", ", 1, object, 'bad'"), true});
    unmapped.name = "FT-06 nonzero radius needs source"; unmapped.call = query("Any", ", 1, object, 1");
    unmapped.error = true; cases.push_back(unmapped);
    for (const auto* policy : {"Friendly_Only", "Enemy_destination", "Enemy_undefended", "No_Threat"}) {
        cases.push_back({"FT-08 strict threat rejects", query(policy), false, false, blocked});
        auto boundary = route(ai::FindTargetService::galactic);
        boundary.threat = ai::to_single(ai::real(1) / ai::real(10000));
        cases.push_back({"FT-08 strict equality passes", query(policy), false, true, boundary});
    }
    for (const auto* policy : {"Any", "Friendly_Ignore_Threat", "Any_Threat"})
        cases.push_back({"FT-08 permissive threat", query(policy), false, true, blocked});
    auto same = blocked; same.same_target = true;
    cases.push_back({"FT-08 same planet cost zero", query("Friendly_Only"), false, true, same});
    auto prefix = route(ai::FindTargetService::galactic, 100);
    cases.push_back({"FT-08 final hop omitted", query("Single_Hop_Disconnected"), false, true, prefix});
    prefix.prefix_threat = ai::real(1);
    cases.push_back({"FT-08 unsafe prefix rejected", query("Single_Hop_Disconnected"), false, false, prefix});
    auto enemy = Case{"FT-08 policy does not add owner filter", query("Friendly_Only", ", 1, object", "Enemy_Unit"),
        false, true, route(ai::FindTargetService::galactic)};
    enemy.enemy = true; cases.push_back(enemy);
    for (const auto* policy : {"Low_Threat", "Medium_Threat", "High_Threat"}) {
        cases.push_back({"FT-09 authored tolerance boundary", query(policy), false, true, route(ai::FindTargetService::land, 1000)});
        cases.push_back({"FT-09 above tolerance", query(policy), false, false, route(ai::FindTargetService::land, 1001)});
    }
    cases.push_back({"FT-09 land zero safety", query("No_Threat"), false, true, route(ai::FindTargetService::land)});
    cases.push_back({"FT-09 land threat rejected", query("No_Threat"), false, false, route(ai::FindTargetService::land, 1)});
    cases.push_back({"FT-09 galactic policy invalid in land", query("Any"), false, false, route(ai::FindTargetService::land)});
    auto invalid = route(ai::FindTargetService::land); invalid.source_valid = false;
    cases.push_back({"FT-09 source zone/mode required", query("Any_Threat"), false, false, invalid});
    for (const auto* policy : {"No_Threat", "Low_Threat", "Medium_Threat", "High_Threat", "Any_Threat"})
        cases.push_back({"FT-12 space bypass", query(policy)});
    auto empty = route(ai::FindTargetService::galactic); empty.empty = true;
    for (const auto* policy : {"Any", "Any_Threat", "Friendly_Ignore_Threat"})
        cases.push_back({"FT-15 permissive empty path", query(policy), false, true, empty});
    for (const auto* policy : {"Friendly_Only", "Enemy_destination", "Enemy_Undefended", "Single_Hop_Disconnected",
        "No_Threat", "Low_Threat", "Medium_Threat", "High_Threat"})
        cases.push_back({"FT-15 strict empty path", query(policy), false, false, empty});
    auto authored = Case{"FT-15 authored tolerance admits empty path", query("Low_Threat"), false, true, empty};
    authored.tolerance = 6553500; cases.push_back(authored);
    auto sentinel = route(ai::FindTargetService::galactic);
    sentinel.distance = ai::to_single(ai::real(1000000000000000000LL));
    cases.push_back({"FT-05 unreachable cost sentinel", query("Any"), false, false, sentinel});
    auto scored = Case{"FT-08 execution distance does not change score", query("Any"), false, true, route(ai::FindTargetService::galactic)};
    scored.competing = true; cases.push_back(scored);
    for (auto& test : cases) if (test.path && test.path->service == ai::FindTargetService::land) {
        const auto flag = test.call.find("Friendly_Unit");
        if (flag != std::string::npos) test.call.replace(flag, 13, "Tactical_Location");
        test.picked_id = 2;
    }
    auto space_default = Case{"FT-12 default space manager has no cost service", query("Any_Threat")};
    space_default.queries = false; cases.push_back(space_default);
    space_default.name = "FT-07 absent default galactic manager"; space_default.call = query("Any");
    space_default.error = true; cases.push_back(space_default);
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto& test : cases) {
        World world;
        world.view->units.front().health = ai::real(0);
        auto destination = world.view->units.front(); destination.id = 11; destination.health = ai::real(1);
        destination.position = {sim::math::Fixed::from_integer(3).value(), sim::math::Fixed::from_integer(4).value(),
            sim::math::Fixed::from_integer(12).value()};
        if (test.enemy) { destination.owner = 1; destination.team = 1; }
        world.view->units.push_back(destination);
        for (const auto id : {12U, 13U}) { auto parent = world.view->units.front(); parent.id = id; world.view->units.push_back(parent); }
        if (test.competing) { auto other = destination; other.id = 14; world.view->units.push_back(other); }
        std::map<std::string, std::string, std::less<>> xml;
        for (const auto& file : ai::ai_xml_files()) xml.emplace(file, "<Root></Root>");
        const auto value = std::to_string(test.tolerance);
        xml["data/xml/gameconstants.xml"] = "<Root><Low_Threat_Reachability_Tolerance>" + value
            + "</Low_Threat_Reachability_Tolerance><Medium_Threat_Reachability_Tolerance>" + value
            + "</Medium_Threat_Reachability_Tolerance><High_Threat_Reachability_Tolerance>" + value
            + "</High_Threat_Reachability_Tolerance></Root>";
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto loaded = ai::load_ai_data(xml, converters);
        expect(loaded.has_value(), test.name + ": data loads"); if (!loaded) return;
        auto equations = ai::EquationSet::parse({{"test.xml", "<Root><Positive>1</Positive><Candidate>Variable_Target.Health > 0.5</Candidate></Root>"}}, converters);
        if (!equations) { expect(false, test.name + ": equations load"); return; }
        loaded.value().equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(loaded).value(), {}); world.host->engine = &engine;
        ai::EngineContractAccess::find_target_fixture(engine);
        ai::EngineContractAccess::target_fixture(engine, 4, 10);
        if (test.competing) ai::EngineContractAccess::add_find_target(engine, 5, 14);
        auto provider = std::make_shared<FindTargetFixture>(); provider->engine = &engine;
        provider->path = test.path; provider->galactic_available = test.galactic;
        provider->current_available = test.current; provider->map_source = test.map_source;
        if (test.queries) world.host->find_target_queries = provider;
        auth::ModuleManifest modules;
        expect(modules.add("REACH.LUA", "function Check(player, human, object, planet_parent, fleet_parent, origin, dead_object, dead_target, stale_target)\n result = "
            + test.call + "\nend\n").has_value(), test.name + ": module loads");
        auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
        if (!scripts) { expect(false, test.name + ": scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "REACH.LUA").has_value(), test.name + ": bindings register");
        auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_player, 2}}, auth::Value{auth::Handle{foc::handle_player, 1}},
            auth::Value{auth::Handle{foc::handle_game_object, 10}}, auth::Value{auth::Handle{foc::handle_game_object, 12}},
            auth::Value{auth::Handle{foc::handle_game_object, 13}}, auth::Value{auth::Handle{ai::handle_ai_target, 4}},
            auth::Value{auth::Handle{foc::handle_game_object, 999}}, auth::Value{auth::Handle{ai::handle_ai_target, 999}},
            auth::Value{auth::Handle{ai::handle_ai_target, 3}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), test.name + ": event queues");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.empty() != test.error, test.name + ": error contract");
        if (report && !test.error && !report.value().diagnostics.empty())
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << test.name << ": " << diagnostic.message << '\n';
        if (!test.error) {
            const auto result = scripts.value().read_global(1, "result");
            const auto* picked = result && result.value() ? std::get_if<auth::Handle>(&result.value()->data) : nullptr;
            expect(test.answer ? picked && picked->kind == ai::handle_ai_target && picked->id == test.picked_id
                : result && result.value() && std::holds_alternative<std::monostate>(result.value()->data), test.name + ": answer contract");
        }
    }
}


// FT-01/02/03, GA-01: query the real Lua bindings, then pass the reachable
// answer straight to the stock callers' consumer APIs without converting it.
void reachable_target_contract() {
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto destination = world.view->units.front();
        destination.id = 11;
        destination.position.x = eawr::sim::math::Fixed::from_integer(25).value();
        world.view->units.push_back(destination);
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"query.xml", "<Root><Positive>1</Positive><Zero>0</Zero>"
            "<Unavailable>Variable_Target.Unavailable_Query</Unavailable></Root>"}}, converters);
        expect(equations.has_value(), "FT-02: query equations parse");
        if (!equations) return;
        ai::AiData data;
        data.equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(data), {});
        world.host->engine = &engine;
        auto query_provider = std::make_shared<FindTargetFixture>();
        query_provider->engine = &engine;
        world.host->find_target_queries = query_provider;
        ai::EngineContractAccess::find_target_fixture(engine);
        auth::ModuleManifest modules;
        expect(modules.add("QUERY.LUA",
            "function PassTarget(value) return value end\n"
            "function Check(player, tf, object)\n"
            " target = FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit', 'Any', 1)\n"
            " valid = target.Is_Valid()\n"
            " converted = target.Get_Game_Object()\n"
            " returned = PassTarget(target)\n"
            " object_move = object.Move_To(returned)\n"
            " force_move = tf.Move_To(returned)\n"
            " produced = tf.Produce_Force(returned)\n"
            " fired = object.Fire_Special_Weapon(returned, player)\n"
            " force_fired = tf.Fire_Special_Weapon('SHIP', returned)\n"
            " plain = FindTarget(tf, 'Positive', 'Friendly_Unit', 1)\n"
            " region = FindTarget.Reachable_Target(player, 'Positive', 'Tactical_Location', 'Any', 1)\n"
            " region_valid = region.Is_Valid()\n"
            " region_object = region.Get_Game_Object()\n"
            " plain_region = FindTarget(tf, 'Positive', 'Tactical_Location', 1)\n"
            " distance_object = object.Get_Distance(converted)\n"
            " distance_wrapped = object.Get_Distance(returned)\n"
            " distance_region = converted.Get_Distance(region)\n"
            " distance_force = converted.Get_Distance(tf)\n"
            " distance_position = object.Get_Distance(converted.Get_Position())\n"
            " empty = FindTarget.Reachable_Target(player, 'Positive', 'Enemy_Unit', 'Any', 1)\n"
            " zero = FindTarget.Reachable_Target(player, 'Zero', 'Friendly_Unit', 'Any', 1)\n"
            " unavailable = FindTarget.Reachable_Target(player, 'Unavailable', 'Friendly_Unit', 'Any', 1)\n"
            " continued = true\n"
            "end\n").has_value(), "FT-02: query module loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        if (!scripts) { expect(false, "FT-02: query scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "QUERY.LUA").has_value(), "FT-02: query bindings register");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_player, 2}},
            auth::Value{auth::Handle{ai::handle_taskforce, 1}}, auth::Value{auth::Handle{foc::handle_game_object, 10}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "FT-02: query queues");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.size() == 1
            && report.value().diagnostics.front().code == auth::codes::unsupported_api
            && report.value().diagnostics.front().message.find("skipped 1 candidates") != std::string::npos,
            "FT-02/PE-28: an unavailable selected perception emits one counted diagnostic without aborting consumers");
        const auto check_handle = [&](const char* name, std::uint32_t kind, std::uint64_t id) {
            const auto value = scripts.value().read_global(1, name);
            const auto* handle = value && value.value() ? std::get_if<auth::Handle>(&value.value()->data) : nullptr;
            expect(handle && handle->kind == kind && handle->id == id, std::string("FT-01/02: ") + name);
        };
        check_handle("target", ai::handle_ai_target, 1);
        check_handle("returned", ai::handle_ai_target, 1);
        check_handle("converted", foc::handle_game_object, 11);
        check_handle("plain", foc::handle_game_object, 11);
        check_handle("region", ai::handle_ai_target, 2);
        check_handle("plain_region", ai::handle_ai_target, 2);
        for (const char* name : {"distance_object", "distance_wrapped", "distance_region", "distance_force", "distance_position"}) {
            const auto value = scripts.value().read_global(1, name);
            expect(value && value.value() && std::get_if<Real>(&value.value()->data)
                && std::get<Real>(value.value()->data) == ai::real(25),
                "EX-51: unit distance extracts live objects, wrapped objects, region centres, TaskForces and positions");
        }
        for (const char* name : {"valid", "region_valid"}) {
            const auto value = scripts.value().read_global(1, name);
            expect(value && value.value() && std::get_if<bool>(&value.value()->data)
                && std::get<bool>(value.value()->data), "FT-02: reachable targets support validity tests");
        }
        for (const char* name : {"empty", "zero", "region_object", "unavailable"}) {
            const auto value = scripts.value().read_global(1, name);
            expect(value && value.value() && std::holds_alternative<std::monostate>(value.value()->data),
                "FT-02/03: empty, zero-score and region conversion answer nil");
        }
        const auto continued = scripts.value().read_global(1, "continued");
        expect(continued && continued.value() && std::get<bool>(continued.value()->data), "PE-28: unavailable query leaves the caller running");
        expect(report && report.value().commands.size() == 3, "FT-02: object move, force move and force production stage");
        if (report && report.value().commands.size() == 3) {
            const auto& move = report.value().commands[0];
            expect(move.verb == foc::verb_move && move.arguments.size() == 5
                && std::get<Real>(move.arguments[2].data) == ai::real(25), "FT-02: object move resolves wrapped position");
            const auto& force = report.value().commands[1].arguments;
            expect(force.size() == 4 && std::get<std::string>(force[0].data) == "move"
                && std::get<Real>(std::get<std::vector<auth::Value>>(force[3].data)[0].data) == ai::real(25),
                "FT-02: force move resolves wrapped position");
            const auto& produce = report.value().commands[2].arguments;
            expect(produce.size() == 2 && std::get<std::string>(produce[0].data) == "produce",
                "FT-02: force production accepts the unconverted staging target");
        }
    }
}


// WGA-34/35: run installed, unmodified stock redistribution Lua. The fixture
// supplies planet/freestore/perception services; target search and handle
// conversion use the production bindings. This is not a full campaign host.
void installed_galactic_freestore() {
    std::string game_root;
    game_root = eawr::testing::game_root().value_or("");
    if (game_root.empty()) { std::cout << "SKIPPED: EAWR_EAW_GAME_ROOT is not set\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [layer, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mounted = eawr::vfs::resolve_manifest_mount(layer, std::filesystem::path(game_root) / folder / "Data");
        expect(mounted.has_value(), "WGA-34: installed data layer resolves");
        if (!mounted) return;
        mounts.push_back(std::move(mounted).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(filesystem.has_value(), "WGA-34: installed data mounts");
    if (!filesystem) return;
    auto libraries = filesystem.value().enumerate("data/scripts/library", ".lua");
    expect(libraries.has_value(), "WGA-34: installed libraries enumerate");
    if (!libraries) return;
    std::map<std::string, std::string> stock;
    const auto load = [&](const std::string& path) {
        const auto bytes = filesystem.value().open(path);
        expect(bytes.has_value(), "WGA-34: installed Lua opens: " + path);
        if (bytes) stock.emplace(std::filesystem::path(path).filename().string(),
            std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
    };
    for (const auto& library : libraries.value()) load(library.canonical_path);
    load("data/scripts/freestore/galacticfreestore.lua");
    load("data/scripts/freestore/galacticherofreestore.lua");
    if (failures != 0) return;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto planet = world.view->units.front();
        planet.id = 700;
        world.view->units.push_back(planet);
        planet.id = 12; // a transport for the land redistribution path
        world.view->units.insert(world.view->units.begin() + 1, planet);
        auto mode = std::make_shared<ModeWorld>();
        mode->friendly_planet_fixture = true;
        mode->world_view = world.view;
        world.host->ai_world = mode;
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"fixture.xml", "<Root><Space_Priority_Defense_Score>1</Space_Priority_Defense_Score>"
            "<Ground_Priority_Defense_Score>1</Ground_Priority_Defense_Score></Root>"}}, converters);
        if (!equations) { expect(false, "WGA-34: fixture equations parse"); return; }
        ai::AiData data;
        data.equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(data), {});
        world.host->engine = &engine;
        auto query_provider = std::make_shared<FindTargetFixture>();
        query_provider->engine = &engine;
        world.host->find_target_queries = query_provider;
        auto& player = ai::EngineContractAccess::collect_fixture(engine);
        ai::EngineContractAccess::build_targets(engine, player);
        auth::ModuleManifest modules;
        for (const auto& [path, bytes] : stock) expect(modules.add(path, bytes).has_value(), "WGA-34: installed module registers");
        expect(modules.add("REDISTRIBUTE.LUA", "require('galacticfreestore')\n"
            "function Check(player, store, ship, transport)\n"
            " PlayerObject = player\n FreeStore = store\n"
            " ship_moved = MoveUnit(ship)\n transport_moved = MoveUnit(transport)\n"
            " target_valid = TestValid(FindTarget.Reachable_Target(player, 'Space_Priority_Defense_Score', 'Friendly', 'Friendly_Only', 1))\n"
            "end\n").has_value(), "WGA-34: stock redistribution driver loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        if (!scripts) { expect(false, "WGA-34: scheduler starts"); return; }
        auto& scheduler = scripts.value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scheduler, world.host, errors);
        foc::tactical_ai_detail::register_methods(scheduler, world.host, errors);
        const auto fixed_result = [](auth::Value value) {
            return [value](auth::BindingContext&, const auth::ValueList&) -> eawr::core::Result<auth::ValueList> {
                return eawr::core::Result<auth::ValueList>::success({value});
            };
        };
        expect(scheduler.register_binding("_ScriptMessage", [](auth::BindingContext&, const auth::ValueList&)
            -> eawr::core::Result<auth::ValueList> { return eawr::core::Result<auth::ValueList>::success({}); }).has_value(), "WGA-34: fixture debug sink registers");
        expect(scheduler.register_binding("EvaluatePerception", [](auth::BindingContext&, const auth::ValueList& args)
            -> eawr::core::Result<auth::ValueList> {
                const auto* name = args.empty() ? nullptr : std::get_if<std::string>(&args[0].data);
                const bool global = name && name->find("Global") != std::string::npos;
                return eawr::core::Result<auth::ValueList>::success({auth::Value{ai::real(global ? 1600 : 0)}});
            }).has_value(), "WGA-34: fixture force provider registers");
        // WGA-436/437/439: the production pool-fed callbacks answer the stock destination helpers. Only the
        // native planetary predicate owner (WGA-438) stays a fixture: this contract has no native action owner.
        {
            namespace ga = eawr::script::foc::galactic_ai;
            auto input = std::make_shared<ga::FreestoreInput>();
            auto pool = std::make_shared<ga::FreestorePool>();
            pool->qualified = true;
            ga::FreestoreObject ship; ship.id = 10; ship.type = 1; ship.owner = 2; ship.planet = 500;
            auto transport = ship; transport.id = 12; transport.type = 2; transport.transport = true;
            pool->objects = {ship, transport};
            pool->surfaces = {{700, 0, 10, true}};
            input->pool = pool;
            input->planet_restrictions[700] = {};
            input->type_categories[2] = 0;
            expect(ga::register_freestore_object_methods(scheduler, input).has_value(),
                "WGA-34/35: production object callbacks register");
        }
        expect(scheduler.register_method(foc::handle_game_object, "Get_Is_Planet_AI_Usable",
            fixed_result(auth::Value{true})).has_value(), "WGA-34: fixture planet registers");
        constexpr std::uint32_t fixture_store = 900;
        expect(scheduler.register_method(fixture_store, "Is_Unit_Safe",
            fixed_result(auth::Value{true})).has_value(), "WGA-35: fixture safety provider registers");
        expect(scheduler.register_method(fixture_store, "Move_Object", [](auth::BindingContext& context, const auth::ValueList& args)
            -> eawr::core::Result<auth::ValueList> {
                context.issue_command("fixture.redistribute", {args[1], args[2]});
                return eawr::core::Result<auth::ValueList>::success({});
            }).has_value(), "WGA-34: fixture movement sink registers");
        expect(errors.empty() && scheduler.create_instance(1, "REDISTRIBUTE.LUA").has_value(), "WGA-34: stock instance starts");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_player, 2}}, auth::Value{auth::Handle{fixture_store, 1}},
            auth::Value{auth::Handle{foc::handle_game_object, 10}}, auth::Value{auth::Handle{foc::handle_game_object, 12}}};
        expect(scheduler.submit_event(std::move(event)).has_value(), "WGA-34: stock service queues");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto report = scheduler.service(executor);
        expect(report && report.value().diagnostics.empty(), "WGA-34/35: stock first planet picks have no Lua fault");
        if (report) for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.message << '\n';
        expect(report && report.value().commands.size() == 2, "WGA-34/35: stock ship and transport both redistribute");
        if (report) for (const auto& command : report.value().commands) {
            const auto* target = command.arguments.size() == 2 ? std::get_if<auth::Handle>(&command.arguments[1].data) : nullptr;
            expect(command.verb == "fixture.redistribute" && target && target->kind == foc::handle_game_object && target->id == 700,
                "WGA-34/35: stock conversion sends the picked planet to the freestore");
        }
        for (const char* name : {"ship_moved", "transport_moved", "target_valid"}) {
            const auto value = scheduler.read_global(1, name);
            const auto* result = value && value.value() ? std::get_if<bool>(&value.value()->data) : nullptr;
            expect(result && *result, "WGA-34/35: stock movement and TestValid succeed");
        }
    }
}


// WTA-45 to WTA-47 (#735): a lobby AI's level picks its player type's adjustment entry; Normal is the default.
void ai_difficulty_levels() {
    namespace foc = eawr::script::foc;
    using Level = sim::tactical::AiLevel;
    std::map<std::string, std::string, std::less<>> xml;
    xml["data/xml/difficultyadjustments.xml"] = R"(<Difficulty_Adjustments>
<Difficulty_Adjustment Name="Easy_Default"><Credit_Multiplier>0.5</Credit_Multiplier><Space_AI_Contrast_Multiplier>0.75</Space_AI_Contrast_Multiplier>
<Space_Build_Time_Multiplier>1.5</Space_Build_Time_Multiplier><Damage_Multiplier>0.6</Damage_Multiplier><Health_Multiplier>0.4</Health_Multiplier>
<Shield_Multiplier>0.3</Shield_Multiplier><Space_AI_Goal_Cycle_Sleep_Duration>15.0</Space_AI_Goal_Cycle_Sleep_Duration></Difficulty_Adjustment>
<Difficulty_Adjustment Name="Normal_Default"><Credit_Multiplier>1.0</Credit_Multiplier><Space_AI_Goal_Cycle_Sleep_Duration>0.0</Space_AI_Goal_Cycle_Sleep_Duration></Difficulty_Adjustment>
<Difficulty_Adjustment Name="Hard_Default"><Credit_Multiplier>1.2</Credit_Multiplier><Space_AI_Contrast_Multiplier>1.15</Space_AI_Contrast_Multiplier>
<Space_Build_Time_Multiplier>0.9</Space_Build_Time_Multiplier><Damage_Multiplier>1.8</Damage_Multiplier><Health_Multiplier>0.75</Health_Multiplier>
<Shield_Multiplier>0.5</Shield_Multiplier><Space_AI_Goal_Cycle_Sleep_Duration>0.0</Space_AI_Goal_Cycle_Sleep_Duration></Difficulty_Adjustment>
<Difficulty_Adjustment Name="Mod_Only"><Damage_Multiplier>3.0</Damage_Multiplier></Difficulty_Adjustment>
</Difficulty_Adjustments>)";
    xml["data/xml/ai/players/full.xml"] = R"(<AIPlayerType><Name>FullType</Name><Difficulty_Adjustments>
<Easy>Easy_Default</Easy><Normal>Normal_Default</Normal><Hard>Hard_Default</Hard></Difficulty_Adjustments></AIPlayerType>)";
    xml["data/xml/ai/players/modded.xml"] = R"(<AIPlayerType><Name>ModdedType</Name><Difficulty_Adjustments>
<Normal>Mod_Only</Normal><Hard>Missing_Entry</Hard></Difficulty_Adjustments></AIPlayerType>)";
    const auto table = foc::AiDifficultyTable::load(xml);
    expect(table.has_value(), "WTA-46: the difficulty table loads from the AI XML");
    if (!table) return;
    const auto is = [](ai::Real value, std::int64_t numerator, std::int64_t denominator = 1) {
        return ai::to_single(value) == ai::to_single(ai::real(numerator) / ai::real(denominator));
    };
    const auto easy = table.value().resolve("fulltype", Level::easy);
    expect(is(easy.health, 4, 10) && is(easy.shield, 3, 10)
            && is(easy.damage, 6, 10) && is(easy.space_build_time, 15, 10)
            && is(easy.credit, 5, 10) && is(easy.space_contrast, 75, 100)
            && is(easy.space_goal_cycle_sleep, 15),
        "WTA-46: Easy resolves to the Easy entry's health, shield, damage, build time, credit, contrast and sleep");
    const auto hard = table.value().resolve("FullType", Level::hard);
    expect(is(hard.damage, 18, 10) && is(hard.health, 75, 100)
            && is(hard.shield, 5, 10) && is(hard.space_goal_cycle_sleep, 0),
        "WTA-46: Hard keeps health 0.75, shield 0.5 and damage 1.8 as authored");
    const auto normal = table.value().resolve("FullType", Level::normal);
    expect(is(normal.health, 1) && is(normal.shield, 1) && is(normal.damage, 1)
            && is(normal.space_build_time, 1) && is(normal.space_goal_cycle_sleep, 0),
        "WTA-46: Normal leaves every multiplier at 1 and the sleep at 0, also for tags the entry omits");
    {
        // WTA-46 (#735): a start player's adjustment is its AI type's entry for the slot's level; humans get none.
        eawr::skirmish::StartInputs inputs;
        eawr::skirmish::StartFaction faction;
        faction.name = "Empire";
        faction.basic_ai = "FullType";
        inputs.factions.push_back(faction);
        inputs.difficulty = std::make_shared<const foc::AiDifficultyTable>(table.value());
        eawr::skirmish::StartPlayer ai;
        ai.faction = "Empire";
        ai.lobby = true;
        ai.difficulty = Level::easy;
        const auto resolved = eawr::skirmish::player_difficulty(inputs, ai);
        expect(is(resolved.health, 4, 10) && is(resolved.credit, 5, 10) && is(resolved.space_build_time, 15, 10),
            "WTA-46: skirmish::player_difficulty resolves an Easy lobby AI through its faction's Basic_AI type");
        eawr::skirmish::StartPlayer human = ai;
        human.human = true;
        const auto neutral = eawr::skirmish::player_difficulty(inputs, human);
        expect(is(neutral.health, 1) && is(neutral.credit, 1) && is(neutral.damage, 1),
            "WTA-47: a human player gets no adjustment, whatever its level field says");
    }
    const auto modded = table.value().resolve("ModdedType", Level::normal);
    expect(is(modded.damage, 3) && is(modded.health, 1) && is(modded.shield, 1),
        "WTA-46: an entry that omits tags keeps 1 for each multiplier");
    const auto dummy = table.value().resolve("ModdedType", Level::hard);
    expect(is(dummy.damage, 1) && is(dummy.health, 1) && is(dummy.credit, 1),
        "WTA-46: a level with no entry gets the dummy adjustment, not the Normal entry");
    const auto unknown = table.value().resolve("NoSuchType", Level::easy);
    expect(is(unknown.health, 1) && is(unknown.credit, 1),
        "WTA-46: a player type without data gets the dummy adjustment");
}

} // namespace foc_contract
