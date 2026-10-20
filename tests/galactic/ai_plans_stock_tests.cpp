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
#include "galactic_ai_goals.hpp"
#include "galactic_ai_selection.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <iostream>
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
            estimates(executor); selection(executor);
        }
        FailedExecutor failed;
        expect(!g::score_candidates(std::vector<g::SelectionCandidate>(1), 0, failed), "selector propagates executor failure");
        const auto row = producer(1, 1, Channel::ground, 0, 1);
        expect(!g::estimate_single_type(std::vector{row}, 1, 1, Channel::ground, failed), "mean propagates executor failure");
        expect(!g::estimate_production(std::vector{row}, 1, std::vector<g::ProductionRequest>{{1,Channel::ground}}, true, failed), "assignment propagates executor failure");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    if (failures != 0) return 1;
    std::cout << "Galactic AI component contracts passed on 1/2/4/8 workers\n";
    return 0;
}
