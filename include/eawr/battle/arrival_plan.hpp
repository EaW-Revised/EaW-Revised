#pragma once

#include "eawr/sim/commands.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace eawr::units { struct UnitTables; }
namespace eawr::sim::tactical { struct EconomyRules; }

namespace eawr::battle {

enum class ArrivalSource : std::uint8_t {
    launch, purchase, reinforcement_pool, station_spawn, ability_object
};

// Ordered contributions to a type's simultaneous demand. Separate sources add;
// this is capacity metadata, never a simulation spawn instruction or replay data.
struct ArrivalDemand final {
    sim::tactical::PlayerId owner{};
    std::string type;
    std::size_t simultaneous_maximum{};
    ArrivalSource source{};
    sim::EntityId anchor{};
};

struct ArrivalPlan final {
    std::vector<ArrivalDemand> rows;

    void add(sim::tactical::PlayerId owner, std::string type, std::size_t maximum,
             ArrivalSource source, sim::EntityId anchor = 0) {
        if (maximum) rows.push_back({owner, std::move(type), maximum, source, anchor});
    }
};

struct BattleStart;
[[nodiscard]] ArrivalPlan plan_start_arrivals(const BattleStart&, const units::UnitTables&,
                                             const sim::tactical::EconomyRules* economy = nullptr);

} // namespace eawr::battle
