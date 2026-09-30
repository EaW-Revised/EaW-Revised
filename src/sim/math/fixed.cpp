#include "eawr/sim/math/fixed.hpp"

#include "wide.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace eawr::sim::math {
namespace {

core::Diagnostic error(std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.message = std::move(message);
    return diagnostic;
}

core::Result<Fixed> overflow() {
    return core::Result<Fixed>::failure(
        error(diagnostic_codes::overflow, "fixed-point result is outside the Q24 raw domain"));
}

bool decimal_round_up(std::string_view product, std::int64_t decimal_position,
                      std::uint64_t quotient) {
    if (decimal_position < 0) {
        return false;
    }
    const auto position = static_cast<std::size_t>(decimal_position);
    if (position >= product.size()) {
        return false;
    }
    const char first = product[position];
    if (first > '5') {
        return true;
    }
    if (first < '5') {
        return false;
    }
    const bool trailing_nonzero = std::any_of(
        product.begin() + static_cast<std::ptrdiff_t>(position + 1), product.end(),
        [](char digit) { return digit != '0'; });
    return trailing_nonzero || (quotient & 1U) != 0;
}

std::string multiply_decimal_by_scale(std::string digits) {
    std::uint64_t carry = 0;
    for (auto iterator = digits.rbegin(); iterator != digits.rend(); ++iterator) {
        const std::uint64_t value = static_cast<std::uint64_t>(*iterator - '0')
            * static_cast<std::uint64_t>(Fixed::scale) + carry;
        *iterator = static_cast<char>('0' + (value % 10U));
        carry = value / 10U;
    }
    if (carry != 0) {
        digits.insert(0, std::to_string(carry));
    }
    const auto first = digits.find_first_not_of('0');
    return first == std::string::npos ? std::string("0") : digits.substr(first);
}

} // namespace

core::Result<Fixed> Fixed::from_integer(std::int64_t value) {
    detail::SignedWide wide = detail::SignedWide::scaled(value, fractional_bits);
    std::int64_t raw_value = 0;
    if (!detail::magnitude_to_raw(wide.magnitude, wide.negative, raw_value)) {
        return overflow();
    }
    return core::Result<Fixed>::success(from_raw(raw_value));
}

core::Result<Fixed> Fixed::from_ratio(std::int64_t numerator, std::int64_t denominator) {
    if (denominator == 0) {
        return core::Result<Fixed>::failure(error(
            diagnostic_codes::division_by_zero, "fixed-point rational denominator is zero"));
    }
    detail::SignedWide wide{
        detail::shift_left_u64(detail::unsigned_magnitude(numerator), fractional_bits),
        (numerator < 0) != (denominator < 0),
    };
    std::int64_t raw_value = 0;
    if (!detail::rounded_divide_to_raw(
            wide, detail::from_u64(detail::unsigned_magnitude(denominator)), raw_value)) {
        return overflow();
    }
    return core::Result<Fixed>::success(from_raw(raw_value));
}

core::Result<Fixed> Fixed::from_decimal(std::string_view text) {
    if (text.size() > 4096) {
        return core::Result<Fixed>::failure(error(
            diagnostic_codes::decimal_resource_limit,
            "decimal input exceeds the 4096-character resource limit"));
    }
    if (text.empty()) {
        return core::Result<Fixed>::failure(
            error(diagnostic_codes::malformed_decimal, "decimal input is empty"));
    }
    std::size_t position = 0;
    bool negative = false;
    if (text[position] == '+' || text[position] == '-') {
        negative = text[position] == '-';
        if (++position == text.size()) {
            return core::Result<Fixed>::failure(
                error(diagnostic_codes::malformed_decimal, "decimal sign has no digits"));
        }
    }

    std::string digits;
    digits.reserve(text.size());
    std::int64_t fractional_digits = 0;
    bool seen_dot = false;
    bool seen_digit = false;
    while (position < text.size() && text[position] != 'e' && text[position] != 'E') {
        const char value = text[position++];
        if (value == '.') {
            if (seen_dot) {
                return core::Result<Fixed>::failure(error(
                    diagnostic_codes::malformed_decimal, "decimal input contains two points"));
            }
            seen_dot = true;
            continue;
        }
        if (value < '0' || value > '9') {
            return core::Result<Fixed>::failure(error(
                diagnostic_codes::malformed_decimal, "decimal input contains a non-ASCII digit"));
        }
        seen_digit = true;
        digits.push_back(value);
        if (seen_dot) {
            ++fractional_digits;
        }
    }
    if (!seen_digit) {
        return core::Result<Fixed>::failure(
            error(diagnostic_codes::malformed_decimal, "decimal input has no digits"));
    }

    std::int64_t exponent = 0;
    if (position < text.size()) {
        ++position;
        bool exponent_negative = false;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            exponent_negative = text[position] == '-';
            ++position;
        }
        if (position == text.size()) {
            return core::Result<Fixed>::failure(
                error(diagnostic_codes::malformed_decimal, "decimal exponent has no digits"));
        }
        while (position < text.size()) {
            const char value = text[position++];
            if (value < '0' || value > '9') {
                return core::Result<Fixed>::failure(error(
                    diagnostic_codes::malformed_decimal, "decimal exponent is malformed"));
            }
            if (exponent < 100000) {
                exponent = std::min<std::int64_t>(100000, exponent * 10 + (value - '0'));
            }
        }
        if (exponent_negative) {
            exponent = -exponent;
        }
    }

    const auto first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) {
        return core::Result<Fixed>::success(Fixed{});
    }
    digits.erase(0, first_nonzero);
    const std::string product = multiply_decimal_by_scale(std::move(digits));
    const std::int64_t decimal_position = static_cast<std::int64_t>(product.size())
        + exponent - fractional_digits;

    if (decimal_position > 19) {
        return overflow();
    }
    std::uint64_t quotient = 0;
    if (decimal_position > 0) {
        const auto count = static_cast<std::size_t>(decimal_position);
        for (std::size_t index = 0; index < count; ++index) {
            const unsigned digit = index < product.size()
                ? static_cast<unsigned>(product[index] - '0') : 0U;
            if (quotient > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                return overflow();
            }
            quotient = quotient * 10U + digit;
        }
    }
    if (decimal_round_up(product, decimal_position, quotient)) {
        if (quotient == std::numeric_limits<std::uint64_t>::max()) {
            return overflow();
        }
        ++quotient;
    }
    std::int64_t raw_value = 0;
    if (!detail::magnitude_to_raw(detail::from_u64(quotient), negative, raw_value)) {
        return overflow();
    }
    return core::Result<Fixed>::success(from_raw(raw_value));
}

std::int64_t Fixed::trunc_to_integer() const noexcept {
    return raw_ / scale;
}

std::int64_t Fixed::floor_to_integer() const noexcept {
    const std::int64_t quotient = raw_ / scale;
    return raw_ < 0 && raw_ % scale != 0 ? quotient - 1 : quotient;
}

std::int64_t Fixed::ceil_to_integer() const noexcept {
    const std::int64_t quotient = raw_ / scale;
    return raw_ > 0 && raw_ % scale != 0 ? quotient + 1 : quotient;
}

std::int64_t Fixed::nearest_even_to_integer() const noexcept {
    const std::uint64_t magnitude = detail::unsigned_magnitude(raw_);
    std::uint64_t quotient = magnitude / static_cast<std::uint64_t>(scale);
    const std::uint64_t remainder = magnitude % static_cast<std::uint64_t>(scale);
    const std::uint64_t half = static_cast<std::uint64_t>(scale / 2);
    if (remainder > half || (remainder == half && (quotient & 1U) != 0)) {
        ++quotient;
    }
    const std::int64_t signed_value = static_cast<std::int64_t>(quotient);
    return raw_ < 0 ? -signed_value : signed_value;
}

namespace detail {

namespace {

// A rounded magnitude below or at 2^63 with the sign of the exact result, as magnitude_to_raw
// takes it: 2^63 only when negative.
[[nodiscard]] bool signed_result(const std::uint64_t magnitude, const bool negative, Fixed& result) noexcept {
    constexpr std::uint64_t min_magnitude = std::uint64_t{1} << 63U;
    if (magnitude < min_magnitude) {
        result = signed_raw(magnitude, negative);
        return true;
    }
    if (!negative) return false;
    result = Fixed::from_raw(std::numeric_limits<std::int64_t>::min());
    return true;
}

} // namespace

// The 192-bit forms take a 128-bit step first when its quotient stays below 2^63 (#520): the
// same exact quotient, remainder and rounding, so the same bits (math_contract_tests).
bool multiply_wide(Fixed left, Fixed right, Fixed& result) noexcept {
    constexpr unsigned bits = Fixed::fractional_bits;
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    multiply_64(magnitude(left.raw()), magnitude(right.raw()), high, low);
    if (high < (std::uint64_t{1} << (bits - 1U))) {
        std::uint64_t quotient = (high << (64U - bits)) | (low >> bits);
        const std::uint64_t remainder = low & ((std::uint64_t{1} << bits) - 1U);
        constexpr std::uint64_t half = std::uint64_t{1} << (bits - 1U);
        if (remainder > half || (remainder == half && (quotient & 1U) != 0)) ++quotient;
        return signed_result(quotient, (left.raw() < 0) != (right.raw() < 0), result);
    }
    std::int64_t raw = 0;
    if (!rounded_shift_to_raw(SignedWide::product(left.raw(), right.raw()), Fixed::fractional_bits, raw)) {
        return false;
    }
    result = Fixed::from_raw(raw);
    return true;
}

bool divide_wide(Fixed numerator, Fixed denominator, Fixed& result) noexcept {
    if (denominator.raw() == 0) {
        return false;
    }
    constexpr unsigned bits = Fixed::fractional_bits;
    const std::uint64_t n = magnitude(numerator.raw());
    const std::uint64_t d = magnitude(denominator.raw());
    const std::uint64_t high = n >> (64U - bits);
    if (high < d) {
        std::uint64_t remainder = 0;
        std::uint64_t quotient = divide_128(high, n << bits, d, remainder);
        if (quotient < (std::uint64_t{1} << 63U)) {
            const std::uint64_t complement = d - remainder;
            if (remainder > complement || (remainder == complement && (quotient & 1U) != 0)) ++quotient;
            return signed_result(quotient, (numerator.raw() < 0) != (denominator.raw() < 0), result);
        }
    }
    SignedWide wide{
        shift_left_u64(unsigned_magnitude(numerator.raw()), Fixed::fractional_bits),
        (numerator.raw() < 0) != (denominator.raw() < 0),
    };
    std::int64_t raw = 0;
    if (!rounded_divide_to_raw(wide, from_u64(unsigned_magnitude(denominator.raw())), raw)) {
        return false;
    }
    result = Fixed::from_raw(raw);
    return true;
}

} // namespace detail

core::Result<Fixed> add(Fixed left, Fixed right) {
    Fixed result;
    if (!try_add(left, right, result)) {
        return overflow();
    }
    return core::Result<Fixed>::success(result);
}

core::Result<Fixed> subtract(Fixed left, Fixed right) {
    Fixed result;
    if (!try_subtract(left, right, result)) {
        return overflow();
    }
    return core::Result<Fixed>::success(result);
}

core::Result<Fixed> negate(Fixed value) {
    if (value.raw() == std::numeric_limits<std::int64_t>::min()) {
        return overflow();
    }
    return core::Result<Fixed>::success(Fixed::from_raw(-value.raw()));
}

core::Result<Fixed> multiply(Fixed left, Fixed right) {
    Fixed result;
    if (!try_multiply(left, right, result)) {
        return overflow();
    }
    return core::Result<Fixed>::success(result);
}

core::Result<Fixed> divide(Fixed numerator, Fixed denominator) {
    if (denominator.raw() == 0) {
        return core::Result<Fixed>::failure(
            error(diagnostic_codes::division_by_zero, "fixed-point denominator is zero"));
    }
    Fixed result;
    if (!try_divide(numerator, denominator, result)) {
        return overflow();
    }
    return core::Result<Fixed>::success(result);
}

// The radicand value * 2^24 is below 2^87: its floor root and the rounding to the nearest
// (up when the radicand exceeds root^2 + root, as 4 radicand > (2 root + 1)^2) in 128 bits (#520).
bool try_sqrt(Fixed value, Fixed& result) noexcept {
    if (value.raw() < 0) {
        return false;
    }
    const auto magnitude = static_cast<std::uint64_t>(value.raw());
    const std::uint64_t high = magnitude >> (64 - Fixed::fractional_bits);
    const std::uint64_t low = magnitude << Fixed::fractional_bits;
    std::uint64_t root = detail::floor_sqrt_128(high, low);
    if (detail::sqrt_rounds_up(high, low, root)) {
        ++root;
    }
    result = Fixed::from_raw(static_cast<std::int64_t>(root));
    return true;
}

core::Result<Fixed> sqrt(Fixed value) {
    Fixed result;
    if (!try_sqrt(value, result)) {
        return core::Result<Fixed>::failure(error(
            diagnostic_codes::negative_square_root,
            "square root is undefined for a negative fixed-point value"));
    }
    return core::Result<Fixed>::success(result);
}

} // namespace eawr::sim::math
