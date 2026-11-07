#include "galactic_ai_estimates.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"

#include <algorithm>
#include <map>

namespace eawr::script::foc::ai::galactic_goals {
namespace {
using QueueKey = std::pair<sim::EntityId, ProductionChannel>;
bool usable(const ProducerInput& row, gc::PlayerId player) {
    return row.player == player && row.reachable && row.producer != 0
        && row.admission == gc::ProductionAdmission::allowed;
}
}

ProducerIndex::ProducerIndex(std::span<const ProducerInput> producers, gc::PlayerId player)
    : producers_(producers), queue_of_row_(producers.size()) {
    std::map<QueueKey, std::uint32_t> queues;
    std::map<std::pair<gc::TypeId, ProductionChannel>, std::size_t> group_of;
    std::vector<std::vector<std::uint32_t>> members;
    for (std::size_t index = 0; index < producers.size(); ++index) {
        const auto& row = producers[index];
        if (!usable(row, player)) continue;
        const auto queue = queues.try_emplace({row.producer, row.domain}, static_cast<std::uint32_t>(seeds_.size()));
        if (queue.second) seeds_.push_back(numeric::from_fixed(row.queue_seconds));
        queue_of_row_[index] = queue.first->second;
        const auto group = group_of.try_emplace({row.type, row.domain}, members.size());
        if (group.second) members.emplace_back();
        members[group.first->second].push_back(static_cast<std::uint32_t>(index));
    }
    for (const auto& [key, at] : group_of) groups_.push_back({key.first, key.second, std::move(members[at])});
}

std::span<const std::uint32_t> ProducerIndex::rows(gc::TypeId type, ProductionChannel channel) const noexcept {
    const auto found = std::lower_bound(groups_.begin(), groups_.end(), std::pair{type, channel},
        [](const Group& group, const std::pair<gc::TypeId, ProductionChannel>& key) {
            return std::pair{group.type, group.channel} < key;
        });
    if (found == groups_.end() || found->type != type || found->channel != channel) return {};
    return found->rows;
}

core::Result<ProductionEstimate> estimate_production(std::span<const ProducerInput> producers,
    gc::PlayerId player, std::span<const ProductionRequest> requests, bool reachable,
    const sim::PartitionExecutor& executor) {
    if (requests.empty() || !reachable) return estimate_production(ProducerIndex({}, player), requests, reachable, executor);
    return estimate_production(ProducerIndex(producers, player), requests, reachable, executor);
}

ProductionFold::ProductionFold(const ProducerIndex& index)
    : index_(&index), queue_seconds_(index.seeds().begin(), index.seeds().end()) {}

Real ProductionFold::seconds() const {
    return feasible_ ? std::max(max_queue_, structure_total_) : rejected_estimate();
}

core::Result<ProductionFold::Probe> ProductionFold::step(const ProductionRequest& request,
    const sim::PartitionExecutor& executor, bool apply) {
    using Out = core::Result<Probe>;
    if (!feasible_) return Out::success(Probe{});
    if (request.structure) {
        if (!request.target_admitted) { if (apply) feasible_ = false; return Out::success(Probe{}); }
        if (apply) { structure_total_ = to_single(structure_total_ + request.structure_seconds); producers_.push_back(0); }
        return Out::success(Probe{true, 0});
    }
    const auto producers = index_->producers();
    const auto rows = index_->rows(request.type, request.channel);
    scores_.assign(rows.size(), rejected_estimate());
    // Requests depend on earlier queue assignments. Within a request, rows
    // read immutable queues and write disjoint scores, followed by one commit.
    const auto score_range = [&](std::size_t begin, std::size_t end) {
        for (std::size_t at = begin; at < end; ++at) {
            const auto& row = producers[rows[at]];
            Real score = queue_seconds_[index_->queue_of(rows[at])];
            if (Real{} < row.hyperspace_speed) {
                const Real speed = to_single(to_single(row.hyperspace_speed * row.faction_speed_factor)
                    * real(row.logical_fps));
                score = to_single(score + to_single(row.distance / speed));
            }
            const Real levels = real(static_cast<std::int64_t>(row.base_level) - row.required_level);
            scores_[at] = to_single(score * to_single(real(1) + to_single(levels * row.technology_weight)));
        }
    };
    // WGA-58: pure candidate preparation is partitioned once a request matches enough rows to pay for the phase.
    // Below the threshold no phase is dispatched, so an executor failure cannot surface for small requests
    // (contract change from the first PR revision, which always dispatched; scores and picks are identical).
    constexpr std::size_t partition_threshold = 2048;
    if (rows.size() < partition_threshold) score_range(0, rows.size());
    else {
        const auto prepared = executor.execute_phase("galactic-ai-producers", sim::tick_partition_count,
            [&](std::size_t partition) {
                const auto range = sim::partition_range(partition, rows.size());
                score_range(range.begin, range.end);
            });
        if (!prepared) return Out::failure(prepared.error());
    }
    // The visit count keeps the full-provider-scan figure, so work accounting is unchanged.
    if (apply) visits_ += producers.size();
    std::size_t chosen = producers.size();
    Real best = rejected_estimate();
    for (std::size_t at = 0; at < scores_.size(); ++at) {
        // WGA-58: strict comparison keeps the first producer on ties.
        if (scores_[at] < best) { best = scores_[at]; chosen = rows[at]; }
    }
    if (chosen == producers.size()) { if (apply) feasible_ = false; return Out::success(Probe{}); }
    const auto& row = producers[chosen];
    if (apply) {
        const auto queue = index_->queue_of(chosen);
        // Build only: selection travel and level preferences never enter the queue.
        auto& queue_time = queue_seconds_[queue];
        queue_time = to_single(queue_time + numeric::from_fixed(row.build_seconds));
        max_queue_ = std::max(max_queue_, queue_time);
        producers_.push_back(row.producer);
    }
    return Out::success(Probe{true, row.producer});
}

core::Result<ProductionFold::Probe> ProductionFold::probe(const ProductionRequest& request,
    const sim::PartitionExecutor& executor) {
    return step(request, executor, false);
}

core::Result<ProductionFold::Probe> ProductionFold::commit(const ProductionRequest& request,
    const sim::PartitionExecutor& executor) {
    return step(request, executor, true);
}

core::Result<ProductionEstimate> estimate_production(const ProducerIndex& index,
    std::span<const ProductionRequest> requests, bool reachable, const sim::PartitionExecutor& executor) {
    ProductionEstimate out;
    out.feasible = true;
    if (requests.empty()) return core::Result<ProductionEstimate>::success(std::move(out));
    if (!reachable) {
        out.feasible = false;
        out.seconds = rejected_estimate();
        return core::Result<ProductionEstimate>::success(std::move(out));
    }
    ProductionFold fold(index);
    for (const auto& request : requests) {
        const auto stepped = fold.commit(request, executor);
        if (!stepped) return core::Result<ProductionEstimate>::failure(stepped.error());
        if (!fold.feasible()) break;
    }
    out.feasible = fold.feasible();
    out.seconds = fold.seconds();
    if (out.feasible) out.producers = fold.producers();
    out.producer_visits = fold.visits();
    return core::Result<ProductionEstimate>::success(std::move(out));
}

core::Result<Real> estimate_single_type(std::span<const ProducerInput> producers, gc::PlayerId player,
    gc::TypeId type, ProductionChannel channel, const sim::PartitionExecutor& executor) {
    struct Row { Real seconds{}; bool eligible{}; };
    std::vector<Row> rows(producers.size());
    const auto prepared = executor.execute_phase("galactic-ai-producer-mean", sim::tick_partition_count,
        [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, producers.size());
            for (std::size_t index = range.begin; index < range.end; ++index) {
                const auto& producer = producers[index];
                if (!usable(producer, player) || producer.type != type || producer.domain != channel) continue;
                rows[index] = {to_single(numeric::from_fixed(producer.build_seconds)
                    + numeric::from_fixed(producer.queue_seconds)), true};
            }
        });
    if (!prepared) return core::Result<Real>::failure(prepared.error());
    Real total{};
    std::int64_t count{};
    for (const auto& row : rows) {
        if (!row.eligible) continue;
        total = to_single(total + row.seconds);
        ++count;
    }
    return core::Result<Real>::success(count == 0 ? rejected_estimate() : to_single(total / real(count)));
}

} // namespace eawr::script::foc::ai::galactic_goals
