#include "eawr/skirmish/placement.hpp"

#include "skirmish_internal.hpp"

namespace eawr::skirmish {

namespace {

using Box = PlacementBox;

[[nodiscard]] Fixed one() noexcept { return Fixed::from_raw(Fixed::scale); }

[[nodiscard]] bool scaled(const Fixed low, const Fixed high, const Fixed scale, Fixed& out_low, Fixed& out_high) {
    return sim::math::try_multiply(low, scale, out_low) && sim::math::try_multiply(high, scale, out_high);
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

} // namespace eawr::skirmish
