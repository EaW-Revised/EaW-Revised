#pragma once
#include "eawr/sim/galactic/session.hpp"
#include "eawr/sim/galactic/income.hpp"

namespace eawr::sim::galactic::session_detail {
struct NativeIncomeSourceCandidate { EconomicSource source; bool fallback{}; };
void project_native_income(IncomeSetup&, const IncomeSetup&, const PoliticalControlSetup*,
    std::span<const Planet>, std::span<const Fleet>, std::span<const DetachedUnit>,
    const ProductionSetup*, std::span<const CensusObject>, std::vector<NativeIncomeSourceCandidate>&);
using NativeIncomeEdges = std::vector<std::vector<std::pair<std::size_t, std::size_t>>>;
[[nodiscard]] core::Result<std::vector<math::Fixed>> project_native_income_bonuses(
    const IncomeSetup*, std::span<const GamblingIncomeState>, std::span<const Planet>,
    std::span<const math::Fixed>, const PartitionExecutor&);
[[nodiscard]] core::Result<std::vector<PlanetIncomeContribution>> project_native_income_contributions(
    const FiscalSetup&, std::span<const Planet>, std::span<const math::Fixed>, const PoliticalControlSetup*,
    const EconomyCorruptionState*, const IncomeState*, std::span<const math::Fixed>, const NativeIncomeEdges&,
    const PartitionExecutor&);
} // namespace eawr::sim::galactic::session_detail
