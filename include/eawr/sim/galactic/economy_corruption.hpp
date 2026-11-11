#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/world.hpp"

#include <cstdint>

namespace eawr::sim::galactic {
// Project arithmetic bound, not retail: shared by authoring clamps and state validation.
inline constexpr std::int64_t economy_arithmetic_bound_raw = std::int64_t{1} << 48;
// GFS-C-05: host-owned corruption/hero lifecycle supplies these copied inputs.
// U-11 stock station discovery and stale-menu delivery stay named seams.
[[nodiscard]] core::Result<void> validate_economy_corruption(const GalacticSetup&);
// The per-table rules of the setup validator, applied to any state with the same shape (a restored checkpoint).
[[nodiscard]] core::Result<void> validate_economy_corruption_tables(const GalacticSetup&, const EconomyCorruptionState&);
// WGEP-30/92/110: additive, no stacking categories; expired rows are removed by the owner.
[[nodiscard]] core::Result<math::Fixed> technology_purchase_multiplier(std::span<const TechnologyPriceModifier>);
[[nodiscard]] core::Result<std::vector<math::Fixed>> corruption_payments(
    EconomyCorruptionState&, std::span<const Planet>, std::span<const FiscalPlayer>,
    std::span<const math::Fixed> pre_siphon, std::uint64_t frame,
    const PartitionExecutor&, bool collect);
[[nodiscard]] core::Result<void> reroll_planet_income(EconomyCorruptionState&, std::uint64_t seed);
[[nodiscard]] core::Result<math::Fixed> economy_draw(EconomyCorruptionState&, std::uint64_t seed);
} // namespace eawr::sim::galactic
