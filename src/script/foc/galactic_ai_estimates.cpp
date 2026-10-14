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

core::Result<ProductionEstimate> estimate_production(std::span<const ProducerInput> producers,
    gc::PlayerId player, std::span<const ProductionRequest> requests, bool reachable,
    const sim::PartitionExecutor& executor) {
    ProductionEstimate out;
    out.feasible = true;
    if (requests.empty()) return core::Result<ProductionEstimate>::success(std::move(out));
    if (!reachable) {
        out.feasible = false;
        out.seconds = rejected_estimate();
        return core::Result<ProductionEstimate>::success(std::move(out));
    }
    // WGA-58: ordered queue identity setup; pure candidate preparation is partitioned.
    std::map<QueueKey, Real> queues;
    for (const auto& row : producers) {
        if (usable(row, player)) queues.try_emplace({row.producer, row.domain}, numeric::from_fixed(row.queue_seconds));
    }
    Real structure_total{};
    for (const auto& request : requests) {
        if (request.structure) {
            if (!request.target_admitted) { out.feasible = false; break; }
            structure_total = to_single(structure_total + request.structure_seconds);
            out.producers.push_back(0);
            continue;
        }
        std::vector<Real> scores(producers.size(), rejected_estimate());
        // Requests depend on earlier queue assignments. Within a request, rows
        // read immutable queues and write disjoint scores, followed by one commit.
        const auto& copied_queues = queues;
        const auto prepared = executor.execute_phase("galactic-ai-producers", sim::tick_partition_count,
            [&](std::size_t partition) {
                const auto range = sim::partition_range(partition, producers.size());
                for (std::size_t index = range.begin; index < range.end; ++index) {
                    const auto& row = producers[index];
                    if (!usable(row, player) || row.type != request.type || row.domain != request.channel) continue;
                    Real score = copied_queues.at({row.producer, row.domain});
                    if (Real{} < row.hyperspace_speed) {
                        const Real speed = to_single(to_single(row.hyperspace_speed * row.faction_speed_factor)
                            * real(row.logical_fps));
                        score = to_single(score + to_single(row.distance / speed));
                    }
                    const Real levels = real(static_cast<std::int64_t>(row.base_level) - row.required_level);
                    scores[index] = to_single(score * to_single(real(1) + to_single(levels * row.technology_weight)));
                }
            });
        if (!prepared) return core::Result<ProductionEstimate>::failure(prepared.error());
        std::size_t chosen = producers.size();
        Real best = rejected_estimate();
        for (std::size_t index = 0; index < scores.size(); ++index) {
            ++out.producer_visits;
            // WGA-58: strict comparison keeps the first producer on ties.
            if (scores[index] < best) { best = scores[index]; chosen = index; }
        }
        if (chosen == producers.size()) { out.feasible = false; break; }
        const auto& row = producers[chosen];
        auto& queue = queues.at({row.producer, row.domain});
        // Build only: selection travel and level preferences never enter the queue.
        queue = to_single(queue + numeric::from_fixed(row.build_seconds));
        out.seconds = std::max(out.seconds, queue);
        out.producers.push_back(row.producer);
    }
    out.seconds = out.feasible ? std::max(out.seconds, structure_total) : rejected_estimate();
    if (!out.feasible) out.producers.clear();
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
