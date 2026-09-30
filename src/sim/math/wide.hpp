#pragma once

#include <array>
#include <compare>
#include <cstdint>

namespace eawr::sim::math::detail {

struct UInt192 {
    std::array<std::uint64_t, 3> limb{};

    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return limb[0] == 0 && limb[1] == 0 && limb[2] == 0;
    }
    [[nodiscard]] constexpr bool odd() const noexcept { return (limb[0] & 1U) != 0; }
};

[[nodiscard]] int compare(UInt192 left, UInt192 right) noexcept;
[[nodiscard]] bool add_magnitude(UInt192& value, UInt192 addend) noexcept;
void subtract_magnitude(UInt192& value, UInt192 subtrahend) noexcept;
[[nodiscard]] bool increment(UInt192& value) noexcept;
[[nodiscard]] bool bit(UInt192 value, unsigned index) noexcept;
void set_bit(UInt192& value, unsigned index) noexcept;
[[nodiscard]] UInt192 from_u64(std::uint64_t value) noexcept;
[[nodiscard]] UInt192 shift_left_u64(std::uint64_t value, unsigned shift) noexcept;
[[nodiscard]] UInt192 multiply_u64(std::uint64_t left, std::uint64_t right) noexcept;
[[nodiscard]] bool multiply(UInt192 left, UInt192 right, UInt192& result) noexcept;
[[nodiscard]] bool multiply_by_u64(UInt192 left, std::uint64_t right, UInt192& result) noexcept;
void divide(UInt192 numerator, UInt192 denominator, UInt192& quotient, UInt192& remainder) noexcept;

struct SignedWide {
    UInt192 magnitude{};
    bool negative{false};

    [[nodiscard]] static SignedWide product(std::int64_t left, std::int64_t right) noexcept;
    [[nodiscard]] static SignedWide scaled(std::int64_t value, unsigned shift) noexcept;
    void add(SignedWide other) noexcept;
};

[[nodiscard]] std::uint64_t unsigned_magnitude(std::int64_t value) noexcept;
[[nodiscard]] bool rounded_divide_to_raw(
    SignedWide numerator, UInt192 denominator, std::int64_t& raw) noexcept;
[[nodiscard]] bool rounded_shift_to_raw(
    SignedWide numerator, unsigned shift, std::int64_t& raw) noexcept;
[[nodiscard]] bool magnitude_to_raw(UInt192 magnitude, bool negative, std::int64_t& raw) noexcept;
[[nodiscard]] std::uint64_t floor_sqrt(UInt192 value, bool& exceeds_u64) noexcept;

// 128-bit forms for the hot paths (#520), exact on every compiler: the target's 128-bit
// multiply and divide where it has them, 64-bit operations otherwise.
// The product (high:low) of two u64.
void multiply_64(std::uint64_t left, std::uint64_t right, std::uint64_t& high, std::uint64_t& low) noexcept;
// (high:low) / divisor and its remainder, for high < divisor (the quotient fits 64 bits).
[[nodiscard]] std::uint64_t divide_128(
    std::uint64_t high, std::uint64_t low, std::uint64_t divisor, std::uint64_t& remainder) noexcept;
// Floor of the square root of a u64, and of (high:low).
[[nodiscard]] std::uint64_t floor_sqrt_64(std::uint64_t value) noexcept;
[[nodiscard]] std::uint64_t floor_sqrt_128(std::uint64_t high, std::uint64_t low) noexcept;
// Whether the floor root `root` of (high:low) rounds up to the nearest integer root: the value
// exceeds root^2 + root, which is 4 value > (2 root + 1)^2.
[[nodiscard]] bool sqrt_rounds_up(std::uint64_t high, std::uint64_t low, std::uint64_t root) noexcept;

} // namespace eawr::sim::math::detail
