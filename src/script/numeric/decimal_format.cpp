#include "decimal_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace eawr::script::numeric::decimal {
namespace {

using namespace internal;

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
