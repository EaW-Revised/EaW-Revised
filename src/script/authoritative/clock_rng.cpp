#include "eawr/script/authoritative/clock_rng.hpp"

#include "eawr/script/numeric/binary64.hpp"

namespace eawr::script::authoritative {
namespace {

namespace b64 = numeric::binary64;

constexpr std::uint32_t max_duration_term = 2048;
constexpr std::uint64_t exponent_mask = 0x7FF;
constexpr std::uint64_t fraction_mask = (std::uint64_t{1} << 52) - 1;
constexpr std::uint64_t two_to_minus_53 = 0x3CA0000000000000ULL;

constexpr std::uint64_t mix(std::uint64_t value) noexcept {
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

} // namespace

bool valid_tick_duration(const TickDuration duration) noexcept {
    return duration.numerator >= 1 && duration.numerator <= max_duration_term && duration.denominator >= 1 &&
        duration.denominator <= max_duration_term;
}

numeric::LuaNumber time_at_tick(const std::uint64_t tick, const TickDuration duration) noexcept {
    // tick * numerator < 2^51 is exact in binary64; the division rounds once.
    const auto elapsed = b64::from_uint64(tick * duration.numerator);
    return numeric::LuaNumber::from_repr(b64::divide(elapsed, b64::from_uint64(duration.denominator)));
}

std::optional<std::uint64_t> ticks_until(const numeric::LuaNumber seconds, const TickDuration duration) noexcept {
    const std::uint64_t bits = seconds.repr;
    const bool negative = (bits >> 63) != 0;
    const std::uint64_t exponent = (bits >> 52) & exponent_mask;
    const std::uint64_t fraction = bits & fraction_mask;
    if (exponent == 0 && fraction == 0) return 1; // +0 and -0
    if (negative || exponent == exponent_mask) return std::nullopt;
    // seconds = significand x 2^power, significand < 2^53.
    const std::uint64_t significand = exponent == 0 ? fraction : (fraction | (std::uint64_t{1} << 52));
    const int power = exponent == 0 ? -1074 : static_cast<int>(exponent) - 1075;
    // The deadline limit keeps seconds x denominator / numerator below 2^40,
    // so seconds < 2^51 and a normal significand gives power <= -2.
    if (power >= 0) return std::nullopt;
    const std::uint64_t scaled = significand * duration.denominator; // < 2^64
    const std::uint64_t quotient = scaled / duration.numerator + (scaled % duration.numerator != 0 ? 1 : 0);
    const int shift = -power;
    std::uint64_t ticks = 0;
    if (shift >= 64) {
        ticks = quotient != 0 ? 1 : 0;
    } else {
        ticks = (quotient >> shift) + ((quotient & ((std::uint64_t{1} << shift) - 1)) != 0 ? 1 : 0);
    }
    if (ticks == 0) ticks = 1;
    if (ticks >= max_script_tick) return std::nullopt;
    return ticks;
}

std::uint64_t random_word(
    const std::uint64_t seed,
    const std::uint64_t tick,
    const std::uint64_t instance,
    const std::uint64_t draw_index
) noexcept {
    std::uint64_t state = mix(seed ^ 0x6A09E667F3BCC909ULL);
    state = mix(state + tick * 0x9E3779B97F4A7C15ULL);
    state = mix(state ^ (instance * 0xD1B54A32D192ED03ULL));
    return mix(state + draw_index * 0xABC98388FB8FAC03ULL);
}

std::uint64_t RandomStream::next_word() noexcept { return random_word(seed_, tick_, instance_, draws_++); }

std::uint64_t RandomStream::next_below(const std::uint64_t bound) noexcept {
    if (bound == 0) return next_word();
    // Words below 2^64 mod bound are rejected so every residue is equally likely.
    const std::uint64_t threshold = (0 - bound) % bound;
    while (true) {
        const std::uint64_t word = next_word();
        if (word >= threshold) return word % bound;
    }
}

std::int64_t RandomStream::next_in_range(const std::int64_t low, const std::int64_t high) noexcept {
    const std::uint64_t span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low) + 1;
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + next_below(span));
}

numeric::LuaNumber RandomStream::next_unit() noexcept {
    const auto integral = b64::from_uint64(next_word() >> 11);
    return numeric::LuaNumber::from_repr(b64::multiply(integral, two_to_minus_53));
}

} // namespace eawr::script::authoritative
