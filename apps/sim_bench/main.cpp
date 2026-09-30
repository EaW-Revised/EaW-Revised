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

namespace {

namespace tactical = eawr::sim::tactical;
using eawr::sim::math::Fixed;
using Clock = std::chrono::steady_clock;

struct Options {
    std::size_t units = 1200;
    std::uint64_t ticks = 300;
    std::uint64_t warmup = 30;
    std::size_t repeat = 3;
    std::uint64_t seed = 0x5eed;
    std::vector<std::size_t> workers;
    bool dispatch_cost = false;
};

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

class Stream {
public:
    explicit Stream(const std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        state_ += 0x9e3779b97f4a7c15ULL;
        auto value = state_;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    // Uniform in [-bound, bound] whole source units.
    [[nodiscard]] std::int64_t signed_units(const std::int64_t bound) noexcept {
        return static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(2 * bound + 1)) - bound;
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] Fixed whole(const std::int64_t value) noexcept {
    return Fixed::from_raw(value * Fixed::scale);
}

[[nodiscard]] Fixed fraction(const std::int64_t numerator, const std::int64_t denominator) noexcept {
    return Fixed::from_raw(numerator * Fixed::scale / denominator);
}

struct UnitType {
    std::int64_t weight;       // share of the fleet, in percent
    std::int64_t reveal_range; // whole source units
    std::int64_t hull;         // whole source units
    std::size_t hardpoints;
    std::int64_t max_speed;
};

// Fighter, bomber, corvette, frigate, cruiser, capital ship: a FoC-like fleet mix.
constexpr std::array<UnitType, 6> unit_types{{
    {40, 1500, 60, 0, 400},
    {15, 1500, 90, 0, 300},
    {20, 2500, 600, 4, 200},
    {15, 3000, 1500, 8, 150},
    {7, 3500, 3000, 14, 100},
    {3, 4000, 6000, 24, 80},
}};

constexpr tactical::PlayerId player_count = 4;

struct Battle {
    tactical::TacticalReplay replay;
    std::vector<tactical::SensorProfile> sensors;
    tactical::DurabilityTable durability;
    tactical::CombatTable combat;
};

[[nodiscard]] tactical::DurabilityTable durability_table() {
    constexpr std::array<tactical::HardpointRole, 6> roles{
        tactical::HardpointRole::weapon, tactical::HardpointRole::weapon, tactical::HardpointRole::engine,
        tactical::HardpointRole::shield_generator, tactical::HardpointRole::weapon, tactical::HardpointRole::fighter_bay};
    tactical::DurabilityTable table;
    table.rules = {fraction(1, 5), fraction(2, 5), fraction(33, 100)};
    for (std::size_t index = 0; index < unit_types.size(); ++index) {
        const auto& type = unit_types[index];
        tactical::DurabilityProfile profile;
        profile.type_id = index + 1;
        profile.max_hull = whole(type.hull);
        profile.max_speed = whole(type.max_speed);
        profile.destroyed_with_hardpoints = type.hardpoints >= 24;
        for (std::size_t hardpoint = 0; hardpoint < type.hardpoints; ++hardpoint) {
            profile.hardpoints.push_back(tactical::HardpointProfile{
                roles[hardpoint % roles.size()], true, whole(type.hull / 8), {}, {}});
        }
        table.profiles.push_back(std::move(profile));
    }
    return table;
}

// Every type targets with one priority set (bigger ships first) within 1.5 x its sensor range;
// fighters and bombers fire an object weapon, the others their weapon hardpoints (roles above).
[[nodiscard]] tactical::CombatTable combat_table() {
    constexpr std::array<tactical::HardpointRole, 6> roles{
        tactical::HardpointRole::weapon, tactical::HardpointRole::weapon, tactical::HardpointRole::engine,
        tactical::HardpointRole::shield_generator, tactical::HardpointRole::weapon, tactical::HardpointRole::fighter_bay};
    tactical::CombatTable table;
    tactical::PrioritySet set;
    set.unlisted = whole(100);
    for (std::size_t index = 0; index < unit_types.size(); ++index) {
        set.rows.push_back({index + 1, whole(static_cast<std::int64_t>(unit_types.size() - index))});
    }
    table.priority_sets.push_back(set);
    for (std::size_t index = 0; index < unit_types.size(); ++index) {
        const auto& type = unit_types[index];
        tactical::CombatProfile profile;
        profile.type_id = index + 1;
        profile.category_bits = std::uint64_t{1} << index;
        profile.priority_set = 0;
        profile.max_attack_distance = whole(type.reveal_range * 3 / 2);
        const auto weapon = [&](const std::uint32_t hardpoint, const std::int64_t y) {
            tactical::WeaponProfile entry;
            entry.hardpoint = hardpoint;
            entry.range = whole(type.reveal_range);
            entry.min_recharge_hundredths = 50;
            entry.max_recharge_hundredths = 350;
            entry.pulse_count = 2;
            entry.pulse_delay_frames = 15;
            entry.cone_width = hardpoint == tactical::object_weapon ? Fixed{} : whole(120);
            entry.cone_height = hardpoint == tactical::object_weapon ? Fixed{} : whole(60);
            entry.opportunity_when_idle = true;
            entry.opportunity_when_targeting = true;
            entry.fire_a = {whole(20), whole(y), Fixed{}};
            entry.fire_b = entry.fire_a;
            return entry;
        };
        for (std::uint32_t hardpoint = 0; hardpoint < type.hardpoints; ++hardpoint) {
            const auto y = static_cast<std::int64_t>(hardpoint % 2 == 0 ? 10 : -10);
            profile.hardpoints.push_back({hardpoint, {Fixed{}, whole(y), Fixed{}}, true});
            if (roles[hardpoint % roles.size()] == tactical::HardpointRole::weapon) {
                profile.weapons.push_back(weapon(hardpoint, y));
            }
        }
        if (type.hardpoints == 0) {
            profile.weapons.push_back(weapon(tactical::object_weapon, 0));
        }
        table.profiles.push_back(std::move(profile));
    }
    return table;
}

// Four players in two teams; each team's fleet fills a disc of radius 7000 around x = -/+4000,
// so the discs overlap in the middle. Every tick each player orders a group of its own units
// to move, sends another group to attack an enemy, and deals scripted hull damage to random
// units (the only state-changing command of rules v1), so the command phase, destruction
// events and the durability service all have work.
[[nodiscard]] Battle generate(const Options& options) {
    Battle battle;
    Stream stream(options.seed);
    auto& setup = battle.replay.setup;
    setup.seed = options.seed;
    for (tactical::PlayerId player = 1; player <= player_count; ++player) {
        setup.players.push_back(tactical::Player{player, player <= 2 ? 1U : 2U, player <= 2 ? 1U : 2U,
            tactical::player_flag_commandable});
    }
    for (eawr::sim::EntityId id = 1; id <= options.units; ++id) {
        auto roll = static_cast<std::int64_t>(stream.next() % 100);
        std::size_t type = 0;
        while (roll >= unit_types[type].weight) {
            roll -= unit_types[type].weight;
            ++type;
        }
        const auto owner = static_cast<tactical::PlayerId>(1 + (id - 1) % player_count);
        const bool first_team = owner <= 2;
        std::int64_t x = 0;
        std::int64_t y = 0;
        do {
            x = stream.signed_units(7000);
            y = stream.signed_units(7000);
        } while (x * x + y * y > 7000 * 7000);
        tactical::UnitState unit;
        unit.entity_id = id;
        unit.type_id = type + 1;
        unit.owner = owner;
        unit.position = {whole(x + (first_team ? -4000 : 4000)), whole(y), whole(stream.signed_units(300))};
        unit.rotation = first_team ? eawr::sim::math::identity_quat()
                                   : eawr::sim::math::Quat{Fixed{}, Fixed{}, whole(1), Fixed{}};
        setup.units.push_back(unit);
    }
    for (std::size_t index = 0; index < unit_types.size(); ++index) {
        battle.sensors.push_back(tactical::SensorProfile{index + 1, whole(unit_types[index].reveal_range)});
    }
    battle.durability = durability_table();
    battle.combat = combat_table();

    const auto total_ticks = options.warmup + options.ticks;
    const auto own_units = [&](const tactical::PlayerId player, const std::size_t count) {
        std::set<eawr::sim::EntityId> chosen;
        const auto per_player = options.units / player_count;
        while (chosen.size() < std::min<std::size_t>(count, per_player)) {
            chosen.insert(player + player_count * (stream.next() % per_player));
        }
        return std::vector<eawr::sim::EntityId>(chosen.begin(), chosen.end());
    };
    const auto any_units = [&](const std::size_t count) {
        std::set<eawr::sim::EntityId> chosen;
        while (chosen.size() < count) {
            chosen.insert(1 + stream.next() % options.units);
        }
        return std::vector<eawr::sim::EntityId>(chosen.begin(), chosen.end());
    };
    for (std::uint64_t tick = 0; tick < total_ticks; ++tick) {
        for (tactical::PlayerId player = 1; player <= player_count; ++player) {
            const tactical::PlayerId enemy = player <= 2 ? 3 + tick % 2 : 1 + tick % 2;
            const auto target = enemy + player_count * (stream.next() % (options.units / player_count));
            const auto destination = eawr::sim::math::Vec3{
                whole(stream.signed_units(9000)), whole(stream.signed_units(7000)), Fixed{}};
            battle.replay.commands.push_back(tactical::PlayerCommand{
                {tick, player, 1}, own_units(player, 24), tactical::MovePayload{destination}});
            battle.replay.commands.push_back(tactical::PlayerCommand{
                {tick, player, 2}, own_units(player, 16), tactical::AttackPayload{target}});
            battle.replay.commands.push_back(tactical::PlayerCommand{
                {tick, player, 3}, any_units(8), tactical::DamagePayload{whole(4), tactical::hull_target}});
        }
    }
    battle.replay.final_tick_count = total_ticks;
    return battle;
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
