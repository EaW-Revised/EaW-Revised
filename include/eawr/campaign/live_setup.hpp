#pragma once

#include "eawr/campaign/setup.hpp"

namespace eawr::campaign {
[[nodiscard]] core::Result<void> validate_live_campaign_seats(const data::galactic::GalacticData&,
    std::span<const CampaignPlayerBinding>, std::span<const sim::galactic::Seat>);
// Prepare immutable stock providers. The root owns the placement/control pass;
// callers retain it through live-session startup and ordinary child return.
[[nodiscard]] core::Result<CampaignSetupServices> prepare_live_campaign_services(
    const vfs::Vfs&, const data::Catalog&, const data::galactic::GalacticData&,
    const units::GalacticTypeTable&, const CampaignSetupPlan&, std::string_view difficulty);
} // namespace eawr::campaign
