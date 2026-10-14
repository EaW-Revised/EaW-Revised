#include "galactic_ai_execution.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace eawr::script::foc::galactic_ai {
namespace {
core::Diagnostic invalid(std::string message) {
    core::Diagnostic out; out.code = std::string(authoritative::codes::invalid_request);
    out.message = std::move(message); return out;
}
auto key(const ai::GalacticExecutionRoute& row) { return std::tuple{row.player, row.source, row.candidate}; }
bool finite(Number value) { return (value.repr & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL; }
Number single_add(Number a, Number b) { return ai::to_single(a + b); }
constexpr auto missing_cost = std::numeric_limits<std::uint32_t>::max();
bool safe_sum(std::uint32_t a, std::uint32_t b) {
    return (a & 65535U) + (b & 65535U) < 65535U && (a >> 16U) + (b >> 16U) < 65535U;
}
Number decoded_distance(std::uint32_t cost) { return ai::to_single(Number(cost & 65535U) / Number(10)); }
Number decoded_threat(std::uint32_t cost) { return ai::to_single(Number((cost >> 16U) * 100U)); }
}
core::Result<ExecutionRouteBuild> build_execution_routes(const ExecutionRouteInput& input,
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
    paths->costs = std::move(costs); paths->previous = std::move(previous); out.paths = std::move(paths);
    return Result::success(std::move(out));
}
core::Result<bool> refresh_execution_routes(ExecutionRoutes& state, const std::uint64_t mode_frame,
    const bool execution_due, const sim::PartitionExecutor& executor, const RouteProducer& produce) {
    if (!execution_due || mode_frame < state.next_refresh) return core::Result<bool>::success(false);
    if (mode_frame > std::numeric_limits<std::uint64_t>::max() - 90 || state.revision == std::numeric_limits<std::uint64_t>::max())
        return core::Result<bool>::failure(invalid("execution route deadline/revision overflow"));
    auto staged = produce ? produce(executor)
        : core::Result<std::optional<std::vector<ai::GalacticExecutionRoute>>>::success(std::nullopt);
    if (!staged) return core::Result<bool>::failure(staged.error());
    if (staged.value()) {
        auto& rows = *staged.value();
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return key(a) < key(b); });
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (!row.player || !row.source || !row.candidate || (i && key(rows[i - 1]) == key(row)))
                return core::Result<bool>::failure(invalid("execution routes require unique nonzero player/source/candidate keys"));
            if (!row.empty && (!finite(row.distance) || !finite(row.threat) || !finite(row.prefix_threat)
                || !(row.distance >= Number(0)) || !(row.threat >= Number(0))
                || !(row.prefix_threat >= Number(0)) || row.prefix_threat > row.threat))
                return core::Result<bool>::failure(invalid("execution routes require nonnegative decoded distance/threat/prefix"));
        }
    }
    state.available = staged.value().has_value();
    state.paths.reset(); // scalar-only producers cannot qualify native free movement
    state.rows = state.available ? std::move(*staged.value()) : std::vector<ai::GalacticExecutionRoute>{};
    state.next_refresh = mode_frame + 90; // WGA-77, inclusive only inside due execution
    ++state.revision;
    return core::Result<bool>::success(true);
}
core::Result<ExecutionPath> cached_execution_path(const ExecutionRoutes& state, gc::PlayerId player,
    gc::PlanetId source, gc::PlanetId destination) {
    using Result = core::Result<ExecutionPath>;
    if (!state.available || !state.paths || state.paths->player != player)
        return Result::failure(invalid("direct freestore movement requires its retained player's published execution matrix"));
    const auto& matrix = *state.paths; const auto n = matrix.nodes.size();
    if (n > 1000 || matrix.costs.size() != n * n || matrix.previous.size() != n * n)
        return Result::failure(invalid("cached execution matrix framing is unqualified"));
    const auto a = std::find(matrix.nodes.begin(), matrix.nodes.end(), source);
    const auto b = std::find(matrix.nodes.begin(), matrix.nodes.end(), destination);
    if (a == matrix.nodes.end() || b == matrix.nodes.end()) return Result::failure(invalid("cached execution path endpoints are unqualified"));
    const auto start = static_cast<std::size_t>(a - matrix.nodes.begin());
    auto current = static_cast<std::size_t>(b - matrix.nodes.begin());
    ExecutionPath out;
    if (current != start && matrix.previous[start * n + current] == n) {
        out.threat = decoded_threat(missing_cost); return Result::success(std::move(out));
    }
    std::uint32_t full{}; out.planets.push_back(destination);
    while (current != start && out.planets.size() <= n) {
        const auto before = matrix.previous[start * n + current];
        if (before >= n || matrix.costs[before * n + current] == missing_cost || !safe_sum(full, matrix.costs[before * n + current]))
            return Result::failure(invalid("cached execution predecessor chain is unqualified"));
        full += matrix.costs[before * n + current]; current = before; out.planets.push_back(matrix.nodes[current]);
    }
    if (current != start) return Result::failure(invalid("cached execution predecessor cycle is unqualified"));
    std::reverse(out.planets.begin(), out.planets.end()); out.threat = decoded_threat(full);
    return Result::success(std::move(out));
}
core::Result<bool> publish_execution_routes(ExecutionRoutes& state, std::uint64_t frame, bool due,
    const sim::PartitionExecutor& executor, const ExecutionMatrixProducer& producer) {
    if (!due || frame < state.next_refresh) return core::Result<bool>::success(false);
    auto staged = state; std::shared_ptr<const ExecutionPathMatrix> paths;
    auto refreshed = refresh_execution_routes(staged, frame, due, executor, [&](const auto& workers) {
        using Result = core::Result<std::optional<std::vector<ai::GalacticExecutionRoute>>>;
        auto built = producer ? producer(workers) : core::Result<std::optional<ExecutionRouteBuild>>::success(std::nullopt);
        if (!built) return Result::failure(built.error());
        if (!built.value()) return Result::success(std::nullopt);
        if (!built.value()->paths) return Result::failure(invalid("full execution publication requires its packed predecessor matrix"));
        paths = built.value()->paths; return Result::success(std::move(built.value()->rows));
    });
    if (!refreshed || !refreshed.value()) return refreshed;
    staged.paths = std::move(paths); state = std::move(staged); return refreshed;
}
core::Result<void> initialize_execution_matrix(ExecutionRoutes& state, std::uint64_t frame,
    std::uint32_t count, std::uint32_t index, const sim::PartitionExecutor& executor, const ExecutionMatrixProducer& producer) {
    auto staged = state; std::shared_ptr<const ExecutionPathMatrix> paths;
    auto initialized = initialize_execution_routes(staged, frame, count, index, executor, [&](const auto& workers) {
        using Result = core::Result<std::optional<std::vector<ai::GalacticExecutionRoute>>>;
        auto built = producer ? producer(workers) : core::Result<std::optional<ExecutionRouteBuild>>::success(std::nullopt);
        if (!built) return Result::failure(built.error());
        if (!built.value()) return Result::success(std::nullopt);
        if (!built.value()->paths) return Result::failure(invalid("execution initialization requires its packed predecessor matrix"));
        paths = built.value()->paths; return Result::success(std::move(built.value()->rows));
    });
    if (!initialized) return initialized;
    staged.paths = std::move(paths); state = std::move(staged); return initialized;
}
ai::GalacticConnectivity execution_connectivity(const ExecutionRoutes& state) {
    ai::GalacticConnectivity out;
    out.revision = state.revision; out.execution_available = state.available; out.execution_routes = state.rows;
    return out;
}
core::Result<void> initialize_execution_routes(ExecutionRoutes& state, const std::uint64_t mode_frame,
    const std::uint32_t nonhuman_count, const std::uint32_t player_index, const sim::PartitionExecutor& executor, const RouteProducer& producer) {
    auto staged = state; staged.next_refresh = 0;
    auto built = refresh_execution_routes(staged, mode_frame, true, executor, producer);
    if (!built) return core::Result<void>::failure(built.error());
    const auto offset = nonhuman_count < 2 ? std::uint64_t{90} : static_cast<std::uint64_t>(
        single_add(ai::to_single(ai::to_single(Number(90) / Number(nonhuman_count)) * Number(player_index)), *ai::parse_real("0.5")));
    if (mode_frame > std::numeric_limits<std::uint64_t>::max() - offset)
        return core::Result<void>::failure(invalid("execution initialization deadline overflow"));
    staged.next_refresh = mode_frame + offset;
    state = std::move(staged);
    return core::Result<void>::success();
}
core::Result<void> rebuild_execution_routes_after_load(ExecutionRoutes& state, const bool finder_present,
    const sim::PartitionExecutor& executor, const RouteProducer& producer) {
    if (finder_present) return core::Result<void>::success();
    auto staged = state; staged.next_refresh = 0;
    const auto built = refresh_execution_routes(staged, 0, true, executor, producer);
    if (!built) return core::Result<void>::failure(built.error());
    staged.next_refresh = state.next_refresh;
    state = std::move(staged);
    return core::Result<void>::success();
}
bool freestore_eligible(const FreestoreObject& object, const gc::PlayerId player) noexcept {
    if (!object.id || object.owner != player || !player) return false;
    return object.kind == FreestoreKind::unit || (object.kind == FreestoreKind::fleet && !object.directly_parented_to_planet);
}
FreestoreAssignment freestore_assignment(const FreestorePool& pool, const sim::EntityId id,
    const gc::PlayerId player, const std::uint64_t potential_plan) noexcept {
    const auto object = std::lower_bound(pool.objects.begin(), pool.objects.end(), id,
        [](const auto& row, auto value) { return row.id < value; });
    if (object == pool.objects.end() || object->id != id
        || (pool.qualified ? !player || object->owner != player || !pool.qualified_players.contains(player) : !freestore_eligible(*object, player)))
        return FreestoreAssignment::failed;
    const auto reserved = pool.reservations.find(id);
    if ((reserved == pool.reservations.end() ? 0 : reserved->second) != potential_plan) return FreestoreAssignment::failed;
    return object->transit || !object->planet ? FreestoreAssignment::waiting : FreestoreAssignment::assigned;
}
} // namespace eawr::script::foc::galactic_ai
