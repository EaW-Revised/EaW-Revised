#pragma once

// Operand streams shared with tools/generate_lua_numeric_vectors.py. Both
// sides must draw from SplitMix64 in exactly the same order.

#include "eawr/script/numeric/binary64.hpp"
#include "eawr/script/numeric/decimal.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::script::numeric::test {

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

private:
    std::uint64_t state_;
};

inline constexpr std::array<std::uint64_t, 24> specials{
    0x0000000000000000ULL, 0x8000000000000000ULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
    0xFFF8000000000000ULL, 0x7FF8000000000000ULL, 0x7FF8000000000001ULL, 0x7FF0000000000001ULL,
    0xFFF4000000000000ULL, 0x0000000000000001ULL, 0x8000000000000001ULL, 0x000FFFFFFFFFFFFFULL,
    0x0010000000000000ULL, 0x7FEFFFFFFFFFFFFFULL, 0xFFEFFFFFFFFFFFFFULL, 0x3FF0000000000000ULL,
    0xBFF0000000000000ULL, 0x3FE0000000000000ULL, 0x4000000000000000ULL, 0x4008000000000000ULL,
    0x3FB999999999999AULL, 0x4340000000000000ULL, 0x43E0000000000000ULL, 0xC3E0000000000000ULL,
};

inline binary64::Bits draw_operand(SplitMix64& rng) {
    const std::uint64_t selector = rng.next() & 15U;
    const std::uint64_t x = rng.next();
    const std::uint64_t sign = x & binary64::sign_mask;
    const std::uint64_t fraction = x & binary64::fraction_mask;
    const std::uint64_t field = (x >> 52U) & 0x7FFU;
    if (selector <= 4) {
        return x;
    }
    switch (selector) {
    case 5:
        return specials[x % specials.size()];
    case 6:
        return binary64::from_int64(static_cast<std::int64_t>(x % 2001U) - 1000);
    case 7:
        return sign | fraction;
    case 8:
    case 9:
        return sign | ((1013U + (field & 15U)) << 52U) | fraction;
    case 10:
        return sign | ((2046U - (field & 15U)) << 52U) | fraction;
    case 11:
        return sign | ((1U + (field & 15U)) << 52U) | fraction;
    case 12:
        return sign | ((1013U + (field & 31U)) << 52U) | (fraction & 0x000FF00000000000ULL);
    default:
        return sign | ((899U + field % 248U) << 52U) | fraction;
    }
}

inline std::pair<binary64::Bits, binary64::Bits> draw_pair(SplitMix64& rng) {
    const binary64::Bits a = draw_operand(rng);
    const std::uint64_t mode = rng.next() & 3U;
    if (mode == 0) {
        return {a, a ^ (rng.next() & 0x3FU)};
    }
    if (mode == 1) {
        const binary64::Bits b = draw_operand(rng);
        const std::uint64_t shift = rng.next() % 5U;
        const std::uint64_t exponent = (((a >> 52U) & 0x7FFU) + shift - 2U) & 0x7FFU;
        return {a, (b & ~binary64::exponent_mask) | (exponent << 52U)};
    }
    return {a, draw_operand(rng)};
}

inline std::string decimal_text(SplitMix64& rng) {
    const std::uint64_t count = 1U + rng.next() % 24U;
    std::string digits;
    for (std::uint64_t index = 0; index < count; ++index) {
        digits.push_back(static_cast<char>('0' + rng.next() % 10U));
    }
    const std::uint64_t point = rng.next() % (count + 1U);
    std::string text = point == count ? digits : digits.substr(0, point) + "." + digits.substr(point);
    if ((rng.next() & 1U) != 0) {
        const std::int64_t exponent = static_cast<std::int64_t>(rng.next() % 700U) - 350;
        text += (rng.next() & 1U) != 0 ? "E" : "e";
        text += std::to_string(exponent);
    }
    if ((rng.next() & 1U) != 0) {
        text = "-" + text;
    }
    return text;
}

// "%[-+ #0][width][.precision](e|E|f|g|G)" as used by the vectors.
inline decimal::FormatSpec parse_spec(std::string_view text) {
    decimal::FormatSpec spec;
    std::size_t position = 1;
    for (; position < text.size(); ++position) {
        const char flag = text[position];
        if (flag == '-') {
            spec.left_align = true;
        } else if (flag == '+') {
            spec.plus = true;
        } else if (flag == ' ') {
            spec.space = true;
        } else if (flag == '#') {
            spec.alternate = true;
        } else if (flag == '0') {
            spec.zero_pad = true;
        } else {
            break;
        }
    }
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        spec.width = spec.width * 10 + (text[position++] - '0');
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        spec.precision = 0;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            spec.precision = spec.precision * 10 + (text[position++] - '0');
        }
    }
    spec.conversion = position < text.size() ? text[position] : 'g';
    return spec;
}

inline constexpr std::array<std::string_view, 10> sequence_formats{
    "%.14g", "%.17g", "%.3f", "%.12e", "%g", "%.0f", "%#.3g", "%+.5e", "%.20f", "%.1f"};

} // namespace eawr::script::numeric::test
