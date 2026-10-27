#pragma once

#include "eawr/battle/arrival_plan.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation {

// Selected owner-tech hangars, already expanded from squadron containers into
// their authored craft. Values only: planning never creates simulation units.
struct CampaignWarmType final {
    sim::tactical::PlayerId owner{};
    std::string type;
    std::vector<std::pair<std::string, std::size_t>> hangar;
};
struct CampaignWarmSeed final {
    sim::tactical::PlayerId owner{};
    std::string type;
    std::size_t count{};
    sim::EntityId anchor{};
};
struct CampaignWarmArrivals final {
    battle::ArrivalPlan plan;
    bool complete{true};
};

// Each carrier's live complement is starting, not starting+reserve: reserves
// replace lost craft and reuse their slots. Distinct carriers add their demand.
// A cyclic authored hangar has no finite transitive maximum; retain the bounded
// prefix, label it partial and leave the existing fallback available.
[[nodiscard]] inline CampaignWarmArrivals campaign_warm_arrivals(
    std::span<const CampaignWarmSeed> seeds, std::span<const CampaignWarmType> types) {
    using Key = std::pair<sim::tactical::PlayerId, std::string>;
    std::map<Key, const CampaignWarmType*> indexed;
    CampaignWarmArrivals result;
    for (const auto& type : types) {
        if (!indexed.emplace(Key{type.owner, type.type}, &type).second) result.complete = false;
    }
    std::set<Key> visiting;
    const auto visit = [&](auto&& self, const CampaignWarmSeed& seed) -> void {
        if (seed.count == 0) return;
        const Key key{seed.owner, seed.type};
        const auto found = indexed.find(key);
        if (found == indexed.end()) return;
        if (!visiting.insert(key).second) { result.complete = false; return; }
        for (const auto& [craft, count] : found->second->hangar) {
            if (count == 0) continue;
            if (seed.count > std::numeric_limits<std::size_t>::max() / count) {
                result.complete = false;
                continue;
            }
            const auto maximum = seed.count * count;
            result.plan.add(seed.owner, craft, maximum, battle::ArrivalSource::launch, seed.anchor);
            self(self, CampaignWarmSeed{seed.owner, craft, maximum, seed.anchor});
        }
        visiting.erase(key);
    };
    for (const auto& seed : seeds) visit(visit, seed);
    return result;
}

} // namespace eawr::presentation
