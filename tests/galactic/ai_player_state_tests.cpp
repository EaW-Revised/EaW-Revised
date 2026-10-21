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
#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace eawr::script::foc::ai {
struct EngineContractAccess {
    static bool parallel_decision_reads(Engine& engine, authoritative::ScriptScheduler& scripts,
        tactical::PlayerId first, tactical::PlayerId second, const sim::PartitionExecutor& executor) {
        const std::array players{first, second};
        for (std::size_t index = 0; index < players.size(); ++index)
            engine.player_ai(players[index])->taskforces[901 + index].units = {51 + index};
        engine.rebuild_runtime_index(); engine.freeze_galactic_decision_reads();
        std::array<bool, 2> okay{};
        const auto ran = executor.execute_phase("test-engine-decision-scopes", sim::tick_partition_count, [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, players.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto captured = engine.galactic_decision_scope(players[index]);
                if (!captured) continue;
                auto scope = std::move(captured).value();
                const auto other = 1 - index;
                engine.operate({CrossOwnerOp::Kind::taskforce_join, 901 + other, 61 + index, 0, true});
                const Engine::BoundaryScope boundary;
                okay[index] = engine.taskforce(901 + other)->units == std::vector<sim::EntityId>({51 + other, 61 + index})
                    && engine.taskforce(901 + index)->units == std::vector<sim::EntityId>{51 + index};
            }
        });
        const auto committed = engine.commit_effects(scripts); engine.finish_galactic_decision_reads();
        return ran && committed && okay[0] && okay[1]
            && engine.taskforce(901)->units == std::vector<sim::EntityId>({51, 62})
            && engine.taskforce(902)->units == std::vector<sim::EntityId>({52, 61});
    }
    static bool independent_player_reads(Engine& engine, authoritative::ScriptScheduler& scripts,
        tactical::PlayerId first, tactical::PlayerId second) {
        auto& lower = *engine.player_ai(first); auto& higher = *engine.player_ai(second);
        constexpr std::uint64_t retained = 901, later = 902;
        lower.taskforces[retained].units = {51}; engine.rebuild_runtime_index();
        if (engine.galactic_decision_scope(first)) return false;
        engine.freeze_galactic_decision_reads();
        if (engine.galactic_decision_scope(0)) return false;
        lower.taskforces[retained].units.push_back(52);
        lower.taskforces[later].units = {55}; Engine::index_runtime(engine.runtime_index_.taskforces, later, first);
        lower.effects.credits[first] = math::Fixed::from_integer(10).value();
        lower.effects.pads[1000] = PadReservation{first, 1, 100, false, 0};
        bool okay = true;
        {
            auto scoped = engine.galactic_decision_scope(second);
            if (!scoped) return false;
            auto scope = std::move(scoped).value();
            const auto* before = engine.taskforce(retained);
            okay = okay && before && before->units == std::vector<sim::EntityId>{51} && !engine.taskforce(later);
            engine.operate({CrossOwnerOp::Kind::taskforce_join, retained, 53, 0, true});
            const auto* after = engine.taskforce(retained);
            okay = okay && after && after->units == std::vector<sim::EntityId>({51, 53})
                && lower.taskforces.at(retained).units == std::vector<sim::EntityId>({51, 52});
            higher.effects.credits[first] = math::Fixed::from_integer(20).value();
            okay = okay && engine.uncommitted_credits(first) == math::Fixed::from_integer(20).value().raw()
                && !engine.pad_claim(1000);
            engine.set_pad_claim(1000, {second, 2, 100, false, 0});
            okay = okay && engine.pad_claim(1000) && engine.pad_claim(1000)->player == second;
            {
                const Engine::BoundaryScope boundary;
                okay = okay && !engine.staged() && engine.taskforce(retained)
                    && engine.taskforce(retained)->units == std::vector<sim::EntityId>({51, 53});
                engine.operate({CrossOwnerOp::Kind::taskforce_join, retained, 54, 0, true});
                okay = okay && lower.taskforces.at(retained).units == std::vector<sim::EntityId>({51, 52});
            }
            okay = okay && engine.staged() == &higher.effects;
            {
                auto nested = engine.galactic_decision_scope(first);
                if (!nested) return false;
                okay = okay && engine.taskforce(retained)->units == std::vector<sim::EntityId>({51, 52});
            }
            okay = okay && engine.staged() == &higher.effects
                && engine.taskforce(retained)->units == std::vector<sim::EntityId>({51, 53, 54});
        }
        okay = okay && !engine.staged() && !engine.decision_effects();
        const auto committed = engine.commit_effects(scripts);
        engine.finish_galactic_decision_reads();
        return okay && committed && lower.taskforces.at(retained).units == std::vector<sim::EntityId>({51, 52, 53, 54})
            && engine.taskforce(later) && higher.effects.foreign_operations.empty() && higher.effects.foreign_players.empty();
    }
};
} // namespace eawr::script::foc::ai

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
void independent_player_reads() {
    eawr::sim::InlineExecutor executor; s9_fixture::Owner owner;
    owner.begin(executor);
    expect(ai::EngineContractAccess::independent_player_reads(*owner.engine, owner.scripts, owner.shape.first, owner.shape.second),
        "a frozen turn reads its own overlay over foreign barrier-start rows, pads and credits, then merges foreign operations once");
}

void parallel_decision_reads() {
    const auto run = [](const eawr::sim::PartitionExecutor& executor) {
        s9_fixture::Owner owner; owner.begin(executor);
        expect(ai::EngineContractAccess::parallel_decision_reads(*owner.engine, owner.scripts, owner.shape.first, owner.shape.second, executor),
            "decision scopes isolate foreign edits through attachment boundaries and merge identically after joined workers");
    };
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers); run(executor);
    }
    eawr::test_support::ReverseOrderExecutor reverse(8); run(reverse);
}

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
        independent_player_reads();
        parallel_decision_reads();
        order_independence();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "galactic AI player-state contracts passed\n";
    return 0;
}
