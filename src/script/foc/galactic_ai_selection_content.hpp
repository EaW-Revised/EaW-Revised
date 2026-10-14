#pragma once

#include "galactic_ai_plans.hpp"
#include "galactic_ai_world.hpp"
#include "eawr/units/galactic_types.hpp"

namespace eawr::script::foc::ai {
// Immutable original-content values. Campaign owns XML/units conversion; the
// simulation-side selector holds no scene or unit-table dependency.
struct GalacticSelectionContentType {
    GalacticSelectionType value;
    std::shared_ptr<const units::GalacticType> original;
    std::string name, movement_class, lua_script, survival_type;
    Real hyperspace_speed{};
    std::uint64_t garrison_categories{};
    bool named_hero{}, special_weapon{}, company{}, team{}, transport{}, craft{}, ground_base{}, starbase{};
    bool hyperspace_capable{}, initially_locked{};
    std::vector<std::string> required_planets;
    std::vector<sim::galactic::TypeId> required_orbiting;
    std::vector<std::string> space_escorts; // WGA-443: Space_Escort_Unit_Types, authored order
};
struct GalacticSelectionContentPlanet {
    std::string terrain;
    std::uint64_t restricted_categories{};
    std::vector<sim::galactic::TypeId> restricted_types;
    std::vector<GalacticIndigenousPotential> indigenous_potential;
};
struct GalacticSelectionContent {
    AiContent ai;
    std::map<sim::galactic::TypeId, GalacticSelectionContentType> types;
    std::vector<sim::galactic::TypeId> registered_types; // WGEP-149: catalog winner order
    std::map<sim::galactic::PlanetId, GalacticSelectionContentPlanet> planets;
    std::map<tactical::PlayerId, Real> faction_speed;
    std::map<std::string, std::map<std::string, Real, std::less<>>, std::less<>> terrain;
    std::optional<Real> technology_weight;
    std::optional<bool> population_check;
};
struct GalacticHeroSelection {
    Real target_weight{}, escort_weight{}, minimum_cost{}, maximum_cost{};
    bool containment_allowed{};
};
using GalacticHeroSelectionProvider = std::function<core::Result<GalacticHeroSelection>(
    tactical::PlayerId, sim::galactic::PlanetId, sim::galactic::TypeId, const PlanDef&)>;
using GalacticFreestoreSelectionProvider = std::function<core::Result<std::vector<GalacticSelectionObject>>(
    tactical::PlayerId, sim::galactic::PlanetId, const PlanDef&)>;
[[nodiscard]] core::Result<std::vector<sim::galactic::PlanetId>> galactic_force_stages(
    const PlanDef&, std::span<const galactic_goals::GalacticForceConstraints>, sim::galactic::PlanetId,
    std::string_view reachability, std::span<const sim::galactic::PlanetId> production_sites);
// True when the player has an allowed producer at the planet. Reads only that player's rows at
// that planet: the world input keeps producers in canonical (player, planet, domain, type) order.
[[nodiscard]] bool allowed_production_site(const galactic_ai::WorldInput&, tactical::PlayerId, sim::galactic::PlanetId);
// WGA-442: main ground/space totals precede the ordered classified rows.
// Only classified force is scaled; zero/nonpositive domain totals clear its
// classified rows. Missing cached or actual defensive facts refuse.
[[nodiscard]] core::Result<std::vector<GalacticSelectionContrast>> galactic_target_contrast(
    const GalacticWorld&, const Context&, std::span<const std::uint64_t> categories, Real classified_scale);
// WGA-443: repeat starting spawns in retained type/count order, then append
// each space escort once. This describes power, never creates actors.
[[nodiscard]] core::Result<std::vector<GalacticSelectionGarrison>> galactic_proposal_garrisons(
    const GalacticSelectionContent&, sim::galactic::TypeId, std::uint32_t tech, sim::galactic::PlanetId target);
// The callback receives the completed retained parent snapshot. Reachability
// and routes remain B/E's distinct copied relations; missing relations refuse.
[[nodiscard]] GalacticSelectionProvider stock_selection_provider(
    std::shared_ptr<const GalacticSelectionContent>, std::shared_ptr<const galactic_goals::MaterializedPlanCatalog>,
    std::shared_ptr<const GalacticWorld>, std::shared_ptr<const Engine>,
    std::function<std::shared_ptr<const sim::galactic::GalacticSnapshot>()>, GalacticHeroSelectionProvider = {},
    GalacticFreestoreSelectionProvider = {});
} // namespace eawr::script::foc::ai
