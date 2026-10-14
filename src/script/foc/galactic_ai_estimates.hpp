#pragma once

#include "ai_equations.hpp"
#include "eawr/script/foc/galactic_ai.hpp"
#include "eawr/sim/world.hpp"

namespace eawr::script::foc::ai::galactic_goals {
namespace gc = sim::galactic;
using ProducerInput = foc::galactic_ai::ProducerInput;
using ProductionChannel = gc::ProductionDomain;

struct ProductionRequest {
    gc::TypeId type{};
    ProductionChannel channel{};
    // WGA-58: structures/bases use an admitted target, outside mobile queues.
    bool structure{};
    bool target_admitted{};
    Real structure_seconds{};
};
struct ProductionEstimate {
    bool feasible{};
    Real seconds{};
    std::vector<sim::EntityId> producers; // request order; zero for structures
    std::uint64_t producer_visits{};
};
// WGA-58/60 sourced sentinel. Inputs preserve the provider's canonical order;
// production rows must have unique player/producer/domain/type keys and equal
// current queue times for a given producer/channel. Only admitted rows enter.
[[nodiscard]] inline Real rejected_estimate() { return real(1000000000000000000LL); }
[[nodiscard]] core::Result<ProductionEstimate> estimate_production(std::span<const ProducerInput>,
    gc::PlayerId, std::span<const ProductionRequest>, bool reachable, const sim::PartitionExecutor&);
// WGA-59: mean build-plus-current-production time; no travel term here.
[[nodiscard]] core::Result<Real> estimate_single_type(std::span<const ProducerInput>,
    gc::PlayerId, gc::TypeId, ProductionChannel, const sim::PartitionExecutor&);

} // namespace eawr::script::foc::ai::galactic_goals
