// Contracts of the hosted FoC goal-system pieces (#449, docs/behaviour/foc-tactical-ai.md): the
// perceptual equations (PE-01 to PE-05), the AI random (PE-11), the target name hash (PE-12),
// the threat grid (PG-01 to PG-07), the TaskForce definitions (PL-10 to PL-12) and the
// scheduler reads the engine uses (thread slots, instance removal, command sequences). The
// M2 battle with game data (tests/skirmish/foc_plan_tests.cpp) covers the plan bindings.

#include "ai_engine.hpp"

#include "eawr/sim/world.hpp"

#include <iostream>
#include <map>
#include <optional>
#include <string>

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

} // namespace

int main() {
    equations();
    random_and_hash();
    threat_grid();
    taskforce_definitions();
    scheduler_reads();
    target_choice();
    threat_decay_constant();
    if (failures != 0) {
        std::cerr << failures << " FoC AI contract(s) failed\n";
        return 1;
    }
    std::cout << "FoC AI contracts passed\n";
    return 0;
}
