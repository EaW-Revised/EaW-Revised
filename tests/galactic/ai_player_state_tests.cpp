#include <cstddef>
#include <exception>
#include "../support/check.hpp"
// S9a: a galactic player's service call is one player turn. Everything it writes to engine-wide state stages in
// that player's effects; while the barrier owner defers the commit, the staged effects of every player wait for
// commit_galactic_events and apply there in ascending player order (GFS-A20). The deferred barrier must end in
// exactly the state the call-by-call commit reaches, on every worker count and with partitions run in reverse.
#include "ai_s9_fixture.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {
namespace ai = s9_fixture::ai;
namespace lifecycle = s9_fixture::lifecycle;
using s9_fixture::must;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool ok, std::string_view message) {
    if (!ok) local_checker.check_before_print(ok, message, std::cerr, "FAIL: ");
}

struct Outcome {
    std::string engine, world;
    std::uint64_t revision_before{}, revision_mid{}, revision_after{};
    std::uint64_t turns_mid{}, turns_after{};
    bool operator==(const Outcome& other) const { return engine == other.engine && world == other.world; }
};

Outcome run(const eawr::sim::PartitionExecutor& executor, const s9_fixture::Shape& shape, const bool deferred) {
    s9_fixture::Owner owner(shape);
    owner.begin(executor);
    owner.policy(shape.first); owner.policy(shape.second);
    owner.engine->defer_galactic_effects(deferred);
    Outcome outcome;
    outcome.revision_before = owner.engine->native_state_revision();
    std::uint64_t sequence{};
    for (const auto player : {shape.first, shape.second})
        for (const auto service : {lifecycle::Service::learning, lifecycle::Service::planning})
            must(owner.engine->service_galactic_system(player, service, owner.scripts, sequence));
    outcome.revision_mid = owner.engine->native_state_revision();
    outcome.turns_mid = owner.engine->effect_counts().turns;
    must(owner.engine->commit_galactic_events(owner.scripts, sequence));
    outcome.revision_after = owner.engine->native_state_revision();
    outcome.turns_after = owner.engine->effect_counts().turns;
    outcome.world = must(owner.world->state_hash());
    ai::GoalStateContributor contributor(owner.engine, s9_fixture::identity());
    outcome.engine = must(contributor.state_hash());
    return outcome;
}

void staged_turns() {
    const s9_fixture::Shape shape;
    eawr::sim::InlineExecutor executor;
    const auto direct = run(executor, shape, false);
    const auto deferred = run(executor, shape, true);
    // The call-by-call commit made every turn visible as its call returned.
    expect(direct.revision_mid > direct.revision_before, "an immediate-commit service call advances the native revision");
    // The deferred barrier commits nothing of any player's turn until commit_galactic_events; the revision readers
    // see (committed + staged) moves exactly as it does when each call commits, so revision-keyed caches invalidate alike.
    expect(deferred.revision_mid == direct.revision_mid, "a staged turn counts in the visible revision like a committed one");
    expect(deferred.turns_mid == 0, "no turn has committed before the barrier commit");
    expect(deferred.revision_after >= deferred.revision_mid, "the barrier commit never lowers the visible revision");
    expect(deferred.turns_after > 0, "the barrier commit applied the players' turns");
    // Same bytes either way: the commit order is the sequential service order (ascending player ID).
    expect(deferred == direct, "a deferred barrier ends in the state the call-by-call commit reaches");
    expect(deferred.revision_after == direct.revision_after, "the staged revisions sum to the direct ones");
}

// A script instance runs in the chain of the player that owns it: a freestore instance carries its player, a plan
// instance its slot's player; anything outside the AI domains belongs to no chain.
void chain_owners() {
    const s9_fixture::Shape shape;
    eawr::sim::InlineExecutor executor;
    s9_fixture::Owner owner(shape);
    owner.begin(executor);
    expect(owner.engine->galactic_chain_owner(shape.first) == shape.first && owner.engine->galactic_chain_owner(shape.second) == shape.second,
        "a freestore instance belongs to its player");
    expect(owner.engine->galactic_chain_owner(ai::galactic_instance_base(0) + 5) == shape.first, "a first-slot plan instance belongs to the first player");
    expect(owner.engine->galactic_chain_owner(ai::galactic_instance_base(1)) == shape.second, "a second-slot plan instance belongs to the second player");
    expect(owner.engine->galactic_chain_owner(ai::galactic_instance_base(ai::galactic_id_slots)) == 0, "an instance past the player slots has no chain");
    expect(owner.engine->galactic_chain_owner(ai::galactic_instance_base(30)) == 0, "a slot with no player has no chain");
}

void order_independence() {
    for (const auto& shape : {s9_fixture::Shape{}, [] { s9_fixture::Shape renamed; renamed.first = 5; renamed.second = 11; renamed.type = "Renamed";
            renamed.first_faction = "Left"; renamed.second_faction = "Right"; renamed.seed = 7001; return renamed; }()}) {
        eawr::sim::InlineExecutor inline_executor;
        const auto reference = run(inline_executor, shape, true);
        for (const auto workers : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(run(executor, shape, true) == reference, "the deferred barrier equals the inline run on every worker count");
        }
        for (const std::size_t reported : {std::size_t{1}, std::size_t{8}}) {
            eawr::test_support::ReverseOrderExecutor reverse(reported);
            expect(run(reverse, shape, true) == reference, "running every partition highest-first changes nothing");
        }
    }
}
} // namespace

int case_galactic_ai_player_state_tests() {
    try {
        staged_turns();
        chain_owners();
        order_independence();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "galactic AI player-state contracts passed\n";
    return 0;
}
