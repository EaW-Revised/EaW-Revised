#pragma once

// Integer-only decimal <-> binary64 conversion for the authoritative Lua
// profile. Parsing is correctly rounded (nearest, ties to even); formatting
// prints the exact binary value rounded once to the requested digit, ties to
// even. See docs/lua-numeric-profile.md.

#include "eawr/script/numeric/binary64.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::script::numeric::decimal {

struct ParseResult {
    binary64::Bits value{binary64::positive_zero};
    // Bytes consumed, including leading whitespace; zero means no conversion
    // (strtod leaves its end pointer at the start in that case).
    std::size_t consumed{0};
};

// strtod-compatible prefix parse of the retail-era Microsoft CRT grammar:
//   [ascii-space]* [+-]? (digits [. digits?] | . digits) ([eEdD] [+-]? digits)?
// An exponent marker without digits is not consumed. There is no hexadecimal,
// infinity or NaN syntax. Overflow gives a signed infinity; underflow rounds
// to a subnormal or signed zero.
[[nodiscard]] ParseResult parse_prefix(std::string_view text) noexcept;

// Lua 5.0 luaO_str2d: the whole text must be one number, optionally surrounded
// by ASCII whitespace.
[[nodiscard]] std::optional<binary64::Bits> parse_lua_number(std::string_view text) noexcept;

struct FormatSpec {
    char conversion{'g'}; // one of e E f g G
    int precision{-1};    // negative selects the C default of 6
    int width{0};
    bool left_align{false};
    bool plus{false};
    bool space{false};
    bool alternate{false};
    bool zero_pad{false};
};

// Largest precision accepted by format_to; larger requests are clamped. The
// Lua string library itself limits precision to two digits.
inline constexpr int max_precision = 1100;
// A buffer of this size holds any result with width <= max_format_width.
inline constexpr std::size_t max_format_width = 1200;
inline constexpr std::size_t format_buffer_size = 1536;

// printf-style (C99) conversion of one value. Infinity and NaN print as
// "inf"/"nan" ("INF"/"NAN" for E and G) with a '-' when the sign bit is set.
// Writes at most capacity - 1 characters plus a terminator and returns the
// untruncated length.
std::size_t format_to(char* buffer, std::size_t capacity, binary64::Bits value, const FormatSpec& spec) noexcept;

[[nodiscard]] std::string format(binary64::Bits value, const FormatSpec& spec);

// Lua 5.0 tostring / concatenation profile: "%.14g".
inline constexpr std::size_t lua_number_buffer_size = 32;
std::size_t format_lua_number(char* buffer, std::size_t capacity, binary64::Bits value) noexcept;
[[nodiscard]] std::string format_lua_number(binary64::Bits value);

// Shortest "%.<p>g" text, 1 <= p <= 17, that parses back to the same bits;
// intended for diagnostics and canonical text, not Lua-visible conversion.
[[nodiscard]] std::string format_shortest(binary64::Bits value);

} // namespace eawr::script::numeric::decimal
