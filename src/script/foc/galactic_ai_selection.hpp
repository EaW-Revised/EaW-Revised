#pragma once

#include "galactic_ai_estimates.hpp"

#include "eawr/sim/world.hpp"

#include <optional>

namespace eawr::script::foc::ai::galactic_goals {

// WGA-60: array order is the stage order. Facts/weights come from the owned
// world, funding and plan adapters; this component does not fabricate them.
enum class SelectionStage : std::uint8_t {
    freestore, budget, tech, team, production, hero, terrain, contrast, survival, count
};
struct StageContribution {
    Real weight{};
    bool permanent_failure{};
};
struct SelectionCandidate {
    gc::TypeId type{};
    std::array<StageContribution, static_cast<std::size_t>(SelectionStage::count)> stages{};
    std::uint32_t selected_count{};
};
struct CandidateScore {
    Real weight{};
    bool removed{};
    std::optional<SelectionStage> rejection;
    std::uint32_t stages_visited{};
};
struct SelectionScores {
    std::vector<CandidateScore> candidates;
    std::optional<std::size_t> best;
};

// Immutable input, disjoint stage evaluation, ordered variety/tie commit.
[[nodiscard]] core::Result<SelectionScores> score_candidates(std::span<const SelectionCandidate>,
    std::uint32_t selected_total, const sim::PartitionExecutor&);
[[nodiscard]] Real freestore_preference(Real path_cost, Real maximum_cost, bool garrison, bool reserved);
[[nodiscard]] std::vector<Real> production_preferences(std::span<const Real> excess_seconds);

} // namespace eawr::script::foc::ai::galactic_goals
