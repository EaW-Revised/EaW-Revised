#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/platform/campaign_end_inputs.hpp"
#include "eawr/platform/campaign_scoring.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <functional>
#include <memory>

namespace eawr::platform {

// The provider body for CampaignServiceConfiguration::campaign_end_inputs: reads the local
// player's and the winner's faction names from the session's victory monitor (copied
// before the owner is released, WGP-111) and resolves them through the catalog.
[[nodiscard]] core::Result<CampaignEndInputs> campaign_end_inputs_from_game(
    const data::Catalog& catalog, const campaign::GameSession& game,
    const sim::galactic::CampaignResult& result, sim::galactic::PlayerId local);

// GHUD-S61: the production provider. The authored half (above) plus the mounted scoring
// module's title key and efficiencies, evaluated once from the session's saved ledger before
// the owners are released (WGP-111). An absent or failing module leaves the scoring half
// absent (WGP-97's `0%`) and names the reason in `scoring_seam`; it never fails the boundary.
using CampaignEndInputsProvider = std::function<core::Result<CampaignEndInputs>(
    const campaign::GameSession&, const sim::galactic::CampaignResult&, sim::galactic::PlayerId)>;
[[nodiscard]] CampaignEndInputsProvider campaign_end_inputs_provider(
    std::shared_ptr<const data::Catalog> catalog, std::shared_ptr<const CampaignScoringSource> scoring);

} // namespace eawr::platform
