// S9-early tripwire: the galactic AI barrier's partitioned phases must not depend on the order their
// partitions run in. The same barrier runs on the inline executor, on 1/2/4/8 workers and on an
// executor that visits partitions highest first; every state hash must be equal. The test also fails
// if the barrier stops using partitioned phases at all, so the check cannot silently go vacuous.
#include "ai_s9_fixture.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
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
} // namespace

int case_galactic_ai_partition_order_tests() {
    try {
        order_independence();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "galactic AI partition-order tripwire passed\n";
    return 0;
}
