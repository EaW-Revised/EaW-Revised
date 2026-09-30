#pragma once

// lua_Number of the authoritative Lua profile. It is a trivial 64-bit word in
// the active backend's representation (the binary64 interchange encoding for
// option B), so it fits Lua's TValue union and compiled chunks keep their
// 8-byte number layout. Arithmetic and comparison operators forward to the
// backend. There is deliberately no conversion from or to floating types:
// upstream code that tries one does not compile.

#include "eawr/script/numeric/backend.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace eawr::script::numeric {

template <typename T>
concept LuaIntegral = std::integral<T> && !std::same_as<T, bool>;

struct LuaNumber {
    std::uint64_t repr;

    LuaNumber() = default;

    // Integer conversion is exact up to 2^53 and rounds to nearest-even above.
    template <LuaIntegral Integer>
    constexpr LuaNumber(Integer value) noexcept // NOLINT(google-explicit-constructor): C conversions in Lua
        : repr(std::is_signed_v<Integer> ? ActiveBackend::from_int64(static_cast<std::int64_t>(value))
                                         : ActiveBackend::from_uint64(static_cast<std::uint64_t>(value))) {}

    // C's (T)x as compiled for x64 FoC (Win64: int and long are 32 bits).
    // Signed targets truncate to int32 (cvttsd2si): NaN and values outside
    // int32 give INT32_MIN, sign-extended into T. Unsigned targets truncate to
    // int64 (NaN and out-of-range give INT64_MIN) and reduce modulo 2^N.
    template <LuaIntegral Integer>
    constexpr explicit operator Integer() const noexcept {
        const std::int64_t truncated = ActiveBackend::to_int64_truncate(repr);
        if constexpr (std::is_signed_v<Integer>) {
            constexpr std::int64_t low = -2147483647 - 1;
            constexpr std::int64_t high = 2147483647;
            return static_cast<Integer>(truncated < low || truncated > high ? low : truncated);
        } else {
            return static_cast<Integer>(static_cast<std::uint64_t>(truncated));
        }
    }

    [[nodiscard]] static constexpr LuaNumber from_repr(std::uint64_t value) noexcept {
        LuaNumber number;
        number.repr = value;
        return number;
    }

    constexpr LuaNumber& operator+=(LuaNumber other) noexcept {
        repr = ActiveBackend::add(repr, other.repr);
        return *this;
    }
    constexpr LuaNumber& operator-=(LuaNumber other) noexcept {
        repr = ActiveBackend::subtract(repr, other.repr);
        return *this;
    }
    // ipairs increments its index.
    constexpr LuaNumber& operator++() noexcept { return *this += LuaNumber(1); }
    constexpr LuaNumber operator++(int) noexcept {
        const LuaNumber previous = *this;
        *this += LuaNumber(1);
        return previous;
    }
};

static_assert(std::is_trivial_v<LuaNumber>);
static_assert(std::is_standard_layout_v<LuaNumber>);
static_assert(sizeof(LuaNumber) == 8);

[[nodiscard]] constexpr LuaNumber operator+(LuaNumber left, LuaNumber right) noexcept {
    return LuaNumber::from_repr(ActiveBackend::add(left.repr, right.repr));
}
[[nodiscard]] constexpr LuaNumber operator-(LuaNumber left, LuaNumber right) noexcept {
    return LuaNumber::from_repr(ActiveBackend::subtract(left.repr, right.repr));
}
[[nodiscard]] inline LuaNumber operator*(LuaNumber left, LuaNumber right) noexcept {
    return LuaNumber::from_repr(ActiveBackend::multiply(left.repr, right.repr));
}
[[nodiscard]] inline LuaNumber operator/(LuaNumber left, LuaNumber right) noexcept {
    return LuaNumber::from_repr(ActiveBackend::divide(left.repr, right.repr));
}
[[nodiscard]] constexpr LuaNumber operator-(LuaNumber value) noexcept {
    return LuaNumber::from_repr(ActiveBackend::negate(value.repr));
}
[[nodiscard]] constexpr bool operator==(LuaNumber left, LuaNumber right) noexcept {
    return ActiveBackend::equal(left.repr, right.repr);
}
[[nodiscard]] constexpr bool operator<(LuaNumber left, LuaNumber right) noexcept {
    return ActiveBackend::less(left.repr, right.repr);
}
[[nodiscard]] constexpr bool operator<=(LuaNumber left, LuaNumber right) noexcept {
    return ActiveBackend::less_equal(left.repr, right.repr);
}
[[nodiscard]] constexpr bool operator>(LuaNumber left, LuaNumber right) noexcept {
    return ActiveBackend::less(right.repr, left.repr);
}
[[nodiscard]] constexpr bool operator>=(LuaNumber left, LuaNumber right) noexcept {
    return ActiveBackend::less_equal(right.repr, left.repr);
}

} // namespace eawr::script::numeric
