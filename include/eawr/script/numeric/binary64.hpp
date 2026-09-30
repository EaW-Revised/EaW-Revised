#pragma once

// Integer-only IEEE 754 binary64 arithmetic for the authoritative Lua profile.
//
// Values are carried in their binary64 interchange encoding (`Bits`). Every
// operation rounds to nearest, ties to even, and supports subnormals and signed
// zero. Every NaN result, whether propagated or produced by an invalid
// operation, is the one canonical quiet NaN 0xFFF8000000000000 (the x86-64
// "indefinite" value FoC's SSE2 arithmetic produces), so saved state and hashes
// never depend on NaN payloads. There is no global rounding mode or flag state. The algorithms follow the structure of Berkeley SoftFloat 3's f64
// routines (round/pack with ten guard bits and a sticky bit) but are written
// independently; docs/lua-numeric-profile.md is the contract.

#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#endif

namespace eawr::script::numeric::binary64 {

using Bits = std::uint64_t;

inline constexpr Bits sign_mask = 0x8000000000000000ULL;
inline constexpr Bits exponent_mask = 0x7FF0000000000000ULL;
inline constexpr Bits fraction_mask = 0x000FFFFFFFFFFFFFULL;
inline constexpr Bits quiet_bit = 0x0008000000000000ULL;
inline constexpr Bits hidden_bit = 0x0010000000000000ULL;
inline constexpr Bits canonical_nan = 0xFFF8000000000000ULL;
inline constexpr Bits positive_infinity = 0x7FF0000000000000ULL;
inline constexpr Bits negative_infinity = 0xFFF0000000000000ULL;
inline constexpr Bits positive_zero = 0;
inline constexpr Bits negative_zero = sign_mask;
inline constexpr Bits one = 0x3FF0000000000000ULL;

[[nodiscard]] constexpr bool sign_of(Bits value) noexcept { return (value >> 63U) != 0; }
[[nodiscard]] constexpr std::int32_t exponent_of(Bits value) noexcept {
    return static_cast<std::int32_t>((value >> 52U) & 0x7FFU);
}
[[nodiscard]] constexpr Bits fraction_of(Bits value) noexcept { return value & fraction_mask; }
[[nodiscard]] constexpr bool is_nan(Bits value) noexcept { return (value & ~sign_mask) > exponent_mask; }
[[nodiscard]] constexpr bool is_signaling_nan(Bits value) noexcept {
    return is_nan(value) && (value & quiet_bit) == 0;
}
[[nodiscard]] constexpr bool is_infinite(Bits value) noexcept { return (value & ~sign_mask) == exponent_mask; }
[[nodiscard]] constexpr bool is_finite(Bits value) noexcept { return (value & exponent_mask) != exponent_mask; }
[[nodiscard]] constexpr bool is_zero(Bits value) noexcept { return (value & ~sign_mask) == 0; }

[[nodiscard]] constexpr Bits pack(bool sign, std::int32_t exponent, Bits significand) noexcept {
    // The significand may carry the hidden bit, which then increments the
    // exponent field; this is the SoftFloat packToF64UI convention.
    return (static_cast<Bits>(sign) << 63U) + (static_cast<Bits>(static_cast<std::uint32_t>(exponent)) << 52U) +
           significand;
}

namespace detail {

#if defined(__SIZEOF_INT128__)
// A GNU extension; __extension__ keeps -Wpedantic quiet.
__extension__ typedef unsigned __int128 UInt128;
#endif

struct U128 {
    std::uint64_t high;
    std::uint64_t low;
};

[[nodiscard]] constexpr int count_leading_zeros(std::uint64_t value) noexcept {
    if (value == 0) {
        return 64;
    }
    int count = 0;
    if ((value & 0xFFFFFFFF00000000ULL) == 0) { count += 32; value <<= 32U; }
    if ((value & 0xFFFF000000000000ULL) == 0) { count += 16; value <<= 16U; }
    if ((value & 0xFF00000000000000ULL) == 0) { count += 8; value <<= 8U; }
    if ((value & 0xF000000000000000ULL) == 0) { count += 4; value <<= 4U; }
    if ((value & 0xC000000000000000ULL) == 0) { count += 2; value <<= 2U; }
    if ((value & 0x8000000000000000ULL) == 0) { count += 1; }
    return count;
}

// Shift right, OR-ing every shifted-out bit into bit zero ("jamming").
[[nodiscard]] constexpr std::uint64_t shift_right_jam(std::uint64_t value, std::uint32_t distance) noexcept {
    if (distance < 63U) {
        return (value >> distance) | static_cast<std::uint64_t>((value << ((64U - distance) & 63U)) != 0 && distance != 0);
    }
    return static_cast<std::uint64_t>(value != 0);
}

[[nodiscard]] inline U128 multiply_64x64(std::uint64_t left, std::uint64_t right) noexcept {
#if defined(__SIZEOF_INT128__)
    const UInt128 product = static_cast<UInt128>(left) * right;
    return {static_cast<std::uint64_t>(product >> 64U), static_cast<std::uint64_t>(product)};
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    std::uint64_t high = 0;
    const std::uint64_t low = _umul128(left, right, &high);
    return {high, low};
#else
    const std::uint64_t left_low = left & 0xFFFFFFFFU;
    const std::uint64_t left_high = left >> 32U;
    const std::uint64_t right_low = right & 0xFFFFFFFFU;
    const std::uint64_t right_high = right >> 32U;
    const std::uint64_t low_low = left_low * right_low;
    const std::uint64_t low_high = left_low * right_high;
    const std::uint64_t high_low = left_high * right_low;
    const std::uint64_t high_high = left_high * right_high;
    const std::uint64_t middle = (low_low >> 32U) + (low_high & 0xFFFFFFFFU) + (high_low & 0xFFFFFFFFU);
    return {high_high + (low_high >> 32U) + (high_low >> 32U) + (middle >> 32U),
            (middle << 32U) | (low_low & 0xFFFFFFFFU)};
#endif
}

// floor(numerator * 2^shift / divisor) for numerator, divisor < 2^53 and
// shift <= 63, plus whether the division left a remainder. The quotient is
// below 2^63 for every caller.
[[nodiscard]] inline std::uint64_t divide_shifted(std::uint64_t numerator, std::uint32_t shift, std::uint64_t divisor,
                                                  bool& inexact) noexcept {
// clang-cl has __int128 but no __udivti3 in the MSVC runtime, so it takes the
// portable path; every path yields the same exact quotient.
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
    const UInt128 wide = static_cast<UInt128>(numerator) << shift;
    const std::uint64_t quotient = static_cast<std::uint64_t>(wide / divisor);
    inexact = static_cast<std::uint64_t>(wide % divisor) != 0;
    return quotient;
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    const std::uint64_t high = shift == 0 ? 0 : numerator >> (64U - shift);
    const std::uint64_t low = numerator << shift;
    std::uint64_t remainder = 0;
    const std::uint64_t quotient = _udiv128(high, low, divisor, &remainder);
    inexact = remainder != 0;
    return quotient;
#else
    // Eleven quotient bits per step keep remainder << step below 2^64.
    std::uint64_t remainder = numerator;
    std::uint64_t quotient = 0;
    while (shift != 0) {
        const std::uint32_t step = shift < 11U ? shift : 11U;
        remainder <<= step;
        quotient = (quotient << step) | (remainder / divisor);
        remainder %= divisor;
        shift -= step;
    }
    inexact = remainder != 0;
    return quotient;
#endif
}

// Any NaN operand yields the canonical NaN.
[[nodiscard]] constexpr Bits propagate_nan(Bits, Bits) noexcept { return canonical_nan; }

// value = significand * 2^(exponent - 0x43C), significand in [2^62, 2^63)
// unless the value is subnormal; the low ten bits are rounding bits.
[[nodiscard]] constexpr Bits round_pack(bool sign, std::int32_t exponent, std::uint64_t significand) noexcept {
    constexpr std::uint64_t round_increment = 0x200;
    std::uint64_t round_bits = significand & 0x3FFU;
    if (static_cast<std::uint32_t>(exponent) >= 0x7FDU) {
        if (exponent < 0) {
            significand = shift_right_jam(significand, static_cast<std::uint32_t>(-exponent));
            exponent = 0;
            round_bits = significand & 0x3FFU;
        } else if (exponent > 0x7FD || significand + round_increment >= 0x8000000000000000ULL) {
            return pack(sign, 0x7FF, 0);
        }
    }
    significand = (significand + round_increment) >> 10U;
    if (round_bits == 0x200U) {
        significand &= ~std::uint64_t{1};
    }
    if (significand == 0) {
        exponent = 0;
    }
    return pack(sign, exponent, significand);
}

[[nodiscard]] constexpr Bits normalize_round_pack(bool sign, std::int32_t exponent, std::uint64_t significand) noexcept {
    const int shift = count_leading_zeros(significand) - 1;
    exponent -= shift;
    if (shift >= 10 && static_cast<std::uint32_t>(exponent) < 0x7FDU) {
        return pack(sign, significand != 0 ? exponent : 0, significand << static_cast<std::uint32_t>(shift - 10));
    }
    return round_pack(sign, exponent, significand << static_cast<std::uint32_t>(shift));
}

struct Normalized {
    std::int32_t exponent;
    std::uint64_t significand;
};

[[nodiscard]] constexpr Normalized normalize_subnormal(std::uint64_t fraction) noexcept {
    const int shift = count_leading_zeros(fraction) - 11;
    return {1 - shift, fraction << static_cast<std::uint32_t>(shift)};
}

[[nodiscard]] constexpr Bits add_magnitudes(Bits left, Bits right, bool sign) noexcept {
    std::int32_t left_exponent = exponent_of(left);
    std::uint64_t left_significand = fraction_of(left);
    const std::int32_t right_exponent = exponent_of(right);
    std::uint64_t right_significand = fraction_of(right);
    const std::int32_t difference = left_exponent - right_exponent;
    std::int32_t exponent = 0;
    std::uint64_t significand = 0;
    if (difference == 0) {
        if (left_exponent == 0) {
            return left + right_significand;
        }
        if (left_exponent == 0x7FF) {
            if ((left_significand | right_significand) != 0) {
                return propagate_nan(left, right);
            }
            return left;
        }
        exponent = left_exponent;
        significand = (0x0020000000000000ULL + left_significand + right_significand) << 9U;
    } else {
        left_significand <<= 9U;
        right_significand <<= 9U;
        if (difference < 0) {
            if (right_exponent == 0x7FF) {
                return right_significand != 0 ? propagate_nan(left, right) : pack(sign, 0x7FF, 0);
            }
            exponent = right_exponent;
            left_significand = left_exponent != 0 ? left_significand + 0x2000000000000000ULL : left_significand << 1U;
            left_significand = shift_right_jam(left_significand, static_cast<std::uint32_t>(-difference));
        } else {
            if (left_exponent == 0x7FF) {
                return left_significand != 0 ? propagate_nan(left, right) : left;
            }
            exponent = left_exponent;
            right_significand =
                right_exponent != 0 ? right_significand + 0x2000000000000000ULL : right_significand << 1U;
            right_significand = shift_right_jam(right_significand, static_cast<std::uint32_t>(difference));
        }
        significand = 0x2000000000000000ULL + left_significand + right_significand;
        if (significand < 0x4000000000000000ULL) {
            --exponent;
            significand <<= 1U;
        }
    }
    return round_pack(sign, exponent, significand);
}

[[nodiscard]] constexpr Bits subtract_magnitudes(Bits left, Bits right, bool sign) noexcept {
    std::int32_t left_exponent = exponent_of(left);
    std::uint64_t left_significand = fraction_of(left);
    const std::int32_t right_exponent = exponent_of(right);
    std::uint64_t right_significand = fraction_of(right);
    const std::int32_t difference = left_exponent - right_exponent;
    if (difference == 0) {
        if (left_exponent == 0x7FF) {
            if ((left_significand | right_significand) != 0) {
                return propagate_nan(left, right);
            }
            return canonical_nan;
        }
        if (left_significand == right_significand) {
            return positive_zero;
        }
        if (left_exponent != 0) {
            --left_exponent;
        }
        std::uint64_t magnitude = 0;
        if (left_significand < right_significand) {
            sign = !sign;
            magnitude = right_significand - left_significand;
        } else {
            magnitude = left_significand - right_significand;
        }
        int shift = count_leading_zeros(magnitude) - 11;
        std::int32_t exponent = left_exponent - shift;
        if (exponent < 0) {
            shift = left_exponent;
            exponent = 0;
        }
        return pack(sign, exponent, magnitude << static_cast<std::uint32_t>(shift));
    }
    left_significand <<= 10U;
    right_significand <<= 10U;
    std::int32_t exponent = 0;
    std::uint64_t significand = 0;
    if (difference < 0) {
        sign = !sign;
        if (right_exponent == 0x7FF) {
            return right_significand != 0 ? propagate_nan(left, right) : pack(sign, 0x7FF, 0);
        }
        left_significand += left_exponent != 0 ? 0x4000000000000000ULL : left_significand;
        left_significand = shift_right_jam(left_significand, static_cast<std::uint32_t>(-difference));
        right_significand |= 0x4000000000000000ULL;
        exponent = right_exponent;
        significand = right_significand - left_significand;
    } else {
        if (left_exponent == 0x7FF) {
            return left_significand != 0 ? propagate_nan(left, right) : left;
        }
        right_significand += right_exponent != 0 ? 0x4000000000000000ULL : right_significand;
        right_significand = shift_right_jam(right_significand, static_cast<std::uint32_t>(difference));
        left_significand |= 0x4000000000000000ULL;
        exponent = left_exponent;
        significand = left_significand - right_significand;
    }
    return normalize_round_pack(sign, exponent - 1, significand);
}

} // namespace detail

[[nodiscard]] constexpr Bits canonicalize(Bits value) noexcept { return is_nan(value) ? canonical_nan : value; }
[[nodiscard]] constexpr Bits negate(Bits value) noexcept { return is_nan(value) ? canonical_nan : value ^ sign_mask; }
[[nodiscard]] constexpr Bits absolute(Bits value) noexcept { return is_nan(value) ? canonical_nan : value & ~sign_mask; }

[[nodiscard]] constexpr Bits add(Bits left, Bits right) noexcept {
    const bool left_sign = sign_of(left);
    return left_sign == sign_of(right) ? detail::add_magnitudes(left, right, left_sign)
                                       : detail::subtract_magnitudes(left, right, left_sign);
}

[[nodiscard]] constexpr Bits subtract(Bits left, Bits right) noexcept {
    const bool left_sign = sign_of(left);
    return left_sign == sign_of(right) ? detail::subtract_magnitudes(left, right, left_sign)
                                       : detail::add_magnitudes(left, right, left_sign);
}

[[nodiscard]] inline Bits multiply(Bits left, Bits right) noexcept {
    std::int32_t left_exponent = exponent_of(left);
    std::uint64_t left_significand = fraction_of(left);
    std::int32_t right_exponent = exponent_of(right);
    std::uint64_t right_significand = fraction_of(right);
    const bool sign = sign_of(left) != sign_of(right);
    if (left_exponent == 0x7FF) {
        if (left_significand != 0 || (right_exponent == 0x7FF && right_significand != 0)) {
            return detail::propagate_nan(left, right);
        }
        return (static_cast<std::uint64_t>(right_exponent) | right_significand) == 0 ? canonical_nan
                                                                                      : pack(sign, 0x7FF, 0);
    }
    if (right_exponent == 0x7FF) {
        if (right_significand != 0) {
            return detail::propagate_nan(left, right);
        }
        return (static_cast<std::uint64_t>(left_exponent) | left_significand) == 0 ? canonical_nan
                                                                                    : pack(sign, 0x7FF, 0);
    }
    if (left_exponent == 0) {
        if (left_significand == 0) {
            return pack(sign, 0, 0);
        }
        const detail::Normalized normalized = detail::normalize_subnormal(left_significand);
        left_exponent = normalized.exponent;
        left_significand = normalized.significand;
    }
    if (right_exponent == 0) {
        if (right_significand == 0) {
            return pack(sign, 0, 0);
        }
        const detail::Normalized normalized = detail::normalize_subnormal(right_significand);
        right_exponent = normalized.exponent;
        right_significand = normalized.significand;
    }
    std::int32_t exponent = left_exponent + right_exponent - 0x3FF;
    left_significand = (left_significand | hidden_bit) << 10U;
    right_significand = (right_significand | hidden_bit) << 11U;
    const detail::U128 product = detail::multiply_64x64(left_significand, right_significand);
    std::uint64_t significand = product.high | static_cast<std::uint64_t>(product.low != 0);
    if (significand < 0x4000000000000000ULL) {
        --exponent;
        significand <<= 1U;
    }
    return detail::round_pack(sign, exponent, significand);
}

[[nodiscard]] inline Bits divide(Bits left, Bits right) noexcept {
    std::int32_t left_exponent = exponent_of(left);
    std::uint64_t left_significand = fraction_of(left);
    std::int32_t right_exponent = exponent_of(right);
    std::uint64_t right_significand = fraction_of(right);
    const bool sign = sign_of(left) != sign_of(right);
    if (left_exponent == 0x7FF) {
        if (left_significand != 0) {
            return detail::propagate_nan(left, right);
        }
        if (right_exponent == 0x7FF) {
            return right_significand != 0 ? detail::propagate_nan(left, right) : canonical_nan;
        }
        return pack(sign, 0x7FF, 0);
    }
    if (right_exponent == 0x7FF) {
        return right_significand != 0 ? detail::propagate_nan(left, right) : pack(sign, 0, 0);
    }
    if (right_exponent == 0) {
        if (right_significand == 0) {
            return (static_cast<std::uint64_t>(left_exponent) | left_significand) == 0 ? canonical_nan
                                                                                        : pack(sign, 0x7FF, 0);
        }
        const detail::Normalized normalized = detail::normalize_subnormal(right_significand);
        right_exponent = normalized.exponent;
        right_significand = normalized.significand;
    }
    if (left_exponent == 0) {
        if (left_significand == 0) {
            return pack(sign, 0, 0);
        }
        const detail::Normalized normalized = detail::normalize_subnormal(left_significand);
        left_exponent = normalized.exponent;
        left_significand = normalized.significand;
    }
    std::int32_t exponent = left_exponent - right_exponent + 0x3FE;
    left_significand |= hidden_bit;
    right_significand |= hidden_bit;
    std::uint32_t shift = 62;
    if (left_significand < right_significand) {
        --exponent;
        shift = 63;
    }
    // Both significands lie in [2^52, 2^53); the quotient lands in [2^62, 2^63).
    bool inexact = false;
    const std::uint64_t quotient = detail::divide_shifted(left_significand, shift, right_significand, inexact);
    return detail::round_pack(sign, exponent, quotient | static_cast<std::uint64_t>(inexact));
}

// Correctly rounded square root; negative nonzero inputs give the canonical NaN.
[[nodiscard]] Bits square_root(Bits value) noexcept;

// IEEE comparisons: NaN is unordered (every predicate but "not equal" is
// false) and the two zeros are equal.
[[nodiscard]] constexpr bool equal(Bits left, Bits right) noexcept {
    if (is_nan(left) || is_nan(right)) {
        return false;
    }
    return left == right || ((left | right) & ~sign_mask) == 0;
}

[[nodiscard]] constexpr bool less(Bits left, Bits right) noexcept {
    if (is_nan(left) || is_nan(right)) {
        return false;
    }
    const bool left_sign = sign_of(left);
    if (left_sign != sign_of(right)) {
        return left_sign && ((left | right) & ~sign_mask) != 0;
    }
    return left != right && (left_sign != (left < right));
}

[[nodiscard]] constexpr bool less_equal(Bits left, Bits right) noexcept {
    if (is_nan(left) || is_nan(right)) {
        return false;
    }
    const bool left_sign = sign_of(left);
    if (left_sign != sign_of(right)) {
        return left_sign || ((left | right) & ~sign_mask) == 0;
    }
    return left == right || (left_sign != (left < right));
}

[[nodiscard]] constexpr Bits from_int64(std::int64_t value) noexcept {
    const bool sign = value < 0;
    if ((static_cast<std::uint64_t>(value) & 0x7FFFFFFFFFFFFFFFULL) == 0) {
        return sign ? 0xC3E0000000000000ULL : positive_zero;
    }
    const std::uint64_t magnitude = sign ? ~static_cast<std::uint64_t>(value) + 1U : static_cast<std::uint64_t>(value);
    return detail::normalize_round_pack(sign, 0x43C, magnitude);
}

[[nodiscard]] constexpr Bits from_uint64(std::uint64_t value) noexcept {
    if (value == 0) {
        return positive_zero;
    }
    if ((value & 0x8000000000000000ULL) != 0) {
        return detail::round_pack(false, 0x43D, detail::shift_right_jam(value, 1));
    }
    return detail::normalize_round_pack(false, 0x43C, value);
}

// Truncation toward zero. NaN and values outside [-2^63, 2^63) produce
// INT64_MIN, the x86 "integer indefinite" result.
[[nodiscard]] constexpr std::int64_t to_int64_truncate(Bits value) noexcept {
    const std::int32_t exponent = exponent_of(value);
    if (exponent < 0x3FF) {
        return 0;
    }
    if (exponent >= 0x43E) {
        return std::numeric_limits<std::int64_t>::min();
    }
    const std::uint64_t significand = fraction_of(value) | hidden_bit;
    const std::uint64_t magnitude = exponent >= 0x433 ? significand << static_cast<std::uint32_t>(exponent - 0x433)
                                                      : significand >> static_cast<std::uint32_t>(0x433 - exponent);
    return static_cast<std::int64_t>(sign_of(value) ? ~magnitude + 1U : magnitude);
}

// Conversion to any integer type: truncate to int64 as above, then reduce
// modulo 2^N into the target width (C++20 integral conversion).
template <typename Integer>
    requires(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>)
[[nodiscard]] constexpr Integer to_integer(Bits value) noexcept {
    return static_cast<Integer>(static_cast<std::uint64_t>(to_int64_truncate(value)));
}

template <typename Integer>
    requires(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>)
[[nodiscard]] constexpr Bits from_integer(Integer value) noexcept {
    if constexpr (std::is_signed_v<Integer>) {
        return from_int64(static_cast<std::int64_t>(value));
    } else {
        return from_uint64(static_cast<std::uint64_t>(value));
    }
}

enum class Rounding : std::uint8_t {
    nearest_even,
    toward_zero,
    downward,
    upward,
};

// Round to an integral binary64 value in the given direction.
[[nodiscard]] Bits round_to_integral(Bits value, Rounding rounding) noexcept;

} // namespace eawr::script::numeric::binary64
