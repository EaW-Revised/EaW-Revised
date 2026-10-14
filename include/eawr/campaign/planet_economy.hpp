#pragma once
#include "eawr/campaign/setup.hpp"

namespace eawr::campaign {
[[nodiscard]] core::Result<void> prepare_corruption_host(const data::Catalog&, const CampaignSetupPlan&,
    const sim::galactic::GalacticSetup&, sim::galactic::GalacticAbilitySetup&,
    const std::function<sim::galactic::TypeId(std::string_view)>& identity);
// Fresh inputs only. The corruption/hero owning service supplies current type,
// siphon, piracy and buyer presence after startup; this loader does not poll it.
[[nodiscard]] core::Result<sim::galactic::EconomyCorruptionState> prepare_corruption_economy(
    const vfs::Vfs&, const data::Catalog&, const CampaignSetupPlan&, const sim::galactic::GalacticSetup&);
[[nodiscard]] core::Result<sim::galactic::EconomyCorruptionState> prepare_corruption_economy(
    const data::XmlDocument& constants, const data::XmlDocument& market,
    const data::Catalog&, const CampaignSetupPlan&, const sim::galactic::GalacticSetup&);
[[nodiscard]] core::Result<sim::galactic::BlackMarketBuyer> prepare_black_market_buyer(
    const data::Catalog&, std::string_view original_type, sim::EntityId, sim::galactic::PlayerId,
    bool opposing_planet);
} // namespace eawr::campaign
