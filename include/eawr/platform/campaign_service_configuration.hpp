#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/campaign_loading_view.hpp"
#include "eawr/platform/campaign_tactical_view.hpp"
#include "eawr/platform/campaign_end_view.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <functional>

namespace eawr::platform {
struct PreparedCampaignBattle;

// Engine-owned configuration, deliberately separate from the presentation-safe
// live facade. Runs before its simulation thread; captured services outlive stop().
struct CampaignServiceConfiguration {
    // WGP-97/105/111: named owning scoring and authored faction/movie provider.
    // Called once per admitted local player, before shutdown. No tactical scores
    // or guessed faction movie names substitute for an absent provider.
    std::function<core::Result<CampaignEndInputs>(const campaign::GameSession&,
        const sim::galactic::CampaignResult&, sim::galactic::PlayerId)> campaign_end_inputs;
    CampaignEndingMode ending_mode{CampaignEndingMode::solo};
    std::function<core::Result<void>(campaign::GameSession&)> configure;
    // Installs a fresh native AI owner before the simulation thread starts.
    // prepare_load uses it at the restored boundary when restore has no override.
    std::function<core::Result<void>(campaign::GameSession&)> configure_native_ai;
    checkpoint::GameLoadOptions restore;
    std::function<core::Result<std::shared_ptr<const PreparedCampaignBattle>>(const campaign::ConflictRecord&)> presentation;
    std::function<core::Result<void>(const campaign::ConflictRecord&)> tactical_capability;
    std::function<std::optional<CampaignRetreatPolicy>(const campaign::ConflictRecord&)> retreat_policy;
    // WGP-161: copied before presentation preparation. The owning provider may
    // supply current-model and retained mission/map facts absent from the record.
    std::function<CampaignLoadingDescriptor(const campaign::ConflictRecord&,
        const sim::galactic::GalacticSnapshot&)> loading_descriptor;
    // Optional native progress producer. Legacy presentation emits no guessed
    // milestones; its verified result still supplies the preparation outcome.
    // The reporter is synchronous and must not be retained after this call.
    std::function<core::Result<std::shared_ptr<const PreparedCampaignBattle>>(
        const campaign::ConflictRecord&, const CampaignLoadingReporter&)> loading_presentation;
};

} // namespace eawr::platform
