// The FoC tactical AI in the M2 battle (#79, docs/behaviour/foc-tactical-ai.md "#79 host"):
// the Empire AI's retail freestore against an idle Rebel player on the pinned start. The
// fixture must give the same script commands, world events and state hashes on 1, 2, 4 and
// 8 workers (ADR-009), a headless replay of its record must reproduce the world without
// running a script, and the per-tick Lua load stays below the service budget. A real-time
// run paused mid-attack and resumed on fast forward must replay like an unpaused one (#459).
//
//   foc_ai_tests [ticks [hits.csv]]   (needs EAWR_EAW_GAME_ROOT; skipped otherwise)
//
// hits.csv lists every projectile hit of the one-worker run (tick, shooter, target, x, y, z):
// where and when the fleets meet, for placing an eye-check camera.

#include "eawr/data/xml.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

struct Run {
    std::vector<std::string> hashes;       // combined world and script state per tick
    std::vector<std::string> world_hashes; // world state per tick
    std::vector<std::string> commands;     // routed script commands, in order
    std::vector<std::string> events;       // world events, in order
    std::set<std::string> diagnostics;     // distinct script diagnostics
    std::int64_t max_instructions{};
    std::int64_t total_instructions{};
    std::uint64_t ai_hits{};               // projectile hits by the AI player's units
    std::uint64_t first_ai_hit{};          // the tick of the AI's first hit, 0 if none
    std::string hits_csv;                  // every projectile hit
    std::map<std::string, std::uint64_t> orders_by_type; // the AI player's accepted orders per unit type
    double seconds{};
    tactical::TacticalReplay record;
};

std::string command_text(const auth::RoutedCommand& routed) {
    std::string out = routed.command.verb + " t" + std::to_string(routed.command.tick) + " i" +
        std::to_string(routed.command.issuer) + " s" + std::to_string(routed.command.sequence);
    out += routed.submitted ? " key " + std::to_string(routed.key.tick) + "/" + std::to_string(routed.key.player_id) + "/" +
            std::to_string(routed.key.sequence)
                            : " dropped " + routed.dropped.message;
    return out;
}

struct Content {
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    std::map<std::string, std::string> plan_modules; // modules and the selected plans
    std::map<tactical::TypeId, std::string> type_names;
};

std::optional<Content> load(const std::filesystem::path& root) {
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return std::nullopt;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return std::nullopt;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return std::nullopt;
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "FoC unit tables load");
    if (!tables) return std::nullopt;
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) return std::nullopt;
    auto start = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(start), "FoC start builds");
    if (!start) return std::nullopt;
    auto content = skirmish::session_content(tables.value(), skirmish::human_slots(fixture));
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return std::nullopt;
    // #495: the AI battle runs on the map's fog grid, as the live session does.
    auto fog = skirmish::fog_rules(inputs.value());
    expect(static_cast<bool>(fog), "the M2 fog grid builds");
    if (!fog) return std::nullopt;
    content.value().fog = fog.value();
    Content out{start.value(), std::move(content).value(), skirmish::victory_rules(start.value(), tables.value()),
        skirmish::ai_setup(start.value(), inputs.value(), tables.value()), {}, {}, {}};
    auto modules = skirmish::ai_modules(filesystem.value(), out.ai);
    expect(static_cast<bool>(modules), "the AI's Lua files load: " + (modules ? std::string() : modules.error().message));
    if (!modules) return std::nullopt;
    out.modules = std::move(modules).value();
    auto plan_modules = skirmish::ai_modules(filesystem.value(), out.ai, true);
    expect(static_cast<bool>(plan_modules), "the selected plans load from the FoC view");
    if (!plan_modules) return std::nullopt;
    out.plan_modules = std::move(plan_modules).value();
    for (const auto& type : tables.value().units) out.type_names.emplace(skirmish::type_id(type.id), type.id);
    return out;
}

std::optional<Run> run(const Content& content, std::size_t workers, std::uint64_t ticks) {
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, content.victory,
        content.content.abilities);
    expect(static_cast<bool>(world), "the world is created");
    if (!world) return std::nullopt;
    auto session = foc::create_session(std::move(world).value(), content.ai, content.modules);
    expect(static_cast<bool>(session), "the AI session is created: " + (session ? std::string() : session.error().code + " " + session.error().message));
    if (!session) return std::nullopt;
    std::set<eawr::sim::EntityId> ai_units;
    for (const auto& unit : content.start.units) {
        if (unit.state.owner == 2) ai_units.insert(unit.state.entity_id);
    }
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    Run out;
    const auto began = std::chrono::steady_clock::now();
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.value().step(executor);
        expect(static_cast<bool>(stepped), "step " + std::to_string(tick + 1) + (stepped ? std::string() : ": " + stepped.error().message));
        if (!stepped) return std::nullopt;
        const auto& result = stepped.value();
        out.hashes.push_back(result.state_sha256);
        out.world_hashes.push_back(result.world.state_sha256);
        for (const auto& routed : result.script_input) out.commands.push_back(command_text(routed));
        for (const auto& event : result.world.snapshot->events()) {
            if (event.kind == tactical::EventKind::order_accepted && event.player == 2) {
                const auto instances = result.world.snapshot->instances();
                const auto unit = std::find_if(instances.begin(), instances.end(),
                    [&](const tactical::TacticalInstance& instance) { return instance.entity_id == event.unit; });
                const auto name = unit == instances.end() ? content.type_names.end() : content.type_names.find(unit->type_id);
                ++out.orders_by_type[name == content.type_names.end() ? std::string("(gone)") : name->second];
            }
            out.events.push_back(std::to_string(event.tick) + " " + std::string(tactical::to_string(event.kind)) + " " +
                std::to_string(event.unit));
        }
        for (const auto& event : result.world.snapshot->combat_events()) {
            if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
            if (ai_units.contains(event.shooter)) {
                ++out.ai_hits;
                if (out.first_ai_hit == 0) out.first_ai_hit = event.tick;
            }
            out.hits_csv += std::to_string(event.tick) + "," + std::to_string(event.shooter) + "," +
                std::to_string(event.target) + "," + std::to_string(event.aim.x.trunc_to_integer()) + "," +
                std::to_string(event.aim.y.trunc_to_integer()) + "," + std::to_string(event.aim.z.trunc_to_integer()) + "\n";
        }
        for (const auto& diagnostic : result.scripts.diagnostics) out.diagnostics.insert(diagnostic.code + " " + diagnostic.message);
        std::int64_t tick_load = 0;
        for (const auto& load : result.scripts.loads) tick_load += load.instructions;
        out.max_instructions = std::max(out.max_instructions, tick_load);
        out.total_instructions += tick_load;
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    out.record = session.value().record();
    return out;
}

// #76 (space-abilities.md AB-44): a stand-in freestore that asks each of the AI's units for
// POWER_TO_WEAPONS through the Lua ability calls. The requests reach the world as ability
// commands, the same on 1 and 2 workers, and the record replays them without the script.
void ability_probe(const Content& content) {
    Content probe = content;
    probe.ai.freestore_module = "Data/Scripts/Test/AbilityProbe.lua";
    probe.modules[probe.ai.freestore_module] =
        "function Base_Definitions()\n"
        "  ServiceRate = 1\n"
        "  UnitServiceRate = 1\n"
        "end\n"
        "function main()\n"
        "end\n"
        "function On_Unit_Service(object)\n"
        "  if object.Has_Ability(\"Power_To_Weapons\") and object.Is_Ability_Ready(\"Power_To_Weapons\")\n"
        "      and not object.Is_Ability_Active(\"Power_To_Weapons\") then\n"
        "    object.Activate_Ability(\"Power_To_Weapons\", true)\n"
        "  end\n"
        "end\n";
    const auto one = run(probe, 1, 300);
    const auto two = run(probe, 2, 300);
    expect(one && two && one->hashes == two->hashes, "the ability probe hashes alike on 1 and 2 workers");
    if (!one) return;
    std::size_t requests = 0;
    for (const auto& command : one->record.commands) {
        if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
            requests += ability->ability == tactical::AbilityKind::power_to_weapons
                && ability->action == tactical::AbilityAction::activate ? 1 : 0;
        }
    }
    std::cout << "ability probe: " << requests << " POWER_TO_WEAPONS request(s)\n";
    expect(requests > 0, "the Lua Activate_Ability reaches the world as an ability command");
    auto replayed = eawr::platform::headless_tick_hashes(one->record, content.content.sensors, content.content.durability,
        content.content.motion, content.content.combat, content.victory, content.content.fog,
        content.content.abilities);
    expect(replayed && replayed.value() == one->world_hashes, "the ability probe's record replays without the script");
}

} // namespace

int main(int argc, char** argv) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the FoC AI battle\n";
        return 0;
    }
    const std::uint64_t ticks = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 3600;
    auto content = load(*root);
    if (!content) return 1;

    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, ticks);
        if (!result) return 1;
        std::cout << workers << " worker(s): " << ticks << " ticks in " << result->seconds << " s, final "
                  << result->hashes.back() << '\n';
        runs.push_back(std::move(*result));
    }
    const Run& first = runs.front();
    if (argc > 2) {
        std::ofstream hits(argv[2], std::ios::binary);
        hits << "tick,shooter,target,x,y,z\n" << first.hits_csv;
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == first.hashes, "every tick's combined hash matches worker count 1");
        expect(runs[index].commands == first.commands, "the script commands match worker count 1");
        expect(runs[index].events == first.events, "the world events match worker count 1");
        expect(runs[index].diagnostics == first.diagnostics, "the script diagnostics match worker count 1");
    }

    std::size_t submitted = 0;
    for (const auto& command : first.commands) submitted += command.find(" key ") != std::string::npos ? 1 : 0;
    std::cout << "script commands: " << first.commands.size() << " (" << submitted << " submitted), AI hits: "
              << first.ai_hits << ", world events: " << first.events.size() << '\n';
    std::cout << "AI orders by unit type:";
    for (const auto& [type, count] : first.orders_by_type) std::cout << ' ' << type << '=' << count;
    std::cout << '\n';
    std::cout << "Lua load: max " << first.max_instructions << " instructions in a tick, mean "
              << (first.total_instructions / static_cast<std::int64_t>(ticks)) << " (budget "
              << auth::Quotas{}.instructions_per_service << " per instance)\n";
    for (const auto& diagnostic : first.diagnostics) std::cout << "diagnostic: " << diagnostic << '\n';
    expect(submitted > 0, "the AI orders its units");
    // #452: the AI's Attack_Move and Guard_Target reach the session as the real orders, never dropped.
    std::map<std::string, std::size_t> verbs;
    for (const auto& command : first.commands) {
        const std::string verb = command.substr(0, command.find(' '));
        ++verbs[verb];
        if (verb == foc::verb_attack_move || verb == foc::verb_guard) {
            expect(command.find(" key ") != std::string::npos, "the order is submitted: " + command);
        }
    }
    for (const auto& [verb, count] : verbs) std::cout << "  " << verb << ": " << count << '\n';
    expect(first.ai_hits > 0, "the AI's units hit the player");
    // #518: the squadrons the Empire station and the Acclamator launch (SK-23) are the AI's
    // squadrons like its tick-zero ones: it orders their team containers, never their craft (FH-13).
    const auto ordered = [&first](const char* type) {
        const auto found = first.orders_by_type.find(type);
        return found == first.orders_by_type.end() ? std::uint64_t{0} : found->second;
    };
    expect(ordered("TIE_Fighter_Squadron") + ordered("TIE_Bomber_Squadron") > 0,
           "#518: the AI orders its launched squadrons as squadrons");
    expect(ordered("TIE_Fighter") + ordered("TIE_Bomber") + ordered("TIE_Interceptor") == 0,
           "#518: the AI never orders a squadron's craft on its own");
    expect(first.max_instructions < auth::Quotas{}.instructions_per_service / 4, "the AI's Lua load stays below a quarter of the budget");

    // The record replays without scripts: the world hashes come back from the commands alone.
    const auto replay_began = std::chrono::steady_clock::now();
    auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities);
    const double replay_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - replay_began).count();
    expect(replayed && replayed.value() == first.world_hashes, "the headless replay of the record reproduces the world");
    std::cout << "world alone (headless replay, 1 worker): " << replay_seconds << " s; with the AI: " << first.seconds
              << " s\n";

    // The live session hosts the same AI on its simulation thread (driven pacing, 4 workers).
    eawr::platform::LiveSession::Options options;
    options.workers = 4;
    options.pacing = eawr::platform::LiveSession::Pacing::driven;
    auto scripts = std::make_shared<eawr::platform::LiveScripts>();
    scripts->wrap = [&content](tactical::TacticalSession world) {
        return foc::create_session(std::move(world), content->ai, content->modules);
    };
    options.scripts = scripts;
    auto live = eawr::platform::LiveSession::start(content->start.setup, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, options, content->victory, content->content.fog, content->content.abilities);
    expect(static_cast<bool>(live), "the live session starts with the AI");
    if (live) {
        live.value()->advance_to(ticks);
        expect(live.value()->wait_for(ticks, std::chrono::minutes(10)), "the live session reaches the last tick");
        live.value()->stop();
        expect(live.value()->tick_hashes() == first.world_hashes, "the live session's hashes are the AI run's world hashes");
        const auto report = live.value()->script_report();
        expect(report.ticks == ticks && report.max_instructions == first.max_instructions,
               "the live session reports the same Lua load");
    }

    // #459 with the AI: a real-time battle paused mid-attack and resumed on fast forward
    // (TM-07, TP-02). The freestore services inside the tick, on sim seconds (FH-11), so it
    // pauses with the sim, never runs while paused, and services per tick at any rate; its
    // commands keep their next-tick keys. The replay equals an unpaused run's.
    const std::uint64_t pause_tick = first.first_ai_hit + 30;
    const std::uint64_t halt = pause_tick + 150;
    expect(first.first_ai_hit > 0 && halt <= ticks, "the AI attacks early enough to pause mid-attack");
    const auto with_ai = [&](eawr::platform::LiveSession::Pacing pacing, std::uint32_t rate) {
        eawr::platform::LiveSession::Options live_options = options;
        live_options.pacing = pacing;
        live_options.target_rate = rate;
        return eawr::platform::LiveSession::start(content->start.setup, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, live_options, content->victory,
            content->content.fog, content->content.abilities);
    };
    auto paused = with_ai(eawr::platform::LiveSession::Pacing::real_time, 1000);
    auto unpaused = with_ai(eawr::platform::LiveSession::Pacing::driven, 30);
    expect(paused && unpaused, "the paused and unpaused AI sessions start");
    if (first.first_ai_hit > 0 && halt <= ticks && paused && unpaused) {
        using namespace std::chrono_literals;
        auto& session = *paused.value();
        session.halt_at(halt);
        // The approach at the highest rate, then fast forward (TM-06) into the attack.
        expect(session.wait_for(first.first_ai_hit > 60 ? first.first_ai_hit - 60 : 0, std::chrono::minutes(5)),
               "the paused run reaches the approach");
        session.set_target_rate(120);
        expect(session.wait_for(pause_tick, std::chrono::minutes(5)), "the paused run reaches the attack");
        session.set_paused(true);
        std::this_thread::sleep_for(100ms);
        const std::uint64_t paused_at = session.completed_tick();
        const auto services = session.script_report().ticks;
        const auto frame = session.frame().latest;
        expect(paused_at >= pause_tick && paused_at < halt, "the battle pauses mid-attack");
        expect(services == paused_at, "the AI serviced every tick up to the pause");
        std::this_thread::sleep_for(500ms);
        expect(session.completed_tick() == paused_at, "no tick runs while the AI battle is paused");
        expect(session.script_report().ticks == services, "the AI never services while paused");
        expect(session.frame().latest == frame, "the paused battle publishes no new frame");
        session.set_paused(false);
        expect(session.wait_for(halt, std::chrono::minutes(5)), "the resumed run reaches the halt tick");
        expect(!session.wait_for(halt + 1, 300ms), "the resumed run stops at the halt tick");
        session.stop();

        unpaused.value()->advance_to(halt);
        expect(unpaused.value()->wait_for(halt, std::chrono::minutes(5)), "the unpaused run reaches the halt tick");
        unpaused.value()->stop();

        expect(!session.failure() && !unpaused.value()->failure(), "neither AI run failed");
        const auto hashes = session.tick_hashes();
        expect(hashes.size() == halt && std::equal(hashes.begin(), hashes.end(), first.world_hashes.begin()),
               "pausing mid-attack leaves every world hash unchanged");
        expect(hashes == unpaused.value()->tick_hashes(), "the paused and unpaused runs hash alike");
        expect(session.script_report().ticks == halt
                   && session.script_report().max_instructions == unpaused.value()->script_report().max_instructions
                   && session.script_report().total_instructions == unpaused.value()->script_report().total_instructions,
               "the AI serviced once per tick on fast forward, as unpaused");
        const auto replay = session.record();
        const auto plain_replay = unpaused.value()->record();
        const auto bytes = tactical::write_replay(replay);
        const auto plain_bytes = tactical::write_replay(plain_replay);
        expect(bytes && plain_bytes && bytes.value() == plain_bytes.value(),
               "the paused run's replay equals the unpaused run's");
        std::size_t around_pause = 0;
        for (const auto& command : replay.commands) {
            if (command.key.tick > paused_at && command.key.tick <= paused_at + 60) ++around_pause;
        }
        std::cout << "paused at tick " << paused_at << " (first AI hit " << first.first_ai_hit << "), halted at "
                  << halt << ", " << replay.commands.size() << " commands, " << around_pause
                  << " in the 60 ticks after the pause\n";
    }

    // The plans step: every selected plan loads and runs its definition load through the same
    // bindings; the goal system that would start them is not hosted (#78 trigger).
    auto files = content->plan_modules;
    auto plans = foc::inspect_plans(content->ai, files);
    expect(static_cast<bool>(plans), "the selected plans are inspected");
    if (plans) {
        expect(plans.value().size() == 17, "AI-30: 17 selected plans");
        for (const auto& plan : plans.value()) {
            std::cout << "plan " << plan.path << ": category " << (plan.category.empty() ? "(none)" : plan.category)
                      << (plan.loaded ? "" : " NOT LOADED") << '\n';
            for (const auto& diagnostic : plan.diagnostics) std::cout << "  " << diagnostic << '\n';
            expect(plan.loaded && !plan.category.empty(), "the plan " + plan.path + " loads and names its goal category");
        }
    }
    for (const auto& line : foc::unsupported_plan_calls()) std::cout << "unsupported: " << line << '\n';

    ability_probe(*content);

    if (failures != 0) {
        std::cerr << failures << " FoC AI check(s) failed\n";
        return 1;
    }
    std::cout << "FoC AI contracts passed\n";
    return 0;
}
