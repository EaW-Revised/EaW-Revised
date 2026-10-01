#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <optional>
#include <span>

// The free-space search that places a new object near a point (#597, docs/behaviour/space-movement.md
// PL-01 to PL-08). The skirmish start and a bought unit's arrival (#530, PL-08) both run it, so it
// lives here in the simulation; eawr::skirmish re-exports the names. Everything is Q24 and a pure
// function of its inputs.
namespace eawr::sim::tactical {

// An axis-aligned box in the XY plane. As a type's placement box it is relative to the object's
// position; as a blocker it is in world space. Height never matters (PL-04).
struct PlacementBox final {
    math::Fixed min_x;
    math::Fixed min_y;
    math::Fixed max_x;
    math::Fixed max_y;
    friend constexpr bool operator==(const PlacementBox&, const PlacementBox&) noexcept = default;
};

// PL-05: the world box an object at `position` facing `yaw_degrees` blocks, the axis-aligned box
// around its turned placement box.
[[nodiscard]] core::Result<PlacementBox> blocker_bounds(
    const PlacementBox& box, math::Vec3 position, math::Fixed yaw_degrees);

struct FreeSpaceSearch final {
    math::Vec3 centre;
    PlacementBox box; // the new object's placement box (PL-02)
    math::Fixed start_angle_degrees;
    math::Fixed max_distance{math::Fixed::from_raw(std::int64_t{2500} * math::Fixed::scale)};
};

// PL-03 to PL-06: the first candidate point whose box meets no blocker, or nullopt when every
// candidate within the distance is taken (the caller decides, PL-07, PL-08).
[[nodiscard]] core::Result<std::optional<math::Vec3>> find_free_space(
    const FreeSpaceSearch& search, std::span<const PlacementBox> blockers);

// PL-03: the start's search begins 45 degrees clockwise of the marker's facing yaw: the start angle is
// the yaw plus this offset.
inline constexpr std::int64_t start_search_angle_offset_degrees = -45;

} // namespace eawr::sim::tactical
