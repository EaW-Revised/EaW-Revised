#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/free_space.hpp"
#include "eawr/units/unit_tables.hpp"

#include <optional>
#include <span>

// The placement boxes of the skirmish start (#597, docs/behaviour/space-movement.md PL-01 to PL-08).
// The free-space search itself is in the simulation (eawr/sim/tactical/free_space.hpp), so a bought
// unit's arrival (#530) runs the same one; its names are re-exported here.
namespace eawr::skirmish {

using sim::math::Fixed;
using sim::math::Vec3;

using sim::tactical::FreeSpaceSearch;
using sim::tactical::PlacementBox;
using sim::tactical::blocker_bounds;
using sim::tactical::find_free_space;
using sim::tactical::start_search_angle_offset_degrees;

// PL-02: a type's placement box is its model's collision bounds times Scale_Factor, unturned. None
// when the type has no collision bounds (it is then placed on the point, PL-01).
[[nodiscard]] std::optional<PlacementBox> placement_box(const units::UnitType& type);
// The same for a map object type the tables load only for its footprint: its collision half
// extents about its position, times Scale_Factor.
[[nodiscard]] std::optional<PlacementBox> placement_box(const units::ObstacleType& type);

} // namespace eawr::skirmish
