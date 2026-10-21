#pragma once

#include "eawr/campaign/setup_plan.hpp"
#include "eawr/campaign/game_session.hpp"
#include <memory>

namespace eawr::campaign {
struct StartingForceInputs;
void bind_story_identities(GameSession&, const CampaignSetupPlan&);

// Authored metadata overlays explicit live producer inputs. Missing XML stays
// with the producer; these adapters do not invent base owners, alliances or stealth actions.
[[nodiscard]] sim::galactic::ObserverPlayer prepare_observer_player(
    const data::galactic::Faction&, sim::galactic::ObserverPlayer live);
[[nodiscard]] sim::galactic::ObserverPlanet prepare_observer_planet(
    const data::galactic::Planet&, sim::galactic::ObserverPlanet live);
struct EconomicTypeBinding { std::string name; sim::galactic::TypeId type{}; };
// WGEP-73: category selection and explicit append are distinct routes.
[[nodiscard]] core::Result<sim::galactic::EconomicType> prepare_economic_type(
    const units::GalacticEconomicType&, sim::galactic::TypeId, std::string_view source_faction,
    const units::GalacticTypeTable&, std::span<const EconomicTypeBinding>);
[[nodiscard]] core::Result<sim::galactic::EconomicPlanet> prepare_economic_planet(
    const units::GalacticEconomicType&, sim::galactic::PlanetId, bool destroyed, bool intimidation);

// WGEP-29/32..34: owning modifier/faction/base/factory providers must supply
// every multiplier. Absent data stays a diagnostic, never an assumed factor.
struct ProductionPreparation {
    std::optional<sim::math::Fixed> price_multiplier, corruption_price_multiplier, tech_purchase_multiplier;
    std::optional<sim::math::Fixed> time_multiplier, base_tech_multiplier, ai_time_multiplier, corruption_time_multiplier;
    std::optional<std::int64_t> factory_count;
    bool tutorial{};
};
[[nodiscard]] core::Result<sim::galactic::ProductionOffer> prepare_production_offer(
    const units::GalacticType&, sim::galactic::ProductionOffer admission, const ProductionPreparation&);
// WGEP-69: effective inherited original-type values precede the shared zero defaults.
[[nodiscard]] core::Result<sim::galactic::CensusType> prepare_census_type(
    const units::GalacticType&, sim::galactic::TypeId, std::span<const FleetTypeBinding> bindings = {});
// WGP-191: Planet data is separate from the ordinary starting-force type table.
// The resolved object reader supplies power; an absent reader stays unknown.
[[nodiscard]] core::Result<sim::galactic::CensusType> prepare_census_planet(
    const data::galactic::Planet&, sim::galactic::TypeId,
    std::optional<sim::math::Fixed> combat_power = {});
[[nodiscard]] sim::galactic::CensusPlayer prepare_census_player(
    const data::galactic::Faction&, sim::galactic::PlayerId, std::vector<sim::galactic::PlanetId> unusable_planets);

struct StartingForcePlacement {
    // A provider can expand a company/squadron into several live objects. Every returned ID
    // must come from the supplied frontier, and every type from the plan's original bindings.
    std::vector<sim::galactic::Fleet> fleets;
    std::vector<sim::galactic::DetachedUnit> detached_units;
    sim::EntityId next_id{};
    bool created{}; // false for a verified placement with no returned live object
    std::vector<sim::galactic::MovementContents> contents;
    std::vector<sim::galactic::SurfaceForce> landed;
    std::vector<sim::galactic::ProductionObject> structures;
    std::vector<sim::galactic::CensusObject> originals;
    std::optional<sim::galactic::ControlBase> starbase;
    sim::galactic::TypeId starbase_type{};
    std::optional<sim::galactic::ControlBase> ground_base;
    std::vector<sim::math::Fixed> special_control;
    sim::EntityId primary{}; // WGM-135: additional results never replace the primary placement.
};
// Existing persistent subsystem models, prepared before the force pass. Production
// and census are installed after player economy; no purchase occurs at startup.
struct CampaignPlacementContext {
    sim::galactic::MovementSetup movement;
    sim::galactic::PoliticalControlSetup control;
    sim::galactic::ProductionSetup production;
    sim::galactic::PopulationCensus census;
    // Planet original identities are separate from the nested placement journal.
    // Root startup supplies their final owner after the complete control pass.
    std::vector<sim::galactic::CensusObject> location_originals;
};
struct StartingAllegiance {
    sim::galactic::PlayerId player{};
    sim::math::Fixed target;
};
struct CampaignCreditCap {
    sim::galactic::PlayerId player{};
    sim::math::Fixed value;
};

// Startup-only subsystem boundary. Providers stage private work: they must not publish a
// scene, mutate an existing session or change global Lua state before creation succeeds.
// An absent provider is an explicit refusal, never a guessed WGS-U08 placement/control rule.
// Placement/control/cap providers run before accounts exist: capture validated
// source/plan constants (for example plan.fiscal()->credit_cap_per_planet),
// rather than reading the live plan's convenience ledger from staged economy.
// Tech side effects, scene production and hero creation use their owning services. Influence,
// plots/AI and object linking remain follow-up work (#1149).
struct CampaignSetupServices {
    std::function<core::Result<sim::galactic::GalacticAbilitySetup>(const CampaignSetupPlan&,
        const sim::galactic::GalacticSetup&)> ability_host;
    std::function<core::Result<sim::galactic::EconomyCorruptionState>(const CampaignSetupPlan&,
        const sim::galactic::GalacticSetup&)> economy_corruption;
    // Complete sourced live facts after placement/control/production. Records must
    // be empty: startup owns fresh records and the WGS-06 local-home reveal.
    // Missing provider remains the named campaign_observer_facts seam.
    std::function<core::Result<sim::galactic::ObserverSetup>(const CampaignSetupPlan&,
        const sim::galactic::GalacticSetup&)> observer;
    std::function<core::Result<sim::galactic::IncomeSetup>(const CampaignSetupPlan&,
        const sim::galactic::GalacticSetup&)> income;
    // Opt-in, after ownership/economy assignment: explicit profiles, census and
    // initial structures. Returned IDs must preserve the placement frontier.
    std::function<core::Result<sim::galactic::ProductionSetup>(const CampaignSetupPlan&,
        const sim::galactic::GalacticSetup&)> production;
    // Original owned-list metadata, faction capacity and AI usability accompany
    // the existing production provider; simulation derives usage/reservations.
    std::function<core::Result<sim::galactic::PopulationCensus>(const CampaignSetupPlan&,
        const sim::galactic::GalacticSetup&)> census;
    std::function<core::Result<void>(const CampaignLocation&)> location;
    std::function<core::Result<void>(const InitialPlacement&)> mark_initial_placement;
    std::function<core::Result<CampaignPlacementContext>(const CampaignSetupPlan&)> placement_context;
    std::shared_ptr<const StartingForceInputs> stock_starting_forces;
    // Complete stock offers after bases, structures, ownership and tech exist.
    // The returned table retains the placement identities and fresh queues.
    std::function<core::Result<sim::galactic::ProductionSetup>(const CampaignSetupPlan&,
        const sim::galactic::GalacticSetup&)> stock_production;
    std::function<core::Result<std::vector<sim::galactic::MovementAccess>>(const CampaignSetupPlan&,
        const sim::galactic::GalacticSetup&)> stock_access;
    std::function<core::Result<StartingForcePlacement>(const StartingForceRequest&,
        sim::EntityId first_free, const sim::galactic::GalacticSetup&)> place;
    std::function<core::Result<std::vector<sim::galactic::Fleet>>(sim::galactic::PlanetId,
        sim::galactic::PlayerId, const sim::galactic::GalacticSetup&)> unify_human_guard_fleets;
    std::function<core::Result<sim::galactic::PlayerId>(const sim::galactic::Planet&,
        std::span<const StartingAllegiance>, const sim::galactic::GalacticSetup&)> political_control;
    // CGS-02 / WGM-C04: complete typed presence is calculated by the stock
    // control provider and retained before credit-cap/home assignment.
    std::function<core::Result<sim::galactic::PoliticalControlSetup>(const CampaignSetupPlan&,
        const sim::galactic::GalacticSetup&)> initial_political_control;
    std::function<core::Result<sim::math::Fixed>(const sim::galactic::GalacticPlayer&,
        const sim::galactic::GalacticSetup&)> credit_cap;
    std::function<core::Result<void>(const CampaignHome&)> home;
    // WGS-13: set current (unlock=true, notify=false), then maximum. The service owns the
    // prior-max clamp and story/production effects and returns the committed current value.
    std::function<core::Result<std::int64_t>(const CampaignTech&,
        const sim::galactic::GalacticSetup&)> assign_tech;
    std::function<core::Result<bool>(sim::galactic::PlanetId)> home_has_production;
    // A complete owned-object query by ORIGINAL identity, including nested/scene objects.
    std::function<core::Result<std::vector<sim::galactic::TypeId>>(sim::galactic::PlayerId,
        const sim::galactic::GalacticSetup&)> owned_original_types;
    std::function<core::Result<StartingForcePlacement>(const CampaignHero&, sim::EntityId,
        const sim::galactic::GalacticSetup&)> place_hero;
};

// Retains the setup context through GameSession's ordinary child entry/return. A new root
// alone plans/reseeds/recreates players. stop_root destroys the owned game and setup context;
// it is not the child-return path (WGS-03/68). UI/audio cleanup remains the mode interface.
class CampaignRoot final {
public:
    [[nodiscard]] static core::Result<CampaignRoot> create(CampaignSetupPlan plan, const CampaignSetupServices& services);
    [[nodiscard]] GameSession* game() noexcept { return game_ ? &*game_ : nullptr; }
    [[nodiscard]] const GameSession* game() const noexcept { return game_ ? &*game_ : nullptr; }
    [[nodiscard]] const CampaignSetupPlan* plan() const noexcept { return plan_ ? &*plan_ : nullptr; }
    [[nodiscard]] std::span<const CampaignCreditCap> credit_caps() const noexcept { return caps_; }
    void stop_root() noexcept;
private:
    CampaignRoot(CampaignSetupPlan plan, GameSession game, std::vector<CampaignCreditCap> caps);
    std::optional<CampaignSetupPlan> plan_;
    std::optional<GameSession> game_;
    std::vector<CampaignCreditCap> caps_;
};

} // namespace eawr::campaign
