#pragma once

#include "eawr/battle/arrival_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation {

using RosterCraft = std::vector<std::pair<std::string, std::size_t>>;

// A saved battle keeps the tier a hangar chose when it was first set up, which no snapshot carries. The most any
// authored tier up to the owner's current tech starts a craft with covers every tier that setup could have chosen.
[[nodiscard]] inline RosterCraft largest_roster_up_to(std::span<const RosterCraft> tiers, const std::size_t level) {
    std::map<std::string, std::size_t> most;
    for (std::size_t tier = 0; tier < tiers.size() && tier <= level; ++tier)
        for (const auto& [craft, count] : tiers[tier]) {
            auto& slot = most[craft];
            slot = std::max(slot, count);
        }
    return {most.begin(), most.end()};
}

// Craft already alive at the restore occupy slots whatever their carriers' hangars say: a carrier can be gone while its
// craft fly on, and replacements launch while the old craft still hold slots. For every type the plan or the restored
// carriers name, twice the live count joins the wanted capacity: a killed craft keeps its slot until its wreck retires
// while its replacement already launches.
[[nodiscard]] inline battle::ArrivalPlan with_alive_demand(
    const battle::ArrivalPlan& planned, battle::ArrivalPlan wanted,
    const std::map<std::pair<sim::tactical::PlayerId, std::string>, std::size_t>& alive) {
    using Key = std::pair<sim::tactical::PlayerId, std::string>;
    std::set<Key> named;
    for (const auto& row : planned.rows)
        if (row.source == battle::ArrivalSource::launch) named.emplace(row.owner, row.type);
    for (const auto& row : wanted.rows) named.emplace(row.owner, row.type);
    for (const auto& [key, count] : alive)
        if (named.contains(key)) wanted.add(key.first, key.second, 2 * count, battle::ArrivalSource::launch, 0);
    return wanted;
}

struct RestoredRosterDelta final {
    std::vector<battle::ArrivalDemand> additions; // launch rows for the shortfall only
    std::vector<std::pair<sim::tactical::PlayerId, std::string>> fresh; // keys the plan did not name at all
};

// The restored carriers' launch rows beyond what the plan already holds for each owner and type.
[[nodiscard]] inline RestoredRosterDelta restored_roster_delta(const battle::ArrivalPlan& plan,
                                                               const battle::ArrivalPlan& wanted) {
    using Key = std::pair<sim::tactical::PlayerId, std::string>;
    std::map<Key, std::size_t> planned, needed;
    for (const auto& row : plan.rows)
        if (row.source == battle::ArrivalSource::launch) planned[{row.owner, row.type}] += row.simultaneous_maximum;
    for (const auto& row : wanted.rows) needed[{row.owner, row.type}] += row.simultaneous_maximum;
    RestoredRosterDelta delta;
    for (const auto& [key, maximum] : needed) {
        const auto found = planned.find(key);
        const auto have = found == planned.end() ? std::size_t{} : found->second;
        if (maximum <= have) continue;
        delta.additions.push_back({key.first, key.second, maximum - have, battle::ArrivalSource::launch, 0});
        if (!have) delta.fresh.push_back(key);
    }
    return delta;
}

} // namespace eawr::presentation
