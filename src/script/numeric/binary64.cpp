#include "eawr/script/numeric/binary64.hpp"

namespace eawr::script::numeric::binary64 {
namespace {

// floor(sqrt(value)) for a 128-bit value, and whether the root was inexact.
// Classic digit-by-digit (base 4) integer square root; 64 fixed iterations.
std::uint64_t integer_square_root(detail::U128 value, bool& inexact) noexcept {
    detail::U128 remainder = value;
    detail::U128 root{0, 0};
    detail::U128 bit{std::uint64_t{1} << 62U, 0};
    for (int iteration = 0; iteration < 64; ++iteration) {
        // trial = root + bit
        detail::U128 trial{root.high + bit.high, root.low + bit.low};
        if (trial.low < root.low) {
            ++trial.high;
        }
        const bool fits = remainder.high > trial.high || (remainder.high == trial.high && remainder.low >= trial.low);
        // root >>= 1
        root.low = (root.low >> 1U) | (root.high << 63U);
        root.high >>= 1U;
        if (fits) {
            const std::uint64_t borrow = remainder.low < trial.low ? 1U : 0U;
            remainder.low -= trial.low;
            remainder.high -= trial.high + borrow;
            root.low += bit.low;
            if (root.low < bit.low) {
                ++root.high;
            }
            root.high += bit.high;
        }
        // bit >>= 2
        bit.low = (bit.low >> 2U) | (bit.high << 62U);
        bit.high >>= 2U;
    }
    inexact = (remainder.high | remainder.low) != 0;
    return root.low;
}

} // namespace

Bits square_root(Bits value) noexcept {
    std::int32_t exponent = exponent_of(value);
    std::uint64_t significand = fraction_of(value);
    const bool sign = sign_of(value);
    if (exponent == 0x7FF) {
        if (significand != 0) {
            return detail::propagate_nan(value, 0);
        }
        return sign ? canonical_nan : value;
    }
    if (sign) {
        return is_zero(value) ? value : canonical_nan;
    }
    if (exponent == 0) {
        if (significand == 0) {
            return value;
        }
        const detail::Normalized normalized = detail::normalize_subnormal(significand);
        exponent = normalized.exponent;
        significand = normalized.significand;
    }
    // value = m * 2^(e - 52) with m in [2^52, 2^53). Make e even by doubling m,
    // then root(m' * 2^72) lies in [2^62, 2^63) and carries ten rounding bits.
    significand |= hidden_bit;
    std::int32_t unbiased = exponent - 0x3FF;
    if ((unbiased & 1) != 0) {
        significand <<= 1U;
        unbiased -= 1;
    }
    // m' * 2^72 as a 128-bit value: m' < 2^54, so the high word is m' >> (64 - 72 + 64).
    const detail::U128 radicand{significand << 8U, 0};
    bool inexact = false;
    const std::uint64_t root = integer_square_root(radicand, inexact);
    return detail::round_pack(false, 0x3FE + unbiased / 2, root | static_cast<std::uint64_t>(inexact));
}

Bits round_to_integral(Bits value, Rounding rounding) noexcept {
    const std::int32_t exponent = exponent_of(value);
    const bool sign = sign_of(value);
    if (exponent <= 0x3FE) {
        if (is_zero(value)) {
            return value;
        }
        switch (rounding) {
        case Rounding::nearest_even:
            if (exponent == 0x3FE && fraction_of(value) != 0) {
                return pack(sign, 0x3FF, 0);
            }
            break;
        case Rounding::toward_zero:
            break;
        case Rounding::downward:
            if (sign) {
                return pack(true, 0x3FF, 0);
            }
            break;
        case Rounding::upward:
            if (!sign) {
                return pack(false, 0x3FF, 0);
            }
            break;
        }
        return pack(sign, 0, 0);
    }
    if (exponent >= 0x433) {
        if (exponent == 0x7FF && fraction_of(value) != 0) {
            return detail::propagate_nan(value, 0);
        }
        return value;
    }
    const std::uint64_t last_bit = std::uint64_t{1} << static_cast<std::uint32_t>(0x433 - exponent);
    const std::uint64_t round_bits = last_bit - 1U;
    Bits result = value;
    switch (rounding) {
    case Rounding::nearest_even:
        result += last_bit >> 1U;
        if ((result & round_bits) == 0) {
            result &= ~last_bit;
        }
        break;
    case Rounding::toward_zero:
        break;
    case Rounding::downward:
        if (sign) {
            result += round_bits;
        }
        break;
    case Rounding::upward:
        if (!sign) {
            result += round_bits;
        }
        break;
    }
    return result & ~round_bits;
}

} // namespace eawr::script::numeric::binary64
