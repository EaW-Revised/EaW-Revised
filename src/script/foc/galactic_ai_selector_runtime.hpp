#pragma once

#include "galactic_ai_selection.hpp"

#include <functional>
#include <map>
#include <optional>

namespace eawr::script::foc::ai {
struct GoalType;
struct PlanDef;
struct GalacticSelectionGarrison {
    sim::galactic::TypeId type{};
    galactic_goals::ProductionChannel channel{};
    Real power{}, terrain_effectiveness{real(1)};
    bool all_main_domains{}; // WGA-444: native proposal garrisons match both main domains
};

// WGA-58..60/66: target-local, immutable facts supplied by the world/content
// adapter. Absence means unavailable; the selector never guesses a hero,
// terrain, population, reach or magic admission result.
struct GalacticSelectionType {
    galactic_goals::ProductionChannel channel{};
    bool structure{}, hero{}, magic_allowed{}, instance_allowed{};
    bool special_weapon{};
    std::uint32_t tech{};
    Real cost{}, build_seconds{}, population{}, survival{};
    std::optional<Real> target_cost, target_build_seconds; // qualified targeted structure/base quotes
    bool terrain_allowed{};
    Real terrain_effectiveness{}, hero_weight{}, escort_weight{}, hero_minimum_cost{}, hero_maximum_cost{};
    bool hero_containment_allowed{};
    std::uint32_t instance_remaining{}; // qualified remaining per-type structure allowance
    std::string survival_type;
    // Qualified proposal expansion; repeated entries preserve spawned count/order.
    std::vector<GalacticSelectionGarrison> garrisons;
};
struct GalacticSelectionObject {
    sim::EntityId object{};
    Real path_cost{};
    bool garrison{};
    sim::galactic::TypeId type{}; // retained whole-object identity, including transit
    bool in_transit{};
};
struct GalacticSelectionContrast {
    galactic_goals::ProductionChannel channel{};
    std::uint64_t category{};
    Real force{};
    bool escort{};
};
struct GalacticSelectionInput {
    std::vector<galactic_ai::ProducerInput> producers;
    std::vector<GalacticSelectionObject> freestore;
    std::map<sim::galactic::TypeId, GalacticSelectionType> types;
    std::vector<GalacticSelectionContrast> contrast;
    std::vector<std::string> missing_facts; // named qualifications required before this plan can select
    std::uint32_t player_tech{};
    bool complete{}, reachable{}, population_check{};
    Real remaining_population{}, minimum_cost{}, maximum_cost{};
    std::uint32_t remaining_structure_slots{};
    std::array<std::uint32_t, 2> remaining_structure_slots_by_channel{};
    std::vector<sim::galactic::PlanetId> production_sites;
    std::vector<sim::galactic::PlanetId> force_stages; // WGA-226/254, authored force order
    // Authored force attachment gates, in the original taskforce order.
    std::vector<std::pair<bool, bool>> attachment_allowed; // hero, special weapon
};
using GalacticSelectionProvider = std::function<core::Result<std::shared_ptr<const GalacticSelectionInput>>(
    sim::galactic::PlayerId, sim::galactic::PlanetId, const GoalType&, const PlanDef&)>;

// Later completion is supplied by the producer owner using the admitted build
// identity, never inferred from a type count or entity-ID floor (GFD-A05/07).
struct GalacticBuildStatus {
    enum class State { waiting, completed, failed } state{State::waiting};
    sim::EntityId object{};
};
using GalacticBuildObserver = std::function<core::Result<GalacticBuildStatus>(sim::galactic::PlayerId, sim::EntityId)>;
} // namespace eawr::script::foc::ai
