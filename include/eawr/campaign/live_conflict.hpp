#pragma once

#include "eawr/campaign/setup.hpp"

namespace eawr::campaign {
class StockSpaceEntry;
// Install runtime campaign services after startup has assigned every persistent
// identity and before ownership transfers to the live simulation thread.
[[nodiscard]] core::Result<void> configure_live_campaign_conflicts(CampaignRoot&,
    const vfs::Vfs&, const data::Catalog&, const data::galactic::GalacticData&,
    const units::GalacticTypeTable&, std::shared_ptr<const StockSpaceEntry> space = {});
[[nodiscard]] core::Result<void> configure_live_campaign_conflicts(GameSession&,
    const vfs::Vfs&, const data::Catalog&, const data::galactic::GalacticData&,
    const units::GalacticTypeTable&, const CampaignSetupPlan&,
    std::shared_ptr<const StockSpaceEntry> space = {});
} // namespace eawr::campaign
