#include "math_support.hpp"

namespace math_test_support {

// #503: the wide division, rounded shift and square root take fast paths; each must give the
// bits of the full 192-step shift-and-subtract division and the 64-step binary search.
namespace wide = eawr::sim::math::detail;

void reference_divide(wide::UInt192 numerator, wide::UInt192 denominator, wide::UInt192& quotient,
                      wide::UInt192& remainder) {
    quotient = {};
    remainder = {};
    if (denominator.is_zero()) return;
    for (int index = 191; index >= 0; --index) {
        const bool incoming = wide::bit(numerator, static_cast<unsigned>(index));
        const bool top = (remainder.limb[2] >> 63U) != 0;
        remainder.limb[2] = (remainder.limb[2] << 1U) | (remainder.limb[1] >> 63U);
        remainder.limb[1] = (remainder.limb[1] << 1U) | (remainder.limb[0] >> 63U);
        remainder.limb[0] = (remainder.limb[0] << 1U) | (incoming ? 1U : 0U);
        if (top || wide::compare(remainder, denominator) >= 0) {
            wide::subtract_magnitude(remainder, denominator);
            wide::set_bit(quotient, static_cast<unsigned>(index));
        }
    }
}

std::uint64_t reference_sqrt(wide::UInt192 value, bool& exceeds) {
    std::uint64_t low = 0;
    std::uint64_t high = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t answer = 0;
    while (low <= high) {
        const std::uint64_t middle = low + ((high - low) / 2U);
        if (wide::compare(wide::multiply_u64(middle, middle), value) <= 0) {
            answer = middle;
            if (middle == std::numeric_limits<std::uint64_t>::max()) break;
            low = middle + 1U;
        } else {
            if (middle == 0) break;
            high = middle - 1U;
        }
    }
    exceeds = answer == std::numeric_limits<std::uint64_t>::max()
        && wide::compare(wide::multiply_u64(answer, answer), value) < 0;
    return answer;
}

// A value of `bits` significant bits at most (splitmix64 limbs, masked).
wide::UInt192 random_wide(std::uint64_t& state, const unsigned bits) {
    const auto next = [&state] {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    };
    wide::UInt192 value{};
    for (unsigned limb = 0; limb < 3; ++limb) {
        const unsigned low = limb * 64U;
        if (bits <= low) break;
        const std::uint64_t word = next();
        value.limb[limb] = bits - low >= 64U ? word : word & ((std::uint64_t{1} << (bits - low)) - 1U);
    }
    return value;
}

bool same(const wide::UInt192& left, const wide::UInt192& right) { return left.limb == right.limb; }

// The 32-bit digit schoolbook product the 64-bit limb forms replaced (#520): the low 192 bits
// and whether any bit above them is set.
bool reference_multiply(const wide::UInt192& left, const wide::UInt192& right, wide::UInt192& result) {
    std::array<std::uint32_t, 6> a{};
    std::array<std::uint32_t, 6> b{};
    for (unsigned index = 0; index < 3; ++index) {
        a[index * 2] = static_cast<std::uint32_t>(left.limb[index]);
        a[index * 2 + 1] = static_cast<std::uint32_t>(left.limb[index] >> 32U);
        b[index * 2] = static_cast<std::uint32_t>(right.limb[index]);
        b[index * 2 + 1] = static_cast<std::uint32_t>(right.limb[index] >> 32U);
    }
    std::array<std::uint32_t, 12> out{};
    for (unsigned i = 0; i < 6; ++i) {
        std::uint64_t carry = 0;
        for (unsigned j = 0; j < 6; ++j) {
            const std::uint64_t total = static_cast<std::uint64_t>(a[i]) * b[j] + out[i + j] + carry;
            out[i + j] = static_cast<std::uint32_t>(total);
            carry = total >> 32U;
        }
        for (unsigned k = i + 6U; carry != 0 && k < 12U; ++k) {
            const std::uint64_t total = static_cast<std::uint64_t>(out[k]) + carry;
            out[k] = static_cast<std::uint32_t>(total);
            carry = total >> 32U;
        }
    }
    for (unsigned index = 0; index < 3; ++index) {
        result.limb[index] = static_cast<std::uint64_t>(out[index * 2]) | (static_cast<std::uint64_t>(out[index * 2 + 1]) << 32U);
    }
    bool overflow = false;
    for (unsigned index = 6; index < 12; ++index) overflow = overflow || out[index] != 0;
    return overflow;
}

// #520: the 64-bit limb products and the two-limb long division against the digit and bitwise
// references, on random widths and on limbs at the estimate's edges (Knuth's corrections).
void wide_limb_tests() {
    std::uint64_t state = 5200;
    constexpr std::array<unsigned, 10> widths{1, 32, 63, 64, 65, 96, 127, 128, 160, 192};
    for (int round = 0; round < 20'000; ++round) {
        const auto left = random_wide(state, widths[static_cast<std::size_t>(round) % widths.size()]);
        const auto right = random_wide(state, widths[static_cast<std::size_t>(round / 10) % widths.size()]);
        wide::UInt192 product{};
        wide::UInt192 expected{};
        const bool overflow = wide::multiply(left, right, product);
        const bool expected_overflow = reference_multiply(left, right, expected);
        expect(overflow == expected_overflow && same(product, expected), "wide multiply matches the digit form");
        const bool by_overflow = wide::multiply_by_u64(left, right.limb[0], product);
        const bool by_expected = reference_multiply(left, wide::from_u64(right.limb[0]), expected);
        expect(by_overflow == by_expected && same(product, expected), "multiply_by_u64 matches the digit form");
        static_cast<void>(reference_multiply(wide::from_u64(left.limb[0]), wide::from_u64(right.limb[0]), expected));
        expect(same(wide::multiply_u64(left.limb[0], right.limb[0]), expected), "multiply_u64 matches the digit form");
        wide::UInt192 denominator = right;
        denominator.limb[2] = 0;
        if (denominator.limb[1] == 0) denominator.limb[1] = 1;
        wide::UInt192 quotient{}, remainder{}, expected_quotient{}, expected_remainder{};
        wide::divide(left, denominator, quotient, remainder);
        reference_divide(left, denominator, expected_quotient, expected_remainder);
        expect(same(quotient, expected_quotient) && same(remainder, expected_remainder), "two-limb divide matches the reference");
    }
    const std::uint64_t top = std::uint64_t{1} << 63U;
    const std::array<std::uint64_t, 6> high_limbs{1, 2, top - 1U, top, top + 1U, ~std::uint64_t{0}};
    const std::array<std::uint64_t, 4> low_limbs{0, 1, top, ~std::uint64_t{0}};
    for (const auto divisor_high : high_limbs) {
        for (const auto divisor_low : low_limbs) {
            const wide::UInt192 denominator{{{divisor_low, divisor_high, 0}}};
            const std::array<std::uint64_t, 9> limbs{0, 1, top - 1U, top, ~std::uint64_t{0}, divisor_high - 1U, divisor_high,
                divisor_high + 1U, divisor_low};
            for (const auto a : limbs) {
                for (const auto b : limbs) {
                    for (const auto c : limbs) {
                        const wide::UInt192 numerator{{{c, b, a}}};
                        wide::UInt192 quotient{}, remainder{}, expected_quotient{}, expected_remainder{};
                        wide::divide(numerator, denominator, quotient, remainder);
                        reference_divide(numerator, denominator, expected_quotient, expected_remainder);
                        expect(same(quotient, expected_quotient) && same(remainder, expected_remainder),
                            "two-limb divide edge matches the reference");
                    }
                }
            }
        }
    }
}

void wide_fast_path_tests() {
    std::uint64_t state = 503;
    constexpr std::array<unsigned, 12> widths{1, 2, 24, 32, 33, 63, 64, 65, 88, 127, 128, 192};
    for (int round = 0; round < 4000; ++round) {
        const unsigned numerator_bits = widths[static_cast<std::size_t>(round) % widths.size()];
        const unsigned denominator_bits = widths[static_cast<std::size_t>(round / 12) % widths.size()];
        const auto numerator = random_wide(state, numerator_bits);
        auto denominator = random_wide(state, denominator_bits);
        if (denominator.is_zero()) denominator.limb[0] = 1;
        wide::UInt192 quotient{}, remainder{}, expected_quotient{}, expected_remainder{};
        wide::divide(numerator, denominator, quotient, remainder);
        reference_divide(numerator, denominator, expected_quotient, expected_remainder);
        expect(same(quotient, expected_quotient) && same(remainder, expected_remainder), "wide divide matches the reference");

        const wide::SignedWide signed_numerator{numerator, (round & 1) != 0};
        for (const unsigned shift : {1U, 24U, 40U, 63U}) {
            std::int64_t fast = 0;
            std::int64_t slow = 0;
            const bool fast_ok = wide::rounded_shift_to_raw(signed_numerator, shift, fast);
            const bool slow_ok = wide::rounded_divide_to_raw(signed_numerator, wide::shift_left_u64(1U, shift), slow);
            expect(fast_ok == slow_ok && (!fast_ok || fast == slow), "rounded shift matches the rounded division");
        }

        bool exceeds = false;
        bool expected_exceeds = false;
        const auto root = wide::floor_sqrt(numerator, exceeds);
        const auto expected_root = reference_sqrt(numerator, expected_exceeds);
        expect(root == expected_root && exceeds == expected_exceeds, "floor sqrt matches the binary search");
    }
    // Edges: the largest one-limb divisor, squares around 2^128, a numerator below the divisor.
    const wide::UInt192 all_ones{{{~0ULL, ~0ULL, ~0ULL}}};
    for (const wide::UInt192& denominator : {wide::UInt192{{{~0ULL, 0, 0}}}, wide::UInt192{{{1, 0, 0}}},
             wide::UInt192{{{0, 1, 0}}}, wide::UInt192{{{0, 0, 1ULL << 63U}}}, all_ones}) {
        wide::UInt192 quotient{}, remainder{}, expected_quotient{}, expected_remainder{};
        wide::divide(all_ones, denominator, quotient, remainder);
        reference_divide(all_ones, denominator, expected_quotient, expected_remainder);
        expect(same(quotient, expected_quotient) && same(remainder, expected_remainder), "wide divide edge");
    }
    for (const wide::UInt192& value : {wide::multiply_u64(~0ULL, ~0ULL), wide::UInt192{{{~0ULL, ~0ULL, 0}}},
             wide::UInt192{{{0, 0, 1}}}, wide::UInt192{{{2, 0, 0}}}, wide::UInt192{{{3, 0, 0}}}, wide::UInt192{}}) {
        bool exceeds = false;
        bool expected_exceeds = false;
        const auto root = wide::floor_sqrt(value, exceeds);
        expect(root == reference_sqrt(value, expected_exceeds) && exceeds == expected_exceeds, "floor sqrt edge");
    }
}

// #520: Fixed multiply, divide and sqrt, the Vec2 length and dot, and atan2's normalisation
// take 64- and 128-bit fast paths; each must give the bits of the 192-bit forms (the
// reference forms below are the pre-#520 code).
std::int64_t reference_round_shift(const wide::SignedWide& value, const unsigned shift, bool& ok) {
    std::int64_t raw = 0;
    ok = wide::rounded_divide_to_raw(value, wide::shift_left_u64(1U, shift), raw);
    return raw;
}

bool reference_sqrt_fixed(const Fixed value, std::int64_t& raw) {
    if (value.raw() < 0) return false;
    const auto radicand = wide::shift_left_u64(static_cast<std::uint64_t>(value.raw()), Fixed::fractional_bits);
    bool exceeds = false;
    std::uint64_t root = reference_sqrt(radicand, exceeds);
    if (exceeds) return false;
    wide::UInt192 four{};
    static_cast<void>(wide::multiply_by_u64(radicand, 4U, four));
    if (wide::compare(four, wide::multiply_u64(root * 2U + 1U, root * 2U + 1U)) > 0) ++root;
    return wide::magnitude_to_raw(wide::from_u64(root), false, raw);
}

bool reference_length(const std::int64_t x, const std::int64_t y, std::int64_t& raw) {
    wide::UInt192 sum = wide::multiply_u64(wide::unsigned_magnitude(x), wide::unsigned_magnitude(x));
    static_cast<void>(wide::add_magnitude(sum, wide::multiply_u64(wide::unsigned_magnitude(y), wide::unsigned_magnitude(y))));
    bool exceeds = false;
    std::uint64_t root = reference_sqrt(sum, exceeds);
    if (exceeds) return false;
    wide::UInt192 four{};
    static_cast<void>(wide::multiply_by_u64(sum, 4U, four));
    if (root != std::numeric_limits<std::uint64_t>::max()
        && wide::compare(four, wide::multiply_u64(root * 2U + 1U, root * 2U + 1U)) > 0) {
        ++root;
    }
    return wide::magnitude_to_raw(wide::from_u64(root), false, raw);
}

// The pre-#520 atan2: doubling one step at a time, rounding through the 192-bit shift.
std::int64_t reference_atan2(std::int64_t y, std::int64_t x) {
    constexpr std::int64_t quarter = Fixed::scale / 4;
    constexpr std::int64_t half = Fixed::scale / 2;
    if (x == 0) return y > 0 ? quarter : -quarter;
    if (y == 0) return x > 0 ? 0 : -half;
    const bool original_y_positive = y > 0;
    constexpr std::uint64_t safe = std::uint64_t{1} << 60U;
    while (wide::unsigned_magnitude(x) > safe || wide::unsigned_magnitude(y) > safe) {
        x /= 2;
        y /= 2;
    }
    while (wide::unsigned_magnitude(x) <= safe / 2U && wide::unsigned_magnitude(y) <= safe / 2U) {
        x *= 2;
        y *= 2;
    }
    std::int64_t z = 0;
    if (x < 0) {
        x = -x;
        y = -y;
        z = original_y_positive ? std::int64_t{1} << 61 : -(std::int64_t{1} << 61);
    }
    for (unsigned index = 0; index < wide::cordic_atan_turns_q62.size(); ++index) {
        const std::int64_t old_x = x;
        const std::int64_t old_y = y;
        if (old_y == 0) break;
        if (old_y > 0) {
            x = old_x + (old_y >> index);
            y = old_y - (old_x >> index);
            z += wide::cordic_atan_turns_q62[index];
        } else {
            x = old_x - (old_y >> index);
            y = old_y + (old_x >> index);
            z -= wide::cordic_atan_turns_q62[index];
        }
    }
    bool ok = false;
    const auto rounded = reference_round_shift({wide::from_u64(wide::unsigned_magnitude(z)), z < 0}, 38U, ok);
    return wrap_turn(Fixed::from_raw(rounded)).raw();
}

void fixed_fast_path_tests() {
    std::uint64_t state = 520;
    const auto next = [&state] {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    };
    // A raw value of at most `bits` magnitude bits, either sign; now and then a power of two
    // or its neighbours.
    const auto raw_of = [&](const unsigned bits) {
        std::uint64_t magnitude = bits >= 64U ? next() : next() & ((std::uint64_t{1} << bits) - 1U);
        switch (next() % 8U) {
        case 0: magnitude = std::uint64_t{1} << (bits - 1U); break;
        case 1: magnitude = (std::uint64_t{1} << (bits - 1U)) - 1U; break;
        default: break;
        }
        if (bits >= 64U) magnitude &= ~(std::uint64_t{1} << 63U);
        const auto value = static_cast<std::int64_t>(magnitude);
        return (next() & 1U) != 0 ? -value : value;
    };
    const std::array<std::int64_t, 9> edges{0, 1, -1, Fixed::scale, -Fixed::scale, std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<std::int64_t>::min(), (std::int64_t{1} << 39) - 1, -(std::int64_t{1} << 39)};
    const auto check_pair = [&](const std::int64_t a, const std::int64_t b) {
        const Fixed left = Fixed::from_raw(a);
        const Fixed right = Fixed::from_raw(b);
        // The 64-bit fast paths and the 128-bit steps of the wide forms against the 192-bit
        // rounded product and quotient.
        Fixed fast;
        Fixed step;
        std::int64_t exact = 0;
        const bool product_ok = wide::rounded_shift_to_raw(wide::SignedWide::product(a, b), Fixed::fractional_bits, exact);
        bool fast_ok = try_multiply(left, right, fast);
        bool step_ok = detail::multiply_wide(left, right, step);
        expect(fast_ok == product_ok && (!fast_ok || fast.raw() == exact), "multiply fast path matches the 192-bit product");
        expect(step_ok == product_ok && (!step_ok || step.raw() == exact), "multiply's 128-bit step matches the 192-bit product");
        if (b != 0) {
            const wide::SignedWide shifted{
                wide::shift_left_u64(wide::unsigned_magnitude(a), Fixed::fractional_bits), (a < 0) != (b < 0)};
            const bool quotient_ok = wide::rounded_divide_to_raw(shifted, wide::from_u64(wide::unsigned_magnitude(b)), exact);
            fast_ok = try_divide(left, right, fast);
            step_ok = detail::divide_wide(left, right, step);
            expect(fast_ok == quotient_ok && (!fast_ok || fast.raw() == exact), "divide fast path matches the 192-bit quotient");
            expect(step_ok == quotient_ok && (!step_ok || step.raw() == exact), "divide's 128-bit step matches the 192-bit quotient");
        }
        Fixed dot_fast;
        const bool dot_ok = try_dot(Vec2{left, right}, Vec2{right, left}, dot_fast);
        const auto dot_slow = dot(Vec2{left, right}, Vec2{right, left});
        expect(dot_ok == dot_slow.has_value() && (!dot_ok || dot_fast == dot_slow.value()), "try_dot matches dot");
        wide::SignedWide sum = wide::SignedWide::product(a, b);
        sum.add(wide::SignedWide::product(b, a));
        bool reference_ok = false;
        const auto reference = reference_round_shift(sum, Fixed::fractional_bits, reference_ok);
        expect(dot_slow.has_value() == reference_ok && (!reference_ok || dot_slow.value().raw() == reference),
            "dot matches the 192-bit rounded sum");
        std::int64_t expected = 0;
        const bool length_expected = reference_length(a, b, expected);
        const auto length_value = length(Vec2{left, right});
        expect(length_value.has_value() == length_expected && (!length_expected || length_value.value().raw() == expected),
            "length matches the 192-bit form");
        const bool sqrt_expected = reference_sqrt_fixed(left, expected);
        Fixed root;
        const bool sqrt_ok = try_sqrt(left, root);
        expect(sqrt_ok == sqrt_expected && (!sqrt_expected || root.raw() == expected), "sqrt matches the binary search");
        if (a != 0 || b != 0) {
            expect(atan2_turn(left, right).value().raw() == reference_atan2(a, b), "atan2 matches the stepwise normalisation");
        }
        const auto both = sin_cos_turn(left);
        expect(both.sine == sin_turn(left) && both.cosine == cos_turn(left), "sin_cos_turn matches sin_turn and cos_turn");
    };
    for (const auto a : edges) {
        for (const auto b : edges) check_pair(a, b);
    }
    constexpr std::array<unsigned, 14> widths{1, 2, 12, 24, 25, 31, 32, 38, 39, 40, 44, 52, 62, 64};
    for (int round = 0; round < 200'000; ++round) {
        const auto a = raw_of(widths[next() % widths.size()]);
        const auto b = raw_of(widths[next() % widths.size()]);
        check_pair(a, b);
    }
    // Rounding ties: products and quotients whose remainder is exactly half.
    for (int round = 0; round < 20'000; ++round) {
        const auto b = raw_of(1U + static_cast<unsigned>(next() % 24U)) | 1;
        const auto q = raw_of(1U + static_cast<unsigned>(next() % 30U));
        check_pair(q * Fixed::scale / b + (Fixed::scale / 2) / b, b);
        check_pair(q, Fixed::scale / 2 + b);
    }
    // Ties past 64 bits (the wide forms' 128-bit steps): an odd product times 2^23, and an odd
    // multiple of k over k 2^25 with the numerator above 2^39.
    for (int round = 0; round < 20'000; ++round) {
        const auto odd_a = static_cast<std::int64_t>((next() >> 25U) | 1U);
        const auto odd_b = static_cast<std::int64_t>((next() >> 44U) | 1U);
        check_pair(odd_a << 20, odd_b << 3);
        check_pair(-(odd_a << 20), odd_b << 3);
        const auto k = static_cast<std::int64_t>((next() >> 34U) | 1U);
        const auto q = static_cast<std::int64_t>(next() >> 39U);
        check_pair((2 * q + 1) * k, k << 25);
        check_pair(-(2 * q + 1) * k, k << 25);
    }
    for (std::uint64_t value = 0; value < 5000; ++value) {
        bool exceeds = false;
        expect(wide::floor_sqrt_64(value) == reference_sqrt(wide::from_u64(value), exceeds), "floor_sqrt_64 small values");
    }
    for (int round = 0; round < 50'000; ++round) {
        const auto value = random_wide(state, 1U + static_cast<unsigned>(next() % 128U));
        bool exceeds = false;
        const auto expected = reference_sqrt(value, exceeds);
        expect(wide::floor_sqrt_128(value.limb[1], value.limb[0]) == expected, "floor_sqrt_128 matches the binary search");
        // 2 r + 1 fits a u64 below 2^63 (a larger root overflows a Fixed before it rounds).
        if (expected < std::uint64_t{1} << 63U) {
            const bool up = wide::sqrt_rounds_up(value.limb[1], value.limb[0], expected);
            wide::UInt192 four{};
            static_cast<void>(wide::multiply_by_u64(value, 4U, four));
            expect(up == (wide::compare(four, wide::multiply_u64(expected * 2U + 1U, expected * 2U + 1U)) > 0),
                "sqrt_rounds_up matches 4 v > (2 r + 1)^2");
        }
        std::uint64_t high = 0;
        std::uint64_t low = 0;
        wide::multiply_64(value.limb[0], value.limb[1], high, low);
        const auto product = wide::multiply_u64(value.limb[0], value.limb[1]);
        expect(product.limb[0] == low && product.limb[1] == high, "multiply_64 matches multiply_u64");
    }
}


} // namespace math_test_support
