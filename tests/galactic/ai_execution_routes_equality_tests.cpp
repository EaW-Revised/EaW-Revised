// #4309/#5082: compare published rows, predecessors and work against the former 16-bit all-pairs
// formulation. Its derived words are widened at output only; its numeric refusals now build routes.
// Also cover the widened domain, memo reuse and partition determinism.

#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "galactic_ai_execution.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::script::foc::galactic_ai::reference {
namespace {
core::Diagnostic invalid(std::string message) {
    core::Diagnostic out; out.code = std::string(authoritative::codes::invalid_request);
    out.message = std::move(message); return out;
}
bool finite(Number value) { return (value.repr & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL; }
Number single_add(Number a, Number b) { return ai::to_single(a + b); }
constexpr auto missing_cost = std::numeric_limits<std::uint32_t>::max();
bool safe_sum(std::uint32_t a, std::uint32_t b) {
    return (a & 65535U) + (b & 65535U) < 65535U && (a >> 16U) + (b >> 16U) < 65535U;
}
Number decoded_distance(std::uint32_t cost) { return ai::to_single(Number(cost & 65535U) / Number(10)); }
Number decoded_threat(std::uint32_t cost) { return ai::to_single(Number((cost >> 16U) * 100U)); }
}
core::Result<ExecutionRouteBuild> reference_build_execution_routes(const ExecutionRouteInput& input,
    const sim::PartitionExecutor& executor) {
    using Result = core::Result<ExecutionRouteBuild>;
    const auto n = input.planets.size();
    if (!input.player || !finite(input.maximum_distance) || input.maximum_distance < Number(0)
        || n > 1000) // paths beyond the sourced range reader's cap are unqualified
        return Result::failure(invalid("invalid execution graph player, distance or matrix size"));
    std::set<gc::PlanetId> ids;
    for (const auto& planet : input.planets) if (!planet.planet || !ids.insert(planet.planet).second
        || !finite(planet.layer_z_adjust) || !finite(planet.enemy_space_power) || !finite(planet.nonallied_starbase_power))
        return Result::failure(invalid("execution graph needs unique planets and qualified finite force/position facts"));
    std::map<std::pair<gc::PlanetId, gc::PlanetId>, Number> trades;
    for (const auto& route : input.trade_routes) {
        const auto speed = ai::to_single(numeric::from_fixed(route.hs_speed_factor));
        if (!ids.contains(route.from) || !ids.contains(route.to) || !(speed > Number(0)))
            return Result::failure(invalid("unqualified execution trade-route endpoints or speed"));
        trades.try_emplace({route.from, route.to}, speed); trades.try_emplace({route.to, route.from}, speed);
    }
    std::vector<std::uint32_t> costs(n * n, missing_cost);
    std::vector<std::size_t> previous(n * n, n);
    std::vector<std::uint8_t> invalid_rows(n);
    auto staged = executor.execute_phase("galactic-ai-execution-edges", sim::tick_partition_count, [&](std::size_t part) {
        const auto range = sim::partition_range(part, n);
        for (auto source = range.begin; source < range.end; ++source) for (std::size_t dest = 0; dest < n; ++dest) {
            const auto index = source * n + dest;
            if (source == dest) { costs[index] = 0; continue; }
            const auto& a = input.planets[source]; const auto& b = input.planets[dest];
            if (!a.usable || !b.usable) continue;
            const auto dx = ai::to_single(ai::to_single(numeric::from_fixed(b.position.x)) - ai::to_single(numeric::from_fixed(a.position.x)));
            const auto dy = ai::to_single(ai::to_single(numeric::from_fixed(b.position.y)) - ai::to_single(numeric::from_fixed(a.position.y)));
            const auto dz = ai::to_single(single_add(ai::to_single(numeric::from_fixed(b.position.z)), ai::to_single(a.layer_z_adjust))
                - ai::to_single(numeric::from_fixed(a.position.z)));
            auto distance = ai::to_single(ai::square_root(single_add(single_add(ai::to_single(dx * dx), ai::to_single(dy * dy)), ai::to_single(dz * dz))));
            const auto trade = trades.find({a.planet, b.planet});
            if (trade != trades.end()) distance = ai::to_single(distance / trade->second);
            else if (distance > ai::to_single(input.maximum_distance)) continue;
            const auto force = b.enemy_space_power + b.nonallied_starbase_power;
            const auto scaled_distance = ai::to_single(distance * Number(10));
            const auto integer_distance = ai::truncate(scaled_distance);
            const auto integer_force = ai::truncate(force);
            // Debug-build assertions do not define a normal-game recovery.
            // Refuse invalid inputs instead of advertising clamping as parity.
            if (!finite(force) || integer_distance < 0 || integer_distance >= 65535
                || integer_force < 0 || static_cast<std::uint64_t>(integer_force) / 100 >= 65535) { invalid_rows[source] = 1; continue; }
            costs[index] = (static_cast<std::uint32_t>(integer_force) / 100U << 16U) | static_cast<std::uint32_t>(integer_distance);
            previous[index] = source;
        }
    });
    if (!staged) return Result::failure(staged.error());
    if (std::any_of(invalid_rows.begin(), invalid_rows.end(), [](auto value) { return value != 0; }))
        return Result::failure(invalid("execution packed edge exceeds the qualified numeric domain"));
    // Each intermediate stage freezes its row and column before partitioning
    // sources. Nonnegative qualified costs leave the intermediate diagonal zero.
    // Strict improvements therefore match ordered intermediate/source/destination
    // relaxation while writes remain disjoint (ADR-009).
    for (std::size_t intermediate = 0; intermediate < n; ++intermediate) {
        std::vector<std::uint32_t> into(n), from(n);
        std::vector<std::size_t> predecessor(n);
        for (std::size_t i = 0; i < n; ++i) {
            into[i] = costs[i * n + intermediate]; from[i] = costs[intermediate * n + i]; predecessor[i] = previous[intermediate * n + i];
        }
        staged = executor.execute_phase("galactic-ai-execution-relax", sim::tick_partition_count, [&](std::size_t part) {
            const auto range = sim::partition_range(part, n);
            for (auto source = range.begin; source < range.end; ++source) for (std::size_t dest = 0; dest < n; ++dest) {
                if (into[source] == missing_cost || from[dest] == missing_cost) continue;
                if (!safe_sum(into[source], from[dest])) { invalid_rows[source] = 1; continue; }
                const auto candidate = into[source] + from[dest];
                if (candidate < costs[source * n + dest]) { costs[source * n + dest] = candidate; previous[source * n + dest] = predecessor[dest]; }
            }
        });
        if (!staged) return Result::failure(staged.error());
    }
    if (std::any_of(invalid_rows.begin(), invalid_rows.end(), [](auto value) { return value != 0; }))
        return Result::failure(invalid("execution packed relaxation exceeds the qualified numeric domain"));
    ExecutionRouteBuild out; out.rows.resize(n * n);
    staged = executor.execute_phase("galactic-ai-execution-decode", sim::tick_partition_count, [&](std::size_t part) {
        const auto range = sim::partition_range(part, n);
        for (auto source = range.begin; source < range.end; ++source) for (std::size_t dest = 0; dest < n; ++dest) {
            auto& row = out.rows[source * n + dest]; row.player = input.player; row.source = input.planets[source].planet; row.candidate = input.planets[dest].planet;
            std::uint32_t full = 0, prefix = 0;
            if (source != dest) {
                if (previous[source * n + dest] == n) { row.empty = true; full = prefix = missing_cost; }
                else {
                    auto current = dest; std::size_t steps = 0;
                    while (current != source && steps < n) {
                        const auto before = previous[source * n + current];
                        if (before == n) { invalid_rows[source] = 1; break; }
                        const auto edge = costs[before * n + current];
                        if (edge == missing_cost || !safe_sum(full, edge)) { invalid_rows[source] = 1; break; }
                        full += edge; if (steps) prefix += edge;
                        current = before; ++steps;
                    }
                    if (current != source) invalid_rows[source] = 1;
                }
            }
            row.distance = decoded_distance(full); row.threat = decoded_threat(full); row.prefix_threat = decoded_threat(prefix);
        }
    });
    if (!staged) return Result::failure(staged.error());
    if (std::any_of(invalid_rows.begin(), invalid_rows.end(), [](auto value) { return value != 0; }))
        return Result::failure(invalid("execution predecessor reconstruction outside the qualified domain"));
    out.edges_tested = static_cast<std::uint64_t>(n) * n; out.relaxations_tested = static_cast<std::uint64_t>(n) * n * n;
    auto paths = std::make_shared<ExecutionPathMatrix>(); paths->player = input.player;
    for (const auto& planet : input.planets) paths->nodes.push_back(planet.planet);
    for (const auto cost : costs) paths->costs.push_back(cost == missing_cost ? std::numeric_limits<std::uint64_t>::max()
        : (static_cast<std::uint64_t>(cost >> 16U) << 32U) | (cost & 65535U));
    paths->previous = std::move(previous); out.paths = std::move(paths);
    return Result::success(std::move(out));
}
} // namespace eawr::script::foc::galactic_ai::reference

namespace {
namespace ga = eawr::script::foc::galactic_ai;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using ga::Number;
using eawr::sim::math::Fixed;

struct Random {
    std::uint64_t state;
    std::uint64_t next() { state = state * 6364136223846793005ULL + 1442695040888963407ULL; return state >> 33U; }
    std::uint64_t below(std::uint64_t limit) { return limit ? next() % limit : 0; }
};

struct Shape { std::int64_t extent, reach, force_scale, count; std::uint64_t unusable_per_16, trade_per_16, grid; };

ga::ExecutionRouteInput random_input(Random& random, const Shape& shape) {
    ga::ExecutionRouteInput input; input.player = static_cast<gc::PlayerId>(1 + random.below(4));
    input.maximum_distance = Number(shape.reach);
    std::set<std::int64_t> used_ids;
    for (std::int64_t i = 0; i < shape.count; ++i) {
        ga::ExecutionPlanet planet;
        std::int64_t id = 0;
        do { id = static_cast<std::int64_t>(1 + random.below(static_cast<std::uint64_t>(shape.count) * 8)); } while (!used_ids.insert(id).second);
        planet.planet = static_cast<gc::PlanetId>(id);
        // grid: integer lattice points make many equal distances and equal forces (ties).
        const auto coordinate = [&]() -> std::int64_t {
            return shape.grid ? static_cast<std::int64_t>(random.below(shape.grid)) * (shape.extent / static_cast<std::int64_t>(shape.grid))
                              : static_cast<std::int64_t>(random.below(static_cast<std::uint64_t>(shape.extent) + 1));
        };
        planet.position = {Fixed::from_integer(coordinate()).value(), Fixed::from_integer(coordinate()).value(),
            Fixed::from_integer(random.below(4) == 0 ? static_cast<std::int64_t>(random.below(3)) : 0).value()};
        planet.layer_z_adjust = random.below(5) == 0 ? Number(static_cast<std::int64_t>(random.below(20))) : Number(0);
        const auto force = [&]() -> Number {
            if (!shape.force_scale) return Number(0);
            const auto raw = static_cast<std::int64_t>(random.below(static_cast<std::uint64_t>(shape.force_scale) + 1));
            return shape.grid ? Number(raw / 100 * 100) : Number(raw);
        };
        planet.enemy_space_power = force(); planet.nonallied_starbase_power = random.below(3) == 0 ? force() : Number(0);
        planet.usable = random.below(16) >= shape.unusable_per_16;
        input.planets.push_back(planet);
    }
    for (std::int64_t from = 0; from < shape.count; ++from) for (std::int64_t to = from + 1; to < shape.count; ++to) {
        if (random.below(16) >= shape.trade_per_16 || random.below(6) != 0) continue;
        gc::Route route; route.id = static_cast<gc::RouteId>(1 + input.trade_routes.size());
        route.from = input.planets[static_cast<std::size_t>(from)].planet; route.to = input.planets[static_cast<std::size_t>(to)].planet;
        route.hs_speed_factor = random.below(3) == 0 ? Fixed::from_ratio(1, static_cast<std::int64_t>(1 + random.below(40))).value()
                                                     : Fixed::from_integer(static_cast<std::int64_t>(1 + random.below(4))).value();
        input.trade_routes.push_back(route);
    }
    return input;
}

std::string describe(const ga::ExecutionRouteBuild& build) {
    std::string out = std::to_string(build.edges_tested) + "/" + std::to_string(build.relaxations_tested) + ";";
    for (const auto& row : build.rows)
        out += std::to_string(row.player) + ":" + std::to_string(row.source) + ":" + std::to_string(row.candidate) + ":" + (row.empty ? "e" : "r")
            + ":" + std::to_string(row.distance.repr) + ":" + std::to_string(row.threat.repr) + ":" + std::to_string(row.prefix_threat.repr) + ";";
    if (!build.paths) return out + "nopaths";
    out += "P" + std::to_string(build.paths->player) + ":";
    for (const auto node : build.paths->nodes) out += std::to_string(node) + ",";
    out += "C:";
    for (const auto cost : build.paths->costs) out += std::to_string(cost) + ",";
    out += "B:";
    for (const auto before : build.paths->previous) out += std::to_string(before) + ",";
    return out;
}

struct Tally { std::uint64_t succeeded{}, refused_edge{}, refused_relax{}, refused_decode{}, reused{}; };

void compare(const ga::ExecutionRouteInput& input, const eawr::sim::PartitionExecutor& executor, Tally& tally,
    const std::shared_ptr<const ga::ExecutionBuildMemo>& memo, std::shared_ptr<const ga::ExecutionBuildMemo>* next, std::string_view label) {
    auto expected = ga::reference::reference_build_execution_routes(input, executor);
    auto actual = ga::build_execution_routes(input, executor, memo);
    if (!expected) {
        expect(static_cast<bool>(actual), "former 16-bit numeric refusal builds in the widened domain");
        {
            const auto& message = expected.error().message;
            if (message.find("edge") != std::string::npos) ++tally.refused_edge;
            else if (message.find("relaxation") != std::string::npos) ++tally.refused_relax;
            else ++tally.refused_decode;
        }
        if (next) next->reset();
        return;
    }
    expect(static_cast<bool>(actual), label);
    if (!actual) return;
    expect(describe(expected.value()) == describe(actual.value()), label);
    ++tally.succeeded;
    if (actual.value().reused) ++tally.reused;
    if (next) *next = actual.value().memo;
}

void randomized(const eawr::sim::PartitionExecutor& executor, std::uint64_t seed, int rounds, Tally& tally) {
    Random random{seed};
    static constexpr Shape shapes[] = {
        // extent reach force_scale count unusable trade grid
        {200, 90, 0, 9, 0, 0, 0},          // no force at all: pure distance ties
        {240, 100, 900, 12, 1, 4, 6},      // lattice ties in distance and force
        {600, 250, 4000, 16, 2, 6, 0},     // moderate, mixed
        {3000, 1500, 20000, 14, 1, 6, 0},  // long lanes and big force: half-overflow refusals in relaxation
        {3000, 2200, 6000000, 10, 0, 3, 0}, // edge force near the numeric limit: staging and relaxation refusals
        {9000, 7000, 3000, 8, 0, 4, 0},    // distances beyond the packed range: staging refusals
        {120, 60, 300, 24, 3, 8, 8},       // larger, sparse, many unusable planets
        {400, 400, 2500, 30, 1, 6, 5},     // dense lattice with ties
    };
    for (int round = 0; round < rounds; ++round) for (const auto& shape : shapes) {
        auto input = random_input(random, shape);
        std::shared_ptr<const ga::ExecutionBuildMemo> memo;
        compare(input, executor, tally, nullptr, &memo, "randomized graph equals the all-pairs reference");
        // The same input again reuses the memo; a changed force invalidates it; both equal the reference.
        if (memo) {
            compare(input, executor, tally, memo, &memo, "identical staged edges reuse the previous build");
            auto changed = input;
            auto& planet = changed.planets[random.below(changed.planets.size())];
            switch (random.below(4)) {
            case 0: planet.usable = !planet.usable; break;
            case 1: planet.position.x = Fixed::from_integer(planet.position.x.trunc_to_integer() + 1 + static_cast<std::int64_t>(random.below(40))).value(); break;
            case 2: planet.layer_z_adjust = planet.layer_z_adjust + Number(static_cast<std::int64_t>(1 + random.below(30))); break;
            default: planet.enemy_space_power = planet.enemy_space_power + Number(100 * static_cast<std::int64_t>(1 + random.below(5))); break;
            }
            // Several force-only generations in a row keep reusing the geometry and must keep matching.
            for (int generation = 0; generation < 3; ++generation) {
                std::shared_ptr<const ga::ExecutionBuildMemo> step;
                compare(changed, executor, tally, memo, &step, "force-only generation equals the reference");
                if (step) memo = step;
                auto& mover = changed.planets[random.below(changed.planets.size())];
                mover.enemy_space_power = mover.enemy_space_power + Number(100 * static_cast<std::int64_t>(1 + random.below(9)));
            }
            std::shared_ptr<const ga::ExecutionBuildMemo> after;
            compare(changed, executor, tally, memo, &after, "changed force rebuilds instead of reusing");
        }
    }
}

void memo_contract() {
    const eawr::sim::InlineExecutor executor;
    Random random{4309};
    Shape shape{600, 250, 4000, 16, 0, 6, 0};
    auto input = random_input(random, shape);
    auto first = ga::build_execution_routes(input, executor);
    for (int attempt = 0; attempt < 64 && !first; ++attempt) { input = random_input(random, shape); first = ga::build_execution_routes(input, executor); }
    expect(static_cast<bool>(first) && !first.value().reused && first.value().memo, "first build has no memo to reuse and keeps one");
    if (!first) return;
    auto again = ga::build_execution_routes(input, executor, first.value().memo);
    expect(static_cast<bool>(again) && again.value().reused && again.value().paths == first.value().paths
        && again.value().memo == first.value().memo && describe(again.value()) == describe(first.value()),
        "identical edges reuse the memoized rows and the same published matrix");
    auto other_player = input; other_player.player = input.player + 1;
    auto rebuilt = ga::build_execution_routes(other_player, executor, first.value().memo);
    expect(static_cast<bool>(rebuilt) && !rebuilt.value().reused && rebuilt.value().paths != first.value().paths, "another player's build never reuses the memo");
    auto reordered = input; std::reverse(reordered.planets.begin(), reordered.planets.end());
    rebuilt = ga::build_execution_routes(reordered, executor, first.value().memo);
    expect(static_cast<bool>(rebuilt) && !rebuilt.value().reused, "a different node order never reuses the memo");
    auto moved = input; moved.planets[0].position.x = Fixed::from_integer(moved.planets[0].position.x.trunc_to_integer() + 1).value();
    rebuilt = ga::build_execution_routes(moved, executor, first.value().memo);
    auto fresh = ga::build_execution_routes(moved, executor);
    expect(static_cast<bool>(rebuilt) && static_cast<bool>(fresh) && describe(rebuilt.value()) == describe(fresh.value()), "moved planet rebuilds to the memo-free result");
}

void wide_contract() {
    const auto position = [](std::int64_t x) { return eawr::sim::math::Vec3{Fixed::from_integer(x).value(), {}, {}}; };
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        ga::ExecutionRouteInput input; input.player = 2; input.maximum_distance = Number(8000);
        input.planets = {{1, position(0), {}, Number(8000000), {}, true},
            {2, position(7000), {}, Number(9000000), {}, true},
            {3, position(14000), {}, Number(10000000), {}, true},
            {4, position(1), {}, {}, {}, false}};
        expect(!ga::reference::reference_build_execution_routes(input, executor), "large edge exceeds both old halves");
        auto built = ga::build_execution_routes(input, executor);
        expect(static_cast<bool>(built), "both widened edge halves and relaxation build");
        if (!built) continue;
        const auto& row = built.value().rows[2];
        expect(!row.empty && row.distance == Number(14000) && row.threat == Number(19000000)
            && row.prefix_threat == Number(9000000), "large route decodes full and prefix costs without carry");
        expect(built.value().paths->costs[2] == ((std::uint64_t{190000} << 32U) | 140000U)
            && built.value().paths->previous[2] == 1, "large route keeps exact components and predecessor");
        expect(built.value().rows[3].empty && built.value().rows[3].distance == (Number(65535) / Number(10))
            && built.value().rows[3].threat == Number(6553500), "dense missing edge never wraps into a cheap route");
        ga::ExecutionRoutes state; state.available = true; state.paths = built.value().paths;
        const auto path = ga::cached_execution_path(state, 2, 1, 3);
        expect(path && path.value().planets == std::vector<gc::PlanetId>{1, 2, 3}
            && path.value().threat == Number(19000000), "cached path uses wide costs");
        auto again = ga::build_execution_routes(input, executor, built.value().memo);
        expect(again && again.value().reused && describe(again.value()) == describe(built.value()), "wide edge memo reuses exact result");
        if (baseline.empty()) baseline = describe(built.value());
        else expect(baseline == describe(built.value()), "wide routes identical on 1/2/4/8 workers");

        // Every edge fits 16 bits, but a candidate relaxation overflows both former halves.
        input.maximum_distance = Number(4000);
        input.planets = {{1, position(0), {}, Number(4000000), {}, true},
            {2, position(3500), {}, Number(4000000), {}, true},
            {3, position(7000), {}, Number(4000000), {}, true}};
        auto legacy = ga::reference::reference_build_execution_routes(input, executor);
        expect(!legacy && legacy.error().message.find("relaxation") != std::string::npos, "fixture reaches the former relaxation refusal");
        built = ga::build_execution_routes(input, executor);
        expect(built && built.value().rows[2].distance == Number(7000) && built.value().rows[2].threat == Number(8000000),
            "former relaxation overflow now builds an exact two-hop route");

        // Largest integer threat halves whose two-way sum fits below the reserved boundary.
        input.maximum_distance = Number(1);
        input.planets = {{1, position(0), {}, Number(214748364700LL), {}, true},
            {2, position(1), {}, Number(214748364700LL), {}, true}};
        expect(static_cast<bool>(ga::build_execution_routes(input, executor)), "threat sum 2^32-2 fits exactly");
        input.planets[1].enemy_space_power = Number(214748364800LL);
        auto refused = ga::build_execution_routes(input, executor);
        expect(!refused && refused.error().message.find("relaxation") != std::string::npos, "threat sum at reserved boundary refuses");
        input.planets[1].enemy_space_power = Number(429496729500LL);
        refused = ga::build_execution_routes(input, executor);
        expect(!refused && refused.error().message.find("edge") != std::string::npos, "threat edge beyond widened bound refuses");

        input.maximum_distance = Number(500000000);
        input.planets = {{1, position(0), {}, {}, {}, true}, {2, position(214748352), {}, {}, {}, true}};
        expect(static_cast<bool>(ga::build_execution_routes(input, executor)), "distance sum just below widened boundary fits");
        input.planets[1].position = position(214748368);
        refused = ga::build_execution_routes(input, executor);
        expect(!refused && refused.error().message.find("relaxation") != std::string::npos, "distance half carry refuses beyond widened sum bound");
        input.planets[1].position = position(429496736);
        refused = ga::build_execution_routes(input, executor);
        expect(!refused && refused.error().message.find("edge") != std::string::npos, "distance edge beyond widened bound refuses");
    }
}
} // namespace

int case_galactic_ai_execution_routes_equality_tests(int, char**) {
    try {
        memo_contract();
        wide_contract();
        Tally tally;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter threaded(workers);
            randomized(threaded, 0x4309ULL + workers, 60, tally);
        }
        const eawr::sim::InlineExecutor inline_executor;
        randomized(inline_executor, 0x43090ULL, 60, tally);
        std::cout << "execution route equality: " << tally.succeeded << " built (" << tally.reused << " reused), refused "
                  << tally.refused_edge << " edge / " << tally.refused_relax << " relaxation / " << tally.refused_decode << " decode\n";
        // The shapes must really reach every refusal and the success path, or the comparison proves nothing.
        expect(tally.succeeded > 1000 && tally.refused_edge > 50 && tally.refused_relax > 50, "randomized shapes cover success and both numeric refusals");
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
