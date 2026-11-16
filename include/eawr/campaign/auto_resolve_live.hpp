#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/conflict_policy.hpp"
#include "eawr/campaign/setup_plan.hpp"

namespace eawr::units { struct GalacticType; struct GalacticTypeTable; }
namespace eawr::campaign {
class StockSpaceEntry;
// The session-independent, immutable inputs of the stock auto-resolver (unit-table flags, category enum, terrain, bounty,
// type facts). Opaque: built by prepare_auto_resolver_inputs, shared read-only by every adapter set of one campaign.
class AutoResolverInputs;
class SharedUnitTables;
// WAR-48 / CGS-22: the type an AI learns a combatant under (see the definition).
[[nodiscard]] std::string auto_resolve_learning_type(const units::GalacticTypeTable&, const units::GalacticType&);
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
// The heavy, session-independent half of make_stock_auto_resolver (the full unit-table load of every bound type, the
// effective XML/Lua reads): depends only on the files, catalog, data, type table and plan, so it may run before the
// session exists and be shared by every adapter set built for that plan. Immutable once returned.
[[nodiscard]] core::Result<std::shared_ptr<const AutoResolverInputs>> prepare_auto_resolver_inputs(
    const vfs::Vfs&, const data::Catalog&, const data::galactic::GalacticData&,
    const units::GalacticTypeTable&, const CampaignSetupPlan&,
    std::shared_ptr<const SharedUnitTables> shared = {});
// True when two inputs hold the same derived rows: each type's flags (targeting, unit_ai, selectable), the category enum,
// contrast list, movement classes, immune and unqualified sets, bounty values, terrain table, planet types and terrain and
// the per-id type facts. For tests: a shared unit-table load and an own load must not differ in any of them.
[[nodiscard]] bool auto_resolver_inputs_equivalent(const AutoResolverInputs&, const AutoResolverInputs&);
// Frozen effective XML/Lua inputs; each invocation reads the supplied live snapshot. `prepared` must come from
// prepare_auto_resolver_inputs with the same files, catalog, data, types and plan: inputs built for anything else are
// refused with an error (never rebuilt here); absent, they are built here.
[[nodiscard]] core::Result<LiveConflictAdapters> make_stock_auto_resolver(
    const vfs::Vfs&, const data::Catalog&, const data::galactic::GalacticData&,
    const units::GalacticTypeTable&, const CampaignSetupPlan&,
    std::span<const sim::galactic::CensusObject> originals = {}, AutoResolveTimeProvider time = {},
    std::pair<GameSession::AutoResolveStateWriter, GameSession::AutoResolveStateReader>* checkpoint = nullptr,
    std::shared_ptr<const AutoResolverInputs> prepared = {});
[[nodiscard]] core::Result<AutoResolveRoster> collect_live_auto_resolve_roster(
    const ConflictRecord&, const sim::galactic::GalacticSnapshot&, const CampaignSetupPlan&,
    const units::GalacticTypeTable&, std::span<const sim::galactic::CensusObject> originals = {});
// Tactical map/marker construction remains with the child-content owner.
[[nodiscard]] core::Result<void> install_live_conflict_adapters(GameSession&, const vfs::Vfs&,
    const data::Catalog&, const data::galactic::GalacticData&, const units::GalacticTypeTable&,
    const CampaignSetupPlan&, GameSession::ContentResolver content = {}, AutoResolveTimeProvider time = {},
    std::shared_ptr<const StockSpaceEntry> space = {}, std::shared_ptr<const AutoResolverInputs> prepared = {});
} // namespace eawr::campaign
