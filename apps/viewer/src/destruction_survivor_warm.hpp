#pragma once

#include "eawr/battle/arrival_plan.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation {

struct SurvivorWarmPlayer final {
    sim::tactical::PlayerId owner{};
    sim::tactical::FactionId faction{};
};
struct SurvivorWarmChoice final {
    std::vector<sim::tactical::FactionId> affiliations;
    std::vector<std::string> bodies;
};
struct SurvivorWarmType final {
    std::string type;
    std::int32_t minimum{1}, maximum{1};
    std::vector<SurvivorWarmChoice> choices;
    bool ejection{};
};
struct SurvivorWarmSeed final {
    sim::tactical::PlayerId owner{};
    std::string type;
    std::size_t count{};
    sim::EntityId anchor{};
};
struct SurvivorWarmPlan final {
    battle::ArrivalPlan plan;
    std::set<std::pair<sim::tactical::PlayerId, std::string>> admitted;
    bool bounded{};
};

// DS-02/04/05: load-time capacity only. Every possible positive-weight choice
// contributes physical bodies, with the same affiliation fallback as creation.
// The per-owner/type warm prefix bounds cycles and large authored counts;
// admission remains available after that prefix, without limiting simulation.
[[nodiscard]] inline SurvivorWarmPlan plan_survivor_warm(
    const std::span<const SurvivorWarmPlayer> players, const std::span<const SurvivorWarmType> types,
    const std::span<const SurvivorWarmSeed> seeds, const std::size_t warm_limit) {
    using Key = std::pair<sim::tactical::PlayerId, std::string>;
    SurvivorWarmPlan result;
    std::map<std::string, const SurvivorWarmType*> indexed;
    std::map<Key, std::size_t> rows;
    const auto attempts = [](const SurvivorWarmType& type) -> std::uint64_t {
        if (type.ejection) return 0;
        const auto low = static_cast<std::int64_t>(std::min(type.minimum, type.maximum));
        const auto high = static_cast<std::int64_t>(std::max(type.minimum, type.maximum));
        return static_cast<std::uint64_t>(std::max<std::int64_t>(0, std::min(high, low + 32766)));
    };
    const auto owner_of = [&](const sim::tactical::PlayerId previous, const SurvivorWarmChoice& choice) {
        const auto affiliated = [&](const SurvivorWarmPlayer& player) {
            return std::find(choice.affiliations.begin(), choice.affiliations.end(), player.faction) != choice.affiliations.end();
        };
        const auto old = std::find_if(players.begin(), players.end(), [&](const auto& player) { return player.owner == previous; });
        if (old == players.end() || affiliated(*old)) return previous;
        const auto first = std::find_if(players.begin(), players.end(), affiliated);
        return first == players.end() ? previous : first->owner;
    };
    for (const auto& type : types) {
        indexed.emplace(type.type, &type);
        if (attempts(type) == 0) continue;
        for (const auto& player : players) for (const auto& choice : type.choices)
            for (const auto& body : choice.bodies) result.admitted.emplace(owner_of(player.owner, choice), body);
    }
    // Iteration bounds stack use even for a deep authored chain. Only newly
    // reserved capacity is propagated, so each owner/type expands at most limit.
    std::vector<SurvivorWarmSeed> pending(seeds.begin(), seeds.end());
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto seed = pending[cursor];
        const auto found = indexed.find(seed.type);
        if (found == indexed.end() || seed.count == 0) continue;
        const auto count = attempts(*found->second);
        if (count == 0) continue;
        const auto requested = count > warm_limit || seed.count > warm_limit / count
            ? warm_limit : seed.count * static_cast<std::size_t>(count);
        if (count > warm_limit || seed.count > warm_limit / count) result.bounded = true;
        for (const auto& choice : found->second->choices) for (const auto& body : choice.bodies) {
            const Key key{owner_of(seed.owner, choice), body};
            const auto [row, inserted] = rows.emplace(key, result.plan.rows.size());
            if (inserted) result.plan.rows.push_back({key.first, body, 0, battle::ArrivalSource::destruction_survivor, seed.anchor});
            auto& demand = result.plan.rows[row->second];
            const auto additional = std::min(requested, warm_limit - demand.simultaneous_maximum);
            if (additional < requested) result.bounded = true;
            demand.simultaneous_maximum += additional;
            if (additional) pending.push_back({key.first, body, additional, seed.anchor});
        }
    }
    return result;
}

} // namespace eawr::presentation
