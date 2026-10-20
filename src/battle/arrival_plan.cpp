#include "eawr/battle/arrival_plan.hpp"
#include "eawr/battle/start.hpp"
#include "eawr/sim/tactical/economy.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace eawr::battle {
namespace tt = sim::tactical;

ArrivalPlan plan_start_arrivals(const BattleStart& start, const units::UnitTables& tables,
                              const tt::EconomyRules* economy) {
    ArrivalPlan plan;
    const bool campaign = start.setup.campaign.has_value();
    for (const auto& launch : start.launches) {
        const auto* type = tables.find(launch.squadron);
        if (!type || launch.count <= 0) continue;
        // Preserve the established skirmish slot stream. Campaign demand has no menu cap.
        if (!campaign && (launch.count > 64 || type->members.empty())) continue;
        // Preserve the legacy launch/member order, including repeated craft types.
        for (std::int32_t index = 0; index < launch.count; ++index) {
            if (type->members.empty()) plan.add(launch.owner, type->id, 1, ArrivalSource::launch, launch.spawner);
            for (const auto& member : type->members)
                plan.add(launch.owner, member.craft, 1, ArrivalSource::launch, launch.spawner);
        }
    }
    if (!start.setup.campaign) return plan;
    using Key = std::tuple<tt::PlayerId, std::string, ArrivalSource>;
    std::map<Key, std::size_t> roster;
    const auto want = [&](const tt::PlayerId owner, const tt::TypeId type, const bool expand,
                          const ArrivalSource source, const std::size_t count = 1) {
        const auto* hero = economy ? economy->hero(type) : nullptr;
        const auto deployed = hero ? hero->deployed : type;
        const auto named = std::find_if(tables.units.begin(), tables.units.end(),
            [&](const auto& row) { return type_id(row.id) == deployed; });
        if (named == tables.units.end()) return;
        if (named->members.empty()) roster[{owner, named->id, source}] += count;
        else if (expand) for (const auto& member : named->members) roster[{owner, member.craft, source}] += count;
    };
    for (const auto& station : start.units) {
        const auto* type = tables.find(station.type);
        if (!type || !type->spawner) continue;
        std::map<std::string, std::int32_t> entries;
        const auto note = [&](const auto& list) {
            for (const auto& entry : list) entries[entry.squadron] = std::max(entries[entry.squadron], entry.count);
        };
        note(type->spawner->starting);
        for (const auto& tier : type->spawner->campaign_starting) note(tier);
        for (const auto& [name, count] : entries) if (count > 0)
            want(station.state.owner, type_id(name), true, ArrivalSource::station_spawn, static_cast<std::size_t>(count));
    }
    // Carried squadron containers with explicit members do not count their craft twice.
    const auto carried = [&](auto&& self, const tt::PlayerId owner, const tt::CarriedObject& object,
                             const std::size_t depth) -> void {
        if (depth > 32) return; // Same carried-tree bound as tactical setup/replay validation.
        want(owner, object.type_id, object.members.empty(), ArrivalSource::reinforcement_pool);
        for (const auto& member : object.members) self(self, owner, member, depth + 1);
    };
    for (const auto& reserve : start.setup.campaign->reserves) {
        if (reserve.restricted) continue; // WR-09: these rows cannot reinforce.
        want(reserve.owner, reserve.type, true, ArrivalSource::reinforcement_pool);
        for (const auto& object : reserve.contained) carried(carried, reserve.owner, object, 1);
    }
    for (const auto& [key, maximum] : roster) {
        const auto& [owner, name, source] = key;
        const auto near = std::find_if(start.units.begin(), start.units.end(),
            [&](const auto& unit) { return unit.state.owner == owner; });
        plan.add(owner, name, maximum, source, near == start.units.end() ? 0 : near->state.entity_id);
    }
    return plan;
}
} // namespace eawr::battle
