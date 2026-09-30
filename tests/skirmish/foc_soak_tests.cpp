// Long FoC-AI battles on the M2 start with the AI on both sides (#615, #627): the Rebel slot plays
// as a lobby AI too, the goal system runs, and the battle's dogfights and fleet actions go on for
// `--ticks` frames or until a side wins. A seed passes when
//   - no tick fails (EAWR-SIM-*),
//   - the invariants of soak_invariants.hpp hold after every tick (hull overlap, squadron slot
//     crowding, positions inside the map, no unit faster than its type),
//   - the battle ends or reaches the cap, and
//   - with --hash-workers, a second run of the seed at that many simulation workers reaches the
//     same state hash on every tick (results identical for any worker count).
// A failing seed keeps its summary and a replay file; one bad seed, even one that throws, does not
// end the run. The nightly soak (tools/soak) runs this with an output directory.
//
//   foc_soak_tests [--seeds 12,18,40-47] [--ticks 9000] [--battles N] [--workers N]
//                  [--hash-workers N] [--hash-every N] [--out DIR] [--strict] [--time-budget S]
//
// See soak_options.hpp for every flag. Needs EAWR_EAW_GAME_ROOT: skipped (exit 0) without it, an
// error (exit 2) under --strict. Exit 1 when a seed failed.

#include "m2_battle.hpp"
#include "soak_invariants.hpp"
#include "soak_json.hpp"
#include "soak_options.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace foc = eawr::script::foc;
namespace soak = eawr::soak;
using Clock = std::chrono::steady_clock;

std::mutex output_mutex;

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

// One thing that failed a seed: a step, an invariant, the worker comparison or the run itself.
struct Failure {
    std::string kind;   // sim-failure, invariant, worker-divergence, not-decided, setup, exception
    std::string rule;   // the invariant, else the kind
    std::string guards; // the behaviour rule it guards
    std::uint64_t tick{};
    std::string units;
    std::string message;
};

struct SeedResult {
    std::uint64_t seed{};
    bool ran{};
    std::vector<Failure> failures;
    std::string outcome; // ended, capped
    std::uint64_t ticks_run{};
    std::optional<tactical::BattleOutcome> decided;
    std::string final_hash;
    double seconds{};
    double slowest_seconds{};
    std::uint64_t slowest_tick{};
    soak::Metrics metrics;
    std::size_t dropped_violations{};
    bool compared{};
    std::uint64_t compared_ticks{};
    std::string replay_file;
    std::string replay_error;
    [[nodiscard]] bool passed() const noexcept { return ran && failures.empty(); }
};

using soak::Battle;
using soak::Loaded;

std::string utc_time(const std::chrono::system_clock::time_point at) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    std::ostringstream text;
    text << std::put_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
    return text.str();
}

void write_replay_file(const std::filesystem::path& path, const tactical::TacticalReplay& replay, SeedResult& result) {
    auto bytes = tactical::write_replay(replay);
    if (!bytes) {
        result.replay_error = bytes.error().code + ' ' + bytes.error().message;
        return;
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
    if (!file) {
        result.replay_error = "the replay file could not be written";
        return;
    }
    result.replay_file = path.filename().string();
}

// One seed's battle. Failures go into the result; an exception is the caller's to contain.
void run_battle(const Loaded& loaded, const soak::Options& options, const std::uint64_t seed, const bool compare,
    SeedResult& result) {
    auto primary_built = soak::build_battle(loaded, {.seed = seed});
    if (const auto* reason = std::get_if<std::string>(&primary_built)) {
        result.failures.push_back({"setup", "setup", "the M2 start builds", 0, {}, *reason});
        return;
    }
    auto& primary = *std::get<std::unique_ptr<Battle>>(primary_built);
    std::unique_ptr<Battle> secondary_built;
    if (compare) {
        auto second = soak::build_battle(loaded, {.seed = seed});
        if (const auto* reason = std::get_if<std::string>(&second)) {
            result.failures.push_back({"setup", "setup", "the M2 start builds", 0, {}, *reason});
            return;
        }
        secondary_built = std::move(std::get<std::unique_ptr<Battle>>(second));
    }
    result.compared = compare;
    const eawr::platform::ThreadWorkerAdapter executor(options.workers);
    const eawr::platform::ThreadWorkerAdapter compare_executor(compare ? options.hash_workers : std::size_t{1});
    soak::Invariants invariants(primary.content.motion, primary.content.fog, options.limits);
    auto& session = *primary.session;
    bool sim_failed = false;
    bool diverged = false;
    const auto began = Clock::now();
    for (std::uint64_t tick = 0; tick < options.ticks; ++tick) {
        const auto stepping = Clock::now();
        auto stepped = session.step(executor);
        const double took = std::chrono::duration<double>(Clock::now() - stepping).count();
        if (took > result.slowest_seconds) {
            result.slowest_seconds = took;
            result.slowest_tick = tick + 1;
        }
        if (!stepped) {
            // The world is at completed tick `tick`; the step that would complete tick + 1 failed.
            result.failures.push_back({"sim-failure", stepped.error().code,
                "the tick steps without a simulation failure", tick + 1, {},
                "step " + std::to_string(tick + 1) + " (world at completed tick " + std::to_string(tick) + "): "
                    + stepped.error().code + ' ' + stepped.error().message});
            sim_failed = true;
            break;
        }
        result.ticks_run = tick + 1;
        result.final_hash = stepped.value().state_sha256;
        if (compare) {
            auto other = secondary_built->session->step(compare_executor);
            if (!other || other.value().state_sha256 != stepped.value().state_sha256) {
                std::string message = "the run at " + std::to_string(options.workers) + " worker(s) and the run at "
                    + std::to_string(options.hash_workers) + " worker(s) differ at completed tick "
                    + std::to_string(tick + 1) + ": ";
                message += other ? stepped.value().state_sha256 + " against " + other.value().state_sha256
                                 : "the second run failed: " + other.error().code + ' ' + other.error().message;
                result.failures.push_back({"worker-divergence", "worker-divergence",
                    "results are identical for 1/2/4/8 workers (docs/simulation.md, ADR-009)", tick + 1, {}, message});
                diverged = true;
                break;
            }
            result.compared_ticks = tick + 1;
        }
        if (options.invariants && (tick + 1) % options.check_every == 0) {
            const auto units = session.world().units();
            invariants.observe(tick + 1, units, session.world().squadrons());
        }
        const auto& decided = session.world().outcome();
        if (decided && tick + 1 >= decided->end_tick) {
            result.decided = decided;
            result.outcome = "ended";
            break;
        }
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - began).count();
    if (!sim_failed && !diverged && result.outcome.empty()) {
        result.outcome = "capped";
        result.decided = session.world().outcome();
    }
    result.metrics = invariants.metrics();
    result.dropped_violations = invariants.dropped();
    for (const auto& violation : invariants.violations()) {
        result.failures.push_back({"invariant", violation.rule, violation.guards, violation.tick, violation.units,
            violation.message});
    }
    if (options.require_decision && !sim_failed && !diverged && result.outcome == "capped" && !result.decided) {
        result.failures.push_back({"not-decided", "not-decided", "every battle ends or reaches the cap: none was decided",
            result.ticks_run, {}, "the battle reached the cap of " + std::to_string(options.ticks) + " ticks undecided"});
    }
    if (!result.failures.empty() && options.out) {
        const auto path = *options.out / ("seed-" + std::to_string(seed) + ".eawr-replay");
        write_replay_file(path, sim_failed ? session.world().record_through_next_tick() : session.world().record(),
            result);
    }
}

std::string failure_json(const Failure& failure) {
    std::ostringstream text;
    text << "{\"kind\":" << soak::json_string(failure.kind) << ",\"rule\":" << soak::json_string(failure.rule)
         << ",\"guards\":" << soak::json_string(failure.guards) << ",\"tick\":" << failure.tick
         << ",\"units\":" << soak::json_string(failure.units) << ",\"message\":" << soak::json_string(failure.message)
         << '}';
    return text.str();
}

std::string seed_json(const soak::Options& options, const SeedResult& result) {
    std::ostringstream text;
    text << "{\"schema\":1,\"seed\":" << result.seed << ",\"status\":"
         << soak::json_string(!result.ran ? "not-run" : result.passed() ? "passed" : "failed");
    if (result.ran) {
        text << ",\"outcome\":" << soak::json_string(result.outcome) << ",\"ticks\":" << result.ticks_run
             << ",\"cap\":" << options.ticks << ",\"seconds\":" << soak::json_number(result.seconds)
             << ",\"slowestTick\":{\"tick\":" << result.slowest_tick
             << ",\"seconds\":" << soak::json_number(result.slowest_seconds) << "},\"finalHash\":"
             << soak::json_string(result.final_hash) << ",\"workers\":" << options.workers;
        if (result.decided) {
            text << ",\"battle\":{\"winner\":" << result.decided->winner << ",\"decidedTick\":"
                 << result.decided->decided_tick << ",\"endTick\":" << result.decided->end_tick << '}';
        }
        if (result.compared) {
            text << ",\"workerComparison\":{\"workers\":" << options.hash_workers << ",\"ticksCompared\":"
                 << result.compared_ticks << '}';
        }
        text << ",\"metrics\":{\"maxHullPenetration\":" << soak::json_number(result.metrics.max_hull_penetration)
             << ",\"maxHullRun\":" << result.metrics.max_hull_run << ",\"maxSlotRun\":" << result.metrics.max_slot_run
             << ",\"maxMapExcess\":" << soak::json_number(result.metrics.max_map_excess)
             << ",\"maxSpeedRatio\":" << soak::json_number(result.metrics.max_speed_ratio)
             << ",\"maxCraftSpeedRatio\":" << soak::json_number(result.metrics.max_craft_speed_ratio)
             << ",\"ticksObserved\":" << result.metrics.ticks_observed << '}';
        text << ",\"failures\":[";
        for (std::size_t at = 0; at < result.failures.size(); ++at) {
            text << (at == 0 ? "" : ",") << failure_json(result.failures[at]);
        }
        text << "],\"droppedViolations\":" << result.dropped_violations;
        if (!result.passed()) {
            text << ",\"reproduce\":" << soak::json_string("foc_soak_tests " + soak::reproduction_flags(options, result.seed)
                    + " --strict")
                 << ",\"replay\":" << soak::json_string(result.replay_file);
            if (!result.replay_file.empty()) {
                text << ",\"replayCommand\":"
                     << soak::json_string("sim_headless --replay " + result.replay_file
                            + " --hash-out replay.hash --game-root <game root>");
            }
            if (!result.replay_error.empty()) text << ",\"replayError\":" << soak::json_string(result.replay_error);
        }
    }
    text << "}\n";
    return text.str();
}

bool write_text(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    file << content;
    return static_cast<bool>(file);
}

std::string summary_json(const soak::Options& options, const std::vector<SeedResult>& results, const double seconds,
    const std::string& started) {
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t not_run = 0;
    std::size_t ended = 0;
    const SeedResult* slowest = nullptr;
    for (const auto& result : results) {
        if (!result.ran) {
            ++not_run;
            continue;
        }
        if (result.passed()) {
            ++passed;
        } else {
            ++failed;
        }
        if (result.outcome == "ended") ++ended;
        if (slowest == nullptr || result.slowest_seconds > slowest->slowest_seconds) slowest = &result;
    }
    std::ostringstream text;
    text << "{\"schema\":1,\"tool\":\"foc_soak_tests\",\"started\":" << soak::json_string(started)
         << ",\"seeds\":" << results.size() << ",\"passed\":" << passed << ",\"failed\":" << failed
         << ",\"notRun\":" << not_run << ",\"ended\":" << ended << ",\"capped\":" << (passed + failed - ended)
         << ",\"ticks\":" << options.ticks << ",\"workers\":" << options.workers
         << ",\"hashWorkers\":" << options.hash_workers << ",\"battles\":" << options.battles
         << ",\"totalSeconds\":" << soak::json_number(seconds);
    if (slowest != nullptr) {
        text << ",\"slowestTick\":{\"seed\":" << slowest->seed << ",\"tick\":" << slowest->slowest_tick
             << ",\"seconds\":" << soak::json_number(slowest->slowest_seconds) << '}';
    }
    text << ",\"limits\":{\"spawnWindow\":" << options.limits.spawn_window << ",\"hullPenetration\":"
         << soak::json_number(options.limits.hull_penetration) << ",\"hullTicks\":" << options.limits.hull_ticks
         << ",\"slotRadius\":" << soak::json_number(options.limits.slot_radius) << ",\"slotTicks\":"
         << options.limits.slot_ticks << ",\"mapMargin\":" << soak::json_number(options.limits.map_margin)
         << ",\"speedFactor\":" << soak::json_number(options.limits.speed_factor) << ",\"craftSpeedFactor\":"
         << soak::json_number(options.limits.craft_speed_factor) << "},\"results\":[";
    bool first = true;
    for (const auto& result : results) {
        text << (first ? "" : ",") << "{\"seed\":" << result.seed << ",\"status\":"
             << soak::json_string(!result.ran ? "not-run" : result.passed() ? "passed" : "failed");
        if (result.ran) {
            text << ",\"outcome\":" << soak::json_string(result.outcome) << ",\"ticks\":" << result.ticks_run
                 << ",\"seconds\":" << soak::json_number(result.seconds);
            if (!result.failures.empty()) {
                text << ",\"kind\":" << soak::json_string(result.failures.front().kind) << ",\"tick\":"
                     << result.failures.front().tick << ",\"replay\":" << soak::json_string(result.replay_file);
            }
        }
        text << '}';
        first = false;
    }
    text << "]}\n";
    return text.str();
}

void print_result(const soak::Options& options, const SeedResult& result) {
    const std::lock_guard lock(output_mutex);
    if (!result.ran) {
        std::cout << "seed " << result.seed << ": not run (the time budget ended)" << std::endl;
        return;
    }
    std::cout << "seed " << result.seed << ": ";
    if (result.passed()) {
        std::cout << result.ticks_run << " ticks, battle " << result.outcome << ", " << std::fixed
                  << std::setprecision(1) << result.seconds << " s" << (result.compared ? ", workers agree" : "")
                  << std::endl;
        return;
    }
    const auto& first = result.failures.front();
    std::cout << "FAILED " << first.kind << " at tick " << first.tick << ": " << first.message;
    if (first.kind == "invariant") std::cout << " [" << first.rule << ": " << first.units << ']';
    if (result.failures.size() > 1) std::cout << " (+" << (result.failures.size() - 1) << " more)";
    std::cout << "\n  reproduce: foc_soak_tests " << soak::reproduction_flags(options, result.seed) << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    auto parsed = soak::parse_options(arguments);
    if (const auto* reason = std::get_if<std::string>(&parsed)) {
        std::cerr << "foc_soak_tests: " << *reason << "\nusage: foc_soak_tests [--seeds 12,18,40-47] [--ticks N] "
                  << "[--battles N] [--workers N] [--hash-workers N] [--hash-every N] [--out DIR] [--strict] "
                  << "[--time-budget SECONDS] (see tests/skirmish/soak_options.hpp)\n";
        return 2;
    }
    const auto options = std::get<soak::Options>(std::move(parsed));

    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        if (options.strict) {
            std::cerr << "foc_soak_tests: --strict: EAWR_EAW_GAME_ROOT is not set, so nothing was soaked\n";
            return 2;
        }
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the FoC soak battle\n";
        return 0;
    }
    std::error_code directory_error;
    if (options.strict && !std::filesystem::is_directory(std::filesystem::path(*root) / "corruption" / "Data", directory_error)) {
        std::cerr << "foc_soak_tests: --strict: EAWR_EAW_GAME_ROOT has no corruption/Data folder\n";
        return 2;
    }
    if (options.out) {
        std::filesystem::create_directories(*options.out, directory_error);
        if (directory_error || !std::filesystem::is_directory(*options.out)) {
            std::cerr << "foc_soak_tests: --out: cannot create the directory: " << directory_error.message() << '\n';
            return 2;
        }
    }

    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        if (!manifest) {
            std::cerr << "foc_soak_tests: the FoC " << folder << " layer does not mount: " << manifest.error().code << ' '
                      << manifest.error().message << '\n';
            return 1;
        }
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) {
        std::cerr << "foc_soak_tests: the FoC vfs does not mount: " << filesystem.error().message << '\n';
        return 1;
    }
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    if (!catalog) {
        std::cerr << "foc_soak_tests: the FoC catalog does not load: " << catalog.error().message << '\n';
        return 1;
    }
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto tables = eawr::units::load_unit_tables(input);
    if (!tables) {
        std::cerr << "foc_soak_tests: the FoC unit tables do not load: " << tables.error().message << '\n';
        return 1;
    }
    const Loaded loaded{filesystem.value(), catalog.value().catalog, tables.value()};

    const auto started_at = std::chrono::system_clock::now();
    const auto began = Clock::now();
    std::vector<SeedResult> results(options.seeds.size());
    for (std::size_t at = 0; at < results.size(); ++at) results[at].seed = options.seeds[at];
    std::atomic<std::size_t> next = 0;
    std::vector<std::thread> pool;
    for (std::size_t index = 0; index < std::min(options.battles, options.seeds.size()); ++index) {
        pool.emplace_back([&] {
            for (auto at = next++; at < results.size(); at = next++) {
                auto& result = results[at];
                if (options.time_budget > 0.0
                    && std::chrono::duration<double>(Clock::now() - began).count() >= options.time_budget) {
                    print_result(options, result);
                    continue;
                }
                result.ran = true;
                const bool compare = options.hash_workers != 0 && at % options.hash_every == 0;
                try {
                    run_battle(loaded, options, result.seed, compare, result);
                } catch (const std::exception& error) {
                    result.failures.push_back({"exception", "exception", "a seed's battle runs to its end", result.ticks_run,
                        {}, std::string("an exception ended the seed: ") + error.what()});
                } catch (...) {
                    result.failures.push_back({"exception", "exception", "a seed's battle runs to its end", result.ticks_run,
                        {}, "an unknown exception ended the seed"});
                }
                if (options.out) {
                    if (!write_text(*options.out / ("seed-" + std::to_string(result.seed) + ".json"), seed_json(options, result))) {
                        const std::lock_guard lock(output_mutex);
                        std::cerr << "foc_soak_tests: cannot write the summary of seed " << result.seed << '\n';
                    }
                }
                print_result(options, result);
            }
        });
    }
    for (auto& thread : pool) thread.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - began).count();

    std::size_t failed = 0;
    std::size_t not_run = 0;
    for (const auto& result : results) {
        if (!result.ran) {
            ++not_run;
        } else if (!result.passed()) {
            ++failed;
        }
    }
    if (options.out) {
        if (!write_text(*options.out / "summary.json", summary_json(options, results, seconds, utc_time(started_at)))) {
            std::cerr << "foc_soak_tests: cannot write summary.json\n";
            return 2;
        }
    }
    if (failed != 0) {
        std::cerr << failed << " failure(s) in " << results.size() << " seed(s)\n";
        return 1;
    }
    std::cout << "foc soak: " << (results.size() - not_run) << " seed(s) ran up to " << options.ticks << " ticks in "
              << std::fixed << std::setprecision(1) << seconds << " s" << (not_run != 0 ? ", " + std::to_string(not_run) + " not run" : std::string())
              << std::endl;
    return 0;
}
