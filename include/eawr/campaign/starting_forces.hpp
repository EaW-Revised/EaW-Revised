#pragma once

#include "eawr/campaign/setup.hpp"

namespace eawr::campaign {
struct StartingForceRuntimeType {
    sim::galactic::TypeId type{};
    bool skips_combat{};
    bool always_spawn_in_orbit{};
    sim::galactic::ActorAbilityState abilities;
};
struct StartingForceRelation {
    sim::galactic::PlayerId player{}, other{};
    bool enemy{}; // hostile admission is distinct from not allied
};
struct StartingForceInputs {
    units::GalacticTypeTable types;
    std::vector<FleetTypeBinding> bindings;
    std::vector<StartingForceRuntimeType> runtime_types;
    std::vector<StartingForceRelation> relations;
    sim::galactic::TypeId fleet_type{}, squadron_type{};
};
struct InitialCampaignRelations {
    std::vector<sim::galactic::ControlRelation> control;
    std::vector<StartingForceRelation> placement;
};
[[nodiscard]] core::Result<sim::galactic::NestedCreationSetup> prepare_nested_creation(const StartingForceInputs&);
// WGA-99: the same original-type placement allocator for ordered native magic
// requests. Additional creation expansion retains the plan's supplied owner.
[[nodiscard]] sim::galactic::GalacticSession::NativeForceBuilder stock_native_force_builder(
    std::shared_ptr<const StartingForceInputs>, sim::galactic::GalacticSession::NativeProductionRequest);
// Sparse completed inputs for request-time Lua creation; only the target planet
// and its orbital/presence rows are copied by the stock placement adapter.
[[nodiscard]] sim::galactic::GalacticSession::NativeShadowForceBuilder stock_native_force_shadow_builder(
    std::shared_ptr<const StartingForceInputs>, sim::galactic::GalacticSession::NativeProductionRequest);
// GFS-A22: only ordinary current slot exhaustion becomes a null placement.
// Content/programming errors retain their diagnostics; the legacy path is unchanged.
[[nodiscard]] sim::galactic::GalacticSession::NativeReconciledForceBuilder stock_native_force_reconciliation_builder(
    std::shared_ptr<const StartingForceInputs>, sim::galactic::GalacticSession::NativeProductionRequest);

// WGS-89: neutral precedence, exact same faction, then authored allies/enemies.
[[nodiscard]] core::Result<InitialCampaignRelations> prepare_initial_campaign_relations(
    const data::galactic::GalacticData&, std::span<const CampaignPlayerBinding>);
// WGS-92/88: after inherited authoring, fresh players have no explicit ability
// locks. Automatic appropriate stealth activates during object initialization.
// Enemy status and the two authored container identities remain explicit inputs.
[[nodiscard]] core::Result<StartingForceInputs> prepare_initial_starting_force_inputs(
    const units::GalacticTypeTable&, std::span<const FleetTypeBinding>,
    std::span<const StartingForceRelation>, sim::galactic::TypeId fleet_type,
    sim::galactic::TypeId squadron_type);
// WGM-24/26/38, WGS-92/88: original dispatch roles and fresh ability state,
// with authored faction factors. Geometry, access and contents have their owners.
[[nodiscard]] core::Result<MovementInputs> prepare_initial_movement_inputs(
    const StartingForceInputs&, const data::galactic::GalacticData&,
    std::span<const MovementPlayerBinding>);
// WGS-80 / CGS-03: dispatch against original behaviours and complete typed live
// presence. Unknown or unrepresented branches explicitly refuse.
[[nodiscard]] core::Result<StartingForcePlacement> place_starting_force(
    const StartingForceInputs&, const StartingForceRequest&, sim::EntityId,
    const sim::galactic::GalacticSetup&, const CampaignPlacementContext&,
    const std::function<core::Result<bool>(const sim::galactic::Fleet&)>& combat_skip = {});
// Commits only the request's new identities and scoped base/presence state.
[[nodiscard]] core::Result<void> apply_starting_force_placement(
    const StartingForceInputs&, const StartingForceRequest&, const StartingForcePlacement&,
    sim::galactic::GalacticSetup&, CampaignPlacementContext&);
} // namespace eawr::campaign
