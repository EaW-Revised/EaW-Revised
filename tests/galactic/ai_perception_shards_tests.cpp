#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>
#include "../support/check.hpp"
// S9a: the galactic perception store keeps one shard per player. The layout is invisible: the same restored rows,
// hints and story flags, loaded in any order and spread over the shards in any way the order produces, export
// the same canonical bytes, hash the same incrementally and cold, and a restore round trip changes nothing. The
// worker count is invisible too: 1/2/4/8 workers and a highest-first executor agree with the inline run, and a
// capture saved on N workers restores on M to the same bytes, state and records digest.
#include "ai_s9_fixture.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace ai = s9_fixture::ai;
using s9_fixture::must;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool ok, std::string_view message) {
    if (!ok) local_checker.check_before_print(ok, message, std::cerr, "FAIL: ");
}

constexpr std::uint64_t frame = 100;

std::unique_ptr<ai::GalacticWorld> world(const s9_fixture::Shape& shape, const eawr::sim::PartitionExecutor& executor) {
    auto setup = s9_fixture::setup(shape);
    setup.cache_intervals["TARGETPLAYERID"] = ai::real(30);
    setup.cache_intervals["ENEMYFORCE.SPACETOTALUNNORMALIZED"] = ai::real(30);
    auto result = std::make_unique<ai::GalacticWorld>(std::move(setup));
    must(result->bind(s9_fixture::snapshot(shape), {}, executor, frame, 1000));
    return result;
}

ai::GalacticPerceptionState rows(const s9_fixture::Shape& shape, const bool reversed) {
    ai::GalacticPerceptionState state;
    std::vector<ai::GalacticCachedValue> cache;
    for (const auto player : {shape.first, shape.second}) for (const eawr::sim::EntityId planet : {10, 20}) {
        for (const auto& tokens : {std::vector<std::string>{"VARIABLE_TARGET", "TARGETPLAYERID"},
                std::vector<std::string>{"VARIABLE_TARGET", "ENEMYFORCE", "SPACETOTALUNNORMALIZED"}}) {
            ai::GalacticCachedValue row;
            row.key.player = player; row.key.planet = planet; row.key.tokens = tokens;
            if (tokens.size() == 3) row.key.bindings = {{"PARAMETER_CATEGORY", ai::real(7)}};
            row.value = ai::real(static_cast<std::int64_t>(player * 100 + planet)); row.interval = ai::real(30); row.frame = 50 + planet;
            cache.push_back(std::move(row));
        }
    }
    std::vector<ai::GalacticVisibility> visibility;
    for (const auto player : {shape.first, shape.second}) for (const eawr::sim::EntityId planet : {10, 20})
        visibility.push_back({player, planet, planet == 10, 60 + planet, 0});
    std::vector<ai::GalacticStoryFlag> flags{{shape.first, 10, "FLAG_A"}, {shape.second, 20, "FLAG_B"}, {shape.first, 0, "FLAG_C"}};
    std::vector<ai::GalacticHint> hints{{shape.first, "ALPHA", "SCOUTED", ai::real(1)}, {shape.second, "BETA", "SCOUTED", ai::real(2)}};
    if (reversed) {
        std::reverse(cache.begin(), cache.end()); std::reverse(visibility.begin(), visibility.end());
        std::reverse(flags.begin(), flags.end()); std::reverse(hints.begin(), hints.end());
    }
    state.cache = std::move(cache); state.visibility = std::move(visibility);
    state.flags = std::move(flags); state.hints = std::move(hints);
    return state;
}

struct Outcome {
    std::string captured, records, cold, hash;
    std::size_t cache{}, visibility{}, flags{}, hints{};
    bool operator==(const Outcome&) const = default;
};
Outcome load(const s9_fixture::Shape& shape, const eawr::sim::PartitionExecutor& executor, const bool reversed) {
    auto subject = world(shape, executor);
    must(subject->restore(rows(shape, reversed)));
    Outcome outcome;
    const auto native = must(subject->capture());
    outcome.captured = eawr::core::sha256_hex(native.components.at("GALACTIC_PERCEPTION"));
    outcome.records = must(subject->record_sync_hash(false));
    outcome.cold = must(subject->record_sync_hash(true));
    outcome.hash = must(subject->state_hash());
    const auto state = subject->state();
    outcome.cache = state.cache.size(); outcome.visibility = state.visibility.size();
    outcome.flags = state.flags.size(); outcome.hints = state.hints.size();
    return outcome;
}

void shard_layout_is_invisible() {
    const s9_fixture::Shape shape;
    eawr::sim::InlineExecutor executor;
    const auto forward = load(shape, executor, false);
    const auto backward = load(shape, executor, true);
    expect(forward.cache == 8 && forward.visibility == 4 && forward.flags == 3 && forward.hints == 2,
        "every restored row lands in a shard and comes back out");
    expect(forward == backward, "rows restored in reverse order export and hash the same");
    expect(forward.records == forward.cold, "the incremental record digest equals the cold rebuild across shards");
}

void round_trip_and_mutation() {
    const s9_fixture::Shape shape;
    eawr::sim::InlineExecutor executor;
    auto first = world(shape, executor);
    must(first->restore(rows(shape, false)));
    const auto saved = must(first->capture());
    auto second = world(shape, executor);
    must(second->restore(saved));
    expect(must(second->state_hash()) == must(first->state_hash()), "a capture restores to the same state");
    // Mutations reach only their own player's shard; the order the players change in never shows.
    auto a = world(shape, executor), b = world(shape, executor);
    must(a->restore(rows(shape, false))); must(b->restore(rows(shape, false)));
    expect(a->insert_hint({shape.first, "gamma", "seen", ai::real(3)}), "a new hint inserts");
    expect(a->insert_hint({shape.second, "gamma", "seen", ai::real(4)}), "a hint of the other player inserts");
    expect(!a->insert_hint({shape.first, "GAMMA", "SEEN", ai::real(9)}), "a duplicate hint is refused by its own shard");
    expect(b->insert_hint({shape.second, "gamma", "seen", ai::real(4)}) && b->insert_hint({shape.first, "gamma", "seen", ai::real(3)}), "the same hints insert in the other order");
    a->set_story_flag(shape.first, 20, "late"); a->set_story_flag(shape.second, 10, "late");
    b->set_story_flag(shape.second, 10, "late"); b->set_story_flag(shape.first, 20, "late");
    expect(a->check_story_flag(shape.first, 10, "flag_a", true) && b->check_story_flag(shape.first, 10, "flag_a", true),
        "a story flag is found and cleared in its player's shard");
    expect(!a->check_story_flag(shape.second, 10, "flag_a", false), "another player's shard does not hold it");
    a->visibility_changed(shape.first, 20, true); b->visibility_changed(shape.first, 20, true);
    expect(must(a->state_hash()) == must(b->state_hash()), "mutating the players in either order gives the same state");
    expect(must(a->record_sync_hash(false)) == must(a->record_sync_hash(true)), "the incremental digest still equals the cold rebuild");
}
void worker_counts_are_invisible() {
    const s9_fixture::Shape shape;
    eawr::sim::InlineExecutor inline_executor;
    const auto reference = load(shape, inline_executor, false);
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(load(shape, executor, false) == reference, "the shards load to the inline outcome on every determinism worker count");
        expect(load(shape, executor, true) == reference, "reversed rows load to the inline outcome on every determinism worker count");
    }
    for (const std::size_t reported : {std::size_t{1}, std::size_t{8}}) {
        eawr::test_support::ReverseOrderExecutor reverse(reported);
        expect(load(shape, reverse, false) == reference, "running every partition highest-first changes nothing");
    }
}

void save_at_n_restore_at_m() {
    const s9_fixture::Shape shape;
    for (const auto saved_on : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter saver(saved_on);
        auto first = world(shape, saver);
        must(first->restore(rows(shape, false)));
        expect(first->insert_hint({shape.first, "gamma", "seen", ai::real(3)}), "the hint inserts before the save");
        first->visibility_changed(shape.first, 20, true);
        const auto saved = must(first->capture());
        const auto saved_bytes = eawr::core::sha256_hex(saved.components.at("GALACTIC_PERCEPTION"));
        for (const auto restored_on : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter restorer(restored_on);
            auto second = world(shape, restorer);
            must(second->restore(saved));
            expect(eawr::core::sha256_hex(must(second->capture()).components.at("GALACTIC_PERCEPTION")) == saved_bytes,
                "a capture saved on N workers restores on M and exports the same bytes");
            expect(must(second->state_hash()) == must(first->state_hash()), "the restored state hash equals the saved one");
            expect(must(second->record_sync_hash(false)) == must(first->record_sync_hash(false)),
                "the restored records digest equals the saved one");
            expect(must(second->record_sync_hash(false)) == must(second->record_sync_hash(true)),
                "warm equals cold after the restore");
        }
    }
}
} // namespace

int case_galactic_ai_perception_shards_tests() {
    try {
        shard_layout_is_invisible();
        round_trip_and_mutation();
        worker_counts_are_invisible();
        save_at_n_restore_at_m();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "galactic perception shard contracts passed\n";
    return 0;
}
