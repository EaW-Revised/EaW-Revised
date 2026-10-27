#pragma once

#include "eawr/assets/map.hpp"
#include "eawr/sim/tactical/content.hpp"
#include "eawr/sim/tactical/replay.hpp"

namespace eawr::skirmish {

// Content-load boundary; callers supply movement masks from their own content profile.
// Raw passability bit assignments remain an open R2/R3 input, never a fixed class whitelist.
[[nodiscard]] core::Result<sim::tactical::LandTerrain> load_land_terrain(
    const assets::Map& map, std::vector<sim::tactical::LandMovementMask> movement_masks = {},
    std::optional<sim::tactical::LandBounds> map_bounds = {});

// Appends the terrain identity only for land. Space identity bytes stay untouched.
// Session creation additionally requires the effective land company/occupancy bindings.
[[nodiscard]] core::Result<void> bind_land_terrain(sim::tactical::TacticalContent& content,
    sim::tactical::TacticalSetup& setup, const assets::Map& map,
    std::vector<sim::tactical::LandMovementMask> movement_masks = {},
    std::optional<sim::tactical::LandBounds> map_bounds = {});

} // namespace eawr::skirmish
