#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/auto_resolve_live.hpp"

namespace eawr::campaign {
core::Result<void> configure_live_campaign_conflicts(CampaignRoot& root,
    const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const data::galactic::GalacticData& data, const units::GalacticTypeTable& types,
    std::shared_ptr<const StockSpaceEntry> space) {
    if (!root.game() || !root.plan()) {
        core::Diagnostic error;
        error.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
        error.message = "runtime conflict services require an initialized campaign root";
        return core::Result<void>::failure(std::move(error));
    }
    return configure_live_campaign_conflicts(*root.game(), filesystem, catalog, data, types, *root.plan(), std::move(space));
}
core::Result<void> configure_live_campaign_conflicts(GameSession& game,
    const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const data::galactic::GalacticData& data, const units::GalacticTypeTable& types,
    const CampaignSetupPlan& plan, std::shared_ptr<const StockSpaceEntry> space) {
    return install_live_conflict_adapters(game, filesystem, catalog, data, types, plan, {}, {}, std::move(space));
}
} // namespace eawr::campaign
