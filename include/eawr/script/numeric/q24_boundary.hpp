#pragma once

// Crossing between authoritative Lua numbers and the Q24 simulation.
// Lua -> Q24 rounds once to the nearest 2^-24 multiple, ties to even, and
// rejects infinities, NaN and values outside the Fixed range. Q24 -> Lua is
// exact for |raw| <= 2^53 and rounds to nearest-even above.

#include "eawr/core/result.hpp"
#include "eawr/script/numeric/lua_number.hpp"
#include "eawr/sim/math/fixed.hpp"

#include <cstdint>
#include <string_view>

namespace eawr::script::numeric {

namespace diagnostic_codes {
inline constexpr std::string_view not_finite = "EAWR-SCRIPT-0101";
inline constexpr std::string_view out_of_range = "EAWR-SCRIPT-0102";
inline constexpr std::string_view not_integral = "EAWR-SCRIPT-0103";
} // namespace diagnostic_codes

[[nodiscard]] core::Result<sim::math::Fixed> to_fixed(LuaNumber value);
[[nodiscard]] LuaNumber from_fixed(sim::math::Fixed value) noexcept;

// Integer-valued binding arguments (counts, stable IDs, ticks) are validated
// exactly: finite, integral and within int64. No rounding is applied.
[[nodiscard]] core::Result<std::int64_t> to_exact_integer(LuaNumber value);

} // namespace eawr::script::numeric
