#pragma once

#include "eawr/script/numeric/lua_number.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <bit>
#include <cstdint>

namespace eawr::script::foc::tactical_ai_detail {

// Round a positive integer significand to binary32's 24 bits, ties to even.
// Keep the exponent separately so rounding UINT64_MAX cannot overflow.
[[nodiscard]] inline std::uint64_t round_mode_significand(std::uint64_t value, int& exponent) noexcept {
    const auto width = static_cast<int>(std::bit_width(value));
    const int shift = width > 24 ? width - 24 : 0;
    exponent += shift;
    if (shift == 0) return value;
    auto rounded = value >> shift;
    const auto remainder = value & ((std::uint64_t{1} << shift) - 1);
    const auto half = std::uint64_t{1} << (shift - 1);
    if (remainder > half || (remainder == half && (rounded & 1))) ++rounded;
    if (rounded == (std::uint64_t{1} << 24)) {
        rounded >>= 1;
        ++exponent;
    }
    return rounded;
}

// FH-11: binary32(frame) * binary32(1 / 30), then exact widening to binary64.
// Both rounding steps use integers, unaffected by the host floating environment.
[[nodiscard]] inline numeric::LuaNumber mode_seconds(std::uint64_t frame) noexcept {
    static_assert(sim::tactical::logical_frames_per_second == 30);
    if (frame == 0) return numeric::LuaNumber(0);
    int exponent = 0;
    auto significand = round_mode_significand(frame, exponent);
    // The nearest-even binary32 inverse FPS is 8947849 * 2^-28.
    exponent -= 28;
    significand = round_mode_significand(significand * 8947849U, exponent);
    const auto top = static_cast<int>(std::bit_width(significand)) - 1;
    const auto fraction = (significand << (52 - top)) & ((std::uint64_t{1} << 52) - 1);
    return numeric::LuaNumber::from_repr((static_cast<std::uint64_t>(exponent + top + 1023) << 52) | fraction);
}

} // namespace eawr::script::foc::tactical_ai_detail
