#pragma once

#include "eawr/script/numeric/decimal.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace eawr::script::numeric::decimal::internal {

using binary64::Bits;

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

constexpr std::int64_t floor_divide(std::int64_t numerator, std::int64_t denominator) noexcept {
    const std::int64_t quotient = numerator / denominator;
    return (numerator % denominator != 0 && (numerator < 0) != (denominator < 0)) ? quotient - 1 : quotient;
}

} // namespace eawr::script::numeric::decimal::internal
