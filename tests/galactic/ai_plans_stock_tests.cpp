#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "../support/check.hpp"
#include "../support/result.hpp"
// Supporting pure contracts only. The installed galactic_ai_plans_stock proof
// still requires live A/B/C/E providers and retail U-02..04 qualification.
#include "galactic_ai_estimates.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "galactic_ai_goals.hpp"
#include "galactic_ai_selection.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <span>
#include <limits>
#include <exception>
using eawr::testing::must;

namespace {
namespace ai = eawr::script::foc::ai;
namespace g = ai::galactic_goals;
using R = ai::Real;
using Channel = g::ProductionChannel;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool ok, const char* message) {
    if (!ok) local_checker.check_before_print(ok, message, std::cerr, "FAIL: ");
}

g::ProducerInput producer(std::uint64_t id, std::uint64_t type, Channel channel,
    std::int64_t queue, std::int64_t build) {
    g::ProducerInput row;
    row.producer = id; row.player = 1; row.type = type; row.domain = channel;
    row.queue_seconds = eawr::sim::math::Fixed::from_integer(queue).value();
    row.build_seconds = eawr::sim::math::Fixed::from_integer(build).value();
    row.reachable = true; row.admission = g::gc::ProductionAdmission::allowed;
    row.faction_speed_factor = ai::real(1);
    return row;
}

// Verbatim copy of the pre-index estimator (map over all producers, full scores
// vector per request). The indexed estimator must reproduce it exactly.
g::ProductionEstimate reference_estimate(std::span<const g::ProducerInput> producers, std::uint64_t player,
    std::span<const g::ProductionRequest> requests, bool reachable) {
    using QueueKey = std::pair<eawr::sim::EntityId, Channel>;
    const auto usable = [&](const g::ProducerInput& row) {
        return row.player == player && row.reachable && row.producer != 0
            && row.admission == g::gc::ProductionAdmission::allowed;
    };
    g::ProductionEstimate out;
    out.feasible = true;
    if (requests.empty()) return out;
    if (!reachable) { out.feasible = false; out.seconds = g::rejected_estimate(); return out; }
    std::map<QueueKey, R> queues;
    for (const auto& row : producers)
        if (usable(row)) queues.try_emplace({row.producer, row.domain}, eawr::script::numeric::from_fixed(row.queue_seconds));
    R structure_total{};
    for (const auto& request : requests) {
        if (request.structure) {
            if (!request.target_admitted) { out.feasible = false; break; }
            structure_total = ai::to_single(structure_total + request.structure_seconds);
            out.producers.push_back(0);
            continue;
        }
        std::vector<R> scores(producers.size(), g::rejected_estimate());
        for (std::size_t index = 0; index < producers.size(); ++index) {
            const auto& row = producers[index];
            if (!usable(row) || row.type != request.type || row.domain != request.channel) continue;
            R score = queues.at({row.producer, row.domain});
            if (R{} < row.hyperspace_speed) {
                const R speed = ai::to_single(ai::to_single(row.hyperspace_speed * row.faction_speed_factor)
                    * ai::real(row.logical_fps));
                score = ai::to_single(score + ai::to_single(row.distance / speed));
            }
            const R levels = ai::real(static_cast<std::int64_t>(row.base_level) - row.required_level);
            scores[index] = ai::to_single(score * ai::to_single(ai::real(1) + ai::to_single(levels * row.technology_weight)));
        }
        std::size_t chosen = producers.size();
        R best = g::rejected_estimate();
        for (std::size_t index = 0; index < scores.size(); ++index) {
            ++out.producer_visits;
            if (scores[index] < best) { best = scores[index]; chosen = index; }
        }
        if (chosen == producers.size()) { out.feasible = false; break; }
        const auto& row = producers[chosen];
        auto& queue = queues.at({row.producer, row.domain});
        queue = ai::to_single(queue + eawr::script::numeric::from_fixed(row.build_seconds));
        out.seconds = std::max(out.seconds, queue);
        out.producers.push_back(row.producer);
    }
    out.seconds = out.feasible ? std::max(out.seconds, structure_total) : g::rejected_estimate();
    if (!out.feasible) out.producers.clear();
    return out;
}

void indexed_matches_reference(const eawr::sim::PartitionExecutor& executor) {
    // Large fixture: many producers, each owning several types in both domains,
    // first-row queue seeds that differ from later rows, exact ties, travel,
    // level weights, locked/unreachable/foreign rows.
    std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
    const auto next = [&](std::uint64_t bound) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return seed % bound;
    };
    std::vector<g::ProducerInput> rows;
    for (std::uint64_t id = 1; id <= 1500; ++id) {
        for (const auto channel : {Channel::ground, Channel::space}) {
            const auto queue = static_cast<std::int64_t>(next(4) * 10);
            for (std::uint64_t type = 1; type <= 6; ++type) {
                if (next(3) == 0) continue;
                auto row = producer(id, type + next(40), channel, queue, 5 + static_cast<std::int64_t>(next(5)) * 5);
                if (next(5) == 0) row.queue_seconds = eawr::sim::math::Fixed::from_integer(static_cast<std::int64_t>(next(60))).value();
                if (next(4) == 0) { row.hyperspace_speed = ai::real(1); row.distance = ai::real(static_cast<std::int64_t>(next(5) * 100)); }
                if (next(6) == 0) { row.base_level = static_cast<std::uint32_t>(next(4)); row.technology_weight = ai::real(1) / ai::real(4); }
                if (next(9) == 0) row.admission = g::gc::ProductionAdmission::locked;
                if (next(11) == 0) row.reachable = false;
                if (next(7) == 0) row.player = 2;
                rows.push_back(row);
            }
        }
    }
    expect(rows.size() > 10000, "equality fixture is large");
    for (int round = 0; round < 60; ++round) {
        std::vector<g::ProductionRequest> requests;
        const auto count = 1 + next(40);
        for (std::uint64_t i = 0; i < count; ++i) {
            g::ProductionRequest request{1 + next(45), next(2) ? Channel::ground : Channel::space};
            if (next(12) == 0) { request.structure = true; request.target_admitted = next(8) != 0; request.structure_seconds = ai::real(static_cast<std::int64_t>(next(90))); }
            requests.push_back(request);
        }
        const auto expected = reference_estimate(rows, 1, requests, true);
        const auto actual = must(g::estimate_production(rows, 1, requests, true, executor));
        expect(actual.feasible == expected.feasible && actual.seconds == expected.seconds
            && actual.producers == expected.producers && actual.producer_visits == expected.producer_visits,
            "indexed estimate equals the pre-index estimate");
        const g::ProducerIndex index(rows, 1);
        const auto shared = must(g::estimate_production(index, requests, true, executor));
        expect(shared.feasible == expected.feasible && shared.seconds == expected.seconds
            && shared.producers == expected.producers && shared.producer_visits == expected.producer_visits,
            "shared index estimate equals the pre-index estimate");
        // Growing prefix, as the selector loop asks for it.
        for (std::size_t prefix = 1; prefix <= std::min<std::size_t>(requests.size(), 12); ++prefix) {
            const std::span<const g::ProductionRequest> head(requests.data(), prefix);
            const auto want = reference_estimate(rows, 1, head, true);
            const auto got = must(g::estimate_production(index, head, true, executor));
            expect(got.feasible == want.feasible && got.seconds == want.seconds && got.producers == want.producers
                && got.producer_visits == want.producer_visits, "prefix estimate equals the pre-index estimate");
        }
    }
}

// A round's input is fixed, but the next round sees a changed world: queues grow, builds
// complete, producers appear or get locked. A fresh index must follow every change; an
// index kept from before the change (negative control) must visibly diverge.
void index_follows_changed_producers(const eawr::sim::PartitionExecutor& executor) {
    std::vector<g::ProducerInput> rows;
    for (std::uint64_t id = 1; id <= 40; ++id) {
        rows.push_back(producer(id, 1, Channel::ground, static_cast<std::int64_t>(id % 5) * 10, 20));
        rows.push_back(producer(id, 2, Channel::ground, static_cast<std::int64_t>(id % 5) * 10, 15));
    }
    const std::vector<g::ProductionRequest> requests{{1, Channel::ground}, {2, Channel::ground}, {1, Channel::ground},
        {1, Channel::ground}, {2, Channel::ground}};
    const auto old_rows = rows; // the world the stale index was built from
    const g::ProducerIndex before(old_rows, 1);
    const auto same = [](const g::ProductionEstimate& a, const g::ProductionEstimate& b) {
        return a.feasible == b.feasible && a.seconds == b.seconds && a.producers == b.producers
            && a.producer_visits == b.producer_visits;
    };
    expect(same(must(g::estimate_production(before, requests, true, executor)), reference_estimate(rows, 1, requests, true)),
        "index equals reference before any change");
    int diverged = 0;
    for (int step = 0; step < 6; ++step) {
        // A build completes (queue of the cheapest producer drops), a producer is locked, a new one appears.
        if (step == 0) for (auto& row : rows) if (row.producer == 5) row.queue_seconds = eawr::sim::math::Fixed::from_integer(0).value();
        if (step == 1) for (auto& row : rows) if (row.producer == 10) row.queue_seconds = eawr::sim::math::Fixed::from_integer(500).value();
        if (step == 2) for (auto& row : rows) if (row.producer == 3) row.admission = g::gc::ProductionAdmission::locked;
        if (step == 3) { rows.insert(rows.begin(), producer(99, 1, Channel::ground, 0, 1)); rows.push_back(producer(99, 2, Channel::ground, 0, 1)); }
        if (step == 4) for (auto& row : rows) if (row.producer == 99) row.reachable = false;
        if (step == 5) for (auto& row : rows) if (row.producer == 7) row.build_seconds = eawr::sim::math::Fixed::from_integer(1).value();
        const auto want = reference_estimate(rows, 1, requests, true);
        const g::ProducerIndex fresh(rows, 1);
        expect(same(must(g::estimate_production(fresh, requests, true, executor)), want), "fresh index follows the changed producers");
        expect(same(must(g::estimate_production(rows, 1, requests, true, executor)), want), "span overload follows the changed producers");
        // Negative control: the index built before any change keeps answering from the old world.
        if (!same(must(g::estimate_production(before, requests, true, executor)), want)) ++diverged;
    }
    expect(diverged > 0, "negative control: an index that misses a changed producer diverges from the reference");
}

// The running fold (commit one request, probe the next) must equal the one-shot estimate of the
// whole list at every step, including structures, ties and the first infeasible request.
void fold_matches_reference(const eawr::sim::PartitionExecutor& executor) {
    std::uint64_t seed = 0x1234567887654321ULL;
    const auto next = [&](std::uint64_t bound) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return seed % bound;
    };
    std::vector<g::ProducerInput> rows;
    for (std::uint64_t id = 1; id <= 300; ++id)
        for (const auto channel : {Channel::ground, Channel::space})
            for (std::uint64_t type = 1; type <= 5; ++type) {
                if (next(3) == 0) continue;
                auto row = producer(id, type + next(12), channel, static_cast<std::int64_t>(next(3) * 10), 5 + static_cast<std::int64_t>(next(3)) * 5);
                if (next(4) == 0) { row.hyperspace_speed = ai::real(1); row.distance = ai::real(static_cast<std::int64_t>(next(4) * 100)); }
                if (next(8) == 0) row.admission = g::gc::ProductionAdmission::locked;
                rows.push_back(row);
            }
    const g::ProducerIndex index(rows, 1);
    for (int round = 0; round < 80; ++round) {
        g::ProductionFold fold(index);
        std::vector<g::ProductionRequest> committed;
        for (int step = 0; step < 30; ++step) {
            g::ProductionRequest request{1 + next(18), next(2) ? Channel::ground : Channel::space};
            if (next(10) == 0) { request.structure = true; request.target_admitted = next(6) != 0; request.structure_seconds = ai::real(static_cast<std::int64_t>(next(80))); }
            auto prefix_plus = committed; prefix_plus.push_back(request);
            const auto want = reference_estimate(rows, 1, prefix_plus, true);
            const auto probed = must(fold.probe(request, executor));
            expect(probed.feasible == want.feasible && (!want.feasible || probed.producer == want.producers.back()),
                "probe equals the one-shot estimate of prefix plus candidate");
            const auto before_seconds = fold.seconds();
            must(fold.probe(request, executor));
            expect(fold.seconds() == before_seconds && fold.feasible() == (committed.empty() || reference_estimate(rows, 1, committed, true).feasible),
                "probe leaves the fold unchanged");
            must(fold.commit(request, executor));
            committed.push_back(request);
            const auto total = reference_estimate(rows, 1, committed, true);
            expect(fold.feasible() == total.feasible && fold.seconds() == total.seconds
                && (!total.feasible || fold.producers() == total.producers), "commit equals the one-shot estimate of the prefix");
            if (!total.feasible) break;
        }
    }
}

void estimates(const eawr::sim::PartitionExecutor& executor) {
    // Queues share identity across types, but never across ground/space.
    std::vector<g::ProducerInput> rows{
        producer(10, 1, Channel::ground, 10, 20), producer(10, 2, Channel::ground, 10, 15),
        producer(10, 3, Channel::space, 50, 30), producer(20, 1, Channel::ground, 40, 20),
        producer(20, 2, Channel::ground, 40, 15), producer(20, 3, Channel::space, 0, 30),
        producer(30, 1, Channel::ground, 0, 1), producer(40, 1, Channel::ground, 0, 1)};
    rows[6].reachable = false; rows[7].player = 2;
    const std::vector<g::ProductionRequest> requests{
        {1, Channel::ground}, {2, Channel::ground}, {3, Channel::space},
        {3, Channel::space}, {3, Channel::space}};
    const auto result = must(g::estimate_production(rows, 1, requests, true, executor));
    expect(result.feasible && result.seconds == ai::real(80), "WGA-58 channel maximum, not sum");
    expect(result.producers == std::vector<std::uint64_t>{10,10,20,20,10}, "WGA-58 shared channel queue and producer selection");
    expect(result.producer_visits == requests.size() * rows.size(), "producer work count is independent of workers");
    expect(must(g::estimate_single_type(rows, 1, 1, Channel::ground, executor)) == ai::real(45), "WGA-59 mean, not minimum; excludes unreachable/other player");
    expect(must(g::estimate_single_type(rows, 1, 99, Channel::space, executor)) == g::rejected_estimate(), "WGA-59 no producer sentinel");
    const auto empty = must(g::estimate_production(rows, 1, {}, false, executor));
    expect(empty.feasible && empty.seconds == R{} && empty.producer_visits == 0, "WGA-58 empty list zero");
    auto unreachable = must(g::estimate_production(rows, 1, requests, false, executor));
    expect(!unreachable.feasible && unreachable.seconds == g::rejected_estimate(), "WGA-58 unreachable sentinel");
    const std::vector<g::ProductionRequest> missing{{1, Channel::ground}, {99, Channel::space}};
    const auto failed = must(g::estimate_production(rows, 1, missing, true, executor));
    expect(!failed.feasible && failed.producers.empty() && failed.seconds == g::rejected_estimate(), "missing location invalidates whole estimate");
    const std::vector<g::ProductionRequest> structures{{0, Channel::ground, true, true, ai::real(55)},
        {0, Channel::space, true, true, ai::real(60)}};
    const auto built = must(g::estimate_production(rows, 1, structures, true, executor));
    expect(built.feasible && built.seconds == ai::real(115) && built.producer_visits == 0, "WGA-58 structure serial total");
    auto no_target = structures; no_target[1].target_admitted = false;
    expect(!must(g::estimate_production(rows, 1, no_target, true, executor)).feasible, "structure needs admitted target");

    auto a = producer(7, 1, Channel::ground, 0, 20);
    auto b = producer(8, 1, Channel::ground, 0, 20);
    a.distance = ai::real(300); a.hyperspace_speed = ai::real(1);
    b.distance = ai::real(600); b.hyperspace_speed = ai::real(1);
    const std::vector<g::ProductionRequest> one{{1, Channel::ground}};
    auto travelled = must(g::estimate_production(std::vector{a,b}, 1, one, true, executor));
    expect(travelled.producers == std::vector<std::uint64_t>{7} && travelled.seconds == ai::real(20), "travel selects producer, does not inflate queue");
    a.hyperspace_speed = R{}; a.distance = ai::real(100000);
    expect(must(g::estimate_production(std::vector{a,b}, 1, one, true, executor)).producers.front() == 7, "zero hyperspace speed adds no travel");
    a = producer(7, 1, Channel::ground, 10, 20); b = producer(8, 1, Channel::ground, 15, 20);
    a.base_level = 2; a.technology_weight = ai::real(1);
    expect(must(g::estimate_production(std::vector{a,b}, 1, one, true, executor)).producers.front() == 8, "level multiplier affects selection only");
    a = producer(7, 1, Channel::ground, 0, 20); b = producer(8, 1, Channel::ground, 0, 20);
    expect(must(g::estimate_production(std::vector{b,a}, 1, one, true, executor)).producers.front() == 8, "first equal producer retains provider order");
    a.admission = g::gc::ProductionAdmission::locked;
    expect(must(g::estimate_production(std::vector{a,b}, 1, one, true, executor)).producers.front() == 8, "producer admission precedes estimate");
}

void selection(const eawr::sim::PartitionExecutor& executor) {
    std::vector<g::SelectionCandidate> input(4);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index].type = index + 1;
        input[index].stages[0].weight = ai::real(2);
    }
    input[0].stages[4].weight = ai::real(-1);
    input[1].stages[4].weight = ai::real(-1); // signed tie: first survives
    input[2].stages[1] = {g::rejected_estimate(), false};
    input[2].stages[8].weight = ai::real(1000);
    input[3].stages[5] = {g::rejected_estimate(), true};
    auto scores = must(g::score_candidates(input, 0, executor));
    expect(scores.best == 0 && scores.candidates[0].weight == ai::real(1), "WGA-60 signed contributions and first tie");
    expect(!scores.candidates[2].removed && scores.candidates[2].rejection == g::SelectionStage::budget
        && scores.candidates[2].stages_visited == 2, "temporary rejection stops later stages but retains proposal");
    expect(scores.candidates[3].removed && scores.candidates[3].stages_visited == 6, "permanent rejection removes at rejecting stage");
    input[0].selected_count = 1;
    scores = must(g::score_candidates(input, 2, executor));
    expect(scores.best == 1, "variety yields at half allocation when another candidate survives");
    input[1].stages[0].weight = g::rejected_estimate();
    expect(must(g::score_candidates(input, 2, executor)).best == 0, "sole survivor does not yield to variety");
    input[0].stages[0].weight = g::rejected_estimate();
    expect(!must(g::score_candidates(input, 0, executor)).best, "all rejected means no winner");
    input = std::vector<g::SelectionCandidate>(2);
    input[0].stages[4].weight = ai::real(-2); input[1].stages[4].weight = ai::real(-1);
    expect(must(g::score_candidates(input, 0, executor)).best == 1, "negative valid score can win");
    expect(!must(g::score_candidates({}, 0, executor)).best, "empty selection");
    expect(g::freestore_preference(R{}, R{}, false, false) == ai::real(2), "zero path maximum skips fraction");
    expect(g::freestore_preference(ai::real(5), ai::real(10), true, false) == ai::real(9) / ai::real(2), "garrison adds two independently");
    expect(g::freestore_preference(ai::real(5), ai::real(10), true, true) == ai::real(1), "reserved freestore preference");
    const auto preferences = g::production_preferences(std::vector<R>{R{}, ai::real(240)});
    expect(preferences[0] == ai::real(1)/ai::real(2) && preferences[1] == -ai::real(1)/ai::real(2), "production preserves signed preference endpoints");
    const auto equal = g::production_preferences(std::vector<R>{ai::real(20), ai::real(20)});
    expect(equal[0] == R{} && equal[1] == R{}, "zero production range contributes zero");
}

void exclusions() {
    g::ProposalCounts state;
    state.ordinary["CONQUER"] = 1;
    const std::vector<std::string> types{"CONQUER"};
    expect(g::proposable(state, "CONQUER") && !g::proposable(state, "UNKNOWN"), "WGA-57 ordinary count required");
    state = must(g::activate_exclusions(state, types));
    state = must(g::activate_exclusions(state, types));
    expect(state.exclusions.at("CONQUER") == 2 && !g::proposable(state, "CONQUER"), "WGA-57 0 to 1 to 2 exclusion");
    state = g::release_exclusions(state, types);
    expect(state.exclusions.at("CONQUER") == 1 && !g::proposable(state, "CONQUER"), "one release does not clear other goal");
    state = g::release_exclusions(state, types);
    expect(state.exclusions.contains("CONQUER") && state.exclusions.at("CONQUER") == 0 && g::proposable(state, "CONQUER"), "zero row retained and proposable");
    state = g::release_exclusions(state, types);
    expect(state.exclusions.at("CONQUER") == 0, "exclusion release never underflows");
    state.blockers = 1; expect(!g::proposable(state, "CONQUER"), "global proposal blocker");
    state.blockers = 0; state.ordinary["CONQUER"] = 0;
    expect(!g::proposable(state, "CONQUER"), "zero ordinary proposal count");
    state.exclusions["CONQUER"] = std::numeric_limits<std::uint64_t>::max();
    const std::vector<std::string> overflow{"OTHER", "CONQUER"};
    expect(!g::activate_exclusions(state, overflow) && !state.exclusions.contains("OTHER"), "overflow has no partial mutation");
}
class FailedExecutor final : public eawr::sim::PartitionExecutor {
public:
    std::size_t worker_count() const noexcept override { return 1; }
    eawr::core::Result<void> execute(std::size_t, const std::function<void(std::size_t)>&) const override {
        return eawr::core::Result<void>::failure({"test.failure", eawr::core::Severity::error, "failed phase", {}, {}, {}, {}});
    }
};
}

int case_galactic_ai_plans_tests() {
    try {
        exclusions();
        for (const auto workers : {1u, 2u, 4u, 8u}) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            estimates(executor); indexed_matches_reference(executor); index_follows_changed_producers(executor); fold_matches_reference(executor); selection(executor);
        }
        FailedExecutor failed;
        expect(!g::score_candidates(std::vector<g::SelectionCandidate>(1), 0, failed), "selector propagates executor failure");
        const auto row = producer(1, 1, Channel::ground, 0, 1);
        expect(!g::estimate_single_type(std::vector{row}, 1, 1, Channel::ground, failed), "mean propagates executor failure");
        std::vector<g::ProducerInput> crowd;
        for (std::uint64_t id = 1; id <= 2100; ++id) crowd.push_back(producer(id, 1, Channel::ground, 0, 1));
        expect(!g::estimate_production(crowd, 1, std::vector<g::ProductionRequest>{{1,Channel::ground}}, true, failed), "assignment propagates executor failure");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    if (failures != 0) return 1;
    std::cout << "Galactic AI component contracts passed on 1/2/4/8 workers\n";
    return 0;
}
