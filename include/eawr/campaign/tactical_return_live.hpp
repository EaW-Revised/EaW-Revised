#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/setup_plan.hpp"

namespace eawr::sim::galactic { class GalacticSnapshot; }
namespace eawr::campaign {
struct StartingForceInputs;
// WGS-80: the original-type inputs of nested creation, rebuilt from the live production authoring of a fresh or
// restored owner. Shared by the due heroes and the plot rewards that create units (#683).
[[nodiscard]] core::Result<std::shared_ptr<StartingForceInputs>> rebuild_starting_force_inputs(
    const sim::galactic::GalacticSnapshot&, const CampaignSetupPlan&);
// WGM-109/110/132..135: stock due placement and live census, rebound on every fresh/restore owner.
[[nodiscard]] core::Result<void> bind_stock_hero_returns(GameSession&, std::shared_ptr<const StartingForceInputs>);
// WR-54: an optional owner can exclude locomotors that it suspends; stopped is eligible.
using RetreatLocomotorQualifier = std::function<core::Result<bool>(sim::tactical::TypeId)>;
struct StockTacticalReturnServices {
    std::function<core::Result<void>(const ConflictRecord&, sim::tactical::TacticalContent&)> bind;
    GameSession::HeroDefeatResolver hero;
    std::function<core::Result<void>(GameSession&)> bind_game;
};
[[nodiscard]] core::Result<StockTacticalReturnServices> prepare_stock_tactical_return(
    const vfs::Vfs&, const data::Catalog&, const CampaignSetupPlan&, RetreatLocomotorQualifier = {});
} // namespace eawr::campaign
