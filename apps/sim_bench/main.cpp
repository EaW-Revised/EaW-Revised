// Scaling evidence for the simulation worker pool (#267, docs/simulation.md): a synthetic large
// space battle, generated from a seed (no game data), stepped headless with several worker
// counts. Prints ticks/s and where each tick's time goes, and fails when any worker count
// changes a state hash.

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/visibility.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark_internal.hpp"

namespace {
using namespace sim_bench;

void print_help(std::ostream& output) {
    output << "Usage: sim_bench [--units <n>] [--ticks <n>] [--warmup <n>] [--repeat <n>] [--seed <n>]\n"
              "                 [--workers <n|hardware>[,<n|hardware>...]] [--dispatch-cost 1]\n"
              "\n"
              "Generates a synthetic space battle of <n> units (default 1200) from the seed, steps it\n"
              "<warmup> + <ticks> ticks (default 30 + 300) with each worker count (default 1, 2, 4, 8 and\n"
              "the hardware thread count), <repeat> times each (default 3), and prints ticks/s and the\n"
              "time per tick of each partitioned phase and of the serial remainder. Exits 1 when two\n"
              "worker counts disagree on any tick's state hash.\n"
              "\n"
              "--dispatch-cost 1 measures the pool instead (#637): the median and p99 time of a phase\n"
              "whose partitions do nothing, on the pool and on the calling thread, per worker count.\n";
}

template <typename T>
[[nodiscard]] std::optional<T> parse_number(const std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::vector<std::size_t>> parse_workers(std::string_view text) {
    std::vector<std::size_t> workers;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto item = text.substr(0, comma);
        std::optional<std::size_t> count;
        if (item == "hardware") {
            count = eawr::platform::ThreadWorkerAdapter::hardware_worker_count();
        } else {
            count = parse_number<std::size_t>(item);
        }
        if (!count || *count == 0 || *count > eawr::platform::ThreadWorkerAdapter::max_worker_count) {
            return std::nullopt;
        }
        workers.push_back(*count);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    if (workers.empty()) {
        return std::nullopt;
    }
    return workers;
}

[[nodiscard]] std::optional<Options> parse_options(const int argc, const char* const argv[]) {
    Options options;
    options.workers = eawr::platform::determinism_worker_counts();
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) {
            return std::nullopt;
        }
        const std::string_view value(argv[++index]);
        bool valid = true;
        if (argument == "--units") {
            const auto units = parse_number<std::size_t>(value);
            valid = units && *units >= 8 && *units <= 200'000;
            options.units = units.value_or(0);
        } else if (argument == "--ticks") {
            const auto ticks = parse_number<std::uint64_t>(value);
            valid = ticks && *ticks >= 1 && *ticks <= 100'000;
            options.ticks = ticks.value_or(0);
        } else if (argument == "--warmup") {
            const auto warmup = parse_number<std::uint64_t>(value);
            valid = warmup && *warmup <= 100'000;
            options.warmup = warmup.value_or(0);
        } else if (argument == "--repeat") {
            const auto repeat = parse_number<std::size_t>(value);
            valid = repeat && *repeat >= 1 && *repeat <= 100;
            options.repeat = repeat.value_or(0);
        } else if (argument == "--seed") {
            const auto seed = parse_number<std::uint64_t>(value);
            valid = seed.has_value();
            options.seed = seed.value_or(0);
        } else if (argument == "--dispatch-cost") {
            valid = value == "0" || value == "1";
            options.dispatch_cost = value == "1";
        } else if (argument == "--workers") {
            auto workers = parse_workers(value);
            valid = workers.has_value();
            options.workers = std::move(workers).value_or(std::vector<std::size_t>{});
        } else {
            valid = false;
        }
        if (!valid) {
            return std::nullopt;
        }
    }
    return options;
}

// Forwards to the pool and adds up the time of each named phase.
class TimingExecutor final : public eawr::sim::PartitionExecutor {
public:
    explicit TimingExecutor(const eawr::sim::PartitionExecutor& inner) : inner_(inner) {}

    [[nodiscard]] std::size_t worker_count() const noexcept override { return inner_.worker_count(); }

    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override {
        return execute_phase("unnamed", partition_count, partition);
    }

    [[nodiscard]] eawr::core::Result<void> execute_phase(
        const std::string_view phase,
        const std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override {
        const auto start = Clock::now();
        auto result = inner_.execute_phase(phase, partition_count, partition);
        totals[std::string(phase)] += Clock::now() - start;
        return result;
    }

    mutable std::map<std::string, Clock::duration> totals;

private:
    const eawr::sim::PartitionExecutor& inner_;
};

struct Run {
    double seconds{};
    std::map<std::string, double> phase_seconds;
    std::string digest; // SHA-256 of every tick's state hash, warm-up included
    std::size_t final_units{};
};

[[nodiscard]] double seconds_of(const Clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

[[nodiscard]] std::optional<Run> run(const Battle& battle, const Options& options, const std::size_t workers) {
    auto created = tactical::TacticalSession::from_replay(
        battle.replay, battle.sensors, battle.durability, tactical::MotionTable{}, std::nullopt, battle.combat);
    if (!created) {
        std::cerr << eawr::core::format_diagnostic(created.error()) << '\n';
        return std::nullopt;
    }
    auto session = std::move(created).value();
    const eawr::platform::ThreadWorkerAdapter pool(workers);
    const TimingExecutor timed(pool);
    std::string hashes;
    Clock::duration timed_ticks{};
    for (std::uint64_t tick = 0; tick < options.warmup + options.ticks; ++tick) {
        if (tick == options.warmup) {
            timed.totals.clear();
        }
        const auto start = Clock::now();
        auto stepped = session.step(timed);
        if (!stepped) {
            std::cerr << eawr::core::format_diagnostic(stepped.error()) << '\n';
            return std::nullopt;
        }
        if (tick >= options.warmup) {
            timed_ticks += Clock::now() - start;
        }
        hashes += stepped.value().state_sha256;
    }
    Run result;
    result.seconds = seconds_of(timed_ticks);
    for (const auto& [phase, duration] : timed.totals) {
        result.phase_seconds[phase] = seconds_of(duration);
    }
    result.digest = eawr::sim::sha256_hex(
        std::span(reinterpret_cast<const std::uint8_t*>(hashes.data()), hashes.size()));
    result.final_units = session.units().size();
    return result;
}

// Serial steps of a tick, timed alone on the final state: the canonical state hash (its byte
// encoding and the SHA-256 over it) and the sensor field build.
struct SerialProbe {
    double encode_seconds{};
    double sha_seconds{};
    std::size_t state_bytes{};
    double sensor_field_seconds{};
};

[[nodiscard]] SerialProbe probe_serial(const Battle& battle, const Options& options) {
    auto session = tactical::TacticalSession::from_replay(battle.replay, battle.sensors, battle.durability).value();
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 0; tick < options.warmup + options.ticks; ++tick) {
        static_cast<void>(session.step(executor));
    }
    constexpr int samples = 20;
    const auto units = session.units();
    SerialProbe probe;
    auto start = Clock::now();
    for (int sample = 0; sample < samples; ++sample) {
        probe.state_bytes = session.canonical_state_bytes().size();
    }
    probe.encode_seconds = seconds_of(Clock::now() - start) / samples;
    const auto bytes = session.canonical_state_bytes();
    start = Clock::now();
    for (int sample = 0; sample < samples; ++sample) {
        static_cast<void>(eawr::sim::sha256(bytes));
    }
    probe.sha_seconds = seconds_of(Clock::now() - start) / samples;
    start = Clock::now();
    for (int sample = 0; sample < samples; ++sample) {
        static_cast<void>(tactical::SensorField::build(session.players(), units, battle.sensors));
    }
    probe.sensor_field_seconds = seconds_of(Clock::now() - start) / samples;
    return probe;
}

[[nodiscard]] std::string fixed(const double value, const int digits) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(digits) << value;
    return text.str();
}

// The pool's own cost per phase (#637): wake every thread for 64 empty partitions and wait for
// them, against the same phase on the calling thread. ThreadWorkerAdapter::inline_budget comes
// from these numbers.
int dispatch_cost(const Options& options) {
    constexpr std::size_t phases = 20'000;
    const auto percentile = [](std::vector<double>& values, const double share) {
        std::sort(values.begin(), values.end());
        return values[std::min(values.size() - 1, static_cast<std::size_t>(share * static_cast<double>(values.size())))];
    };
    std::cout << "sim_bench --dispatch-cost: " << phases << " phases of " << eawr::sim::tick_partition_count
              << " empty partitions, microseconds per phase\n"
              << "workers  pool median  pool p99  inline median\n";
    for (const auto workers : options.workers) {
        std::vector<double> pooled;
        std::vector<double> inlined;
        pooled.reserve(phases);
        inlined.reserve(phases);
        const eawr::platform::ThreadWorkerAdapter pool(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
        const eawr::sim::InlineExecutor inline_executor;
        std::atomic<std::size_t> touched{0};
        const std::function<void(std::size_t)> job = [&touched](const std::size_t) { touched.fetch_add(1, std::memory_order_relaxed); };
        for (std::size_t phase = 0; phase < phases; ++phase) {
            auto began = Clock::now();
            if (!pool.execute(eawr::sim::tick_partition_count, job)) return 1;
            pooled.push_back(std::chrono::duration<double, std::micro>(Clock::now() - began).count());
            began = Clock::now();
            if (!inline_executor.execute(eawr::sim::tick_partition_count, job)) return 1;
            inlined.push_back(std::chrono::duration<double, std::micro>(Clock::now() - began).count());
        }
        std::cout << std::setw(7) << workers << std::fixed << std::setprecision(1) << std::setw(13) << percentile(pooled, 0.5)
                  << std::setw(10) << percentile(pooled, 0.99) << std::setw(15) << percentile(inlined, 0.5) << '\n';
    }
    return 0;
}

} // namespace

int main(const int argc, const char* const argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_help(std::cout);
        return 0;
    }
    const auto options = parse_options(argc, argv);
    if (!options) {
        print_help(std::cerr);
        return 2;
    }
    if (options->dispatch_cost) return dispatch_cost(*options);
    const auto battle = generate(*options);
    std::cout << "sim_bench: synthetic space battle, " << options->units << " units, " << player_count
              << " players in 2 teams, " << options->ticks << " timed ticks after " << options->warmup
              << " warm-up, seed " << options->seed << ", median of " << options->repeat << " run(s)\n"
              << "hardware threads: " << eawr::platform::ThreadWorkerAdapter::hardware_worker_count()
              << ", partitions per phase: " << eawr::sim::tick_partition_count << "\n\n";

    struct Row {
        std::size_t workers;
        Run median;
        double best_seconds;
    };
    std::vector<Row> rows;
    std::set<std::string> digests;
    std::vector<std::string> phases;
    for (const auto workers : options->workers) {
        std::vector<Run> runs;
        for (std::size_t attempt = 0; attempt < options->repeat; ++attempt) {
            auto result = run(battle, *options, workers);
            if (!result) {
                return 1;
            }
            digests.insert(result->digest);
            for (const auto& [phase, seconds] : result->phase_seconds) {
                static_cast<void>(seconds);
                if (std::find(phases.begin(), phases.end(), phase) == phases.end()) {
                    phases.push_back(phase);
                }
            }
            runs.push_back(std::move(*result));
        }
        std::sort(runs.begin(), runs.end(), [](const Run& left, const Run& right) { return left.seconds < right.seconds; });
        rows.push_back(Row{workers, runs[runs.size() / 2], runs.front().seconds});
    }

    const auto ticks = static_cast<double>(options->ticks);
    const auto base = rows.front().median.seconds;
    std::cout << "| workers | ticks/s | best ticks/s | ms/tick | speed-up |";
    for (const auto& phase : phases) {
        std::cout << ' ' << phase << " ms |";
    }
    std::cout << " serial ms |\n|---:|---:|---:|---:|---:|";
    for (std::size_t index = 0; index <= phases.size(); ++index) {
        std::cout << "---:|";
    }
    std::cout << '\n';
    for (const auto& row : rows) {
        const auto per_tick = row.median.seconds / ticks;
        double parallel = 0;
        std::cout << "| " << row.workers << " | " << fixed(ticks / row.median.seconds, 1) << " | "
                  << fixed(ticks / row.best_seconds, 1) << " | " << fixed(per_tick * 1000, 3) << " | "
                  << fixed(base / row.median.seconds, 2) << "x |";
        for (const auto& phase : phases) {
            const auto found = row.median.phase_seconds.find(phase);
            const auto seconds = found == row.median.phase_seconds.end() ? 0.0 : found->second;
            parallel += seconds;
            std::cout << ' ' << fixed(seconds / ticks * 1000, 3) << " |";
        }
        std::cout << ' ' << fixed((row.median.seconds - parallel) / ticks * 1000, 3) << " |\n";
    }
    const auto probe = probe_serial(battle, *options);
    std::cout << "\nserial steps alone on the final state: state encoding " << fixed(probe.encode_seconds * 1000, 3)
              << " ms and SHA-256 " << fixed(probe.sha_seconds * 1000, 3) << " ms (" << probe.state_bytes / 1024
              << " KiB), sensor field build " << fixed(probe.sensor_field_seconds * 1000, 3) << " ms\n"
              << "units alive after the run: " << rows.front().median.final_units << " of " << options->units << '\n';
    if (digests.size() != 1) {
        std::cout << "FAIL: worker counts disagree on the state hashes (" << digests.size() << " different runs)\n";
        return 1;
    }
    std::cout << "every run agrees on every tick's state hash: " << *digests.begin() << '\n';
    return 0;
}
