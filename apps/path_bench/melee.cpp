// The close-range battle benchmark "melee" (#601, docs/performance/battle-bench.md): both M2 sides'
// capital ships and fighter/bomber squadrons spawned within weapon range of each other on the M2
// map with no stations and no map objects, every ship and squadron given an attack-move onto the
// enemy's centre at tick 0. The fight is a replay (setup + commands), so it is deterministic from
// the seed, `sim_headless --replay` and the viewer's `--eawr-live-session replay` play the same
// fight (--replay-out), and it runs with the replay path's content: the M2 fog grid, the ability
// table and the fixture's victory rules (no star base, so the battle never ends early).
//
//   path_bench --melee s|m|l [--seed 601] [--ticks 4500] [--workers 1,2,4,8,hardware]
//              [--game-root <dir>] [--csv <prefix>] [--replay-out <file>] [--timing on|off] [--list 1]
//              [--execution live|legacy] [--profile on [--profile-interval <us>]]
//
// Prints each worker count's cost per tick of every named phase and of the serial remainder
// (mean, p99, worst over the fight) against the 30 Hz tick budget, and the live unit, craft and
// projectile counts. Exits 1 when two worker counts disagree on a tick's state hash. Without the
// game data it prints SKIPPED and exits 0 (the ctest path_bench_melee_workers). --list 1 lists the
// survivors. --profile on samples every run's threads (sampler.hpp; Windows x64, a build with
// EAWR_DEBUG_SYMBOLS names the functions) and prints the top functions and the phases they ran in;
// the sampling perturbs the tick times, so a profile run is not a timing run.

#include "melee.hpp"
#include "sampler.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/skirmish/melee.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace eawr::bench {
namespace {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
using eawr::sim::math::Fixed;
using Clock = std::chrono::steady_clock;

constexpr double tick_budget_ms = 1000.0 / tactical::logical_frames_per_second;

struct Options {
    std::string size;
    std::optional<std::string> game_root;
    std::uint64_t seed = 601;
    std::uint64_t ticks = 4500;
    std::vector<std::size_t> workers;
    std::optional<std::string> csv;
    std::optional<std::string> replay_out;
    bool timing = true;
    bool list = false;
    bool profile = false;
    bool live_execution = true;
    unsigned profile_interval_us = 1000;
};

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

template <typename T>
[[nodiscard]] std::optional<T> parse_number(const std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<Options> parse_options(const int argc, const char* const argv[]) {
    Options options;
    options.game_root = environment("EAWR_EAW_GAME_ROOT");
    options.workers = {1, 2, 4, 8, eawr::platform::ThreadWorkerAdapter::hardware_worker_count()};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) return std::nullopt;
        const std::string_view value(argv[++index]);
        if (argument == "--melee") {
            if (value != "s" && value != "m" && value != "l") return std::nullopt;
            options.size = std::string(value);
        } else if (argument == "--game-root") {
            options.game_root = std::string(value);
        } else if (argument == "--seed") {
            const auto seed = parse_number<std::uint64_t>(value);
            if (!seed) return std::nullopt;
            options.seed = *seed;
        } else if (argument == "--ticks") {
            const auto ticks = parse_number<std::uint64_t>(value);
            if (!ticks || *ticks < 1 || *ticks > 36'000) return std::nullopt;
            options.ticks = *ticks;
        } else if (argument == "--workers") {
            options.workers.clear();
            std::string_view rest = value;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const auto item = rest.substr(0, comma);
                const auto count = item == "hardware"
                    ? std::optional(eawr::platform::ThreadWorkerAdapter::hardware_worker_count())
                    : parse_number<std::size_t>(item);
                if (!count || *count == 0 || *count > eawr::platform::ThreadWorkerAdapter::max_worker_count) {
                    return std::nullopt;
                }
                options.workers.push_back(*count);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
            if (options.workers.empty()) return std::nullopt;
        } else if (argument == "--csv") {
            options.csv = std::string(value);
        } else if (argument == "--replay-out") {
            options.replay_out = std::string(value);
        } else if (argument == "--execution") {
            if (value != "live" && value != "legacy") return std::nullopt;
            options.live_execution = value == "live";
        } else if (argument == "--profile") {
            if (value != "on" && value != "off") return std::nullopt;
            options.profile = value == "on";
        } else if (argument == "--profile-interval") {
            const auto interval = parse_number<unsigned>(value);
            if (!interval || *interval < 100 || *interval > 100'000) return std::nullopt;
            options.profile_interval_us = *interval;
        } else if (argument == "--list") {
            options.list = value == "1";
        } else if (argument == "--timing") {
            if (value != "on" && value != "off") return std::nullopt;
            options.timing = value == "on";
        } else {
            return std::nullopt;
        }
    }
    if (options.size.empty()) return std::nullopt;
    return options;
}

struct Content {
    units::UnitTables tables;
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
};

[[nodiscard]] std::optional<Content> load(const std::filesystem::path& root) {
    const auto fail = [](const std::string& what) {
        std::cerr << "path_bench --melee: " << what << '\n';
        return std::optional<Content>{};
    };
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root / folder / "Data");
        if (!manifest) return fail("the " + id + " layer does not mount");
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) return fail("the FoC vfs does not mount");
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    if (!catalog) return fail("the FoC catalog does not load");
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto tables = eawr::units::load_unit_tables(input);
    if (!tables) return fail("the FoC unit tables do not load");
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    if (!inputs) return fail("the M2 start inputs do not read");
    auto start = skirmish::build_start(fixture, inputs.value());
    if (!start) return fail("the M2 start does not build");
    // As sim_headless --replay and the viewer's replay session bind a replay of the M2 content.
    auto content = skirmish::session_content(tables.value(), skirmish::human_slots(fixture));
    if (!content) return fail("the session content does not build");
    auto fog = skirmish::fog_rules(inputs.value());
    if (!fog) return fail("the M2 fog rules do not build");
    content.value().fog = fog.value();
    return Content{std::move(tables).value(), std::move(start).value(), std::move(content).value()};
}

// Forwards to the pool and adds up each named phase's time in the tick.
class TimingExecutor final : public eawr::sim::PartitionExecutor {
public:
    explicit TimingExecutor(const eawr::sim::PartitionExecutor& inner) : inner_(inner) {}
    [[nodiscard]] std::size_t worker_count() const noexcept override { return inner_.worker_count(); }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t partition_count, const std::function<void(std::size_t)>& partition) const override {
        return execute_phase("unnamed", partition_count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override {
        if (profiling) Sampler::enter_phase(phase);
        const auto start = Clock::now();
        auto result = inner_.execute_phase(phase, partition_count, partition);
        tick[std::string(phase)] += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (profiling) Sampler::enter_phase({});
        return result;
    }
    mutable std::map<std::string, double> tick;
    bool profiling = false;

private:
    const eawr::sim::PartitionExecutor& inner_;
};

struct TickRecord {
    double ms{};
    std::map<std::string, double> phases;
    std::size_t units{};
    std::size_t craft{};
    std::size_t squadrons{};
    std::size_t projectiles{};
    std::uint64_t candidates{}; // #636: the projectile broad phase's work (TacticalTick)
    std::uint64_t exact_tests{};
};

struct Run {
    std::vector<TickRecord> ticks;
    std::vector<std::string> hashes;
    std::map<tactical::PlayerId, std::size_t> survivors;
    std::vector<std::string> survivor_lines; // --list: each survivor's type, place, hull and target
};

[[nodiscard]] std::optional<Run> run(
    const Content& content, const tactical::TacticalReplay& replay, const std::size_t workers,
    const bool live_execution, Sampler* sampler) {
    const auto victory = skirmish::victory_rules(replay.setup, content.tables, skirmish::human_slots(skirmish::m2_fixture()));
    auto created = tactical::TacticalSession::from_replay(replay, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, victory, content.content.abilities);
    if (!created) {
        std::cerr << "path_bench --melee: " << eawr::core::format_diagnostic(created.error()) << '\n';
        return std::nullopt;
    }
    auto session = std::move(created).value();
    if (live_execution) session.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
    std::vector<eawr::sim::StateHash> pending;
    pending.reserve(replay.final_tick_count);
    const eawr::platform::ThreadWorkerAdapter pool(workers, live_execution
        ? eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost
        : eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
    TimingExecutor executor(pool);
    executor.profiling = sampler != nullptr;
    if (sampler != nullptr) sampler->start();
    Run out;
    out.ticks.reserve(replay.final_tick_count);
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
        executor.tick.clear();
        const auto began = Clock::now();
        auto stepped = session.step(executor);
        const auto elapsed = Clock::now() - began;
        if (!stepped) {
            std::cerr << "path_bench --melee: step " << tick + 1 << ": " << eawr::core::format_diagnostic(stepped.error()) << '\n';
            return std::nullopt;
        }
        pending.push_back(std::move(stepped.value().state_hash));
        TickRecord record;
        record.ms = std::chrono::duration<double, std::milli>(elapsed).count();
        record.phases = executor.tick;
        record.units = session.units().size();
        record.squadrons = session.squadrons().size();
        for (const auto& squadron : session.squadrons()) record.craft += squadron.members.size();
        record.projectiles = session.projectiles().size();
        record.candidates = stepped.value().projectile_candidates;
        record.exact_tests = stepped.value().projectile_exact_tests;
        out.ticks.push_back(std::move(record));
    }
    if (sampler != nullptr) sampler->stop();
    // Resolve completed hashes outside the timed stepping loop, as live reports do.
    out.hashes.reserve(pending.size());
    for (const auto& hash : pending) out.hashes.push_back(hash.get());
    std::map<tactical::TypeId, std::string> names;
    for (const auto& type : content.tables.units) names.emplace(skirmish::type_id(type.id), type.id);
    for (const auto& unit : session.units()) {
        ++out.survivors[unit.owner];
        std::ostringstream line;
        const auto name = names.find(unit.type_id);
        line << "unit " << unit.entity_id << ' ' << (name == names.end() ? std::string("?") : name->second) << " player "
             << unit.owner << " at (" << unit.position.x.raw() / Fixed::scale << ", " << unit.position.y.raw() / Fixed::scale
             << ") order " << static_cast<int>(unit.order.kind);
        if (const auto health = session.durability_state(unit.entity_id)) line << " hull " << health->hull.raw() / Fixed::scale;
        if (const auto combat = session.combat_state(unit.entity_id)) line << " target " << combat->attack_target;
        out.survivor_lines.push_back(line.str());
    }
    return out;
}

struct Stats {
    double mean{};
    double p99{};
    double worst{};
};

[[nodiscard]] Stats stats(std::vector<double> values) {
    Stats out;
    if (values.empty()) return out;
    double sum = 0;
    for (const auto value : values) sum += value;
    out.mean = sum / static_cast<double>(values.size());
    std::sort(values.begin(), values.end());
    // Nearest rank.
    auto rank = static_cast<std::size_t>(0.99 * static_cast<double>(values.size()) + 0.999999);
    rank = std::clamp<std::size_t>(rank, 1, values.size());
    out.p99 = values[rank - 1];
    out.worst = values.back();
    return out;
}

[[nodiscard]] std::vector<std::string> phase_names(const Run& run) {
    std::vector<std::string> names;
    for (const auto& record : run.ticks) {
        for (const auto& [name, ms] : record.phases) {
            static_cast<void>(ms);
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

[[nodiscard]] double serial_ms(const TickRecord& record) {
    double parallel = 0;
    for (const auto& [name, ms] : record.phases) {
        static_cast<void>(name);
        parallel += ms;
    }
    return std::max(0.0, record.ms - parallel);
}

void report(const Run& run, const std::size_t workers, const bool timing) {
    std::cout << std::fixed << std::setprecision(3);
    std::size_t peak_units = 0;
    std::size_t peak_craft = 0;
    std::size_t peak_projectiles = 0;
    double projectiles = 0;
    std::uint64_t candidates = 0;
    std::uint64_t exact_tests = 0;
    for (const auto& record : run.ticks) {
        candidates += record.candidates;
        exact_tests += record.exact_tests;
        peak_units = std::max(peak_units, record.units);
        peak_craft = std::max(peak_craft, record.craft);
        peak_projectiles = std::max(peak_projectiles, record.projectiles);
        projectiles += static_cast<double>(record.projectiles);
    }
    std::cout << "  " << workers << " worker(s), " << run.ticks.size() << " ticks: peak " << peak_units << " units ("
              << peak_craft << " craft), " << peak_projectiles << " projectiles in flight at most, "
              << std::setprecision(1) << projectiles / static_cast<double>(std::max<std::size_t>(run.ticks.size(), 1))
              << " on average; survivors";
    for (const auto& [player, count] : run.survivors) std::cout << " player " << player << ": " << count;
    std::cout << '\n' << std::setprecision(3);
    // Deterministic, so the same at every worker count and on every host.
    std::cout << "    projectile broad phase: " << candidates << " index candidates, " << exact_tests
              << " exact collision tests\n";
    if (!timing) return;
    std::vector<double> totals;
    std::vector<double> serial;
    for (const auto& record : run.ticks) {
        totals.push_back(record.ms);
        serial.push_back(serial_ms(record));
    }
    const auto total = stats(totals);
    const auto over = std::count_if(totals.begin(), totals.end(), [](const double ms) { return ms > tick_budget_ms; });
    std::cout << "    | phase | mean ms | p99 ms | worst ms |\n    |---|---:|---:|---:|\n";
    std::cout << "    | **tick** | " << total.mean << " | " << total.p99 << " | " << total.worst << " |\n";
    for (const auto& name : phase_names(run)) {
        std::vector<double> values;
        for (const auto& record : run.ticks) {
            const auto found = record.phases.find(name);
            values.push_back(found == record.phases.end() ? 0.0 : found->second);
        }
        const auto phase = stats(values);
        std::cout << "    | " << name << " | " << phase.mean << " | " << phase.p99 << " | " << phase.worst << " |\n";
    }
    const auto rest = stats(serial);
    std::cout << "    | serial remainder | " << rest.mean << " | " << rest.p99 << " | " << rest.worst << " |\n";
    std::cout << "    tick budget " << tick_budget_ms << " ms: " << over << " ticks over it\n";
}

void write_csv(const std::string& path, const Run& run) {
    std::ofstream out(path, std::ios::binary);
    const auto names = phase_names(run);
    out << "tick,ms";
    for (const auto& name : names) out << ',' << name;
    out << ",serial,units,craft,squadrons,projectiles\n";
    for (std::size_t index = 0; index < run.ticks.size(); ++index) {
        const auto& record = run.ticks[index];
        out << index << ',' << record.ms;
        for (const auto& name : names) {
            const auto found = record.phases.find(name);
            out << ',' << (found == record.phases.end() ? 0.0 : found->second);
        }
        out << ',' << serial_ms(record) << ',' << record.units << ',' << record.craft << ',' << record.squadrons << ','
            << record.projectiles << '\n';
    }
}

} // namespace

int attach_main(const int argc, const char* const argv[]) {
    std::uint32_t process_id = 0;
    unsigned seconds = 60;
    unsigned interval_us = 1000;
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string_view argument(argv[index]);
        const std::string_view value(argv[index + 1]);
        if (argument == "--profile-attach") {
            process_id = parse_number<std::uint32_t>(value).value_or(0);
        } else if (argument == "--profile-seconds") {
            seconds = parse_number<unsigned>(value).value_or(0);
        } else if (argument == "--profile-interval") {
            interval_us = parse_number<unsigned>(value).value_or(0);
        } else {
            process_id = 0;
            break;
        }
    }
    if (process_id == 0 || seconds == 0 || seconds > 3600 || interval_us < 100 || interval_us > 100'000) {
        std::cerr << "usage: path_bench --profile-attach <pid> [--profile-seconds 60] [--profile-interval <us>]\n";
        return 2;
    }
    Sampler sampler(interval_us, process_id);
    if (!Sampler::supported() || !sampler.attached()) {
        std::cerr << "path_bench --profile-attach: cannot sample process " << process_id << '\n';
        return 1;
    }
    std::cout << "sampling process " << process_id << " for " << seconds << " s\n" << std::flush;
    sampler.start();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    sampler.stop();
    sampler.report(std::cout, 60);
    return 0;
}

int melee_main(const int argc, const char* const argv[]) {
    const auto options = parse_options(argc, argv);
    if (!options) {
        std::cerr << "usage: path_bench --melee s|m|l [--seed <n>] [--ticks <n>] [--workers <n|hardware>[,...]]\n"
                     "                  [--game-root <dir>] [--csv <prefix>] [--replay-out <file>] [--timing on|off]\n"
                     "                  [--list 1] [--execution live|legacy] [--profile on [--profile-interval <us>]]\n";
        return 2;
    }
    if (!options->game_root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the melee benchmark\n";
        return 0;
    }
    auto content = load(*options->game_root);
    if (!content) return 1;
    auto melee = skirmish::build_melee(
        content->tables, content->start, *skirmish::melee_size(options->size), options->seed, options->ticks);
    if (!melee) {
        std::cerr << "path_bench --melee: " << eawr::core::format_diagnostic(melee.error()) << '\n';
        return 1;
    }
    const auto* replay = &melee.value().replay;
    std::cout << "melee " << options->size << ", seed " << options->seed << ": " << melee.value().ships << " ships and "
              << melee.value().squadrons << " squadrons (" << melee.value().craft << " craft), "
              << replay->setup.units.size() << " units in all, " << options->ticks
              << " ticks; hardware threads " << eawr::platform::ThreadWorkerAdapter::hardware_worker_count()
              << "; execution " << (options->live_execution ? "live" : "legacy") << '\n';
    if (options->replay_out) {
        auto bytes = tactical::write_replay(*replay);
        if (!bytes) {
            std::cerr << "path_bench --melee: the replay does not write: " << eawr::core::format_diagnostic(bytes.error()) << '\n';
            return 1;
        }
        std::ofstream file(*options->replay_out, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
        if (!file) {
            std::cerr << "path_bench --melee: cannot write " << *options->replay_out << '\n';
            return 1;
        }
    }
    std::optional<std::vector<std::string>> reference;
    bool agree = true;
    std::vector<std::pair<std::size_t, double>> means;
    for (const auto workers : options->workers) {
        std::optional<Sampler> sampler;
        if (options->profile) sampler.emplace(options->profile_interval_us);
        const auto result = run(*content, *replay, workers, options->live_execution, sampler ? &*sampler : nullptr);
        if (!result) return 1;
        if (sampler) std::cout << "  (profiled: the sampling perturbs these tick times)\n";
        report(*result, workers, options->timing);
        if (sampler) sampler->report(std::cout, 40);
        if (options->list && !reference) {
            for (const auto& line : result->survivor_lines) std::cout << "    " << line << '\n';
        }
        std::string joined;
        for (const auto& hash : result->hashes) joined += hash;
        std::cout << "    hashes digest "
                  << eawr::sim::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(joined.data()), joined.size()))
                  << '\n';
        if (options->csv) write_csv(*options->csv + "-" + options->size + "-w" + std::to_string(workers) + ".csv", *result);
        double sum = 0;
        for (const auto& record : result->ticks) sum += record.ms;
        means.emplace_back(workers, sum / static_cast<double>(std::max<std::size_t>(result->ticks.size(), 1)));
        if (!reference) {
            reference = result->hashes;
        } else if (*reference != result->hashes) {
            std::cout << "    FAIL: the hashes differ from the first worker count\n";
            agree = false;
        }
    }
    if (options->timing && means.size() > 1) {
        std::cout << "  mean tick by workers:";
        for (const auto& [workers, mean] : means) {
            std::cout << ' ' << workers << ": " << std::setprecision(3) << mean << " ms (" << std::setprecision(2)
                      << means.front().second / mean << "x)";
        }
        std::cout << '\n';
    }
    if (agree) std::cout << "every worker count agrees on every tick's state hash\n";
    return agree ? 0 : 1;
}

} // namespace eawr::bench
