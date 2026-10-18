#pragma once

#include "eawr/campaign/conflict_entry.hpp"
#include "eawr/campaign/setup_plan.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/platform/campaign_battle_view.hpp"
#include "eawr/script/foc/tactical_ai.hpp"

#include <memory>

namespace eawr::sim::tactical { class TacticalSession; }

namespace eawr::campaign {

// WGT-07/08: explicit GC-to-tactical binding, never an equality of numeric domains.
struct SpaceEntryType {
    sim::galactic::TypeId galactic{};
    std::string tactical;
    sim::math::Fixed population{};
    bool transport{}, squadron{}, superweapon{}, space_allowed{true};
    bool attach_to_flagship{}, named_hero{}, generic_hero{}, orbital{}, special_weapon{};
    std::string unique_space_unit, additional_structure, every_marker_type, base_position;
    std::optional<sim::math::Fixed> hover_offset;
    sim::tactical::TypeId purchase_type{}; // original tactical CRC when deployed type differs
    std::vector<std::string> escorts;
    // WGT-119: rated in the parent object's source context, before deployment substitution.
    std::optional<sim::math::Fixed> source_flagship_rating;
};
struct SpaceEntrySide {
    sim::galactic::PlayerId player{};
    std::string faction;
    sim::math::Fixed cap{};
    bool human{};
};
struct SpaceEntryRider {
    ConflictForce force;
    std::string tactical;
    sim::EntityId transport{};
    std::vector<sim::tactical::CarriedObject> members;
    std::vector<ConflictForce> member_forces; // campaign identities of the nested tree
};
struct SpaceEntryStructure {
    sim::EntityId id{};
    sim::galactic::TypeId type{};
    sim::galactic::PlayerId owner{};
    std::optional<std::uint32_t> remembered_marker;
};
// Ordered, frozen provider inputs. Planet lists may include noncombatants (WGT-118).
struct SpaceEntryUpgradePlayer {
    sim::galactic::PlayerId player{};
    std::string faction;
    bool playable{true};
    std::vector<sim::galactic::PlayerId> allies;
    std::vector<std::string> galaxy, planet;
    std::vector<std::string> nonfirst_galaxy; // explicit Previous_Upgrade_Level_Type provider fact
};
// WGT-123: frozen directed relationships, independent of retail numeric teams.
struct SpaceEntryRelation {
    sim::galactic::PlayerId player{}, other{};
    bool allied{}, enemy{};
};
// Frozen content is prepared before live ownership. Choice-time work below has no I/O.
// No health or ability state is accepted here (WGT-24).
struct SpaceEntryInputs {
    skirmish::StartInputs map;
    std::vector<SpaceEntryType> types;
    std::vector<SpaceEntrySide> sides;
    std::vector<SpaceEntryRider> riders;
    std::vector<sim::EntityId> roster_order; // live fleet/member order before canonical ID sorting
    std::vector<SpaceEntryStructure> structures;
    std::string station_type;
    sim::galactic::TypeId station_galactic_type{};
    sim::galactic::PlayerId planet_owner{};
    std::vector<SpaceEntryUpgradePlayer> upgrade_players;
    std::optional<sim::math::Vec3> invader_target; // WGT-23/45: presence only admits intro, supplied by scene provider
    std::vector<SpaceEntryRelation> relations{};
};

struct SpaceEntryPlan;
// Retained rows whose type has no upgrade object in `tables` (planetary corruption markers, a mod's galactic-only
// upgrade). Entry skips them without refusing; the return keeps them untouched, like another layer's rows (WGT-14).
[[nodiscard]] std::vector<ReturnedUpgrade> space_entry_unbound_upgrades(const ConflictRecord&, const units::UnitTables& tables);
// WGT-113/116..118: resolve the three families against the pre-force scene only.
[[nodiscard]] core::Result<std::vector<sim::tactical::CampaignUpgrade>> space_entry_upgrades(
    const ConflictRecord&, const SpaceEntryInputs&, const SpaceEntryPlan&, skirmish::SkirmishStart&);
struct SpaceEntryPlan {
    ConflictRecord conflict;
    skirmish::Fixture fixture;
    skirmish::StartInputs map;
    std::vector<ForcePlacement> placements;
    std::vector<SpaceEntryRider> riders;
    std::vector<std::pair<sim::EntityId, std::uint32_t>> structure_markers;
    std::vector<std::pair<sim::EntityId, skirmish::MapPlacement>> fixed_poses;
    sim::EntityId first_free{};
    std::uint64_t marker_draws{};
    std::vector<sim::EntityId> reserve_order{}; // WGT-07: transport source order, then bucket leftovers forward
};

struct SpaceFormationResult { bool attacker_cinematic_admitted{}; };
// WGT-45..48: load-time registration/formation. Optional target coordinates do not affect poses.
[[nodiscard]] core::Result<SpaceFormationResult> form_space_entry(skirmish::SkirmishStart&,
    const SpaceEntryInputs&, const SpaceEntryPlan&, std::optional<sim::math::Vec3> invader_target);

// Pure split and marker/defence preparation; allocations are committed only by staging
// the returned conflict. A failure consumes no parent allocator or random stream.
[[nodiscard]] core::Result<SpaceEntryPlan> prepare_space_entry(const ConflictRecord&,
    const SpaceEntryInputs&, sim::EntityId first_free, std::uint64_t session_seed);
[[nodiscard]] core::Result<ConflictEntry> build_space_entry(const ConflictRecord&,
    const SpaceEntryInputs&, sim::EntityId first_free, std::uint64_t session_seed);
[[nodiscard]] core::Result<sim::tactical::TacticalContent> space_entry_content(
    const sim::tactical::TacticalSetup&, const skirmish::StartInputs&);

// Display metadata projected from the immutable record for a fresh restored provider.
// Does not rerun placement, supply gameplay policies, or invent unsaved marker IDs.
[[nodiscard]] core::Result<skirmish::SkirmishStart> space_entry_presentation_start(
    const ConflictRecord&, const skirmish::StartInputs&);

class StockSpaceEntry final {
public:
    [[nodiscard]] core::Result<ResolvedConflictEntry> resolve(const ConflictRecord&,
        const sim::galactic::GalacticSnapshot&, sim::EntityId first_free) const;
    [[nodiscard]] core::Result<sim::tactical::TacticalContent> content(const ConflictRecord&) const;
    [[nodiscard]] core::Result<void> decorate_result(const ConflictRecord&,
        const sim::tactical::TacticalSession&, BattleResult&) const;
    [[nodiscard]] core::Result<script::foc::PreparedAiSession> prepare_ai(const ConflictRecord&, GalacticParentQuery = {},
        std::shared_ptr<script::foc::AiJournal> journal = {}) const;
    [[nodiscard]] GameSession::HeroDefeatResolver hero_defeat_resolver() const;
    [[nodiscard]] core::Result<void> bind_game(GameSession&) const;
    // Frozen map/cap/type closure diagnostics. Snapshot-dependent placement and
    // current station/passenger checks remain authoritative in resolve().
    [[nodiscard]] core::Result<void> availability(const ConflictRecord&) const;
    [[nodiscard]] bool available(const ConflictRecord&) const noexcept;
    [[nodiscard]] core::Result<std::shared_ptr<const platform::PreparedCampaignBattle>> presentation(
        const ConflictRecord&, const sim::tactical::TacticalContent& resolved_content) const;
private:
    struct Impl;
    explicit StockSpaceEntry(std::shared_ptr<const Impl>);
    std::shared_ptr<const Impl> impl_;
    friend core::Result<std::shared_ptr<const StockSpaceEntry>> prepare_stock_space_entry(
        const vfs::Vfs&, const data::Catalog&, const data::galactic::GalacticData&,
        const units::GalacticTypeTable&, const CampaignSetupPlan&);
};
[[nodiscard]] core::Result<std::shared_ptr<const StockSpaceEntry>> prepare_stock_space_entry(
    const vfs::Vfs&, const data::Catalog&, const data::galactic::GalacticData&,
    const units::GalacticTypeTable&, const CampaignSetupPlan&);
struct SpaceEntryScriptServices {
    GameSession::ScriptFactory scripts;
    GameSession::ChildConfigurator bind;
};
// Keep this pair together for a live child and its restored scripts: both closures
// refer to the same prepared host. Recreating the pair never re-runs placement.
[[nodiscard]] SpaceEntryScriptServices space_entry_script_services(std::shared_ptr<const StockSpaceEntry>, GalacticParentQueryFactory = {},
    std::shared_ptr<script::foc::AiJournal> journal = {});

} // namespace eawr::campaign
