#pragma once

#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/world.hpp"
#include <functional>

namespace eawr::sim::galactic {
// WGEP-42/130/147: even unchanged assignment notifies before the unlock pass.
using TechnologyAssigned = std::function<core::Result<void>(PlayerId, std::int64_t)>;
[[nodiscard]] core::Result<std::size_t> assign_technology(GalacticPlayerEconomy&,
    ProductionPlayer&, std::span<const TechnologyType>, std::int64_t target,
    bool process_unlocks, const TechnologyAssigned& = {});
struct NestedCreationResult {
    EntityId next_id{};
    std::vector<Fleet> fleets;
    std::vector<MovementContents> contents;
    std::vector<SurfaceForce> landed;
    std::vector<CensusObject> originals;
};
// WGS-80/92 / WGEP-41: pure shared company/squadron placement at the live frontier.
[[nodiscard]] core::Result<NestedCreationResult> place_nested_force(
    const NestedCreationSetup&, TypeId, PlayerId, PlanetId, EntityId,
    std::span<const Fleet>, const MovementSetup&, const PoliticalControlSetup&,
    const std::function<core::Result<bool>(const Fleet&)>& combat_skip = {});

enum class ProductionAdmission : std::uint8_t {
    allowed, unavailable, ownership, credits, locked, tech, prerequisites,
    slots, planet_limit, current_limit, lifetime_limit, population, queue_full,
    wrong_entry, unsupported_payment, resource_limit, stale_entry
};
// WGEP-18..34: immutable inputs shared by enqueue and presentation queries.
// The caller supplies the live owner, census and resource bounds, never guesses.
struct ProductionReservations {
    math::Fixed population;
    std::vector<std::pair<TypeId, std::uint64_t>> counts; // ascending type
};
[[nodiscard]] ProductionReservations production_reservations(const ProductionSetup&, PlayerId);
// GHUD-S84: immutable display/command checks share exact object identity and authored prerequisites.
[[nodiscard]] ProductionAdmission ground_sale_admission(const PlanetProduction&, PlayerId, EntityId);
[[nodiscard]] math::Fixed ground_sale_refund(const ProductionObject&, const ProductionOffer&);
struct ObjectSale {
    ProductionAdmission admission{ProductionAdmission::unavailable};
    TypeId type{};
    math::Fixed refund{};
    bool unit{};
};
// GHUD-S93..94: shared publication/command query; all identity and cargo facts are live inputs.
[[nodiscard]] ObjectSale object_sale(const ProductionSetup&, std::span<const CensusType>,
    std::span<const CensusObject>, std::span<const Fleet>, const PoliticalControlSetup*,
    const MovementSetup*, PlayerId, PlanetId, EntityId);
struct ProductionAdmissionInputs {
    const ProductionSetup& production;
    const PlanetProduction& planet;
    const ProductionPlayer& player;
    const GalacticPlayerEconomy& account;
    std::span<const Fleet> fleets;
    PlayerId owner{};
    const PopulationTotal* population{};
    EntityId next_id{};
    std::size_t live_object_count{};
    const ProductionReservations* reservations{};
    std::span<const TypeId> local_unlocks{};
};
[[nodiscard]] ProductionAdmission production_admission(const ProductionAdmissionInputs&,
    const ProductionOffer*, ProductionDomain, bool eligibility_only = false);
// WGEP-50 / GHUD-S57: the population gate on its own, independent of the admission order (credits fail first there).
[[nodiscard]] bool production_population_fits(const ProductionAdmissionInputs&, const ProductionOffer&);
// WGEP-29..31: the same location adjustment and rounding used for a purchase.
[[nodiscard]] core::Result<ProductionOffer> prepared_production_offer(const ProductionOffer&,
    const IncomeSetup*, std::span<const Planet>, PlanetId, PlayerId,
    const EconomyCorruptionState* = nullptr);
// The same adjustment with the reductions production_reduction_sources gathered once from the
// same income inputs and planets, for many offers at one boundary (#3003).
[[nodiscard]] core::Result<ProductionOffer> prepared_production_offer(const ProductionOffer&,
    const IncomeSetup*, std::span<const ProductionReductionSource>, PlanetId, PlayerId,
    const EconomyCorruptionState* = nullptr);
struct ObservedProductionOffer {
    PlayerId player{};
    PlanetId planet{};
    TypeId type{};
    ProductionDomain domain{};
    ProductionKind kind{};
    ProductionAdmission admission{ProductionAdmission::unavailable};
    std::optional<math::Fixed> price, seconds;
    // The authored Build_Cost_Credits before every multiplier: presentation compares it with `price` (GHUD-S57).
    std::optional<math::Fixed> base_price;
    // GHUD-S57: the population fits, evaluated on its own so a credits failure does not hide it.
    bool population_fits{};
    // WGUI-91 eventual-query semantics await the production provider trace.
    std::optional<bool> eventual;
    bool already_in_production{};
    // WGUI-170: capability ignores credits, queue/population admission and host resource bounds.
    bool current{};
    friend bool operator==(const ObservedProductionOffer&, const ObservedProductionOffer&) = default;
};
[[nodiscard]] core::Result<void> validate_production(const GalacticSetup&);
// WGEP-61: the countdown truncates seconds * logical FPS, unlike fiscal rounding.
[[nodiscard]] core::Result<std::uint64_t> production_frames(math::Fixed seconds);
[[nodiscard]] core::Result<void> validate_census(const GalacticSetup&);
// Explicit owned-list inputs; per-object arithmetic runs in partitioned phases.
[[nodiscard]] core::Result<std::vector<PopulationTotal>> population_totals(
    const PopulationCensus&, std::span<const CensusObject>, const ProductionSetup&,
    const PartitionExecutor&);
[[nodiscard]] bool population_admitted(const PopulationTotal&, math::Fixed population);
} // namespace eawr::sim::galactic
