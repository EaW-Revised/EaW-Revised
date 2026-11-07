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
// Producer rows grouped once per input by (type, domain) for one player. Row
// positions stay ascending inside a group, so "first minimal wins" is the same
// as a scan over the whole provider order. The index borrows the rows: it must
// not outlive the span it was built from.
class ProducerIndex {
public:
    ProducerIndex(std::span<const ProducerInput> producers, gc::PlayerId player);
    [[nodiscard]] std::span<const ProducerInput> producers() const noexcept { return producers_; }
    [[nodiscard]] std::span<const std::uint32_t> rows(gc::TypeId type, ProductionChannel channel) const noexcept;
    // Dense id of the (producer, domain) queue a usable row belongs to.
    [[nodiscard]] std::uint32_t queue_of(std::size_t row) const noexcept { return queue_of_row_[row]; }
    [[nodiscard]] Real seed(std::uint32_t queue) const noexcept { return seeds_[queue]; }
    [[nodiscard]] std::span<const Real> seeds() const noexcept { return seeds_; }
private:
    std::span<const ProducerInput> producers_;
    std::vector<std::uint32_t> queue_of_row_;
    std::vector<Real> seeds_; // first usable row of each (producer, domain)
    struct Group { gc::TypeId type{}; ProductionChannel channel{}; std::vector<std::uint32_t> rows; };
    std::vector<Group> groups_; // sorted by (type, channel)
};
// WGA-58/60 sourced sentinel. Inputs preserve the provider's canonical order;
// production rows must have unique player/producer/domain/type keys and equal
// current queue times for a given producer/channel. Only admitted rows enter.
[[nodiscard]] inline Real rejected_estimate() { return real(1000000000000000000LL); }
[[nodiscard]] core::Result<ProductionEstimate> estimate_production(std::span<const ProducerInput>,
    gc::PlayerId, std::span<const ProductionRequest>, bool reachable, const sim::PartitionExecutor&);
[[nodiscard]] core::Result<ProductionEstimate> estimate_production(const ProducerIndex&,
    std::span<const ProductionRequest>, bool reachable, const sim::PartitionExecutor&);
// Left fold of estimate_production over a request list. commit() extends the list by one request and keeps the
// queue state, probe() answers "what if this request came next" without changing the fold, so a selection round
// pays one request per candidate instead of replaying the committed prefix. Once a request is infeasible the fold
// stays infeasible, as the one-shot estimate stops at its first failure. Not thread safe (scratch buffer).
class ProductionFold {
public:
    struct Probe { bool feasible{}; sim::EntityId producer{}; }; // producer is zero for structures
    explicit ProductionFold(const ProducerIndex& index);
    [[nodiscard]] core::Result<Probe> probe(const ProductionRequest&, const sim::PartitionExecutor&);
    [[nodiscard]] core::Result<Probe> commit(const ProductionRequest&, const sim::PartitionExecutor&);
    [[nodiscard]] bool feasible() const noexcept { return feasible_; }
    [[nodiscard]] Real seconds() const; // the estimate's seconds: rejected sentinel once infeasible
    [[nodiscard]] const std::vector<sim::EntityId>& producers() const noexcept { return producers_; }
    [[nodiscard]] std::uint64_t visits() const noexcept { return visits_; }
private:
    [[nodiscard]] core::Result<Probe> step(const ProductionRequest&, const sim::PartitionExecutor&, bool apply);
    const ProducerIndex* index_;
    bool feasible_{true};
    Real max_queue_{}, structure_total_{};
    std::vector<Real> queue_seconds_; // current time per dense queue id; starts at the seed
    std::vector<sim::EntityId> producers_;
    std::vector<Real> scores_;
    std::uint64_t visits_{};
};
// WGA-59: mean build-plus-current-production time; no travel term here.
[[nodiscard]] core::Result<Real> estimate_single_type(std::span<const ProducerInput>,
    gc::PlayerId, gc::TypeId, ProductionChannel, const sim::PartitionExecutor&);

} // namespace eawr::script::foc::ai::galactic_goals
