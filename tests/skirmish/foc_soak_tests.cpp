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
#include <set>
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

// #957: a tick phase's wall-clock time over a seed's ticks, in milliseconds.
struct PhaseStats {
    double mean{};
    double p99{};
    double worst{};
    std::uint64_t worst_tick{};
};
struct TimingStats {
    PhaseStats total, world, ai, lua;
    bool present{};
};

struct SeedResult {
    std::uint64_t seed{};
    TimingStats timing;
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

PhaseStats phase_stats(const std::vector<double>& samples) {
    PhaseStats stats;
    if (samples.empty()) return stats;
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (std::size_t at = 0; at < samples.size(); ++at) {
        sum += samples[at];
        if (samples[at] > stats.worst) {
            stats.worst = samples[at];
            stats.worst_tick = at + 1;
        }
    }
    stats.mean = sum / static_cast<double>(samples.size());
    stats.p99 = sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(static_cast<double>(sorted.size()) * 0.99))];
    return stats;
}

foc::AiSchedule schedule_of(const soak::Options& options) {
    foc::AiSchedule schedule;
    if (options.ai_faithful) {
        schedule.mode = *options.ai_faithful ? foc::AiSchedule::Mode::faithful : foc::AiSchedule::Mode::staggered;
    }
    if (options.ai_attach_cap) schedule.attach_per_tick = *options.ai_attach_cap;
    return schedule;
}

std::uint64_t steady_nanoseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
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

// #957 (SCH-04): the most work of one kind any tick of a seed's battle does, counted by the AI's
// journal (deterministic: no clock), under one schedule.
struct ScheduleCounts {
    std::uint32_t max_attached{};
    std::uint32_t max_maintenances{};
    std::uint32_t max_deferred{};
    std::uint64_t attached{};
    std::uint64_t ticks_with_attach{};
};

std::variant<ScheduleCounts, std::string> count_schedule(const Loaded& loaded, const soak::Options& options,
    const std::uint64_t seed, const foc::AiSchedule& schedule) {
    auto journal = std::make_shared<foc::AiJournal>();
    auto built = soak::build_battle(loaded, {.seed = seed, .anonymous_content = false, .schedule = schedule, .journal = journal});
    if (const auto* reason = std::get_if<std::string>(&built)) return *reason;
    auto& battle = *std::get<std::unique_ptr<Battle>>(built);
    const eawr::platform::ThreadWorkerAdapter executor(options.workers);
    for (std::uint64_t tick = 0; tick < options.ticks; ++tick) {
        auto stepped = battle.session->step(executor);
        if (!stepped) return "step " + std::to_string(tick + 1) + ": " + stepped.error().code + ' ' + stepped.error().message;
    }
    ScheduleCounts counts;
    for (const auto& cost : journal->costs) {
        counts.max_attached = std::max(counts.max_attached, cost.plans_attached);
        counts.max_maintenances = std::max(counts.max_maintenances, cost.maintenances);
        counts.max_deferred = std::max(counts.max_deferred, cost.plans_deferred);
        counts.attached += cost.plans_attached;
        counts.ticks_with_attach += cost.plans_attached != 0 ? 1 : 0;
    }
    return counts;
}

// #957 (SCH-02): the frames each AI player's goal, planning and execution services ran on, from the
// journal's service rows, for a battle of two AI players or (extra = 1) three.
std::variant<std::vector<foc::AiServiceEvent>, std::string> service_frames(const Loaded& loaded,
    const soak::Options& options, const std::uint64_t seed, const foc::AiSchedule& schedule, const std::uint32_t extra,
    const std::uint64_t ticks) {
    auto journal = std::make_shared<foc::AiJournal>();
    auto built = soak::build_battle(loaded,
        {.seed = seed, .anonymous_content = false, .schedule = schedule, .journal = journal, .extra_ai_players = extra});
    if (const auto* reason = std::get_if<std::string>(&built)) return *reason;
    auto& battle = *std::get<std::unique_ptr<Battle>>(built);
    const eawr::platform::ThreadWorkerAdapter executor(options.workers);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = battle.session->step(executor);
        if (!stepped) return "step " + std::to_string(tick + 1) + ": " + stepped.error().code + ' ' + stepped.error().message;
    }
    return journal->services;
}

// SCH-02 as a regression test: from the first service on, the staggered schedule never runs two AI
// players' planning services, or their execution services, on one frame, and no two start their
// goal service on one frame; the faithful schedule runs every player's services together, on the
// same frames the staggered players' services recur on.
void check_phases(const Loaded& loaded, const soak::Options& options, const std::uint64_t seed, SeedResult& result) {
    const auto fail = [&](const std::string& rule, const std::string& message) {
        result.failures.push_back({"schedule", rule, "the AI players' services are staggered from the first service on (SCH-02)",
            options.ticks, {}, message});
    };
    constexpr std::uint64_t ticks = 60;
    foc::AiSchedule staggered = schedule_of(options);
    staggered.mode = foc::AiSchedule::Mode::staggered;
    foc::AiSchedule faithful = staggered;
    faithful.mode = foc::AiSchedule::Mode::faithful;
    for (const std::uint32_t extra : {0U, 1U}) {
        const std::size_t players = 2 + extra;
        const std::string label = std::to_string(players) + " AI players";
        const auto spread = service_frames(loaded, options, seed, staggered, extra, ticks);
        const auto together = service_frames(loaded, options, seed, faithful, extra, ticks);
        if (const auto* reason = std::get_if<std::string>(&spread)) return fail("phase-setup", label + ", staggered: " + *reason);
        if (const auto* reason = std::get_if<std::string>(&together)) return fail("phase-setup", label + ", faithful: " + *reason);
        const auto& a = std::get<std::vector<foc::AiServiceEvent>>(spread);
        const auto& b = std::get<std::vector<foc::AiServiceEvent>>(together);
        if (a.empty() || b.empty()) return fail("vacuous", label + ": no AI service ran, so the check measured nothing");
        // The services of one kind at each frame, and each player's first frame of each kind.
        using Kind = bool foc::AiServiceEvent::*;
        const auto per_frame = [](const std::vector<foc::AiServiceEvent>& rows, const Kind kind) {
            std::map<std::uint64_t, std::vector<std::uint32_t>> frames;
            for (const auto& row : rows) {
                if (row.*kind) frames[row.frame].push_back(row.player);
            }
            return frames;
        };
        const auto first_of = [](const std::vector<foc::AiServiceEvent>& rows, const Kind kind) {
            std::map<std::uint32_t, std::uint64_t> first;
            for (const auto& row : rows) {
                if (row.*kind) first.emplace(row.player, row.frame);
            }
            return first;
        };
        const std::uint64_t origin = std::min(a.front().frame, b.front().frame);
        if (a.front().frame != b.front().frame) {
            fail("origin", label + ": the schedules' first service frames differ (" + std::to_string(a.front().frame) + " and "
                    + std::to_string(b.front().frame) + ")");
        }
        const std::pair<const char*, Kind> kinds[] = {{"goal", &foc::AiServiceEvent::goals},
            {"planning", &foc::AiServiceEvent::planning}, {"execution", &foc::AiServiceEvent::execution}};
        for (const auto& [name, kind] : kinds) {
            // Staggered: every player has its first service, on distinct frames, and (planning and
            // execution) no frame in the run carries two players.
            const auto first = first_of(a, kind);
            std::set<std::uint64_t> starts;
            for (const auto& [player, frame] : first) starts.insert(frame);
            if (first.size() != players) fail("phase-missing", label + ": a player never ran its " + name + " service");
            if (starts.size() != first.size()) {
                fail("phase-shared", label + ": two players start their " + name + " service on one frame (staggered)");
            }
            if (kind != &foc::AiServiceEvent::goals) {
                for (const auto& [frame, who] : per_frame(a, kind)) {
                    if (who.size() > 1) {
                        fail("phase-shared", label + ": " + std::to_string(who.size()) + " players ran their " + name
                                + " service on frame " + std::to_string(frame) + " (staggered)");
                        break;
                    }
                }
            }
            // Faithful: all players, every time.
            for (const auto& [frame, who] : per_frame(b, kind)) {
                if (who.size() != players) {
                    fail("faithful", label + ": a " + name + " service on frame " + std::to_string(frame)
                            + " did not run for every player (faithful)");
                    break;
                }
            }
            const auto together_first = first_of(b, kind);
            for (const auto& [player, frame] : together_first) {
                if (frame != origin) fail("faithful", label + ": a player's first " + name + " service is not on the first frame (faithful)");
            }
        }
        // The first planning frame of the player at position 0 is the origin, as in the faithful schedule.
        const auto planning = first_of(a, &foc::AiServiceEvent::planning);
        if (!planning.empty() && std::min_element(planning.begin(), planning.end(), [](const auto& l, const auto& r) {
                return l.second < r.second;
            })->second != origin) {
            fail("origin", label + ": no player's first planning service is on the first frame");
        }
        std::cout << "seed " << seed << " phases (" << label << "): first planning frames";
        for (const auto& [player, frame] : planning) std::cout << " p" << player << "@" << frame;
        std::cout << ", first goal frames";
        for (const auto& [player, frame] : first_of(a, &foc::AiServiceEvent::goals)) std::cout << " p" << player << "@" << frame;
        std::cout << std::endl;
    }
}

// The work budget as a deterministic regression test (#957): under the staggered schedule no tick
// attaches more plans than its cap, and the faithful schedule on the same seed does (so the check
// measures something).
void check_schedule(const Loaded& loaded, const soak::Options& options, const std::uint64_t seed, SeedResult& result) {
    const auto fail = [&](const std::string& rule, const std::string& message) {
        result.failures.push_back({"schedule", rule, "the AI's scheduled work per tick stays within its cap (SCH-04)",
            options.ticks, {}, message});
    };
    foc::AiSchedule staggered = schedule_of(options);
    staggered.mode = foc::AiSchedule::Mode::staggered;
    foc::AiSchedule faithful = staggered;
    faithful.mode = foc::AiSchedule::Mode::faithful;
    const auto spread = count_schedule(loaded, options, seed, staggered);
    const auto together = count_schedule(loaded, options, seed, faithful);
    if (const auto* reason = std::get_if<std::string>(&spread)) return fail("schedule-setup", "staggered run: " + *reason);
    if (const auto* reason = std::get_if<std::string>(&together)) return fail("schedule-setup", "faithful run: " + *reason);
    const auto& a = std::get<ScheduleCounts>(spread);
    const auto& b = std::get<ScheduleCounts>(together);
    std::cout << "seed " << seed << " schedule: staggered max " << a.max_attached << " attaches and " << a.max_maintenances
              << " maintenances a tick (" << a.attached << " plans over " << a.ticks_with_attach << " ticks), faithful max "
              << b.max_attached << " and " << b.max_maintenances << " (" << b.attached << " plans over " << b.ticks_with_attach
              << " ticks)" << std::endl;
    if (a.max_attached > staggered.attach_per_tick) {
        fail("attach-cap", "a staggered tick attached " + std::to_string(a.max_attached) + " plans, over the cap of "
                + std::to_string(staggered.attach_per_tick));
    }
    if (a.attached == 0) fail("vacuous", "the staggered run attached no plan: the check measured nothing");
    if (b.max_attached <= staggered.attach_per_tick) {
        fail("vacuous", "the faithful run never attached more than " + std::to_string(staggered.attach_per_tick)
                + " plan(s) in a tick, so the cap is not tested");
    }
}

// One seed's battle. Failures go into the result; an exception is the caller's to contain.
void run_battle(const Loaded& loaded, const soak::Options& options, const std::uint64_t seed, const bool compare,
    SeedResult& result) {
    const auto journal = options.tick_trace ? std::make_shared<foc::AiJournal>() : std::shared_ptr<foc::AiJournal>();
    if (options.check_schedule) {
        result.ticks_run = options.ticks;
        result.outcome = "capped";
        check_schedule(loaded, options, seed, result);
        check_phases(loaded, options, seed, result);
        return;
    }
    auto primary_built = soak::build_battle(loaded, {.seed = seed, .anonymous_content = false, .schedule = schedule_of(options), .journal = journal});
    if (const auto* reason = std::get_if<std::string>(&primary_built)) {
        result.failures.push_back({"setup", "setup", "the M2 start builds", 0, {}, *reason});
        return;
    }
    auto& primary = *std::get<std::unique_ptr<Battle>>(primary_built);
    std::unique_ptr<Battle> secondary_built;
    if (compare) {
        auto second = soak::build_battle(loaded, {.seed = seed, .anonymous_content = false, .schedule = schedule_of(options), .journal = nullptr});
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
    session.set_step_clock(steady_nanoseconds);
    std::vector<double> total_ms, world_ms, ai_ms, lua_ms;
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
        {
            const auto& timing = stepped.value().timing;
            const auto ms = [](const std::uint64_t ns) { return static_cast<double>(ns) / 1.0e6; };
            total_ms.push_back(took * 1000.0);
            world_ms.push_back(ms(timing.world_ns));
            ai_ms.push_back(ms(timing.engine_ns));
            lua_ms.push_back(ms(timing.service_ns));
        }
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
    result.timing = {phase_stats(total_ms), phase_stats(world_ms), phase_stats(ai_ms), phase_stats(lua_ms), !total_ms.empty()};
    if (options.tick_trace && journal) {
        std::error_code ignored;
        std::filesystem::create_directories(*options.tick_trace, ignored);
        std::ofstream trace(*options.tick_trace / ("seed-" + std::to_string(seed) + "-ticks.csv"), std::ios::binary);
        trace << "tick,total_ms,world_ms,ai_ms,lua_ms,goals_evaluated,maintenances,plans_attached,plans_deferred,plans_pumped,"
                 "plan_instances,plan_instructions,freestore_runs,freestore_instructions\n";
        trace << std::fixed << std::setprecision(3);
        for (std::size_t at = 0; at < total_ms.size() && at < journal->costs.size(); ++at) {
            const auto& cost = journal->costs[at];
            trace << at + 1 << ',' << total_ms[at] << ',' << world_ms[at] << ',' << ai_ms[at] << ',' << lua_ms[at] << ','
                  << cost.goals_evaluated << ',' << cost.maintenances << ',' << cost.plans_attached << ',' << cost.plans_deferred
                  << ',' << cost.plans_pumped << ',' << cost.plan_instances << ',' << cost.plan_instructions << ','
                  << cost.freestore_runs << ',' << cost.freestore_instructions << '\n';
        }
    }
    if (options.tick_trace && journal) {
        // The plan timeline (#957 behaviour comparison): who plans what against which target, when.
        std::ofstream plans(*options.tick_trace / ("seed-" + std::to_string(seed) + "-plans.csv"), std::ios::binary);
        plans << "tick,player,plan,goal,target,event,detail\n";
        for (const auto& event : journal->plans) {
            std::string detail = event.detail;
            std::replace(detail.begin(), detail.end(), ',', ';');
            plans << event.tick << ',' << event.player << ',' << event.plan << ',' << event.goal << ',' << event.target << ','
                  << event.event << ',' << detail << '\n';
        }
    }
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
        if (result.timing.present) {
            const auto phase = [&](const char* name, const PhaseStats& stats) {
                text << '"' << name << "\":{\"mean\":" << soak::json_number(stats.mean) << ",\"p99\":" << soak::json_number(stats.p99)
                     << ",\"worst\":" << soak::json_number(stats.worst) << ",\"worstTick\":" << stats.worst_tick << '}';
            };
            text << ",\"timingMs\":{";
            phase("total", result.timing.total);
            text << ',';
            phase("world", result.timing.world);
            text << ',';
            phase("ai", result.timing.ai);
            text << ',';
            phase("lua", result.timing.lua);
            text << '}';
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
                  << std::setprecision(1) << result.seconds << " s" << (result.compared ? ", workers agree" : "");
        if (result.timing.present) {
            std::cout << std::setprecision(2) << "; tick ms: mean " << result.timing.total.mean << " p99 " << result.timing.total.p99
                      << " worst " << result.timing.total.worst << "; ai p99 " << result.timing.ai.p99 << " worst "
                      << result.timing.ai.worst << "; lua p99 " << result.timing.lua.p99 << " worst " << result.timing.lua.worst;
        }
        std::cout << std::endl;
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
