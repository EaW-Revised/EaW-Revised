#include "orders_internal.hpp"

#include "motion_internal.hpp"

#include <algorithm>

namespace eawr::sim::tactical::detail {

namespace {

using math::Fixed;
using motion_detail::whole;

// OR-05: the slot radius is 0.9 times the approach distance, rounded once to Q24.
constexpr Fixed slot_fraction = Fixed::from_raw(15099494);
// OR-07: FoC treats a quadratic coefficient below 0.0001 (units per frame, squared) as zero.
constexpr Fixed linear_threshold = Fixed::from_raw(1678);

} // namespace

Fixed approach_distance(const bool guard, const std::optional<Fixed>& attack_distance, const Fixed guard_range) noexcept {
    const auto distance = attack_distance && attack_distance->raw() > 0 ? *attack_distance : Fixed{};
    return guard ? std::min(distance, guard_range) : distance;
}

std::uint64_t prediction_frame(const std::uint64_t frame, const math::Vec3& unit, const Fixed max_speed,
    const math::Vec3& target, const Fixed target_yaw, const Fixed target_speed, const Fixed minimum_range) {
    if (max_speed.raw() <= 0) return frame;
    motion_detail::Calc calc;
    // D, the target's offset, and V, its velocity along its facing (OR-07). FoC's quadratic in the
    // meeting time t, |D + V t| = s t, is solved here in units of the distance: with u = D / s,
    // w = V / s, L = |u| and t = L tau, (|w|^2 - 1) tau^2 + 2 (u / L . w) tau + 1 = 0, so no term
    // grows with the square of the distance.
    const math::Vec3 offset{calc.sub(target.x, unit.x), calc.sub(target.y, unit.y), calc.sub(target.z, unit.z)};
    const math::Vec3 velocity{calc.mul(calc.cos_deg(target_yaw), target_speed),
                              calc.mul(calc.sin_deg(target_yaw), target_speed), Fixed{}};
    const auto quadratic_real = calc.sub(calc.add(calc.mul(velocity.x, velocity.x), calc.mul(velocity.y, velocity.y)),
                                         calc.mul(max_speed, max_speed));
    const math::Vec3 u{calc.div(offset.x, max_speed), calc.div(offset.y, max_speed), calc.div(offset.z, max_speed)};
    const auto length = calc.length(u);
    if (!calc.ok() || length.raw() <= 0) return frame;
    const auto wx = calc.div(velocity.x, max_speed);
    const auto wy = calc.div(velocity.y, max_speed);
    const auto a = calc.sub(calc.add(calc.mul(wx, wx), calc.mul(wy, wy)), whole(1));
    const auto b = calc.mul(whole(2), calc.add(calc.mul(calc.div(u.x, length), wx), calc.mul(calc.div(u.y, length), wy)));
    if (!calc.ok()) return frame;
    std::optional<Fixed> tau;
    if (calc.abs(quadratic_real) >= linear_threshold) {
        const auto discriminant = calc.sub(calc.mul(b, b), calc.mul(whole(4), a));
        if (!calc.ok() || discriminant.raw() < 0) return frame;
        const auto twice_a = calc.mul(whole(2), a);
        if (discriminant.raw() == 0) {
            tau = calc.div(calc.neg(b), twice_a);
        } else {
            const auto root = calc.sqrt(discriminant);
            const auto first = calc.div(calc.add(calc.neg(b), root), twice_a);
            const auto second = calc.div(calc.sub(calc.neg(b), root), twice_a);
            const auto larger = std::max(first, second);
            const auto smaller = std::min(first, second);
            if (larger.raw() < 0) return frame;
            tau = smaller.raw() > 0 ? smaller : larger;
        }
    } else if (b.raw() != 0) {
        tau = calc.div(whole(-1), b);
    }
    if (!tau || !calc.ok() || tau->raw() < 0) return frame;
    auto meet = calc.mul(*tau, length);
    if (meet.raw() > 0) {
        // Brought forward by the fraction of the distance the approach does not need to fly.
        const auto distance = calc.length(offset);
        if (distance.raw() <= 0 || minimum_range > distance) {
            meet = Fixed{};
        } else {
            meet = std::max(Fixed{}, calc.sub(meet, calc.div(calc.mul(meet, minimum_range), distance)));
        }
    }
    if (!calc.ok()) return frame;
    return frame + static_cast<std::uint64_t>(meet.raw() / Fixed::scale);
}

core::Result<math::Vec3> approach_slot(const math::Vec3& unit, const math::Vec3& predicted, const Fixed approach) {
    motion_detail::Calc calc;
    const auto dx = calc.sub(unit.x, predicted.x);
    const auto dy = calc.sub(unit.y, predicted.y);
    const auto length = calc.length(dx, dy);
    if (!calc.ok()) return core::Result<math::Vec3>::failure(calc.error("approach slot"));
    if (length.raw() == 0) return core::Result<math::Vec3>::success(predicted);
    const auto radius = calc.mul(approach, slot_fraction);
    math::Vec3 slot{calc.add(predicted.x, calc.div(calc.mul(dx, radius), length)),
                    calc.add(predicted.y, calc.div(calc.mul(dy, radius), length)), predicted.z};
    if (!calc.ok()) return core::Result<math::Vec3>::failure(calc.error("approach slot"));
    return core::Result<math::Vec3>::success(slot);
}

} // namespace eawr::sim::tactical::detail
