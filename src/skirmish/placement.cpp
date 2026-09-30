#include "eawr/skirmish/placement.hpp"

#include "eawr/sim/math/trig.hpp"
#include "skirmish_internal.hpp"

#include <algorithm>
#include <string>

namespace eawr::skirmish {

namespace {

using Box = PlacementBox;

[[nodiscard]] core::Diagnostic overflow(std::string what) {
    return detail::error(diagnostic_codes::fixture, "free-space placement overflows: " + std::move(what));
}

[[nodiscard]] Fixed one() noexcept { return Fixed::from_raw(Fixed::scale); }

// A turn of `degrees`, wrapped to [-1/2, 1/2).
[[nodiscard]] core::Result<Fixed> turns_of(const Fixed degrees) {
    auto turns = sim::math::divide(degrees, Fixed::from_raw(std::int64_t{360} * Fixed::scale));
    if (!turns) return turns;
    return core::Result<Fixed>::success(sim::math::wrap_turn(turns.value()));
}

[[nodiscard]] bool scaled(const Fixed low, const Fixed high, const Fixed scale, Fixed& out_low, Fixed& out_high) {
    return sim::math::try_multiply(low, scale, out_low) && sim::math::try_multiply(high, scale, out_high);
}

// PL-04: two boxes overlap when their open intervals meet on both axes (unverified whether
// touching boxes block; the remake lets them touch).
[[nodiscard]] bool overlaps(const Box& left, const Box& right) noexcept {
    return left.min_x < right.max_x && right.min_x < left.max_x && left.min_y < right.max_y
        && right.min_y < left.max_y;
}

[[nodiscard]] bool shifted(const Box& box, const Fixed x, const Fixed y, Box& out) noexcept {
    return sim::math::try_add(box.min_x, x, out.min_x) && sim::math::try_add(box.min_y, y, out.min_y)
        && sim::math::try_add(box.max_x, x, out.max_x) && sim::math::try_add(box.max_y, y, out.max_y);
}

} // namespace

std::optional<PlacementBox> placement_box(const units::UnitType& type) {
    if (!type.collision) return std::nullopt;
    const Fixed scale = type.scale_factor.value_or(one());
    Box box;
    if (!scaled(type.collision->min.x, type.collision->max.x, scale, box.min_x, box.max_x)
        || !scaled(type.collision->min.y, type.collision->max.y, scale, box.min_y, box.max_y)) {
        return std::nullopt;
    }
    return box;
}

std::optional<PlacementBox> placement_box(const units::ObstacleType& type) {
    const auto& footprint = type.footprint;
    if (!footprint.collision_x || !footprint.collision_y) return std::nullopt;
    const Fixed scale = type.scale_factor.value_or(one());
    Fixed half_x;
    Fixed half_y;
    if (!sim::math::try_multiply(*footprint.collision_x, scale, half_x)
        || !sim::math::try_multiply(*footprint.collision_y, scale, half_y)) {
        return std::nullopt;
    }
    return Box{Fixed::from_raw(-half_x.raw()), Fixed::from_raw(-half_y.raw()), half_x, half_y};
}

core::Result<PlacementBox> blocker_bounds(const PlacementBox& box, const Vec3 position, const Fixed yaw_degrees) {
    using BoxResult = core::Result<PlacementBox>;
    auto turns = turns_of(yaw_degrees);
    if (!turns) return BoxResult::failure(turns.error());
    const auto [sin, cos] = sim::math::sin_cos_turn(turns.value());
    const Fixed abs_sin = Fixed::from_raw(sin.raw() < 0 ? -sin.raw() : sin.raw());
    const Fixed abs_cos = Fixed::from_raw(cos.raw() < 0 ? -cos.raw() : cos.raw());
    // Centre and half extents of the model-space box; the centre turns with the yaw (x' = x cos -
    // y sin, y' = x sin + y cos, as the start turns squadron offsets) and the half extents become
    // those of the turned box's axis-aligned hull.
    const Fixed two = Fixed::from_raw(2 * Fixed::scale);
    Fixed sum_x, sum_y, span_x, span_y, centre_x, centre_y, half_x, half_y;
    if (!sim::math::try_add(box.min_x, box.max_x, sum_x) || !sim::math::try_add(box.min_y, box.max_y, sum_y)
        || !sim::math::try_subtract(box.max_x, box.min_x, span_x)
        || !sim::math::try_subtract(box.max_y, box.min_y, span_y) || !sim::math::try_divide(sum_x, two, centre_x)
        || !sim::math::try_divide(sum_y, two, centre_y) || !sim::math::try_divide(span_x, two, half_x)
        || !sim::math::try_divide(span_y, two, half_y)) {
        return BoxResult::failure(overflow("box extents"));
    }
    Fixed xc, ys, xs, yc, reach_xc, reach_ys, reach_xs, reach_yc, world_x, world_y, reach_x, reach_y;
    if (!sim::math::try_multiply(centre_x, cos, xc) || !sim::math::try_multiply(centre_y, sin, ys)
        || !sim::math::try_multiply(centre_x, sin, xs) || !sim::math::try_multiply(centre_y, cos, yc)
        || !sim::math::try_multiply(half_x, abs_cos, reach_xc) || !sim::math::try_multiply(half_y, abs_sin, reach_ys)
        || !sim::math::try_multiply(half_x, abs_sin, reach_xs) || !sim::math::try_multiply(half_y, abs_cos, reach_yc)
        || !sim::math::try_subtract(xc, ys, world_x) || !sim::math::try_add(xs, yc, world_y)
        || !sim::math::try_add(reach_xc, reach_ys, reach_x) || !sim::math::try_add(reach_xs, reach_yc, reach_y)
        || !sim::math::try_add(world_x, position.x, world_x) || !sim::math::try_add(world_y, position.y, world_y)) {
        return BoxResult::failure(overflow("turned box"));
    }
    Box bounds;
    if (!sim::math::try_subtract(world_x, reach_x, bounds.min_x) || !sim::math::try_subtract(world_y, reach_y, bounds.min_y)
        || !sim::math::try_add(world_x, reach_x, bounds.max_x) || !sim::math::try_add(world_y, reach_y, bounds.max_y)) {
        return BoxResult::failure(overflow("world box"));
    }
    return BoxResult::success(bounds);
}

core::Result<std::optional<Vec3>> find_free_space(
    const FreeSpaceSearch& search, const std::span<const PlacementBox> blockers) {
    using Found = core::Result<std::optional<Vec3>>;
    const Box& box = search.box;
    // PL-03: rings a step of 1.2 times the box's larger side apart, from the point itself outward
    // while the radius is below the search distance.
    Fixed width, depth;
    if (!sim::math::try_subtract(box.max_x, box.min_x, width) || !sim::math::try_subtract(box.max_y, box.min_y, depth)) {
        return Found::failure(overflow("box size"));
    }
    auto factor = Fixed::from_decimal("1.2");
    if (!factor) return Found::failure(factor.error());
    Fixed step;
    if (!sim::math::try_multiply(std::max(width, depth), factor.value(), step)) return Found::failure(overflow("ring step"));
    auto start = turns_of(search.start_angle_degrees);
    if (!start) return Found::failure(start.error());
    // 22.5 degrees is exactly 1/16 turn.
    const Fixed sixteenth = Fixed::from_raw(Fixed::scale / 16);
    const auto free_at = [&](const Fixed x, const Fixed y, bool& is_free) {
        Box candidate;
        if (!shifted(box, x, y, candidate)) return false;
        is_free = std::none_of(blockers.begin(), blockers.end(),
            [&](const Box& blocker) { return overlaps(candidate, blocker); });
        return true;
    };
    for (Fixed radius{}; radius < search.max_distance;) {
        // PL-03: 17 bearings from the start angle through a full turn (the last repeats the first);
        // the point itself is tried once.
        for (std::int64_t index = 0; index <= 16; ++index) {
            Fixed turn;
            Fixed offset;
            if (!sim::math::try_multiply(sixteenth, Fixed::from_raw(index * Fixed::scale), offset)
                || !sim::math::try_add(start.value(), offset, turn)) {
                return Found::failure(overflow("bearing"));
            }
            const auto [sin, cos] = sim::math::sin_cos_turn(sim::math::wrap_turn(turn));
            Fixed along_x, along_y, x, y;
            if (!sim::math::try_multiply(radius, cos, along_x) || !sim::math::try_multiply(radius, sin, along_y)
                || !sim::math::try_add(search.centre.x, along_x, x) || !sim::math::try_add(search.centre.y, along_y, y)) {
                return Found::failure(overflow("candidate"));
            }
            bool is_free = false;
            if (!free_at(x, y, is_free)) return Found::failure(overflow("candidate box"));
            if (is_free) return Found::success(Vec3{x, y, search.centre.z});
            if (radius == Fixed{}) break;
        }
        if (step <= Fixed{}) break; // a degenerate box has only the point itself
        if (!sim::math::try_add(radius, step, radius)) return Found::failure(overflow("ring radius"));
    }
    return Found::success(std::nullopt);
}

} // namespace eawr::skirmish
