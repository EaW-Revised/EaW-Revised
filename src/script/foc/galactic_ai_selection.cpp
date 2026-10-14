#include "galactic_ai_selection.hpp"

#include <algorithm>

namespace eawr::script::foc::ai::galactic_goals {

core::Result<SelectionScores> score_candidates(std::span<const SelectionCandidate> input,
    std::uint32_t selected_total, const sim::PartitionExecutor& executor) {
    SelectionScores out;
    out.candidates.resize(input.size());
    const Real reject = rejected_estimate();
    if (!input.empty()) {
        auto prepared = executor.execute_phase("galactic-ai-selection", sim::tick_partition_count,
            [&](std::size_t partition) {
                const auto range = sim::partition_range(partition, input.size());
                for (std::size_t index = range.begin; index < range.end; ++index) {
                    auto& score = out.candidates[index];
                    for (std::size_t stage = 0; stage < input[index].stages.size(); ++stage) {
                        ++score.stages_visited;
                        const auto& contribution = input[index].stages[stage];
                        if (contribution.weight == reject) {
                            score.weight = -reject;
                            score.removed = contribution.permanent_failure;
                            score.rejection = static_cast<SelectionStage>(stage);
                            break;
                        }
                        score.weight = to_single(score.weight + contribution.weight);
                    }
                }
            });
        if (!prepared) return core::Result<SelectionScores>::failure(prepared.error());
    }
    // PL-24/WGA-60: use the pre-variety eligible set for every decision.
    const auto eligible = static_cast<std::size_t>(std::count_if(out.candidates.begin(), out.candidates.end(),
        [&](const CandidateScore& score) { return -reject < score.weight; }));
    for (std::size_t index = 0; index < input.size(); ++index) {
        auto& score = out.candidates[index];
        if (!(-reject < score.weight)) continue;
        if (selected_total > 0 && eligible > 1
            && static_cast<std::uint64_t>(input[index].selected_count) * 2 >= selected_total) {
            score.weight = -reject;
            continue;
        }
        if (!out.best || out.candidates[*out.best].weight < score.weight) out.best = index;
    }
    return core::Result<SelectionScores>::success(std::move(out));
}

Real freestore_preference(Real path_cost, Real maximum_cost, bool garrison, bool reserved) {
    if (reserved) return real(1);
    Real weight = real(2); // WGA-60: galactic admission preference
    if (maximum_cost != Real{}) weight = to_single(weight + to_single(real(1) - path_cost / maximum_cost));
    if (garrison) weight = to_single(weight + real(2));
    return weight;
}

std::vector<Real> production_preferences(std::span<const Real> excess_seconds) {
    std::vector<Real> out(excess_seconds.size());
    if (excess_seconds.empty()) return out;
    Real mean{};
    for (const auto value : excess_seconds) mean = to_single(mean + value);
    mean = to_single(mean / real(static_cast<std::int64_t>(excess_seconds.size())));
    const auto [low, high] = std::minmax_element(excess_seconds.begin(), excess_seconds.end());
    const Real range = to_single(*high - *low);
    if (range == Real{}) return out;
    const Real half = to_single(real(1) / real(2));
    const Real scale = to_single(std::min(to_single(range / real(120)), real(2)) / real(2));
    for (std::size_t index = 0; index < out.size(); ++index) {
        out[index] = std::clamp(to_single(to_single((mean - excess_seconds[index]) / range) * scale), -half, half);
    }
    return out;
}

} // namespace eawr::script::foc::ai::galactic_goals
