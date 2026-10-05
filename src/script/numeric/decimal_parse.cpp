#include "decimal_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace eawr::script::numeric::decimal {
namespace {

using namespace internal;

constexpr bool is_ascii_space(char character) noexcept {
    return character == ' ' || (character >= '\t' && character <= '\r');
}

constexpr bool is_ascii_digit(char character) noexcept { return character >= '0' && character <= '9'; }

constexpr int hex_digit_value(char character) noexcept {
    if (is_ascii_digit(character)) {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

constexpr char ascii_lower(char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
}

// Case-insensitive match of `word` at `position`.
bool matches_word(std::string_view text, std::size_t position, std::string_view word) noexcept {
    if (text.size() - position < word.size()) {
        return false;
    }
    for (std::size_t index = 0; index < word.size(); ++index) {
        if (ascii_lower(text[position + index]) != word[index]) {
            return false;
        }
    }
    return true;
}

// C99 hexadecimal floating constant after "0x": hexdigits [. hexdigits] [p [+-] digits],
// correctly rounded. Returns the end position, or `start` when no digit follows.
std::size_t parse_hex(std::string_view text, std::size_t start, bool negative, Bits& value) noexcept {
    const std::size_t size = text.size();
    std::size_t position = start;
    std::uint64_t mantissa = 0;
    int kept = 0;
    bool sticky = false;
    std::int64_t exponent2 = 0;
    bool any_digit = false;
    // Fifteen significant hex digits (60 bits) leave room for a sticky bit.
    const auto take = [&](int digit, bool fractional) noexcept {
        any_digit = true;
        if (kept == 0 && digit == 0) {
            if (fractional) {
                exponent2 -= 4;
            }
            return;
        }
        if (kept < 15) {
            mantissa = (mantissa << 4U) | static_cast<std::uint64_t>(digit);
            ++kept;
            if (fractional) {
                exponent2 -= 4;
            }
        } else {
            sticky = sticky || digit != 0;
            if (!fractional) {
                exponent2 += 4;
            }
        }
    };
    while (position < size && hex_digit_value(text[position]) >= 0) {
        take(hex_digit_value(text[position++]), false);
    }
    if (position < size && text[position] == '.') {
        std::size_t cursor = position + 1;
        while (cursor < size && hex_digit_value(text[cursor]) >= 0) {
            take(hex_digit_value(text[cursor++]), true);
        }
        if (any_digit) {
            position = cursor;
        }
    }
    if (!any_digit) {
        return start;
    }
    if (position < size && (text[position] == 'p' || text[position] == 'P')) {
        std::size_t cursor = position + 1;
        bool exponent_negative = false;
        if (cursor < size && (text[cursor] == '+' || text[cursor] == '-')) {
            exponent_negative = text[cursor] == '-';
            ++cursor;
        }
        if (cursor < size && is_ascii_digit(text[cursor])) {
            std::int64_t written = 0;
            while (cursor < size && is_ascii_digit(text[cursor])) {
                if (written < 100000000) {
                    written = written * 10 + (text[cursor] - '0');
                }
                ++cursor;
            }
            exponent2 += exponent_negative ? -written : written;
            position = cursor;
        }
    }
    if (mantissa == 0) {
        value = negative ? binary64::negative_zero : binary64::positive_zero;
        return position;
    }
    // value = (mantissa * 2 + sticky) * 2^(exponent2 - 1)
    const std::int64_t clamped = std::clamp<std::int64_t>(exponent2 - 1, -4000, 4000);
    value = binary64::detail::normalize_round_pack(negative, static_cast<std::int32_t>(0x43C + clamped),
                                                   (mantissa << 1U) | static_cast<std::uint64_t>(sticky));
    return position;
}

constexpr std::array<Bits, 23> exact_powers_of_ten{
    0x3ff0000000000000ULL, 0x4024000000000000ULL, 0x4059000000000000ULL, 0x408f400000000000ULL,
    0x40c3880000000000ULL, 0x40f86a0000000000ULL, 0x412e848000000000ULL, 0x416312d000000000ULL,
    0x4197d78400000000ULL, 0x41cdcd6500000000ULL, 0x4202a05f20000000ULL, 0x42374876e8000000ULL,
    0x426d1a94a2000000ULL, 0x42a2309ce5400000ULL, 0x42d6bcc41e900000ULL, 0x430c6bf526340000ULL,
    0x4341c37937e08000ULL, 0x4376345785d8a000ULL, 0x43abc16d674ec800ULL, 0x43e158e460913d00ULL,
    0x4415af1d78b58c40ULL, 0x444b1ae4d6e2ef50ULL, 0x4480f0cf064dd592ULL};

// Significant digits kept before the remaining ones collapse into a sticky
// digit. Any binary64 rounding boundary has at most 767 significant digits.
constexpr int max_parse_digits = 780;

Bits round_big_integer(const ParseBig& value, bool negative) noexcept {
    bool sticky = false;
    const std::uint64_t top = value.top_bits(sticky);
    const int length = value.bit_length();
    const std::uint64_t significand = (top >> 1U) | (top & 1U) | static_cast<std::uint64_t>(sticky);
    return binary64::detail::round_pack(negative, 0x43C + length - 63, significand);
}

// floor(numerator / denominator) for a quotient below 2^64; numerator keeps
// the remainder.
std::uint64_t divide_to_64_bits(ParseBig& numerator, ParseBig denominator, bool& inexact) noexcept {
    denominator.shift_left(63);
    std::uint64_t quotient = 0;
    for (int bit = 63; bit >= 0; --bit) {
        if (compare(numerator, denominator) >= 0) {
            numerator.subtract(denominator);
            quotient |= std::uint64_t{1} << static_cast<std::uint32_t>(bit);
        }
        denominator.shift_right_one();
    }
    inexact = !numerator.is_zero();
    return quotient;
}

} // namespace

ParseResult parse_prefix(std::string_view text) noexcept {
    const std::size_t size = text.size();
    std::size_t position = 0;
    while (position < size && is_ascii_space(text[position])) {
        ++position;
    }
    bool negative = false;
    if (position < size && (text[position] == '+' || text[position] == '-')) {
        negative = text[position] == '-';
        ++position;
    }
    if (matches_word(text, position, "inf")) {
        position += matches_word(text, position, "infinity") ? 8U : 3U;
        return {negative ? binary64::negative_infinity : binary64::positive_infinity, position};
    }
    if (matches_word(text, position, "nan")) {
        position += 3;
        if (position < size && text[position] == '(') {
            std::size_t cursor = position + 1;
            while (cursor < size && (is_ascii_digit(text[cursor]) || text[cursor] == '_' ||
                                     (ascii_lower(text[cursor]) >= 'a' && ascii_lower(text[cursor]) <= 'z'))) {
                ++cursor;
            }
            if (cursor < size && text[cursor] == ')') {
                position = cursor + 1;
            }
        }
        return {binary64::canonical_nan, position};
    }
    if (position + 1 < size && text[position] == '0' && (text[position + 1] == 'x' || text[position + 1] == 'X')) {
        Bits value = 0;
        const std::size_t end = parse_hex(text, position + 2, negative, value);
        if (end != position + 2) {
            return {value, end};
        }
    }

    std::array<std::uint8_t, max_parse_digits + 1> digits{};
    int count = 0;
    bool sticky = false;
    std::int64_t exponent10 = 0;
    bool any_digit = false;
    while (position < size && is_ascii_digit(text[position])) {
        any_digit = true;
        const auto digit = static_cast<std::uint8_t>(text[position] - '0');
        if (count == 0 && digit == 0) {
            // leading zero
        } else if (count < max_parse_digits) {
            digits[static_cast<std::size_t>(count++)] = digit;
        } else {
            sticky = sticky || digit != 0;
            ++exponent10;
        }
        ++position;
    }
    if (position < size && text[position] == '.') {
        ++position;
        while (position < size && is_ascii_digit(text[position])) {
            any_digit = true;
            const auto digit = static_cast<std::uint8_t>(text[position] - '0');
            if (count == 0 && digit == 0) {
                --exponent10;
            } else if (count < max_parse_digits) {
                digits[static_cast<std::size_t>(count++)] = digit;
                --exponent10;
            } else {
                sticky = sticky || digit != 0;
            }
            ++position;
        }
    }
    if (!any_digit) {
        return {};
    }
    if (position < size && (text[position] == 'e' || text[position] == 'E')) {
        std::size_t cursor = position + 1;
        bool exponent_negative = false;
        if (cursor < size && (text[cursor] == '+' || text[cursor] == '-')) {
            exponent_negative = text[cursor] == '-';
            ++cursor;
        }
        if (cursor < size && is_ascii_digit(text[cursor])) {
            std::int64_t written = 0;
            while (cursor < size && is_ascii_digit(text[cursor])) {
                if (written < 100000000) {
                    written = written * 10 + (text[cursor] - '0');
                }
                ++cursor;
            }
            exponent10 += exponent_negative ? -written : written;
            position = cursor;
        }
    }

    ParseResult result;
    result.consumed = position;
    if (count == 0) {
        result.value = negative ? binary64::negative_zero : binary64::positive_zero;
        return result;
    }
    if (sticky) {
        digits[static_cast<std::size_t>(count++)] = 1;
        --exponent10;
    }
    // value = D * 10^exponent10 with D of `count` digits, D's first digit nonzero.
    const std::int64_t leading_exponent = count + exponent10 - 1;
    if (leading_exponent > 309) {
        result.value = negative ? binary64::negative_infinity : binary64::positive_infinity;
        return result;
    }
    if (leading_exponent < -325) {
        result.value = negative ? binary64::negative_zero : binary64::positive_zero;
        return result;
    }
    if (count <= 19) {
        std::uint64_t small = 0;
        for (int index = 0; index < count; ++index) {
            small = small * 10U + digits[static_cast<std::size_t>(index)];
        }
        if (small <= (std::uint64_t{1} << 53U) && exponent10 >= -22 && exponent10 <= 22) {
            // Clinger's fast path: both operands are exact, one rounding.
            const Bits exact = binary64::from_uint64(small);
            const Bits power = exact_powers_of_ten[static_cast<std::size_t>(exponent10 < 0 ? -exponent10 : exponent10)];
            const Bits magnitude = exponent10 < 0 ? binary64::divide(exact, power) : binary64::multiply(exact, power);
            result.value = negative ? binary64::negate(magnitude) : magnitude;
            return result;
        }
    }

    // Size bounds: D < 10^781 (2595 bits). For exponent10 >= 0 the value is
    // below 10^310. Otherwise 10^-exponent10 <= 10^(781 + 325) (3675 bits) and
    // both operands are aligned to at most 3675 + 64 bits.
    ParseBig numerator;
    numerator.set(0);
    int index = 0;
    while (index < count) {
        std::uint32_t chunk = 0;
        std::uint32_t chunk_scale = 1;
        for (int piece = 0; piece < 9 && index < count; ++piece, ++index) {
            chunk = chunk * 10U + digits[static_cast<std::size_t>(index)];
            chunk_scale *= 10U;
        }
        if (numerator.is_zero()) {
            numerator.set(chunk);
        } else {
            numerator.multiply_small(chunk_scale);
            numerator.add_small(chunk);
        }
    }
    if (exponent10 >= 0) {
        numerator.multiply_pow10(exponent10);
        result.value = round_big_integer(numerator, negative);
        return result;
    }
    ParseBig denominator;
    denominator.set(1);
    denominator.multiply_pow10(-exponent10);
    int shift = 63 - (numerator.bit_length() - denominator.bit_length());
    if (shift > 0) {
        numerator.shift_left(shift);
    } else if (shift < 0) {
        denominator.shift_left(-shift);
    }
    bool inexact = false;
    std::uint64_t quotient = divide_to_64_bits(numerator, denominator, inexact);
    if (quotient >= 0x8000000000000000ULL) {
        inexact = inexact || (quotient & 1U) != 0;
        quotient >>= 1U;
        --shift;
    }
    result.value = binary64::detail::round_pack(negative, 0x43C - shift, quotient | static_cast<std::uint64_t>(inexact));
    return result;
}

std::optional<Bits> parse_lua_number(std::string_view text) noexcept {
    const ParseResult parsed = parse_prefix(text);
    if (parsed.consumed == 0) {
        return std::nullopt;
    }
    std::size_t position = parsed.consumed;
    while (position < text.size() && is_ascii_space(text[position])) {
        ++position;
    }
    if (position != text.size()) {
        return std::nullopt;
    }
    return parsed.value;
}

} // namespace eawr::script::numeric::decimal
