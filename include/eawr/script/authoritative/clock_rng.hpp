#pragma once

// Script time and randomness of the authoritative Lua profile (#247,
// docs/lua-sandbox.md). Both are pure functions of simulation state: time is
// the completed tick count, randomness a counter-based stream keyed by the
// session seed, tick, script instance and draw index, so a draw never depends
// on how many draws other instances made in the same tick.

#include "eawr/script/numeric/lua_number.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace eawr::script::authoritative {

inline constexpr std::string_view rng_identity = "eawr-script-rng-v1";

// Seconds per tick as an exact fraction; numerator and denominator in 1..2048.
struct TickDuration {
    std::uint32_t numerator{1};
    std::uint32_t denominator{30};
};

inline constexpr std::uint64_t max_script_tick = std::uint64_t{1} << 40;

[[nodiscard]] bool valid_tick_duration(TickDuration duration) noexcept;

// GetCurrentTime: tick x duration, rounded once to binary64 (nearest, ties to
// even). Requires tick <= max_script_tick.
[[nodiscard]] numeric::LuaNumber time_at_tick(std::uint64_t tick, TickDuration duration) noexcept;

// The number of ticks until a positive duration has fully elapsed: the integer
// ceiling of seconds / tick duration, computed exactly, so a deadline never
// fires early. Zero (either sign) gives 1: the next service. Negative, NaN,
// infinite and durations of max_script_tick ticks or more give nullopt.
[[nodiscard]] std::optional<std::uint64_t> ticks_until(numeric::LuaNumber seconds, TickDuration duration) noexcept;

// The draw_index-th 64-bit word of an instance's stream in one tick.
[[nodiscard]] std::uint64_t random_word(
    std::uint64_t seed,
    std::uint64_t tick,
    std::uint64_t instance,
    std::uint64_t draw_index
) noexcept;

// Draw sequence of one instance in one tick. Bounded draws use rejection
// sampling: each rejected word consumes a draw index.
class RandomStream {
public:
    RandomStream(std::uint64_t seed, std::uint64_t tick, std::uint64_t instance, std::uint64_t draws = 0) noexcept
        : seed_(seed), tick_(tick), instance_(instance), draws_(draws) {}

    [[nodiscard]] std::uint64_t next_word() noexcept;
    // Uniform in [0, bound); bound 0 means the full 64-bit range.
    [[nodiscard]] std::uint64_t next_below(std::uint64_t bound) noexcept;
    // Uniform integer in [low, high]; low <= high.
    [[nodiscard]] std::int64_t next_in_range(std::int64_t low, std::int64_t high) noexcept;
    // (word >> 11) x 2^-53 in binary64: uniform over the 2^53 multiples of
    // 2^-53 in [0, 1), exact.
    [[nodiscard]] numeric::LuaNumber next_unit() noexcept;

    [[nodiscard]] std::uint64_t draws() const noexcept { return draws_; }

private:
    std::uint64_t seed_;
    std::uint64_t tick_;
    std::uint64_t instance_;
    std::uint64_t draws_;
};

} // namespace eawr::script::authoritative
