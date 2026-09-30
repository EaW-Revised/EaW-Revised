#pragma once

#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <optional>

// Orders that send a unit towards another unit (#452, docs/behaviour/space-orders.md): the
// approach distance, the in-range tests, the intercept prediction and the approach slot. Pure
// Q24 functions of their arguments: the session gathers the inputs and plans the moves.
namespace eawr::sim::tactical::detail {

// OR-03, OR-04, OR-14: the unit's Targeting_Max_Attack_Distance (zero without one) plus the
// target's extent, which is not loaded and counts as zero (space weapon fire P-04); a guard takes
// at most the guard range. It is both the approach distance and the range the in-range tests use.
[[nodiscard]] math::Fixed approach_distance(
    bool guard, const std::optional<math::Fixed>& attack_distance, math::Fixed guard_range) noexcept;

// OR-07: the frame at which the unit, flying straight at `max_speed` (> 0), meets a target at
// `target` that flies along its facing `target_yaw` (degrees) at `target_speed`, brought forward
// by the part of the way `minimum_range` covers; `frame` itself when there is no meeting or a
// value leaves Q24.
[[nodiscard]] std::uint64_t prediction_frame(std::uint64_t frame, const math::Vec3& unit, math::Fixed max_speed,
    const math::Vec3& target, math::Fixed target_yaw, math::Fixed target_speed, math::Fixed minimum_range);

// OR-05: the approach slot, 0.9 times `approach` from `predicted` on the XY line towards `unit`,
// at the predicted position's height; `predicted` itself when the unit stands on its XY.
[[nodiscard]] core::Result<math::Vec3> approach_slot(
    const math::Vec3& unit, const math::Vec3& predicted, math::Fixed approach);

} // namespace eawr::sim::tactical::detail
