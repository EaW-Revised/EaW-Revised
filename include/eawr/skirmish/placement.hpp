#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/units/unit_tables.hpp"

#include <optional>
#include <span>

// The free-space search that places a new object near a point (#597, docs/behaviour/space-movement.md
// PL-01 to PL-08). The skirmish start places every starting company (and each squadron craft) with it;
// a purchased or reinforcing unit's arrival is meant to reuse it with its own start angle and
// fallback (PL-08). Everything is Q24 and a pure function of its inputs.
namespace eawr::skirmish {

using sim::math::Fixed;
using sim::math::Vec3;

// An axis-aligned box in the XY plane. As a type's placement box it is relative to the object's
// position; as a blocker it is in world space. Height never matters (PL-04).
struct PlacementBox final {
    Fixed min_x;
    Fixed min_y;
    Fixed max_x;
    Fixed max_y;
    friend constexpr bool operator==(const PlacementBox&, const PlacementBox&) noexcept = default;
};

// PL-02: a type's placement box is its model's collision bounds times Scale_Factor, unturned. None
// when the type has no collision bounds (it is then placed on the point, PL-01).
[[nodiscard]] std::optional<PlacementBox> placement_box(const units::UnitType& type);
// The same for a map object type the tables load only for its footprint: its collision half
// extents about its position, times Scale_Factor.
[[nodiscard]] std::optional<PlacementBox> placement_box(const units::ObstacleType& type);

// PL-05: the world box an object at `position` facing `yaw_degrees` blocks, the axis-aligned box
// around its turned placement box.
[[nodiscard]] core::Result<PlacementBox> blocker_bounds(const PlacementBox& box, Vec3 position, Fixed yaw_degrees);

struct FreeSpaceSearch final {
    Vec3 centre;
    PlacementBox box; // the new object's placement box (PL-02)
    Fixed start_angle_degrees;
    Fixed max_distance{Fixed::from_raw(std::int64_t{2500} * Fixed::scale)};
};

// PL-03 to PL-06: the first candidate point whose box meets no blocker, or nullopt when every
// candidate within the distance is taken (the caller decides, PL-07, PL-08).
[[nodiscard]] core::Result<std::optional<Vec3>> find_free_space(
    const FreeSpaceSearch& search, std::span<const PlacementBox> blockers);

// PL-03: the start's search begins 45 degrees clockwise of the marker's facing yaw: the start angle is
// the yaw plus this offset.
inline constexpr std::int64_t start_search_angle_offset_degrees = -45;

} // namespace eawr::skirmish
