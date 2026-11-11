#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/tactical/fog_cells.hpp"

#include <cstdint>

namespace eawr::skirmish {

// The most cells a land fog grid has along one axis (LC-85).
inline constexpr std::uint32_t land_fog_max_cells = 512;

// LC-85: the land fog grid of a terrain `samples_wide` x `samples_tall` samples at `sample_spacing`.
// The grid covers the whole terrain rectangle from its low corner (0, 0) to (width x spacing, height x spacing),
// with row 0 at the top. Each axis has ceil(extent / desired_cell_size) cells, at most land_fog_max_cells, and the
// cell size is then the extent over that count, so the cells fill the terrain exactly. The ramp step follows
// `regrow_seconds` (LandFOWRegrowTime) through the shared regrow rule.
// Fails with diagnostic_codes::input (EAWR-SKIRMISH-0001) when the terrain has no extent, a constant is not positive, or the two
// axes would get cells of different sizes: the shared reveal needs square cells, so such a terrain is refused
// instead of being fogged with a distorted grid (named deviation: retail asserts square cells).
[[nodiscard]] core::Result<sim::tactical::FogRules> land_fog_rules(std::uint32_t samples_wide,
    std::uint32_t samples_tall, sim::math::Fixed sample_spacing, sim::math::Fixed desired_cell_size,
    sim::math::Fixed regrow_seconds);

} // namespace eawr::skirmish
