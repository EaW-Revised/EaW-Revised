#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/session.hpp"
#include "eawr/sim/galactic/synthetic.hpp"
#include "stock_snapshot.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace galaxy = eawr::sim::galactic;
using Clock = std::chrono::steady_clock;
using Pool = eawr::platform::ThreadWorkerAdapter;

double elapsed(const Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void require(const bool ok, const std::string_view message) {
    if (!ok) throw std::runtime_error(std::string(message));
}

// Observe dispatches without changing the production session or including clocks in state.
class Probe final : public eawr::sim::PartitionExecutor {
public:
    explicit Probe(const Pool& pool) : pool_(pool) {}
    std::size_t worker_count() const noexcept override { return pool_.worker_count(); }
    eawr::core::Result<void> execute(const std::size_t count,
        const std::function<void(std::size_t)>& work) const override {
        return pool_.execute(count, work);
    }
    eawr::core::Result<void> execute_phase(const std::string_view name, const std::size_t count,
        const std::function<void(std::size_t)>& work) const override {
        const auto start = Clock::now();
        auto result = pool_.execute_phase(name, count, work);
        times[std::string(name)] += elapsed(start);
        partitions[std::string(name)] += count;
        return result;
    }
    mutable std::map<std::string, double> times;
    mutable std::map<std::string, std::uint64_t> partitions;
private:
    const Pool& pool_;
};

struct Perception {
    std::uint64_t checksum{};
    std::uint64_t visits{};
};

// A deliberately synthetic, faction-by-object evaluation sweep, not an AI policy. It reads
// immutable published inputs, stages disjoint rows and reduces in ascending order (ADR-009).
Perception perceive(const galaxy::GalacticSnapshot& snapshot, const Probe& probe) {
    const auto planets = snapshot.planets();
    const auto routes = snapshot.routes();
    const auto fleets = snapshot.fleets();
    const auto objects = planets.size() + routes.size() + fleets.size();
    struct Row { std::uint64_t score{}, visits{}; };
    std::vector<Row> rows(objects * snapshot.players().size());
    auto result = probe.execute_phase("synthetic-perception", eawr::sim::tick_partition_count,
        [&](const std::size_t partition) {
            const auto range = eawr::sim::partition_range(partition, rows.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto player = snapshot.players()[index / objects].player_id;
                auto object = index % objects;
                std::uint64_t score = 0;
                if (object < planets.size()) {
                    const auto& planet = planets[object];
                    score = planet.id * (planet.owner == player ? 3U : 7U);
                } else if ((object -= planets.size()) < routes.size()) {
                    const auto& route = routes[object];
                    score = route.id + route.from + route.to + player;
                } else {
                    const auto& fleet = fleets[object - routes.size()];
                    score = fleet.id + fleet.orbit + (fleet.owner == player ? 11U : 13U);
                }
                rows[index] = {score, 1};
            }
        });
    require(static_cast<bool>(result), "perception dispatch failed");
    Perception total;
    for (const auto& row : rows) { total.checksum += row.score; total.visits += row.visits; }
    return total;
}

enum class Load { commands, quiet, paused };
struct Evidence {
    std::vector<std::string> ticks;
    std::uint64_t gathered{}, perception{}, encoded{}, fiscal{}, periodic{}, commands{};
    bool operator==(const Evidence&) const = default;
};

Evidence run(const std::uint32_t locations, const std::size_t workers, const std::uint64_t ticks,
    const std::uint64_t warmup, const Load load, const bool budgets) {
    galaxy::SyntheticGalaxyOptions options;
    options.planets = locations;
    options.players = 4;
    options.fleets_per_player = locations / options.players;
    auto setup = galaxy::synthetic_galaxy(options);
    setup.rules.fiscal_cycle_frames = 10; // synthetic workload, not a retail constant
    auto created = galaxy::GalacticSession::create(setup);
    require(static_cast<bool>(created), "synthetic setup rejected");
    auto session = std::move(created.value());
    const Pool pool(workers);
    const Probe probe(pool);
    std::vector<std::uint64_t> sequences(options.players + 1);
    Evidence evidence;
    double total_ms = 0;
    std::uint64_t measured_gather_partitions = 0;
    for (std::uint64_t tick = 0; tick < ticks + warmup; ++tick) {
        if (tick == warmup) { probe.times.clear(); probe.partitions.clear(); }
        const auto start = Clock::now();
        const auto before = session.executed_commands().size();
        const auto submit = [&](const galaxy::SeatId seat, galaxy::CommandPayload payload) {
            require(static_cast<bool>(session.submit({{tick, seat, ++sequences[seat.value]}, std::move(payload)})),
                "synthetic command rejected at admission");
        };
        if (load == Load::paused && tick == 0) submit({1}, galaxy::PauseGalaxy{});
        if (load == Load::commands) {
            // One ownership evaluation and orbit order per object, with changing destinations.
            // Debug orders stand in for AI plans and locomotion until those services land.
            for (std::size_t index = 0; index < setup.planets.size(); ++index) {
                const auto owner = static_cast<galaxy::PlayerId>((index + tick + 1) % options.players + 1);
                submit({owner}, galaxy::DebugSetPlanetOwner{setup.planets[index].id, owner});
            }
            for (std::size_t index = 0; index < setup.fleets.size(); ++index) {
                const auto& fleet = setup.fleets[index];
                submit({fleet.owner}, galaxy::DebugSetFleetOrbit{fleet.id,
                    setup.planets[(index + tick + 1) % setup.planets.size()].id});
            }
        }
        auto stepped = session.step(probe);
        require(static_cast<bool>(stepped), "galactic step failed");
        const auto& step = stepped.value();
        const auto perception = load == Load::paused ? Perception{} : perceive(*step.snapshot, probe);
        if (tick >= warmup) total_ms += elapsed(start);
        const auto commands = session.executed_commands().size() - before;
        if (budgets) {
            const bool serviced = load != Load::paused;
            // Pins are deliberately independent of setup sizes: catches synthetic fixture drift.
            const std::uint64_t objects = locations == 56 ? 182 : 728;
            require(step.work.serviced == serviced, "service admission budget");
            require(step.work.gathered_objects == (serviced ? objects : 0), "gather object budget");
            require(perception.visits == (serviced ? objects * 4 : 0), "perception visit budget");
            require(step.work.fiscal_cycles == (serviced && (tick + 1) % 10 == 0 ? 1U : 0U), "fiscal budget");
            require(step.work.periodic_checks == (serviced && (tick + 1) % 30 == 0 ? 1U : 0U), "periodic budget");
            // #2979 adds the campaign-scoring section on the first ownership change.
            // This workload changes every planet each tick: commands encode 6 sections
            // initially and 4 thereafter (session, planets, fleets, scoring). Quiet and
            // paused worlds keep the original 5-section closure and encode only session
            // after tick zero. Across 60 ticks these totals are 242 / 64 / 64.
            const std::uint64_t total_sections = load == Load::commands ? 6 : 5;
            const std::uint64_t sections = tick == 0 ? total_sections : load == Load::commands ? 4 : 1;
            require(step.work.sections_encoded == sections, "section encoding budget");
            require(step.work.sections_reused == total_sections - sections, "section reuse budget");
            const std::uint64_t expected_commands = load == Load::commands ? locations * 2ULL
                : load == Load::paused && tick == 0 ? 1 : 0;
            require(commands == expected_commands, "command commit budget");
        }
        // Compare every tick, including warmup, snapshot/events and the perception result.
        evidence.ticks.push_back(step.state_hash.get() + ":" + step.snapshot->sha256()
            + ":" + std::to_string(perception.checksum));
        evidence.gathered += step.work.gathered_objects;
        evidence.perception += perception.visits;
        evidence.encoded += step.work.sections_encoded;
        evidence.fiscal += step.work.fiscal_cycles;
        evidence.periodic += step.work.periodic_checks;
        evidence.commands += commands;
        if (tick >= warmup && step.work.serviced) measured_gather_partitions += locations == 56 ? 1U : 64U;
    }
    if (budgets) {
        require(probe.partitions["galactic-gather"] == measured_gather_partitions, "gather dispatch budget");
        require(probe.partitions["synthetic-perception"] == (load == Load::paused ? 0 : ticks * 64),
            "perception dispatch budget");
    }
    double phase_ms = 0;
    for (const auto& [name, ms] : probe.times) { (void)name; phase_ms += ms; }
    std::cout << locations << ',' << workers << ',' << ticks << ',' << warmup << ','
        << (load == Load::commands ? "commands" : load == Load::quiet ? "quiet" : "paused") << ','
        << total_ms << ',' << probe.times["galactic-gather"] << ',' << probe.times["synthetic-perception"]
        << ',' << std::max(0.0, total_ms - phase_ms) << ',' << evidence.gathered << ',' << evidence.perception
        << ',' << evidence.encoded << ',' << evidence.fiscal << ',' << evidence.periodic << ',' << evidence.commands
        << ',' << evidence.ticks.back() << '\n';
    return evidence;
}

std::uint64_t number(const std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    require(error == std::errc{} && end == text.data() + text.size(), "invalid unsigned number");
    return value;
}
} // namespace

int main(const int argc, char** argv) {
    try {
        std::uint64_t ticks = 300, warmup = 30;
        bool budgets = false, stock_snapshot = false;
        auto workers = eawr::platform::determinism_worker_counts();
        for (int index = 1; index < argc; ++index) {
            const std::string_view arg = argv[index];
            if (arg == "--help") {
                std::cout << "gc_bench [--ticks 1..10000] [--warmup 0..1000] [--workers 1,2,4,8,hardware] [--budgets] [--stock-snapshot]\n"
                    << "--stock-snapshot: ticks are publication samples after stock warmup; requires EAWR_EAW_GAME_ROOT\n";
                return 0;
            }
            if (arg == "--budgets") { budgets = true; continue; }
            if (arg == "--stock-snapshot") { stock_snapshot = true; continue; }
            require(index + 1 < argc, "missing option value");
            const std::string_view value = argv[++index];
            if (arg == "--ticks") ticks = number(value);
            else if (arg == "--warmup") warmup = number(value);
            else if (arg == "--workers") {
                workers.clear();
                std::size_t begin = 0;
                do {
                    const auto end = value.find(',', begin);
                    const auto token = value.substr(begin, end == value.npos ? value.size() - begin : end - begin);
                    const auto count = token == "hardware" ? Pool::hardware_worker_count() : number(token);
                    require(count > 0 && count <= Pool::max_worker_count, "workers must be 1..256 or hardware");
                    workers.push_back(static_cast<std::size_t>(count));
                    if (end == value.npos) break;
                    begin = end + 1;
                } while (true);
                require(workers.front() == 1, "worker equality requires 1 as the first worker count");
            } else throw std::runtime_error("unknown option");
        }
        require(ticks > 0 && ticks <= 10000 && warmup <= 1000, "tick/warmup limit exceeded");
        if (stock_snapshot) return stock_snapshot_benchmark(workers, ticks, warmup);
        std::cout << std::fixed << std::setprecision(3)
            << "locations,workers,ticks,warmup,load,total_ms,gather_ms,synthetic_perception_ms,serial_remainder_ms,"
               "gathered_objects,perception_visits,sections_encoded,fiscal_cycles,periodic_checks,commands,final_trace\n";
        for (const auto locations : {56U, 224U}) {
            for (const auto load : {Load::commands, Load::quiet, Load::paused}) {
                if (!budgets && load != Load::commands) continue;
                Evidence baseline;
                bool first = true;
                for (const auto count : workers) {
                    const auto evidence = run(locations, count, ticks, warmup, load, budgets);
                    if (first) { baseline = evidence; first = false; }
                    else require(evidence == baseline, "worker hash/snapshot/perception/work disagreement");
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gc_bench: " << error.what() << '\n';
        return 1;
    }
}
