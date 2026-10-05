#include "eawr/sim/math/trig.hpp"

#include "cordic_constants.hpp"
#include "wide.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <utility>

namespace eawr::sim::math {
namespace {

constexpr unsigned internal_fractional_bits = 62;
constexpr unsigned output_shift = internal_fractional_bits - Fixed::fractional_bits;
constexpr std::int64_t internal_half_turn = std::int64_t{1} << 61;

// Floor of value / 2^shift: truncation for value >= 0, the negated ceiling of the magnitude
// below. C++20 defines >> on a negative value as exactly that floor, so no division (#503).
std::int64_t arithmetic_shift(std::int64_t value, unsigned shift) noexcept {
    return value >> shift;
}

// `value` when `mask` is 0, -value when it is -1: the CORDIC steps' rotation direction without
// a branch (#520); |value| stays far below 2^63.
std::int64_t signed_by(std::int64_t value, std::int64_t mask) noexcept {
    return (value ^ mask) - mask;
}

// rounded_shift_to_raw of a one-limb magnitude, in 64 bits (#520): the quotient is below 2^26.
std::int64_t round_internal(std::int64_t value) noexcept {
    const std::uint64_t magnitude = detail::unsigned_magnitude(value);
    std::uint64_t quotient = magnitude >> output_shift;
    const std::uint64_t remainder = magnitude & ((std::uint64_t{1} << output_shift) - 1U);
    const std::uint64_t half = std::uint64_t{1} << (output_shift - 1U);
    if (remainder > half || (remainder == half && (quotient & 1U) != 0)) {
        ++quotient;
    }
    const auto result = static_cast<std::int64_t>(quotient);
    return value < 0 ? -result : result;
}

struct RawSinCos {
    std::int64_t sine;
    std::int64_t cosine;
};

RawSinCos cordic_sin_cos(std::int64_t raw_angle) noexcept {
    constexpr std::int64_t quarter = Fixed::scale / 4;
    constexpr std::int64_t half = Fixed::scale / 2;
    if (raw_angle == 0) return {0, Fixed::scale};
    if (raw_angle == quarter) return {Fixed::scale, 0};
    if (raw_angle == -quarter) return {-Fixed::scale, 0};
    if (raw_angle == -half) return {0, -Fixed::scale};

    bool negate_cosine = false;
    std::int64_t reduced = raw_angle;
    if (reduced > quarter) {
        reduced = half - reduced;
        negate_cosine = true;
    } else if (reduced < -quarter) {
        reduced = -half - reduced;
        negate_cosine = true;
    }

    std::int64_t x = detail::cordic_gain_inverse_q62;
    std::int64_t y = 0;
    std::int64_t z = reduced * (std::int64_t{1} << output_shift);
    // Rotate toward z = 0: by +atan when z >= 0, by -atan below (mask -1).
    for (unsigned index = 0; index < detail::cordic_atan_turns_q62.size(); ++index) {
        const std::int64_t old_x = x;
        const std::int64_t old_y = y;
        const std::int64_t mask = z >> 63;
        x = old_x - signed_by(arithmetic_shift(old_y, index), mask);
        y = old_y + signed_by(arithmetic_shift(old_x, index), mask);
        z -= signed_by(detail::cordic_atan_turns_q62[index], mask);
    }
    std::int64_t sine = std::clamp(round_internal(y), -Fixed::scale, Fixed::scale);
    std::int64_t cosine = std::clamp(round_internal(x), -Fixed::scale, Fixed::scale);
    if (negate_cosine) cosine = -cosine;
    return {sine, cosine};
}

} // namespace

Fixed sin_turn(Fixed angle) noexcept {
    return Fixed::from_raw(cordic_sin_cos(wrap_turn(angle).raw()).sine);
}

Fixed cos_turn(Fixed angle) noexcept {
    return Fixed::from_raw(cordic_sin_cos(wrap_turn(angle).raw()).cosine);
}

SinCos sin_cos_turn(Fixed angle) noexcept {
    const auto both = cordic_sin_cos(wrap_turn(angle).raw());
    return {Fixed::from_raw(both.sine), Fixed::from_raw(both.cosine)};
}

SinCos TrigCache::sample(Fixed angle) noexcept {
    angle = wrap_turn(angle);
    for (std::size_t index = 0; index < size_; ++index) {
        if (entries_[index].angle == angle) return entries_[index].value;
    }
    const auto value = sin_cos_turn(angle);
    entries_[next_] = {angle, value};
    next_ = (next_ + 1) % entries_.size();
    size_ = std::min(size_ + 1, entries_.size());
    ++builds_;
    return value;
}

core::Result<Fixed> atan2_turn(Fixed y_value, Fixed x_value) noexcept {
    std::int64_t x = x_value.raw();
    std::int64_t y = y_value.raw();
    if (x == 0 && y == 0) {
        core::Diagnostic diagnostic;
        diagnostic.code = diagnostic_codes::undefined_angle;
        diagnostic.message = "atan2 is undefined for the zero vector";
        return core::Result<Fixed>::failure(std::move(diagnostic));
    }
    constexpr std::int64_t quarter = Fixed::scale / 4;
    constexpr std::int64_t half = Fixed::scale / 2;
    if (x == 0) {
        return core::Result<Fixed>::success(
            Fixed::from_raw(y > 0 ? quarter : -quarter));
    }
    if (y == 0) {
        return core::Result<Fixed>::success(
            Fixed::from_raw(x > 0 ? 0 : -half));
    }

    const bool original_y_positive = y > 0;
    constexpr std::uint64_t safe = std::uint64_t{1} << 60U;
    while (detail::unsigned_magnitude(x) > safe || detail::unsigned_magnitude(y) > safe) {
        x /= 2;
        y /= 2;
    }
    // Doubled until either exceeds 2^59: the first k with max(|x|, |y|) 2^k above it, in one
    // step (#520).
    const std::uint64_t largest = std::max(detail::unsigned_magnitude(x), detail::unsigned_magnitude(y));
    if (largest <= safe / 2U) {
        auto doublings = 60U - static_cast<unsigned>(std::bit_width(largest));
        if ((largest << doublings) <= safe / 2U) {
            ++doublings;
        }
        x *= std::int64_t{1} << doublings;
        y *= std::int64_t{1} << doublings;
    }

    std::int64_t z = 0;
    if (x < 0) {
        x = -x;
        y = -y;
        z = original_y_positive ? internal_half_turn : -internal_half_turn;
    }
    for (unsigned index = 0; index < detail::cordic_atan_turns_q62.size(); ++index) {
        const std::int64_t old_x = x;
        const std::int64_t old_y = y;
        if (old_y == 0) {
            break;
        }
        // Rotate toward y = 0: clockwise when y > 0, counter-clockwise below (mask -1).
        const std::int64_t mask = old_y >> 63;
        x = old_x + signed_by(arithmetic_shift(old_y, index), mask);
        y = old_y - signed_by(arithmetic_shift(old_x, index), mask);
        z += signed_by(detail::cordic_atan_turns_q62[index], mask);
    }
    return core::Result<Fixed>::success(wrap_turn(Fixed::from_raw(round_internal(z))));
}

} // namespace eawr::sim::math
