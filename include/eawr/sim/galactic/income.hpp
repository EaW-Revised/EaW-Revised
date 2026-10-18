#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <span>

namespace eawr::sim::galactic {

struct PlanetIncomeContribution {
    math::Fixed base, bonus, alignment, political, trade, total;
    // WGEP-05: an income-blocked planet's actual income is zero without refreshing
    // its sources; its retained political source is not rewritten by collection.
    bool blocked{};
    friend bool operator==(const PlanetIncomeContribution&, const PlanetIncomeContribution&) = default;
};
[[nodiscard]] core::Result<std::vector<IncomeCurvePoint>> parse_income_curve(std::string_view);
[[nodiscard]] core::Result<math::Fixed> evaluate_income_curve(std::span<const IncomeCurvePoint>, math::Fixed);
// All additive contributions use the unmultiplied basic income (WGEP-06).
[[nodiscard]] core::Result<PlanetIncomeContribution> planet_income_contribution(
    math::Fixed basic, math::Fixed black_market_multiplier, math::Fixed bonus,
    math::Fixed alignment_bonus, math::Fixed alignment_penalty,
    math::Fixed maximum_alignment_bonus, math::Fixed maximum_alignment_penalty,
    math::Fixed political_value, math::Fixed route_factor);
[[nodiscard]] core::Result<void> validate_planet_income_rules(const GalacticSetup&);

// WGEP-31: source identities are independent of stacking categories.
struct ProductionReduction {
    EntityId source{};
    std::uint32_t category{};
    math::Fixed reduction;
    friend bool operator==(const ProductionReduction&, const ProductionReduction&) = default;
};

[[nodiscard]] core::Result<math::Fixed> production_reduction_multiplier(
    std::span<const ProductionReduction> global, std::span<const ProductionReduction> local,
    bool price);
// WGEP-10: the mining multiplier applies to the bonus, not the planet's base income.
[[nodiscard]] core::Result<math::Fixed> mining_income_bonus(math::Fixed basic,
    math::Fixed percentage, math::Fixed absolute, bool mining_colony, math::Fixed colony_multiplier);

[[nodiscard]] core::Result<void> validate_income(const GalacticSetup&);
[[nodiscard]] core::Result<math::Fixed> gambling_income_sample(math::Fixed basic,
    const GamblingIncomeAbility&, std::uint32_t win_draw, std::uint32_t factor_draw);
[[nodiscard]] core::Result<math::Fixed> planet_income_bonus(const IncomeSetup&,
    PlanetId, PlayerId current_owner, math::Fixed basic);
[[nodiscard]] core::Result<math::Fixed> income_production_multiplier(const IncomeSetup&,
    std::span<const Planet>, PlanetId, PlayerId, TypeId, bool price);
// One authored reduction of one live source that can reduce a production price or time
// (WGEP-54/74), gathered once when many offers are prepared against the same income inputs and
// planets (#3003). Rows are ordered by (type, price), in income source then authored order.
struct ProductionReductionSource {
    TypeId type{};
    bool price{};
    EntityId id{};
    PlanetId planet{};
    PlayerId planet_owner{};
    bool planet_source{}, intimidation{}, galaxy_wide{};
    std::uint32_t category{};
    math::Fixed reduction;
    friend bool operator==(const ProductionReductionSource&, const ProductionReductionSource&) = default;
};
[[nodiscard]] std::vector<ProductionReductionSource> production_reduction_sources(const IncomeSetup&, std::span<const Planet>);
[[nodiscard]] core::Result<math::Fixed> income_production_multiplier(std::span<const ProductionReductionSource>,
    PlanetId, PlayerId, TypeId, bool price);

} // namespace eawr::sim::galactic
