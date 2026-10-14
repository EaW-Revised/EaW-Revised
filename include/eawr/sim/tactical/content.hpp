#pragma once

#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/fog_cells.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/victory.hpp"
#include "eawr/sim/tactical/visibility.hpp"
#include "eawr/sim/tactical/retreat.hpp"

#include <optional>
#include <vector>

namespace eawr::sim::tactical {

// The content tables one tactical session runs with, as one value: what TacticalSession::create
// otherwise takes as separate arguments, with the same meaning and the same empty defaults. None
// of it is replay data or state; the setup's content identity names it. Galactic conquest builds,
// saves and restores a battle's setup and content together.
struct TacticalContent {
    std::vector<SensorProfile> sensors;
    DurabilityTable durability;
    MotionTable motion;
    std::optional<FogRules> fog;
    CombatTable combat;
    VictoryRules victory;
    AbilityTable abilities;
    EconomyRules economy;
    MapBox map_box;
    std::optional<RetreatConfig> retreat;
};

} // namespace eawr::sim::tactical
