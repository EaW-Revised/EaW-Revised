#pragma once
#include "eawr/sim/galactic/session.hpp"
#include <map>
#include <set>

namespace eawr::sim::galactic::session_detail {
// Frozen at attach; player layers never build or mutate shared schema state.
struct NativePopulationSchema {
    PopulationCensus census;
    std::set<EntityId> backed;
    std::map<EntityId, EntityId> initial_containers;
};
[[nodiscard]] std::shared_ptr<const NativePopulationSchema> native_population_schema(const GalacticSetup&);
struct NativePopulationProjection {
    std::vector<PopulationTotal> totals;
    std::map<PlayerId, std::map<TypeId, std::uint64_t>> original_counts;
    std::vector<std::map<TypeId, std::uint64_t>> resolved_counts;
    std::size_t objects{};
};
[[nodiscard]] core::Result<NativePopulationProjection> project_native_population(
    const NativePopulationSchema&, std::span<const Planet>, std::span<const Fleet>, std::span<const DetachedUnit>,
    const PoliticalControlSetup*, const MovementSetup*, std::span<const CensusObject>,
    const ProductionSetup&, const PartitionExecutor&);
[[nodiscard]] bool apply_native_population(std::vector<ProductionPlayer>&, std::span<const PopulationTotal>,
    std::vector<std::map<TypeId, std::uint64_t>>&);
} // namespace eawr::sim::galactic::session_detail
