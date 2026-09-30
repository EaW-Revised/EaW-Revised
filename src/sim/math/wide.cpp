#include "wide.hpp"

#include <algorithm>
#include <bit>
#include <limits>

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
#include <intrin.h>
#endif

namespace eawr::sim::math::detail {
namespace {

#if defined(__SIZEOF_INT128__)
// A GNU extension; __extension__ keeps -Wpedantic quiet.
__extension__ typedef unsigned __int128 UInt128;
#endif

} // namespace

std::uint64_t unsigned_magnitude(std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

int compare(UInt192 left, UInt192 right) noexcept {
    for (int index = 2; index >= 0; --index) {
        if (left.limb[static_cast<unsigned>(index)] < right.limb[static_cast<unsigned>(index)]) {
            return -1;
        }
        if (left.limb[static_cast<unsigned>(index)] > right.limb[static_cast<unsigned>(index)]) {
            return 1;
        }
    }
    return 0;
}

bool add_magnitude(UInt192& value, UInt192 addend) noexcept {
    std::uint64_t carry = 0;
    for (unsigned index = 0; index < 3; ++index) {
        const std::uint64_t first = value.limb[index] + addend.limb[index];
        const bool carry_first = first < value.limb[index];
        const std::uint64_t second = first + carry;
        const bool carry_second = second < first;
        value.limb[index] = second;
        carry = (carry_first || carry_second) ? 1U : 0U;
    }
    return carry != 0;
}

void subtract_magnitude(UInt192& value, UInt192 subtrahend) noexcept {
    std::uint64_t borrow = 0;
    for (unsigned index = 0; index < 3; ++index) {
        const std::uint64_t first = value.limb[index] - subtrahend.limb[index];
        const bool borrow_first = value.limb[index] < subtrahend.limb[index];
        const std::uint64_t second = first - borrow;
        const bool borrow_second = first < borrow;
        value.limb[index] = second;
        borrow = (borrow_first || borrow_second) ? 1U : 0U;
    }
}

bool increment(UInt192& value) noexcept {
    for (auto& limb : value.limb) {
        ++limb;
        if (limb != 0) {
            return false;
        }
    }
    return true;
}

bool bit(UInt192 value, unsigned index) noexcept {
    return index < 192 && ((value.limb[index / 64U] >> (index % 64U)) & 1U) != 0;
}

void set_bit(UInt192& value, unsigned index) noexcept {
    if (index < 192) {
        value.limb[index / 64U] |= std::uint64_t{1} << (index % 64U);
    }
}

UInt192 from_u64(std::uint64_t value) noexcept {
    return {{{value, 0, 0}}};
}

UInt192 shift_left_u64(std::uint64_t value, unsigned shift) noexcept {
    UInt192 result{};
    if (shift >= 192) {
        return result;
    }
    const unsigned word = shift / 64U;
    const unsigned bits = shift % 64U;
    result.limb[word] = value << bits;
    if (bits != 0 && word + 1U < 3U) {
        result.limb[word + 1U] = value >> (64U - bits);
    }
    return result;
}

// The products on 64-bit limbs through multiply_64 (#520): schoolbook with the carries in
// 64 bits, the same exact low 192 bits and overflow as the 32-bit digit form they replace
// (math_contract_tests keeps that form as the reference).
UInt192 multiply_u64(std::uint64_t left, std::uint64_t right) noexcept {
    UInt192 result{};
    multiply_64(left, right, result.limb[1], result.limb[0]);
    return result;
}

bool multiply(UInt192 left, UInt192 right, UInt192& result) noexcept {
    std::array<std::uint64_t, 6> out{};
    for (unsigned i = 0; i < 3; ++i) {
        std::uint64_t carry = 0;
        for (unsigned j = 0; j < 3; ++j) {
            std::uint64_t high = 0;
            std::uint64_t low = 0;
            multiply_64(left.limb[i], right.limb[j], high, low);
            low += carry;
            high += low < carry ? 1U : 0U;
            const unsigned k = i + j;
            out[k] += low;
            high += out[k] < low ? 1U : 0U;
            carry = high;
        }
        out[i + 3U] = carry;
    }
    result = {{{out[0], out[1], out[2]}}};
    return out[3] != 0 || out[4] != 0 || out[5] != 0;
}

bool multiply_by_u64(UInt192 left, std::uint64_t right, UInt192& result) noexcept {
    std::uint64_t carry = 0;
    for (unsigned index = 0; index < 3; ++index) {
        std::uint64_t high = 0;
        std::uint64_t low = 0;
        multiply_64(left.limb[index], right, high, low);
        low += carry;
        high += low < carry ? 1U : 0U;
        result.limb[index] = low;
        carry = high;
    }
    return carry != 0;
}

namespace {

// The number of significant bits (0 for zero).
unsigned bit_length(UInt192 value) noexcept {
    for (int index = 2; index >= 0; --index) {
        const std::uint64_t limb = value.limb[static_cast<unsigned>(index)];
        if (limb != 0) {
            return static_cast<unsigned>(index) * 64U + static_cast<unsigned>(std::bit_width(limb));
        }
    }
    return 0;
}

// (high:low) / divisor for high < divisor, in 64-bit operations only (Knuth's algorithm D on
// 32-bit digits, as in Hacker's Delight divlu): the exact quotient and remainder on every
// compiler and target; the fallback of divide_128_by_64 below.
[[maybe_unused]] std::uint64_t divide_128_by_64_portable(
    std::uint64_t high, std::uint64_t low, std::uint64_t divisor, std::uint64_t& remainder) noexcept {
    constexpr std::uint64_t base = std::uint64_t{1} << 32U;
    const unsigned shift = static_cast<unsigned>(std::countl_zero(divisor));
    divisor <<= shift;
    const std::uint64_t divisor_high = divisor >> 32U;
    const std::uint64_t divisor_low = divisor & 0xffffffffU;
    const std::uint64_t numerator_high = shift == 0 ? high : (high << shift) | (low >> (64U - shift));
    const std::uint64_t numerator_low = low << shift;
    const std::uint64_t digit_high = numerator_low >> 32U;
    const std::uint64_t digit_low = numerator_low & 0xffffffffU;

    std::uint64_t quotient_high = numerator_high / divisor_high;
    std::uint64_t estimate_remainder = numerator_high - quotient_high * divisor_high;
    while (quotient_high >= base || quotient_high * divisor_low > ((estimate_remainder << 32U) | digit_high)) {
        --quotient_high;
        estimate_remainder += divisor_high;
        if (estimate_remainder >= base) break;
    }
    const std::uint64_t middle = (numerator_high << 32U) + digit_high - quotient_high * divisor;

    std::uint64_t quotient_low = middle / divisor_high;
    estimate_remainder = middle - quotient_low * divisor_high;
    while (quotient_low >= base || quotient_low * divisor_low > ((estimate_remainder << 32U) | digit_low)) {
        --quotient_low;
        estimate_remainder += divisor_high;
        if (estimate_remainder >= base) break;
    }
    remainder = ((middle << 32U) + digit_low - quotient_low * divisor) >> shift;
    return (quotient_high << 32U) | quotient_low;
}

// The same quotient and remainder through the target's 128-bit division where it has one
// (#520): an exact integer division, so every path gives the same bits (math_contract_tests).
// clang-cl has __int128 but no __udivti3 in the MSVC runtime, so it takes the portable path.
std::uint64_t divide_128_by_64(
    std::uint64_t high, std::uint64_t low, std::uint64_t divisor, std::uint64_t& remainder) noexcept {
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
    const UInt128 value = (static_cast<UInt128>(high) << 64U) | low;
    remainder = static_cast<std::uint64_t>(value % divisor);
    return static_cast<std::uint64_t>(value / divisor);
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    return _udiv128(high, low, divisor, &remainder);
#else
    return divide_128_by_64_portable(high, low, divisor, remainder);
#endif
}

// Floor of the square root of a u64, digit by digit (two bits a step, no division).
std::uint64_t floor_sqrt_u64(std::uint64_t value) noexcept {
    std::uint64_t root = 0;
    std::uint64_t place = std::uint64_t{1} << 62U;
    while (place > value) {
        place >>= 2U;
    }
    while (place != 0) {
        if (value >= root + place) {
            value -= root + place;
            root = (root >> 1U) + place;
        } else {
            root >>= 1U;
        }
        place >>= 2U;
    }
    return root;
}

} // namespace

namespace {

// One quotient limb of `part` (three limbs, below v 2^64) over the normalised two-limb divisor v
// (top bit set): estimated from part's top two limbs over v's top limb, which is at most 2 too
// large (Knuth, TAOCP 4.3.1, Theorems A and B), and lowered while its product exceeds `part`.
// `part` becomes the remainder.
std::uint64_t quotient_limb(UInt192& part, const UInt192& divisor) noexcept {
    std::uint64_t estimate = ~std::uint64_t{0};
    if (part.limb[2] < divisor.limb[1]) {
        std::uint64_t unused = 0;
        estimate = divide_128_by_64(part.limb[2], part.limb[1], divisor.limb[1], unused);
    }
    UInt192 product{};
    static_cast<void>(multiply_by_u64(divisor, estimate, product));
    while (compare(product, part) > 0) {
        --estimate;
        subtract_magnitude(product, divisor);
    }
    subtract_magnitude(part, product);
    return estimate;
}

// A denominator of two limbs (#520; the motion Hermite's S^3): long division on 64-bit limbs,
// the same exact quotient and remainder as the bitwise form (math_contract_tests).
void divide_by_two_limbs(UInt192 numerator, UInt192 denominator, UInt192& quotient, UInt192& remainder) noexcept {
    const auto shift = static_cast<unsigned>(std::countl_zero(denominator.limb[1]));
    const auto shifted = [shift](const std::uint64_t high, const std::uint64_t low) {
        return shift == 0 ? high : (high << shift) | (low >> (64U - shift));
    };
    const UInt192 divisor{{{denominator.limb[0] << shift, shifted(denominator.limb[1], denominator.limb[0]), 0}}};
    const std::uint64_t top = shift == 0 ? 0 : numerator.limb[2] >> (64U - shift);
    UInt192 part{{{shifted(numerator.limb[1], numerator.limb[0]), shifted(numerator.limb[2], numerator.limb[1]), top}}};
    quotient = {};
    quotient.limb[1] = quotient_limb(part, divisor);
    part = {{{numerator.limb[0] << shift, part.limb[0], part.limb[1]}}};
    quotient.limb[0] = quotient_limb(part, divisor);
    remainder = {};
    remainder.limb[0] = shift == 0 ? part.limb[0] : (part.limb[0] >> shift) | (part.limb[1] << (64U - shift));
    remainder.limb[1] = part.limb[1] >> shift;
}

} // namespace

// Exact quotient and remainder. A denominator of one limb (every Fixed division) takes limb-wise
// long division, one of two limbs long division on 64-bit limbs (#520); a wider one shifts and
// subtracts from the numerator's top bit. Both give the
// bits of the full 192-step shift-and-subtract division (math_contract_tests cross-checks them).
void divide(UInt192 numerator, UInt192 denominator, UInt192& quotient, UInt192& remainder) noexcept {
    quotient = {};
    remainder = {};
    if (denominator.is_zero()) {
        return;
    }
    if (denominator.limb[1] == 0 && denominator.limb[2] == 0) {
        const std::uint64_t divisor = denominator.limb[0];
        std::uint64_t carry = 0;
        for (int index = 2; index >= 0; --index) {
            const auto limb = static_cast<unsigned>(index);
            if (carry == 0 && numerator.limb[limb] < divisor) {
                carry = numerator.limb[limb]; // a zero quotient limb
                continue;
            }
            quotient.limb[limb] = divide_128_by_64(carry, numerator.limb[limb], divisor, carry);
        }
        remainder.limb[0] = carry;
        return;
    }
    if (compare(numerator, denominator) < 0) {
        remainder = numerator;
        return;
    }
    if (denominator.limb[2] == 0) {
        divide_by_two_limbs(numerator, denominator, quotient, remainder);
        return;
    }
    for (int index = static_cast<int>(bit_length(numerator)) - 1; index >= 0; --index) {
        const bool incoming = bit(numerator, static_cast<unsigned>(index));
        const bool top = (remainder.limb[2] >> 63U) != 0;
        remainder.limb[2] = (remainder.limb[2] << 1U) | (remainder.limb[1] >> 63U);
        remainder.limb[1] = (remainder.limb[1] << 1U) | (remainder.limb[0] >> 63U);
        remainder.limb[0] = (remainder.limb[0] << 1U) | (incoming ? 1U : 0U);
        if (top || compare(remainder, denominator) >= 0) {
            subtract_magnitude(remainder, denominator);
            set_bit(quotient, static_cast<unsigned>(index));
        }
    }
}

SignedWide SignedWide::product(std::int64_t left, std::int64_t right) noexcept {
    const bool negative_result = (left < 0) != (right < 0);
    return {multiply_u64(unsigned_magnitude(left), unsigned_magnitude(right)), negative_result};
}

SignedWide SignedWide::scaled(std::int64_t value, unsigned shift) noexcept {
    return {shift_left_u64(unsigned_magnitude(value), shift), value < 0};
}

void SignedWide::add(SignedWide other) noexcept {
    if (other.magnitude.is_zero()) {
        return;
    }
    if (magnitude.is_zero()) {
        *this = other;
        return;
    }
    if (negative == other.negative) {
        static_cast<void>(add_magnitude(magnitude, other.magnitude));
        return;
    }
    const int relation = compare(magnitude, other.magnitude);
    if (relation == 0) {
        magnitude = {};
        negative = false;
    } else if (relation > 0) {
        subtract_magnitude(magnitude, other.magnitude);
    } else {
        UInt192 replacement = other.magnitude;
        subtract_magnitude(replacement, magnitude);
        magnitude = replacement;
        negative = other.negative;
    }
}

bool magnitude_to_raw(UInt192 magnitude, bool negative, std::int64_t& raw) noexcept {
    if (magnitude.limb[1] != 0 || magnitude.limb[2] != 0) {
        return false;
    }
    constexpr std::uint64_t min_magnitude = std::uint64_t{1} << 63U;
    const std::uint64_t value = magnitude.limb[0];
    if (negative) {
        if (value > min_magnitude) {
            return false;
        }
        if (value == min_magnitude) {
            raw = std::numeric_limits<std::int64_t>::min();
        } else {
            raw = -static_cast<std::int64_t>(value);
        }
        return true;
    }
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    raw = static_cast<std::int64_t>(value);
    return true;
}

bool rounded_divide_to_raw(
    SignedWide numerator, UInt192 denominator, std::int64_t& raw) noexcept {
    UInt192 quotient{};
    UInt192 remainder{};
    divide(numerator.magnitude, denominator, quotient, remainder);
    UInt192 complement = denominator;
    subtract_magnitude(complement, remainder);
    const int half_relation = compare(remainder, complement);
    if (half_relation > 0 || (half_relation == 0 && quotient.odd())) {
        if (increment(quotient)) {
            return false;
        }
    }
    return magnitude_to_raw(quotient, numerator.negative, raw);
}

// The division by 2^shift of rounded_divide_to_raw, done as a shift: the quotient is the bits
// above `shift`, the remainder the bits below, rounded half to even alike.
bool rounded_shift_to_raw(SignedWide numerator, unsigned shift, std::int64_t& raw) noexcept {
    if (shift == 0 || shift >= 64U) {
        return rounded_divide_to_raw(numerator, shift_left_u64(1U, shift), raw);
    }
    const UInt192& value = numerator.magnitude;
    UInt192 quotient{};
    quotient.limb[0] = (value.limb[0] >> shift) | (value.limb[1] << (64U - shift));
    quotient.limb[1] = (value.limb[1] >> shift) | (value.limb[2] << (64U - shift));
    quotient.limb[2] = value.limb[2] >> shift;
    const std::uint64_t remainder = value.limb[0] & ((std::uint64_t{1} << shift) - 1U);
    const std::uint64_t half = std::uint64_t{1} << (shift - 1U);
    if (remainder > half || (remainder == half && quotient.odd())) {
        if (increment(quotient)) {
            return false;
        }
    }
    return magnitude_to_raw(quotient, numerator.negative, raw);
}

void multiply_64(const std::uint64_t left, const std::uint64_t right, std::uint64_t& high, std::uint64_t& low) noexcept {
#if defined(__SIZEOF_INT128__)
    const UInt128 product = static_cast<UInt128>(left) * right;
    high = static_cast<std::uint64_t>(product >> 64U);
    low = static_cast<std::uint64_t>(product);
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    low = _umul128(left, right, &high);
#else
    constexpr std::uint64_t mask = 0xffffffffU;
    const std::uint64_t low_low = (left & mask) * (right & mask);
    const std::uint64_t low_high = (left & mask) * (right >> 32U);
    const std::uint64_t high_low = (left >> 32U) * (right & mask);
    const std::uint64_t high_high = (left >> 32U) * (right >> 32U);
    const std::uint64_t middle = (low_low >> 32U) + (low_high & mask) + (high_low & mask);
    low = (low_low & mask) | (middle << 32U);
    high = high_high + (low_high >> 32U) + (high_low >> 32U) + (middle >> 32U);
#endif
}

std::uint64_t divide_128(const std::uint64_t high, const std::uint64_t low, const std::uint64_t divisor,
    std::uint64_t& remainder) noexcept {
    return divide_128_by_64(high, low, divisor, remainder);
}

// Integer Newton from one shifted step off 2^(bits / 2): floor((x + floor(v / x)) / 2) is at
// least the floor root for every positive x, and the iteration falls to it.
std::uint64_t floor_sqrt_64(const std::uint64_t value) noexcept {
    if (value < 2U) {
        return value;
    }
    const unsigned half = static_cast<unsigned>(std::bit_width(value)) / 2U;
    std::uint64_t root = ((std::uint64_t{1} << half) + (value >> half)) >> 1U;
    while (true) {
        const std::uint64_t next = (root + value / root) >> 1U;
        if (next >= root) {
            return root;
        }
        root = next;
    }
}

// floor_sqrt's Newton iteration with one 128/64 step each. Below 2^126 the iterate, at or above
// the root, always exceeds `high`, as divide_128_by_64 needs; above, the general form.
std::uint64_t floor_sqrt_128(const std::uint64_t high, const std::uint64_t low) noexcept {
    if (high == 0) {
        return floor_sqrt_64(low);
    }
    if (high >= std::uint64_t{1} << 62U) {
        bool exceeds = false;
        return floor_sqrt({{low, high, 0}}, exceeds);
    }
    const unsigned bits = 64U + static_cast<unsigned>(std::bit_width(high));
    const unsigned half_shift = (bits - 63U) / 2U;
    const unsigned shift = half_shift * 2U;
    const std::uint64_t top = shift >= 64U ? high >> (shift - 64U) : (low >> shift) | (high << (64U - shift));
    std::uint64_t root = (floor_sqrt_64(top) + 1U) << half_shift;
    while (true) {
        std::uint64_t remainder = 0;
        const std::uint64_t quotient = divide_128_by_64(high, low, root, remainder);
        const std::uint64_t next = (root >> 1U) + (quotient >> 1U) + (root & quotient & 1U);
        if (next >= root) {
            return root;
        }
        root = next;
    }
}

bool sqrt_rounds_up(const std::uint64_t high, const std::uint64_t low, const std::uint64_t root) noexcept {
    std::uint64_t square_high = 0;
    std::uint64_t square_low = 0;
    multiply_64(root, root, square_high, square_low);
    const std::uint64_t excess_low = low - square_low;
    const std::uint64_t excess_high = high - square_high - (low < square_low ? 1U : 0U);
    return excess_high != 0 || excess_low > root;
}

// Floor of the square root. Below 2^128 (every sqrt and length of Q24 values) an integer
// Newton iteration from a power of two at or above the root; the binary search otherwise.
// Both return the exact floor.
std::uint64_t floor_sqrt(UInt192 value, bool& exceeds_u64) noexcept {
    exceeds_u64 = false;
    if (value.limb[2] == 0) {
        if (value.limb[1] == 0 && value.limb[0] < 2U) {
            return value.limb[0];
        }
        // Start at or above the root: the exact root of the top (up to 64) bits, plus one, scaled
        // back. With value >> 2k = t, value < (t + 1) 4^k <= (isqrt(t) + 1)^2 4^k.
        const unsigned bits = bit_length(value);
        const unsigned half_shift = bits > 64U ? (bits - 63U) / 2U : 0U;
        const unsigned shift = half_shift * 2U;
        const std::uint64_t top = shift == 0 ? value.limb[0]
            : shift >= 64U ? value.limb[1] >> (shift - 64U)
                           : (value.limb[0] >> shift) | (value.limb[1] << (64U - shift));
        std::uint64_t root = floor_sqrt_u64(top);
        if (half_shift != 0) {
            root = root + 1U > (std::numeric_limits<std::uint64_t>::max() >> half_shift)
                ? std::numeric_limits<std::uint64_t>::max()
                : (root + 1U) << half_shift;
        }
        while (true) {
            UInt192 quotient{};
            UInt192 remainder{};
            divide(value, from_u64(root), quotient, remainder);
            // quotient <= value / isqrt(value) < 2^65 only when root is below the root; the
            // iteration stays at or above it, so the quotient fits one limb.
            const std::uint64_t q = quotient.limb[0];
            const std::uint64_t next = (root >> 1U) + (q >> 1U) + (root & q & 1U);
            if (quotient.limb[1] != 0 || next >= root) {
                break;
            }
            root = next;
        }
        // The binary search's flag: a root of the largest u64 below the value's root.
        exceeds_u64 = root == std::numeric_limits<std::uint64_t>::max() && compare(multiply_u64(root, root), value) < 0;
        return root;
    }
    std::uint64_t low = 0;
    std::uint64_t high = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t answer = 0;
    while (low <= high) {
        const std::uint64_t middle = low + ((high - low) / 2U);
        const UInt192 square = multiply_u64(middle, middle);
        if (compare(square, value) <= 0) {
            answer = middle;
            if (middle == std::numeric_limits<std::uint64_t>::max()) {
                break;
            }
            low = middle + 1U;
        } else {
            if (middle == 0) {
                break;
            }
            high = middle - 1U;
        }
    }
    UInt192 next_square{};
    exceeds_u64 = answer == std::numeric_limits<std::uint64_t>::max()
        && compare(multiply_u64(answer, answer), value) < 0;
    static_cast<void>(next_square);
    return answer;
}

} // namespace eawr::sim::math::detail
