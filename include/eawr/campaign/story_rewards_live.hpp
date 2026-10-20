#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/setup_plan.hpp"
#include "eawr/data/galactic/galactic_data.hpp"

namespace eawr::campaign {

// #683 part 2: the stock plot rewards that change the galaxy, applied to a fresh or restored owner.
//   SPAWN_HERO, UNIQUE_UNIT   WST-40  units created for the owning player at the resolved planet
//   BUILDABLE_UNIT, LOCK_UNIT WST-40/64  production unlock/lock lists and the faction's slicer post-check
//   SET_TECH_LEVEL, SET_MAX_TECH_LEVEL  WST-41
//   CREDITS                   WST-41
// Every other reward type is declined by name (the doc lists them). Each applied reward is a journaled native
// effect (kinds 12, 19, 20), so a recording replays it without the engine; the replayer is registered here.
[[nodiscard]] core::Result<void> bind_story_rewards(GameSession&, const CampaignSetupPlan&,
    const data::galactic::GalacticData&);

} // namespace eawr::campaign
