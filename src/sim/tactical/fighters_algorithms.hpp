#pragma once

#include "eawr/sim/tactical/fighters.hpp"

#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "../math/wide.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

namespace eawr::sim::tactical::fighters_detail {

using math::Fixed;
using math::Vec3;
using motion_detail::Calc;
using motion_detail::clamp180;
using motion_detail::two_pi;
using motion_detail::whole;

// Slot keys of the spawner's keyed draws (FL-01, FL-03, FL-06).
constexpr std::uint32_t service_slot = 0xfffe0001U;
constexpr std::uint32_t entry_slot = 0xfffe0002U;
constexpr std::uint32_t bay_slot = 0xfffe0003U;

constexpr std::uint64_t service_interval = 30; // SPAWN_SQUADRON's service interval base (FL-01)
constexpr std::size_t max_members = 64;
constexpr std::size_t max_entries = 64;
constexpr std::size_t max_bays = 255;

template <typename T>
[[nodiscard]] const T* find_by_type(const std::vector<T>& rows, const TypeId type_id) noexcept {
    const auto found = std::lower_bound(rows.begin(), rows.end(), type_id,
        [](const T& row, const TypeId id) { return row.type_id < id; });
    return found != rows.end() && found->type_id == type_id ? &*found : nullptr;
}

template <typename T>
[[nodiscard]] bool increasing(const std::vector<T>& rows) noexcept {
    for (std::size_t index = 1; index < rows.size(); ++index) {
        if (rows[index].type_id <= rows[index - 1].type_id) return false;
    }
    return true;
}

[[nodiscard]] inline bool rate(const Fixed value, const bool positive) noexcept {
    return (positive ? value.raw() > 0 : value.raw() >= 0) && value.raw() <= max_motion_rate * Fixed::scale;
}

[[nodiscard]] inline bool distance(const Fixed value) noexcept {
    return value.raw() >= 0 && value.raw() <= max_motion_coordinate * Fixed::scale;
}

[[nodiscard]] inline bool point(const Vec3& value) noexcept {
    const auto inside = [](const Fixed component) {
        return component.raw() >= -max_motion_coordinate * Fixed::scale
            && component.raw() <= max_motion_coordinate * Fixed::scale;
    };
    return inside(value.x) && inside(value.y) && inside(value.z);
}

[[nodiscard]] inline core::Result<void> invalid(const std::string& message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "squadron table: " + message));
}

[[nodiscard]] inline Fixed sign(const Fixed value) noexcept {
    return value.raw() > 0 ? whole(1) : value.raw() < 0 ? whole(-1) : Fixed{};
}

// Compare distances in raw Q24 units. The accepted coordinates can make a Q24 square overflow,
// while three squared raw differences fit in the same wide accumulator used by combat queries.
[[nodiscard]] inline math::detail::UInt192 squared_components(const Fixed x, const Fixed y, const Fixed z = Fixed{}) {
    math::detail::UInt192 total{};
    const auto add_square = [&](const Fixed component) {
        const auto raw = component.raw();
        const auto magnitude = raw < 0 ? std::uint64_t{} - static_cast<std::uint64_t>(raw)
                                       : static_cast<std::uint64_t>(raw);
        static_cast<void>(math::detail::add_magnitude(total, math::detail::multiply_u64(magnitude, magnitude)));
    };
    add_square(x);
    add_square(y);
    add_square(z);
    return total;
}

// FD-10 (#615): the share of a look-ahead whose square rounds to zero in Q24 though it is not
// zero (a craft all but stopped): -offset.look over look.look from the exact raw products,
// clamped to [0, 1]. FoC's float division has a tiny positive denominator there, so it gives
// this clamped ratio (nearly always 0 or 1), never a trap.
[[nodiscard]] inline Fixed short_look_share(const Vec3& offset, const Vec3& look, const math::detail::UInt192& look_squared) {
    auto along = math::detail::SignedWide::product(offset.x.raw(), look.x.raw());
    along.add(math::detail::SignedWide::product(offset.y.raw(), look.y.raw()));
    along.add(math::detail::SignedWide::product(offset.z.raw(), look.z.raw()));
    if (along.magnitude.is_zero() || !along.negative) return Fixed{};
    if (math::detail::compare(along.magnitude, look_squared) >= 0) return whole(1);
    // Below the look-ahead's square, under 3 * 2^23 raw^2 when its Q24 square rounds to zero.
    std::int64_t raw{};
    const auto scaled = math::detail::SignedWide::scaled(static_cast<std::int64_t>(along.magnitude.limb[0]), Fixed::fractional_bits);
    return math::detail::rounded_divide_to_raw(scaled, look_squared, raw) ? Fixed::from_raw(raw) : whole(1);
}

// The yaw and pitch that point a craft's nose at `offset` (FM-02): the offset in the craft's
// frame, unyawed then unpitched.
inline void local_angles(Calc& calc, const CraftState& state, const Vec3& offset, Fixed& yaw_delta, Fixed& pitch_delta) {
    const Fixed cy = calc.cos_deg(state.yaw);
    const Fixed sy = calc.sin_deg(state.yaw);
    const Fixed x1 = calc.add(calc.mul(offset.x, cy), calc.mul(offset.y, sy));
    const Fixed y1 = calc.sub(calc.mul(offset.y, cy), calc.mul(offset.x, sy));
    const Fixed cp = calc.cos_deg(state.pitch);
    const Fixed sp = calc.sin_deg(state.pitch);
    const Fixed lx = calc.sub(calc.mul(x1, cp), calc.mul(offset.z, sp));
    const Fixed lz = calc.add(calc.mul(x1, sp), calc.mul(offset.z, cp));
    yaw_delta = lx.raw() == 0 && y1.raw() == 0 ? Fixed{} : clamp180(calc.atan2_deg(y1, lx));
    const Fixed across = calc.length(lx, y1);
    pitch_delta = across.raw() == 0 && lz.raw() == 0 ? Fixed{} : clamp180(calc.neg(calc.atan2_deg(lz, across)));
}

// FD-05: `target` within `range` of `position` and 90 degrees of yaw and of pitch of the nose.
inline bool followable(Calc& calc, const Vec3& position, const CraftState& state, const Vec3& target, const Fixed range,
    Fixed& yaw, Fixed& pitch) {
    const auto squared = squared_components(calc.sub(target.x, position.x), calc.sub(target.y, position.y),
        calc.sub(target.z, position.z));
    if (math::detail::compare(squared_components(range, Fixed{}), squared) < 0) return false;
    local_angles(calc, state, {calc.sub(target.x, position.x), calc.sub(target.y, position.y),
        calc.sub(target.z, position.z)}, yaw, pitch);
    return calc.abs(yaw) <= whole(90) && calc.abs(pitch) <= whole(90);
}

} // namespace eawr::sim::tactical::fighters_detail
