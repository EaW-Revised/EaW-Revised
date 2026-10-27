#pragma once

#include "eawr/battle/arrival_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation {

// Conservative cached estimates charge each additional hull/death-clone pair.
// Shared resident textures are already excluded by the renderer estimate.
struct ReserveWarmCost final {
    sim::tactical::PlayerId owner{};
    std::string type;
    std::size_t existing{};
    std::optional<std::uint64_t> instance_bytes;
};

struct ReserveWarmBudget final {
    bool constrained{};
    std::uint64_t available_bytes{};
};

struct ReserveWarmRow final {
    sim::tactical::PlayerId owner{};
    std::string type;
    sim::EntityId anchor{};
    std::size_t requested{}, admitted{};
    bool estimated{};
};

struct ReserveWarmPlan final {
    std::vector<ReserveWarmRow> rows;
    std::uint64_t additional_bytes{};
    bool valid{true};

    [[nodiscard]] bool complete() const noexcept {
        return valid && std::all_of(rows.begin(), rows.end(), [](const auto& row) {
            return row.admitted == row.requested;
        });
    }
};

// Pure load-time admission. Unknown estimates retain existing capacity, and a
// partial plan leaves the established runtime fallback available. Ordering is
// the first authored contribution to each owner/type; no simulation state moves.
[[nodiscard]] inline ReserveWarmPlan plan_reserve_warm(
    const std::span<const battle::ArrivalDemand> demands,
    const std::span<const ReserveWarmCost> costs,
    const ReserveWarmBudget budget, const bool include_campaign_sources = false) {
    static_assert(std::numeric_limits<std::size_t>::digits <= std::numeric_limits<std::uint64_t>::digits);
    using Key = std::pair<sim::tactical::PlayerId, std::string>;
    ReserveWarmPlan result;
    std::map<Key, std::size_t> indices;
    for (const auto& demand : demands) {
        // GC shares the owner/type pool between station, hangar, purchase and
        // reserve sources, so their capacities add before existing slots deduct.
        if ((!include_campaign_sources && demand.source != battle::ArrivalSource::reinforcement_pool)
            || demand.simultaneous_maximum == 0) continue;
        const auto [found, inserted] = indices.emplace(Key{demand.owner, demand.type}, result.rows.size());
        if (inserted) result.rows.push_back({demand.owner, demand.type, demand.anchor});
        auto& row = result.rows[found->second];
        if (demand.simultaneous_maximum > std::numeric_limits<std::size_t>::max() - row.requested) {
            result.valid = false;
            return result;
        }
        row.requested += demand.simultaneous_maximum;
    }

    std::map<Key, const ReserveWarmCost*> estimates;
    for (const auto& cost : costs) if (!estimates.emplace(Key{cost.owner, cost.type}, &cost).second) {
        result.valid = false;
        return result;
    }
    auto available = budget.constrained ? budget.available_bytes : std::numeric_limits<std::uint64_t>::max();
    for (auto& row : result.rows) {
        const auto found = estimates.find(Key{row.owner, row.type});
        if (found == estimates.end()) continue;
        const auto& cost = *found->second;
        row.admitted = std::min(row.requested, cost.existing);
        if (!cost.instance_bytes) continue;
        row.estimated = true;
        const auto remaining = row.requested - row.admitted;
        if (remaining == 0) continue;
        const auto count = *cost.instance_bytes == 0 ? remaining
            : static_cast<std::size_t>(std::min<std::uint64_t>(remaining,
                available / *cost.instance_bytes));
        if (count == 0) continue;
        const auto bytes = count * *cost.instance_bytes;
        row.admitted += count;
        available -= bytes;
        result.additional_bytes += bytes;
    }
    return result;
}

} // namespace eawr::presentation
