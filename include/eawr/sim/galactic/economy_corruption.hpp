#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/world.hpp"

namespace eawr::sim::galactic {
// GFS-C-05: host-owned corruption/hero lifecycle supplies these copied inputs.
// U-11 stock station discovery and stale-menu delivery stay named seams.
[[nodiscard]] core::Result<void> validate_economy_corruption(const GalacticSetup&);
// WGEP-30/92/110: additive, no stacking categories; expired rows are removed by the owner.
[[nodiscard]] core::Result<math::Fixed> technology_purchase_multiplier(std::span<const TechnologyPriceModifier>);
[[nodiscard]] core::Result<std::vector<math::Fixed>> corruption_payments(
    EconomyCorruptionState&, std::span<const Planet>, std::span<const FiscalPlayer>,
    std::span<const math::Fixed> pre_siphon, std::uint64_t frame,
    const PartitionExecutor&, bool collect);
[[nodiscard]] core::Result<void> reroll_planet_income(EconomyCorruptionState&, std::uint64_t seed);
[[nodiscard]] core::Result<math::Fixed> economy_draw(EconomyCorruptionState&, std::uint64_t seed);
} // namespace eawr::sim::galactic
