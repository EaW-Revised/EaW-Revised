#include "eawr/script/numeric/decimal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace eawr::script::numeric::decimal {
namespace {

using binary64::Bits;

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

// Unsigned multiprecision integer with little-endian 32-bit limbs. Callers
// bound every value below 32 * Capacity bits (see the size notes at each use).
template <std::size_t Capacity>
class BigUnsigned {
public:
    void set(std::uint64_t value) noexcept {
        size_ = 0;
        while (value != 0) {
            limbs_[size_++] = static_cast<std::uint32_t>(value);
            value >>= 32U;
        }
    }

    [[nodiscard]] bool is_zero() const noexcept { return size_ == 0; }

    [[nodiscard]] int bit_length() const noexcept {
        if (size_ == 0) {
            return 0;
        }
        return static_cast<int>(size_ - 1) * 32 + (32 - std::countl_zero(limbs_[size_ - 1]));
    }

    void multiply_small(std::uint32_t factor) noexcept {
        std::uint64_t carry = 0;
        for (std::size_t index = 0; index < size_; ++index) {
            const std::uint64_t product = static_cast<std::uint64_t>(limbs_[index]) * factor + carry;
            limbs_[index] = static_cast<std::uint32_t>(product);
            carry = product >> 32U;
        }
        if (carry != 0 && size_ < Capacity) {
            limbs_[size_++] = static_cast<std::uint32_t>(carry);
        }
    }

    void add_small(std::uint32_t addend) noexcept {
        std::uint64_t carry = addend;
        for (std::size_t index = 0; index < size_ && carry != 0; ++index) {
            const std::uint64_t sum = static_cast<std::uint64_t>(limbs_[index]) + carry;
            limbs_[index] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32U;
        }
        if (carry != 0 && size_ < Capacity) {
            limbs_[size_++] = static_cast<std::uint32_t>(carry);
        }
    }

    void multiply_pow10(std::int64_t exponent) noexcept {
        static constexpr std::array<std::uint32_t, 9> small_powers{
            1U, 10U, 100U, 1000U, 10000U, 100000U, 1000000U, 10000000U, 100000000U};
        while (exponent >= 9) {
            multiply_small(1000000000U);
            exponent -= 9;
        }
        if (exponent > 0) {
            multiply_small(small_powers[static_cast<std::size_t>(exponent)]);
        }
    }

    void shift_left(int bits) noexcept {
        if (size_ == 0 || bits <= 0) {
            return;
        }
        const std::size_t limb_shift = static_cast<std::size_t>(bits) / 32U;
        const std::uint32_t bit_shift = static_cast<std::uint32_t>(bits) % 32U;
        std::size_t new_size = std::min(size_ + limb_shift + 1U, Capacity);
        for (std::size_t index = new_size; index-- > 0;) {
            std::uint32_t value = 0;
            if (index >= limb_shift) {
                const std::size_t source = index - limb_shift;
                if (source < size_) {
                    value = limbs_[source] << bit_shift;
                }
                if (bit_shift != 0 && source >= 1 && source - 1 < size_) {
                    value |= limbs_[source - 1] >> (32U - bit_shift);
                }
            }
            limbs_[index] = value;
        }
        size_ = new_size;
        trim();
    }

    void shift_right_one() noexcept {
        for (std::size_t index = 0; index < size_; ++index) {
            const std::uint32_t next = index + 1 < size_ ? limbs_[index + 1] : 0U;
            limbs_[index] = (limbs_[index] >> 1U) | (next << 31U);
        }
        trim();
    }

    // Requires *this >= other.
    void subtract(const BigUnsigned& other) noexcept {
        std::uint64_t borrow = 0;
        for (std::size_t index = 0; index < size_; ++index) {
            const std::uint64_t subtrahend = (index < other.size_ ? other.limbs_[index] : 0U) + borrow;
            const std::uint64_t minuend = limbs_[index];
            borrow = minuend < subtrahend ? 1U : 0U;
            limbs_[index] = static_cast<std::uint32_t>(minuend + (borrow << 32U) - subtrahend);
        }
        trim();
    }

    [[nodiscard]] friend int compare(const BigUnsigned& left, const BigUnsigned& right) noexcept {
        if (left.size_ != right.size_) {
            return left.size_ < right.size_ ? -1 : 1;
        }
        for (std::size_t index = left.size_; index-- > 0;) {
            if (left.limbs_[index] != right.limbs_[index]) {
                return left.limbs_[index] < right.limbs_[index] ? -1 : 1;
            }
        }
        return 0;
    }

    // The 64 most significant bits (leading one at bit 63) and whether any
    // lower bit is set. Requires a nonzero value.
    [[nodiscard]] std::uint64_t top_bits(bool& sticky) const noexcept {
        const int length = bit_length();
        std::uint64_t result = 0;
        sticky = false;
        for (int bit = 0; bit < length; ++bit) {
            const int source = length - 1 - bit;
            const bool set = ((limbs_[static_cast<std::size_t>(source) / 32U] >> (static_cast<std::uint32_t>(source) % 32U)) & 1U) != 0;
            if (bit < 64) {
                result |= static_cast<std::uint64_t>(set) << (63U - static_cast<std::uint32_t>(bit));
            } else if (set) {
                sticky = true;
                break;
            }
        }
        return result;
    }

private:
    void trim() noexcept {
        while (size_ > 0 && limbs_[size_ - 1] == 0) {
            --size_;
        }
    }

    std::array<std::uint32_t, Capacity> limbs_{};
    std::size_t size_{0};
};

// Formatting magnitudes stay below 2^1200: r <= 2^1024 * 10 or m * 10^324 * 10.
using FormatBig = BigUnsigned<40>;
// Parsing magnitudes stay below 2^3712 (see parse_prefix).
using ParseBig = BigUnsigned<128>;

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

constexpr std::int64_t floor_divide(std::int64_t numerator, std::int64_t denominator) noexcept {
    const std::int64_t quotient = numerator / denominator;
    return (numerator % denominator != 0 && (numerator < 0) != (denominator < 0)) ? quotient - 1 : quotient;
}

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

// ---------------------------------------------------------------- formatting

constexpr std::size_t max_digits = 1424;

struct Digits {
    // Scratch space: only [0, count) is ever read, so it is left uninitialized.
    std::array<char, max_digits> digit;
    int count{0};    // generated digits; zero means the value rounded to zero
    int exponent{0}; // decimal exponent of digit[0]
};

enum class DigitMode : std::uint8_t {
    significant, // parameter = number of significant digits (>= 1)
    fixed,       // parameter = digits after the decimal point (>= 0)
};

// Rounds the exact digit string held in `digits` (count digits, all exact) to
// keep digits, ties to even.
void round_exact_digits(Digits& digits, int keep) noexcept {
    if (keep >= digits.count) {
        return;
    }
    if (keep < 0) {
        digits.count = 0;
        return;
    }
    const char first_dropped = digits.digit[static_cast<std::size_t>(keep)];
    bool any_after = false;
    for (int index = keep + 1; index < digits.count; ++index) {
        any_after = any_after || digits.digit[static_cast<std::size_t>(index)] != 0;
    }
    const bool previous_odd = keep > 0 && (digits.digit[static_cast<std::size_t>(keep - 1)] & 1) != 0;
    const bool round_up = first_dropped > 5 || (first_dropped == 5 && (any_after || previous_odd));
    digits.count = keep;
    if (!round_up) {
        return;
    }
    int index = keep - 1;
    while (index >= 0 && digits.digit[static_cast<std::size_t>(index)] == 9) {
        digits.digit[static_cast<std::size_t>(index)] = 0;
        --index;
    }
    if (index >= 0) {
        ++digits.digit[static_cast<std::size_t>(index)];
        return;
    }
    // All nines (or keep == 0): the value becomes 10^(exponent + 1).
    digits.digit[0] = 1;
    for (int fill = 1; fill < keep; ++fill) {
        digits.digit[static_cast<std::size_t>(fill)] = 0;
    }
    digits.count = keep == 0 ? 1 : keep;
    ++digits.exponent;
}

// Exact digits of an integral magnitude below 2^64.
void integer_digits(std::uint64_t value, Digits& digits) noexcept {
    std::array<char, 20> reversed{};
    int count = 0;
    do {
        reversed[static_cast<std::size_t>(count++)] = static_cast<char>(value % 10U);
        value /= 10U;
    } while (value != 0);
    for (int index = 0; index < count; ++index) {
        digits.digit[static_cast<std::size_t>(index)] = reversed[static_cast<std::size_t>(count - 1 - index)];
    }
    digits.count = count;
    digits.exponent = count - 1;
}

// Digits of a finite nonzero magnitude, rounded once (ties to even).
void generate_digits(Bits magnitude, DigitMode mode, int parameter, Digits& digits) noexcept {
    const std::int32_t biased = binary64::exponent_of(magnitude);
    std::uint64_t significand = binary64::fraction_of(magnitude);
    std::int32_t exponent2 = -1074;
    if (biased != 0) {
        significand |= binary64::hidden_bit;
        exponent2 = biased - 1075;
    }

    // Integral values below 2^64 have short exact expansions.
    const int trailing = std::countr_zero(significand);
    if (exponent2 >= 0 ? exponent2 + (64 - std::countl_zero(significand)) <= 64 : -exponent2 <= trailing) {
        const std::uint64_t integral = exponent2 >= 0 ? significand << static_cast<std::uint32_t>(exponent2)
                                                      : significand >> static_cast<std::uint32_t>(-exponent2);
        integer_digits(integral, digits);
        const int wanted = mode == DigitMode::significant ? parameter : digits.exponent + 1 + parameter;
        if (wanted < digits.count) {
            round_exact_digits(digits, wanted);
        }
        const int target = mode == DigitMode::significant ? parameter : digits.exponent + 1 + parameter;
        while (digits.count < target) {
            digits.digit[static_cast<std::size_t>(digits.count++)] = 0;
        }
        return;
    }

    FormatBig remainder;
    FormatBig scale;
    remainder.set(significand);
    scale.set(1);
    if (exponent2 >= 0) {
        remainder.shift_left(exponent2);
    } else {
        scale.shift_left(-exponent2);
    }
    const std::int64_t binary_log = static_cast<std::int64_t>(63 - std::countl_zero(significand)) + exponent2;
    std::int64_t decimal_exponent = floor_divide(binary_log * 78913, 262144);
    if (decimal_exponent >= 0) {
        scale.multiply_pow10(decimal_exponent);
    } else {
        remainder.multiply_pow10(-decimal_exponent);
    }
    while (compare(remainder, scale) < 0) {
        remainder.multiply_small(10);
        --decimal_exponent;
    }
    for (;;) {
        FormatBig next = scale;
        next.multiply_small(10);
        if (compare(remainder, next) < 0) {
            break;
        }
        scale = next;
        ++decimal_exponent;
    }

    digits.exponent = static_cast<int>(decimal_exponent);
    const int wanted = mode == DigitMode::significant ? parameter : digits.exponent + 1 + parameter;
    if (wanted <= 0) {
        digits.count = 0;
        if (wanted == 0) {
            // The rounding digit is the leading one: compare value to half a unit.
            FormatBig half = scale;
            half.multiply_small(5);
            if (compare(remainder, half) > 0) {
                digits.digit[0] = 1;
                digits.count = 1;
                ++digits.exponent;
            }
        }
        return;
    }
    int produced = 0;
    while (produced < wanted) {
        char digit = 0;
        while (compare(remainder, scale) >= 0) {
            remainder.subtract(scale);
            ++digit;
        }
        digits.digit[static_cast<std::size_t>(produced++)] = digit;
        if (remainder.is_zero()) {
            break;
        }
        if (produced < wanted) {
            remainder.multiply_small(10);
        }
    }
    const bool exhausted = remainder.is_zero();
    while (produced < wanted) {
        digits.digit[static_cast<std::size_t>(produced++)] = 0;
    }
    digits.count = wanted;
    if (exhausted) {
        return;
    }
    remainder.multiply_small(2);
    const int comparison = compare(remainder, scale);
    const bool round_up =
        comparison > 0 || (comparison == 0 && (digits.digit[static_cast<std::size_t>(wanted - 1)] & 1) != 0);
    if (!round_up) {
        return;
    }
    int index = wanted - 1;
    while (index >= 0 && digits.digit[static_cast<std::size_t>(index)] == 9) {
        digits.digit[static_cast<std::size_t>(index)] = 0;
        --index;
    }
    if (index >= 0) {
        ++digits.digit[static_cast<std::size_t>(index)];
        return;
    }
    digits.digit[0] = 1;
    ++digits.exponent;
    if (mode == DigitMode::fixed) {
        digits.digit[static_cast<std::size_t>(wanted)] = 0;
        digits.count = wanted + 1;
    }
}

class Output {
public:
    void put(char character) noexcept {
        if (length_ < buffer_.size()) {
            buffer_[length_] = character;
        }
        ++length_;
    }
    void put(const char* text) noexcept {
        while (*text != '\0') {
            put(*text++);
        }
    }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }
    [[nodiscard]] const char* data() const noexcept { return buffer_.data(); }
    void truncate(std::size_t length) noexcept { length_ = length; }
    char& at(std::size_t index) noexcept { return buffer_[index]; }

private:
    // Scratch space: only [0, length_) is ever read.
    std::array<char, format_buffer_size> buffer_;
    std::size_t length_{0};
};

char digit_at(const Digits& digits, int position) noexcept {
    // position: decimal exponent of the wanted digit
    const int index = digits.exponent - position;
    if (digits.count == 0 || index < 0 || index >= digits.count) {
        return '0';
    }
    return static_cast<char>('0' + digits.digit[static_cast<std::size_t>(index)]);
}

void put_exponent(Output& output, int exponent, bool upper) noexcept {
    output.put(upper ? 'E' : 'e');
    output.put(exponent < 0 ? '-' : '+');
    const int magnitude = exponent < 0 ? -exponent : exponent;
    if (magnitude >= 100) {
        output.put(static_cast<char>('0' + magnitude / 100));
    }
    output.put(static_cast<char>('0' + (magnitude / 10) % 10));
    output.put(static_cast<char>('0' + magnitude % 10));
}

void put_e_style(Output& output, const Digits& digits, int precision, bool alternate, bool upper) noexcept {
    const int exponent = digits.count == 0 ? 0 : digits.exponent;
    output.put(digit_at(digits, exponent));
    if (precision > 0 || alternate) {
        output.put('.');
    }
    for (int index = 1; index <= precision; ++index) {
        output.put(digit_at(digits, exponent - index));
    }
    put_exponent(output, exponent, upper);
}

void put_f_style(Output& output, const Digits& digits, int precision, bool alternate) noexcept {
    const int top = digits.count == 0 ? 0 : std::max(digits.exponent, 0);
    for (int position = top; position >= 0; --position) {
        output.put(digit_at(digits, position));
    }
    if (precision > 0 || alternate) {
        output.put('.');
    }
    for (int index = 1; index <= precision; ++index) {
        output.put(digit_at(digits, -index));
    }
}

// Removes trailing fractional zeros and a then-bare decimal point.
void strip_fraction(Output& output, std::size_t start) noexcept {
    std::size_t length = output.length();
    std::size_t point = length;
    for (std::size_t index = start; index < length; ++index) {
        const char character = output.at(index);
        if (character == '.') {
            point = index;
        } else if (character == 'e' || character == 'E') {
            break;
        }
    }
    if (point == length) {
        return;
    }
    std::size_t exponent_start = length;
    for (std::size_t index = point; index < length; ++index) {
        if (output.at(index) == 'e' || output.at(index) == 'E') {
            exponent_start = index;
            break;
        }
    }
    std::size_t end = exponent_start;
    while (end > point + 1 && output.at(end - 1) == '0') {
        --end;
    }
    if (end == point + 1) {
        end = point;
    }
    if (end == exponent_start) {
        return;
    }
    std::size_t write = end;
    for (std::size_t index = exponent_start; index < length; ++index) {
        output.at(write++) = output.at(index);
    }
    output.truncate(write);
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

std::size_t format_to(char* buffer, std::size_t capacity, Bits value, const FormatSpec& spec) noexcept {
    Output output;
    const bool upper = spec.conversion == 'E' || spec.conversion == 'G';
    const char kind = static_cast<char>(spec.conversion == 'E' ? 'e' : spec.conversion == 'G' ? 'g' : spec.conversion);
    int precision = spec.precision < 0 ? 6 : std::min(spec.precision, max_precision);
    const int width = std::clamp(spec.width, 0, static_cast<int>(max_format_width));

    const bool negative = binary64::sign_of(value);
    if (negative) {
        output.put('-');
    } else if (spec.plus) {
        output.put('+');
    } else if (spec.space) {
        output.put(' ');
    }
    const std::size_t body_start = output.length();
    const bool finite = binary64::is_finite(value);
    if (!finite) {
        // UCRT spellings: inf, nan, nan(ind) for the x64 indefinite (the
        // canonical NaN), nan(snan) for signaling payloads.
        const char* word = "inf";
        if (binary64::is_nan(value)) {
            word = binary64::is_signaling_nan(value)                               ? "nan(snan)"
                   : (value & ~binary64::sign_mask) == 0x7FF8000000000000ULL && negative ? "nan(ind)"
                                                                                          : "nan";
        }
        for (; *word != '\0'; ++word) {
            output.put(upper ? static_cast<char>(*word >= 'a' && *word <= 'z' ? *word - ('a' - 'A') : *word) : *word);
        }
    } else {
        const Bits magnitude = binary64::absolute(value);
        Digits digits;
        if (kind == 'f') {
            if (!binary64::is_zero(magnitude)) {
                generate_digits(magnitude, DigitMode::fixed, precision, digits);
            }
            put_f_style(output, digits, precision, spec.alternate);
        } else if (kind == 'e') {
            if (!binary64::is_zero(magnitude)) {
                generate_digits(magnitude, DigitMode::significant, precision + 1, digits);
            }
            put_e_style(output, digits, precision, spec.alternate, upper);
        } else {
            if (precision == 0) {
                precision = 1;
            }
            int exponent = 0;
            if (!binary64::is_zero(magnitude)) {
                generate_digits(magnitude, DigitMode::significant, precision, digits);
                exponent = digits.exponent;
            }
            if (exponent < -4 || exponent >= precision) {
                put_e_style(output, digits, precision - 1, spec.alternate, upper);
            } else {
                put_f_style(output, digits, precision - 1 - exponent, spec.alternate);
            }
            if (!spec.alternate) {
                strip_fraction(output, body_start);
            }
        }
    }

    // Precision and width are clamped, so the body always fits the buffer.
    std::size_t length = std::min(output.length(), format_buffer_size);
    const auto padded = static_cast<std::size_t>(width);
    const char* source = output.data();
    std::size_t written = 0;
    const auto emit = [&](char character) noexcept {
        if (capacity != 0 && written + 1 < capacity) {
            buffer[written] = character;
        }
        ++written;
    };
    if (length >= padded) {
        for (std::size_t index = 0; index < length; ++index) {
            emit(source[index]);
        }
    } else if (spec.left_align) {
        for (std::size_t index = 0; index < length; ++index) {
            emit(source[index]);
        }
        for (std::size_t index = length; index < padded; ++index) {
            emit(' ');
        }
    } else if (spec.zero_pad && finite) {
        for (std::size_t index = 0; index < body_start; ++index) {
            emit(source[index]);
        }
        for (std::size_t index = length; index < padded; ++index) {
            emit('0');
        }
        for (std::size_t index = body_start; index < length; ++index) {
            emit(source[index]);
        }
    } else {
        for (std::size_t index = length; index < padded; ++index) {
            emit(' ');
        }
        for (std::size_t index = 0; index < length; ++index) {
            emit(source[index]);
        }
    }
    if (capacity != 0) {
        buffer[std::min(written, capacity - 1)] = '\0';
    }
    length = written;
    return length;
}

std::string format(Bits value, const FormatSpec& spec) {
    std::array<char, format_buffer_size> buffer{};
    const std::size_t length = format_to(buffer.data(), buffer.size(), value, spec);
    return std::string(buffer.data(), std::min(length, buffer.size() - 1));
}

std::size_t format_lua_number(char* buffer, std::size_t capacity, Bits value) noexcept {
    FormatSpec spec;
    spec.conversion = 'g';
    spec.precision = 14;
    return format_to(buffer, capacity, value, spec);
}

std::string format_lua_number(Bits value) {
    std::array<char, lua_number_buffer_size> buffer{};
    const std::size_t length = format_lua_number(buffer.data(), buffer.size(), value);
    return std::string(buffer.data(), std::min(length, buffer.size() - 1));
}

std::string format_shortest(Bits value) {
    FormatSpec spec;
    spec.conversion = 'g';
    if (!binary64::is_finite(value)) {
        return format(value, spec);
    }
    for (int precision = 1; precision < 17; ++precision) {
        spec.precision = precision;
        std::string text = format(value, spec);
        const std::optional<Bits> parsed = parse_lua_number(text);
        if (parsed && *parsed == value) {
            return text;
        }
    }
    spec.precision = 17;
    return format(value, spec);
}

} // namespace eawr::script::numeric::decimal
