// S9-early tripwire: the galactic AI barrier's partitioned phases must not depend on the order their
// partitions run in. The same barrier runs on the inline executor, on 1/2/4/8 workers and on an
// executor that visits partitions highest first; every state hash must be equal. The test also fails
// if the barrier stops using partitioned phases at all, so the check cannot silently go vacuous.
// S9b-prep adds the same tripwire for the path-table prepass and the decision-pass executor.
#include "ai_s9_fixture.hpp"
#include "galactic_ai_execution.hpp"
#include "eawr/script/foc/galactic_ai_pass.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdint>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace ai = s9_fixture::ai;
using s9_fixture::must;
int failures{};
void expect(bool ok, std::string_view message) { if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }

struct Outcome {
    std::string world, engine, perception;
    std::vector<std::string> goals;
    bool operator==(const Outcome&) const = default;
};

// One barrier plus the per-player work that follows it: perception reads, goal allocation (which draws
// from the player's stream) and the full native state hashes.
Outcome run(const eawr::sim::PartitionExecutor& executor, const s9_fixture::Shape& shape) {
    s9_fixture::Owner owner(shape);
    owner.begin(executor);
    owner.policy(shape.first); owner.policy(shape.second);
    Outcome outcome;
    for (const auto player : {shape.first, shape.second}) {
        const auto planet = player == shape.first ? eawr::sim::EntityId{10} : eawr::sim::EntityId{20};
        const auto* target = owner.engine->target_of_object(planet);
        const auto value = owner.engine->evaluate("Positive", player, target);
        outcome.perception += (value ? std::to_string(value->repr) : std::string("none")) + ";";
        const auto goal = owner.allocate(player, planet);
        outcome.goals.push_back(goal ? goal->definition + ":" + std::to_string(goal->potential_plan) + ":" + std::to_string(goal->goal) : "none");
    }
    outcome.world = must(owner.world->state_hash());
    ai::GoalStateContributor contributor(owner.engine, s9_fixture::identity());
    outcome.engine = must(contributor.state_hash());
    return outcome;
}

void order_independence() {
    for (const auto& shape : {s9_fixture::Shape{}, [] { s9_fixture::Shape renamed; renamed.first = 5; renamed.second = 11; renamed.type = "Renamed";
            renamed.first_faction = "Left"; renamed.second_faction = "Right"; renamed.seed = 7001; return renamed; }()}) {
        eawr::sim::InlineExecutor inline_executor;
        const auto reference = run(inline_executor, shape);
        expect(!reference.world.empty() && !reference.engine.empty(), "the reference run hashed both owners");
        for (const auto workers : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(run(executor, shape) == reference, "the barrier equals the inline run on every determinism worker count");
        }
        for (const std::size_t reported : {std::size_t{1}, std::size_t{8}}) {
            eawr::test_support::ReverseOrderExecutor reverse(reported);
            const auto reversed = run(reverse, shape);
            expect(reversed == reference, "running every partition highest-first changes nothing");
            // The tripwire: partitioned phases really ran, every one of them in reverse order.
            std::set<std::string> names;
            bool partitioned = false, descending = true;
            for (const auto& phase : reverse.phases) {
                names.insert(phase.name);
                partitioned = partitioned || phase.partitions > 1;
                descending = descending && phase.descending();
            }
            expect(partitioned, "the galactic AI barrier runs at least one partitioned phase");
            expect(descending, "the reverse executor visited every phase's partitions highest first");
            expect(names.contains("galactic-ai-planets") && names.contains("galactic-ai-objects"),
                "the planet and object preparation phases are partitioned");
            if (reported == 1) {
                std::cout << "reverse-order barrier phases:";
                for (const auto& name : names) std::cout << ' ' << name;
                std::cout << '\n';
            }
        }
    }
}

// The path-table prepass (Runtime::prepare_path_tables) is galactic-ai-execution-geometry/edges/relax/decode over a
// copied input. It must be a pure function of that input: equal on the inline executor, on every
// determinism worker count and with the partitions visited highest first.
std::string path_table_trace(const eawr::sim::PartitionExecutor& executor) {
    namespace ga = eawr::script::foc::galactic_ai;
    const auto position = [](std::int64_t x) {
        return eawr::sim::math::Vec3{eawr::sim::math::Fixed::from_integer(x).value(), {}, {}};
    };
    ga::ExecutionRouteInput input; input.player = 2; input.maximum_distance = ga::Number(11);
    input.planets = {{10, position(0), {}, ga::Number(900), {}, true}, {20, position(10), {}, ga::Number(149), {}, true},
        {30, position(20), {}, ga::Number(150), {}, true}, {40, position(200), {}, ga::Number(0), {}, true},
        {50, position(8), {}, ga::Number(30), ga::Number(5), true}};
    const auto built = must(ga::build_execution_routes(input, executor));
    std::ostringstream trace;
    trace << built.edges_tested << ',' << built.relaxations_tested << ';';
    for (const auto& row : built.rows)
        trace << row.source << ':' << row.candidate << ':' << row.empty << ':' << row.distance.repr << ':' << row.threat.repr
              << ':' << row.prefix_threat.repr << ';';
    if (built.paths) {
        for (const auto cost : built.paths->costs) trace << cost << ',';
        for (const auto previous : built.paths->previous) trace << previous << ',';
    }
    return trace.str();
}
void path_table_prepass() {
    eawr::sim::InlineExecutor inline_executor;
    const auto reference = path_table_trace(inline_executor);
    expect(reference.size() > 40, "the reference path table has rows and a predecessor matrix");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(path_table_trace(executor) == reference, "the path-table prepass equals the inline build on every determinism worker count");
    }
    eawr::test_support::ReverseOrderExecutor reverse(8);
    expect(path_table_trace(reverse) == reference, "the path-table prepass is unchanged with its partitions visited highest first");
    std::set<std::string> names;
    bool descending = true;
    for (const auto& phase : reverse.phases) { names.insert(phase.name); descending = descending && phase.descending(); }
    expect(descending, "every path-table phase ran highest partition first");
    expect(names == std::set<std::string>({"galactic-ai-execution-geometry", "galactic-ai-execution-edges", "galactic-ai-execution-relax", "galactic-ai-execution-decode"}),
        "the path-table prepass is exactly the geometry, edge, relax and decode phases");
}

// What the decision chain of one player is handed: inline on the calling thread, lowest partition first.
void decision_pass_executor() {
    namespace ga = eawr::script::foc::galactic_ai;
    const ga::DecisionPassExecutor executor;
    expect(executor.worker_count() == 1, "the decision-pass executor reports one worker");
    std::vector<std::size_t> visited;
    const auto caller = std::this_thread::get_id();
    bool inline_thread = true;
    must(executor.execute_phase("probe-phase", 5, [&](std::size_t part) {
        inline_thread = inline_thread && std::this_thread::get_id() == caller;
        visited.push_back(part);
    }));
    must(executor.execute(3, [&](std::size_t part) {
        inline_thread = inline_thread && std::this_thread::get_id() == caller;
        visited.push_back(100 + part);
    }));
    expect(visited == std::vector<std::size_t>({0, 1, 2, 3, 4, 100, 101, 102}), "the decision-pass executor runs partitions inline, lowest first");
    expect(inline_thread, "named and unnamed decision-pass partitions run on the calling thread");
}
} // namespace

int case_galactic_ai_partition_order_tests() {
    try {
        order_independence();
        path_table_prepass();
        decision_pass_executor();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "galactic AI partition-order tripwire passed\n";
    return 0;
}
