#pragma once

// Numeric backend interface of the authoritative Lua profile. The Lua VM sees
// only LuaNumber (lua_number.hpp), whose operators forward to the selected
// backend; a backend is stateless and stores its values in one 64-bit word.
// Binary64Backend (owner decision #254, option B) is the active backend. A
// signed Q24 backend (option A) can implement the same concept without VM
// changes; it must also re-encode binary64 chunk constants through
// from_binary64 when chunks are loaded.

#include "eawr/script/numeric/binary64.hpp"
#include "eawr/script/numeric/decimal.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace eawr::script::numeric {

// Identifies every observable numeric rule of a backend: arithmetic, NaN and
// integer conversion rules, parsing grammar, formatting and the Q24 boundary.
// Any observable change requires a new version.
struct NumericAbi {
    std::string_view name;
    std::uint32_t version;

    friend constexpr bool operator==(const NumericAbi&, const NumericAbi&) = default;
};

enum class Q24Conversion : std::uint8_t {
    exact,        // the value is representable and was converted unchanged
    rounded,      // nearest, ties to even, onto the 2^-24 grid
    not_finite,   // infinity or NaN
    out_of_range, // outside [-2^39, 2^39 - 2^-24] after rounding
};

template <typename Backend>
concept LuaNumberBackend = requires(std::uint64_t value, std::int64_t integer, std::uint64_t unsigned_integer,
                                    std::string_view text, std::size_t& consumed, char* buffer,
                                    std::size_t capacity, const decimal::FormatSpec& spec, std::int64_t& raw) {
    { Backend::abi } -> std::convertible_to<NumericAbi>;
    { Backend::add(value, value) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::subtract(value, value) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::multiply(value, value) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::divide(value, value) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::negate(value) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::equal(value, value) } noexcept -> std::same_as<bool>;
    { Backend::less(value, value) } noexcept -> std::same_as<bool>;
    { Backend::less_equal(value, value) } noexcept -> std::same_as<bool>;
    { Backend::from_int64(integer) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::from_uint64(unsigned_integer) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::to_int64_truncate(value) } noexcept -> std::same_as<std::int64_t>;
    { Backend::parse_prefix(text, consumed) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::format(buffer, capacity, value, spec) } noexcept -> std::same_as<std::size_t>;
    { Backend::format_lua(buffer, capacity, value) } noexcept -> std::same_as<std::size_t>;
    { Backend::from_binary64(unsigned_integer) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::to_binary64(value) } noexcept -> std::same_as<std::uint64_t>;
    { Backend::to_q24(value, raw) } noexcept -> std::same_as<Q24Conversion>;
    { Backend::from_q24(integer) } noexcept -> std::same_as<std::uint64_t>;
};

// Q24 raw value (signed int64, 24 fractional bits) nearest to a binary64
// value, ties to even; see docs/lua-numeric-profile.md.
[[nodiscard]] constexpr Q24Conversion binary64_to_q24(binary64::Bits value, std::int64_t& raw) noexcept {
    raw = 0;
    if (!binary64::is_finite(value)) {
        return Q24Conversion::not_finite;
    }
    if (binary64::is_zero(value)) {
        return Q24Conversion::exact;
    }
    const std::int32_t biased = binary64::exponent_of(value);
    std::uint64_t significand = binary64::fraction_of(value);
    std::int32_t exponent2 = -1074;
    if (biased != 0) {
        significand |= binary64::hidden_bit;
        exponent2 = biased - 1075;
    }
    const std::int32_t scaled = exponent2 + 24; // raw = significand * 2^scaled
    const bool negative = binary64::sign_of(value);
    std::uint64_t magnitude = 0;
    bool inexact = false;
    if (scaled >= 0) {
        if (scaled > 63 || binary64::detail::count_leading_zeros(significand) < scaled) {
            return Q24Conversion::out_of_range;
        }
        magnitude = significand << static_cast<std::uint32_t>(scaled);
    } else if (scaled < -54) {
        inexact = true; // below half a quantum
    } else {
        const auto shift = static_cast<std::uint32_t>(-scaled);
        magnitude = significand >> shift;
        const std::uint64_t rest = significand & ((std::uint64_t{1} << shift) - 1U);
        const std::uint64_t half = std::uint64_t{1} << (shift - 1U);
        inexact = rest != 0;
        if (rest > half || (rest == half && (magnitude & 1U) != 0)) {
            ++magnitude;
        }
    }
    if (negative ? magnitude > 0x8000000000000000ULL : magnitude > 0x7FFFFFFFFFFFFFFFULL) {
        return Q24Conversion::out_of_range;
    }
    raw = static_cast<std::int64_t>(negative ? ~magnitude + 1U : magnitude);
    return inexact ? Q24Conversion::rounded : Q24Conversion::exact;
}

// binary64 nearest to raw * 2^-24 (exact when |raw| <= 2^53).
[[nodiscard]] constexpr binary64::Bits q24_to_binary64(std::int64_t raw) noexcept {
    const binary64::Bits integral = binary64::from_int64(raw);
    if (binary64::is_zero(integral)) {
        return binary64::positive_zero;
    }
    // |raw| >= 1 keeps the scaled value normal, so the exponent shift is exact.
    return integral - (binary64::Bits{24} << 52U);
}

struct Binary64Backend {
    static constexpr NumericAbi abi{"eawr-lua-binary64-soft", 1};

    [[nodiscard]] static constexpr std::uint64_t add(std::uint64_t left, std::uint64_t right) noexcept {
        return binary64::add(left, right);
    }
    [[nodiscard]] static constexpr std::uint64_t subtract(std::uint64_t left, std::uint64_t right) noexcept {
        return binary64::subtract(left, right);
    }
    [[nodiscard]] static std::uint64_t multiply(std::uint64_t left, std::uint64_t right) noexcept {
        return binary64::multiply(left, right);
    }
    [[nodiscard]] static std::uint64_t divide(std::uint64_t left, std::uint64_t right) noexcept {
        return binary64::divide(left, right);
    }
    [[nodiscard]] static constexpr std::uint64_t negate(std::uint64_t value) noexcept { return binary64::negate(value); }
    [[nodiscard]] static constexpr bool equal(std::uint64_t left, std::uint64_t right) noexcept {
        return binary64::equal(left, right);
    }
    [[nodiscard]] static constexpr bool less(std::uint64_t left, std::uint64_t right) noexcept {
        return binary64::less(left, right);
    }
    [[nodiscard]] static constexpr bool less_equal(std::uint64_t left, std::uint64_t right) noexcept {
        return binary64::less_equal(left, right);
    }
    [[nodiscard]] static constexpr std::uint64_t from_int64(std::int64_t value) noexcept {
        return binary64::from_int64(value);
    }
    [[nodiscard]] static constexpr std::uint64_t from_uint64(std::uint64_t value) noexcept {
        return binary64::from_uint64(value);
    }
    [[nodiscard]] static constexpr std::int64_t to_int64_truncate(std::uint64_t value) noexcept {
        return binary64::to_int64_truncate(value);
    }
    [[nodiscard]] static std::uint64_t parse_prefix(std::string_view text, std::size_t& consumed) noexcept {
        const decimal::ParseResult parsed = decimal::parse_prefix(text);
        consumed = parsed.consumed;
        return parsed.value;
    }
    static std::size_t format(char* buffer, std::size_t capacity, std::uint64_t value,
                              const decimal::FormatSpec& spec) noexcept {
        return decimal::format_to(buffer, capacity, value, spec);
    }
    static std::size_t format_lua(char* buffer, std::size_t capacity, std::uint64_t value) noexcept {
        return decimal::format_lua_number(buffer, capacity, value);
    }
    // Chunk constants keep their bits; a NaN payload becomes the canonical NaN.
    [[nodiscard]] static constexpr std::uint64_t from_binary64(std::uint64_t bits) noexcept {
        return binary64::canonicalize(bits);
    }
    [[nodiscard]] static constexpr std::uint64_t to_binary64(std::uint64_t value) noexcept { return value; }
    [[nodiscard]] static constexpr Q24Conversion to_q24(std::uint64_t value, std::int64_t& raw) noexcept {
        return binary64_to_q24(value, raw);
    }
    [[nodiscard]] static constexpr std::uint64_t from_q24(std::int64_t raw) noexcept { return q24_to_binary64(raw); }
};

static_assert(LuaNumberBackend<Binary64Backend>);

// The single selection point for the authoritative profile.
using ActiveBackend = Binary64Backend;

[[nodiscard]] constexpr NumericAbi numeric_abi() noexcept { return ActiveBackend::abi; }

} // namespace eawr::script::numeric
