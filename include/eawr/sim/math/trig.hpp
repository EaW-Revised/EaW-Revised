#pragma once

#include "eawr/sim/math/fixed.hpp"

namespace eawr::sim::math {

[[nodiscard]] constexpr Fixed wrap_turn(Fixed angle) noexcept {
    constexpr std::int64_t half = Fixed::scale / 2;
    std::int64_t raw = angle.raw() % Fixed::scale;
    if (raw >= half) {
        raw -= Fixed::scale;
    } else if (raw < -half) {
        raw += Fixed::scale;
    }
    return Fixed::from_raw(raw);
}

[[nodiscard]] Fixed sin_turn(Fixed angle) noexcept;
[[nodiscard]] Fixed cos_turn(Fixed angle) noexcept;
// Both from one CORDIC run: {sin_turn(angle), cos_turn(angle)} (#520).
struct SinCos {
    Fixed sine;
    Fixed cosine;
};
[[nodiscard]] SinCos sin_cos_turn(Fixed angle) noexcept;
[[nodiscard]] core::Result<Fixed> atan2_turn(Fixed y, Fixed x) noexcept;

} // namespace eawr::sim::math
