#include "eawr/script/numeric/q24_boundary.hpp"

#include <limits>
#include <string>

namespace eawr::script::numeric {
namespace {

core::Diagnostic failure(std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    return diagnostic;
}

std::string describe(LuaNumber value) { return decimal::format_shortest(ActiveBackend::to_binary64(value.repr)); }

} // namespace

core::Result<sim::math::Fixed> to_fixed(LuaNumber value) {
    std::int64_t raw = 0;
    switch (ActiveBackend::to_q24(value.repr, raw)) {
    case Q24Conversion::exact:
    case Q24Conversion::rounded:
        return core::Result<sim::math::Fixed>::success(sim::math::Fixed::from_raw(raw));
    case Q24Conversion::not_finite:
        return core::Result<sim::math::Fixed>::failure(
            failure(diagnostic_codes::not_finite, "Lua number " + describe(value) + " has no Q24 value"));
    case Q24Conversion::out_of_range:
        break;
    }
    return core::Result<sim::math::Fixed>::failure(
        failure(diagnostic_codes::out_of_range, "Lua number " + describe(value) + " is outside the Q24 range"));
}

LuaNumber from_fixed(sim::math::Fixed value) noexcept {
    return LuaNumber::from_repr(ActiveBackend::from_q24(value.raw()));
}

core::Result<std::int64_t> to_exact_integer(LuaNumber value) {
    const binary64::Bits bits = ActiveBackend::to_binary64(value.repr);
    if (!binary64::is_finite(bits)) {
        return core::Result<std::int64_t>::failure(
            failure(diagnostic_codes::not_finite, "Lua number " + describe(value) + " is not an integer"));
    }
    if (binary64::round_to_integral(bits, binary64::Rounding::toward_zero) != bits) {
        return core::Result<std::int64_t>::failure(
            failure(diagnostic_codes::not_integral, "Lua number " + describe(value) + " is not an integer"));
    }
    // [-2^63, 2^63): the binary exponent of the magnitude is at most 62, or the
    // value is exactly -2^63.
    if (binary64::exponent_of(bits) >= 0x43E && bits != 0xC3E0000000000000ULL) {
        return core::Result<std::int64_t>::failure(
            failure(diagnostic_codes::out_of_range, "Lua number " + describe(value) + " is outside int64"));
    }
    return core::Result<std::int64_t>::success(binary64::to_int64_truncate(bits));
}

} // namespace eawr::script::numeric
