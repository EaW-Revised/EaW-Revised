#pragma once

#include "eawr/core/result.hpp"

#include <bit>
#include <compare>
#include <cstdint>
#include <limits>
#include <string_view>

namespace eawr::sim::math {

class Fixed final {
public:
    static constexpr int fractional_bits = 24;
    static constexpr std::int64_t scale = std::int64_t{1} << fractional_bits;

    constexpr Fixed() noexcept = default;

    [[nodiscard]] static constexpr Fixed from_raw(std::int64_t raw) noexcept {
        return Fixed(raw);
    }

    [[nodiscard]] static core::Result<Fixed> from_integer(std::int64_t value);
    [[nodiscard]] static core::Result<Fixed> from_ratio(
        std::int64_t numerator, std::int64_t denominator);
    [[nodiscard]] static core::Result<Fixed> from_decimal(std::string_view text);

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }

    [[nodiscard]] std::int64_t trunc_to_integer() const noexcept;
    [[nodiscard]] std::int64_t floor_to_integer() const noexcept;
    [[nodiscard]] std::int64_t ceil_to_integer() const noexcept;
    [[nodiscard]] std::int64_t nearest_even_to_integer() const noexcept;

    [[nodiscard]] friend constexpr auto operator<=>(Fixed, Fixed) noexcept = default;

private:
    explicit constexpr Fixed(std::int64_t raw) noexcept : raw_(raw) {}
    std::int64_t raw_{0};
};

[[nodiscard]] core::Result<Fixed> add(Fixed left, Fixed right);
[[nodiscard]] core::Result<Fixed> subtract(Fixed left, Fixed right);
[[nodiscard]] core::Result<Fixed> negate(Fixed value);
[[nodiscard]] core::Result<Fixed> multiply(Fixed left, Fixed right);
[[nodiscard]] core::Result<Fixed> divide(Fixed numerator, Fixed denominator);

namespace detail {
// The general forms of try_multiply and try_divide (fixed.cpp): one 128-bit step where its
// rounded quotient fits 63 bits (#520), the 192-bit form otherwise.
[[nodiscard]] bool multiply_wide(Fixed left, Fixed right, Fixed& result) noexcept;
[[nodiscard]] bool divide_wide(Fixed numerator, Fixed denominator, Fixed& result) noexcept;

[[nodiscard]] constexpr std::uint64_t magnitude(const std::int64_t value) noexcept {
    return value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
}

// A rounded magnitude with the sign of the exact result; the magnitude is below 2^63.
[[nodiscard]] constexpr Fixed signed_raw(const std::uint64_t value, const bool negative) noexcept {
    const auto raw = static_cast<std::int64_t>(value);
    return Fixed::from_raw(negative ? -raw : raw);
}
} // namespace detail

// The same operations without a diagnostic (#503): false where the Result form fails, and
// `result` then unchanged. Hot loops use these; the value is always the Result form's.
// Multiply and divide take one 64-bit step when the exact product or shifted numerator fits
// 64 bits, and the 192-bit form otherwise; both round half to even alike (#520,
// math_contract_tests cross-checks them).
[[nodiscard]] inline bool try_add(const Fixed left, const Fixed right, Fixed& result) noexcept {
    const std::int64_t a = left.raw();
    const std::int64_t b = right.raw();
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b)
        || (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
        return false;
    }
    result = Fixed::from_raw(a + b);
    return true;
}

[[nodiscard]] inline bool try_subtract(const Fixed left, const Fixed right, Fixed& result) noexcept {
    const std::int64_t a = left.raw();
    const std::int64_t b = right.raw();
    if ((b < 0 && a > std::numeric_limits<std::int64_t>::max() + b)
        || (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b)) {
        return false;
    }
    result = Fixed::from_raw(a - b);
    return true;
}

[[nodiscard]] inline bool try_multiply(const Fixed left, const Fixed right, Fixed& result) noexcept {
    const std::uint64_t a = detail::magnitude(left.raw());
    const std::uint64_t b = detail::magnitude(right.raw());
    if (std::bit_width(a) + std::bit_width(b) > 64) return detail::multiply_wide(left, right, result);
    // The product fits 64 bits and its rounded quotient 40.
    const std::uint64_t product = a * b;
    constexpr std::uint64_t half = std::uint64_t{1} << (Fixed::fractional_bits - 1);
    std::uint64_t quotient = product >> Fixed::fractional_bits;
    const std::uint64_t remainder = product & ((std::uint64_t{1} << Fixed::fractional_bits) - 1U);
    if (remainder > half || (remainder == half && (quotient & 1U) != 0)) ++quotient;
    result = detail::signed_raw(quotient, (left.raw() < 0) != (right.raw() < 0));
    return true;
}

[[nodiscard]] inline bool try_divide(const Fixed numerator, const Fixed denominator, Fixed& result) noexcept {
    if (denominator.raw() == 0) return false;
    const std::uint64_t n = detail::magnitude(numerator.raw());
    if (n >= std::uint64_t{1} << (63 - Fixed::fractional_bits)) {
        return detail::divide_wide(numerator, denominator, result);
    }
    // The shifted numerator is below 2^63, so the rounded quotient is too.
    const std::uint64_t d = detail::magnitude(denominator.raw());
    const std::uint64_t shifted = n << Fixed::fractional_bits;
    std::uint64_t quotient = shifted / d;
    const std::uint64_t remainder = shifted % d;
    const std::uint64_t complement = d - remainder;
    if (remainder > complement || (remainder == complement && (quotient & 1U) != 0)) ++quotient;
    result = detail::signed_raw(quotient, (numerator.raw() < 0) != (denominator.raw() < 0));
    return true;
}

[[nodiscard]] core::Result<Fixed> sqrt(Fixed value);
// sqrt without a diagnostic: false for a negative value (#520).
[[nodiscard]] bool try_sqrt(Fixed value, Fixed& result) noexcept;

namespace diagnostic_codes {
inline constexpr std::string_view overflow = "EAWR-MATH-0001";
inline constexpr std::string_view division_by_zero = "EAWR-MATH-0002";
inline constexpr std::string_view negative_square_root = "EAWR-MATH-0003";
inline constexpr std::string_view malformed_decimal = "EAWR-MATH-0004";
inline constexpr std::string_view decimal_resource_limit = "EAWR-MATH-0005";
inline constexpr std::string_view zero_normalization = "EAWR-MATH-0006";
inline constexpr std::string_view undefined_angle = "EAWR-MATH-0007";
inline constexpr std::string_view non_unit_quaternion = "EAWR-MATH-0008";
} // namespace diagnostic_codes

} // namespace eawr::sim::math
