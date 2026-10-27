#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/conflict_policy.hpp"
#include "eawr/campaign/setup_plan.hpp"

namespace eawr::campaign {
class StockSpaceEntry;
[[nodiscard]] std::array<AutoResolveProgress::SideRoster, 2> live_auto_resolve_progress_roster(
    const AutoResolveStrength&, const AutoResolveRoster&, const AutoResolveRoundState&);
struct LiveConflictAdapters {
    GameSession::AutoResolver resolve;
    GameSession::ConflictService admission;
    GameSession::NativeConflictService native_admission;
    AutoResolveOutcomeServices outcomes;
    GameSession::AutoResolveProgressReader progress;
    GameSession::AutoResolveWithdrawal withdraw;
    sim::galactic::GalacticSession::MovementAccessProvider access;
    // WAR-127: the bounty value of a bound type (authored override, else the largest category default).
    std::function<core::Result<sim::math::Fixed>(std::string_view)> bounty_value;
};
// WAR-13/14: canonical manifest of the selected layer and raid participants.
[[nodiscard]] std::vector<ConflictForce> live_conflict_force_manifest(const ConflictRecord&, const LayerPresence&);
// WAR-86/WAR-U04: true when an authored `Abilities` subtree holds a damage-triggered (`Take_Damage`)
// handler of a kind not sourced as inert for an auto-resolve request. The blaster redirect and absorb kinds
// (and anything under them) accept only a projectile damage source (WAR-126) and do not count.
[[nodiscard]] bool has_unqualified_damage_handler(const data::XmlNode& abilities);
using AutoResolveTimeProvider = std::function<core::Result<sim::math::Fixed>(const sim::galactic::GalacticSnapshot&)>;
// Frozen effective XML/Lua inputs; each invocation reads the supplied live snapshot.
[[nodiscard]] core::Result<LiveConflictAdapters> make_stock_auto_resolver(
    const vfs::Vfs&, const data::Catalog&, const data::galactic::GalacticData&,
    const units::GalacticTypeTable&, const CampaignSetupPlan&,
    std::span<const sim::galactic::CensusObject> originals = {}, AutoResolveTimeProvider time = {},
    std::pair<GameSession::AutoResolveStateWriter, GameSession::AutoResolveStateReader>* checkpoint = nullptr);
[[nodiscard]] core::Result<AutoResolveRoster> collect_live_auto_resolve_roster(
    const ConflictRecord&, const sim::galactic::GalacticSnapshot&, const CampaignSetupPlan&,
    const units::GalacticTypeTable&, std::span<const sim::galactic::CensusObject> originals = {});
// Tactical map/marker construction remains with the child-content owner.
[[nodiscard]] core::Result<void> install_live_conflict_adapters(GameSession&, const vfs::Vfs&,
    const data::Catalog&, const data::galactic::GalacticData&, const units::GalacticTypeTable&,
    const CampaignSetupPlan&, GameSession::ContentResolver content = {}, AutoResolveTimeProvider time = {},
    std::shared_ptr<const StockSpaceEntry> space = {});
} // namespace eawr::campaign
