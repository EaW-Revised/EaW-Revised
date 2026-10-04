#include "eawr/presentation/animation/idle_playback.hpp"

#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <utility>

namespace eawr::presentation::animation {
namespace {

constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();

bool space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

// (a * b) mod m without a wider type: shift-and-add, every sum kept below m.
std::uint64_t multiply_mod(std::uint64_t a, std::uint64_t b, const std::uint64_t m) noexcept {
    std::uint64_t result = 0;
    a %= m;
    b %= m;
    while (b != 0) {
        if ((b & 1U) != 0) result = result >= m - a ? result - (m - a) : result + a;
        a = a >= m - a ? a - (m - a) : a + a;
        b >>= 1U;
    }
    return result;
}

} // namespace

std::optional<RateMod> parse_rate_mod(std::string_view text) noexcept {
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    constexpr std::size_t most_fraction_digits = 6;
    std::uint64_t numerator = 0;
    std::uint64_t denominator = 1;
    std::size_t digits = 0;
    bool fraction = false;
    std::size_t fraction_digits = 0;
    for (const char value : text) {
        if (value == '.' && !fraction) {
            fraction = true;
            continue;
        }
        if (value < '0' || value > '9') return std::nullopt;
        if (fraction && ++fraction_digits > most_fraction_digits) return std::nullopt;
        if (numerator > (most - 9U) / 10U) return std::nullopt;
        numerator = numerator * 10U + static_cast<std::uint64_t>(value - '0');
        if (fraction) denominator *= 10U;
        ++digits;
    }
    if (digits == 0) return std::nullopt;
    const std::uint64_t common = numerator == 0 ? denominator : std::gcd(numerator, denominator);
    numerator /= common;
    denominator /= common;
    if (numerator > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return RateMod{static_cast<std::uint32_t>(numerator), static_cast<std::uint32_t>(denominator)};
}

std::uint32_t idle_start_frame(const std::string_view identity, const std::uint32_t frames) noexcept {
    if (frames == 0) return 0;
    // FNV-1a 64, then the splitmix64 finaliser so neighbouring records spread.
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char value : identity) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 0x100000001b3ULL;
    }
    hash ^= hash >> 30U;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27U;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31U;
    return static_cast<std::uint32_t>(hash % frames);
}

std::optional<ClipPosition> idle_position(const IdlePlayback& playback, const std::uint32_t start_frame,
                                          const std::uint32_t playable_frames, const std::uint32_t frames_per_second,
                                          const std::uint64_t tick, const std::uint32_t ticks_per_second) noexcept {
    if (frames_per_second == 0 || ticks_per_second == 0 || playback.rate.denominator == 0) return std::nullopt;
    // One frame is ticks * denominator positions, so a tick moves the clip by
    // fps * numerator positions at Rate_Mod and fps * denominator at rate 1.
    const std::uint64_t subdivisions = static_cast<std::uint64_t>(ticks_per_second) * playback.rate.denominator;
    if (subdivisions > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    ClipPosition result{0, static_cast<std::uint32_t>(subdivisions)};
    if (playable_frames == 0) return result;
    if (playable_frames > most / subdivisions) return std::nullopt;
    const std::uint64_t period = playable_frames * subdivisions;
    const std::uint64_t speed = static_cast<std::uint64_t>(frames_per_second) * playback.rate.numerator;
    const std::uint64_t restart_speed = static_cast<std::uint64_t>(frames_per_second) * playback.rate.denominator;
    if (speed > most - period) return std::nullopt;
    const std::uint64_t start = playback.random_start ? (start_frame % playable_frames) * subdivisions : 0U;
    if (playback.loop) {
        // start and the advance both lie below period; add them without wrapping 64 bits.
        const std::uint64_t advance = multiply_mod(tick, speed, period);
        result.position = advance >= period - start ? advance - (period - start) : start + advance;
        return result;
    }
    // The first pass ends on the first tick at or past the clip's end.
    const std::uint64_t remaining = period - start;
    const std::uint64_t end_tick = speed == 0 ? most : remaining / speed + (remaining % speed != 0 ? 1U : 0U);
    if (tick < end_tick) {
        result.position = start + tick * speed;  // below remaining + speed <= period + speed
        return result;
    }
    result.position = playback.restarts ? multiply_mod(tick - end_tick, restart_speed, period) : period;
    return result;
}

core::Result<Pose> sample_idle(const Player& player, const IdlePlayback& playback, const std::uint32_t start_frame,
                               const std::uint64_t tick, const std::uint32_t ticks_per_second) {
    Pose output;
    auto sampled = sample_idle(player, playback, start_frame, tick, ticks_per_second, output);
    if (!sampled) return core::Result<Pose>::failure(std::move(sampled.error()));
    return core::Result<Pose>::success(std::move(output));
}

core::Result<void> sample_idle(const Player& player, const IdlePlayback& playback, const std::uint32_t start_frame,
    const std::uint64_t tick, const std::uint32_t ticks_per_second, Pose& output) {
    const auto refuse = [](std::string message) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::invalid_request);
        diagnostic.message = std::move(message);
        return core::Result<void>::failure(std::move(diagnostic));
    };
    const float rate = player.frames_per_second();
    if (!(rate >= 1.0F) || rate >= static_cast<float>(std::numeric_limits<std::uint32_t>::max())
        || std::floor(rate) != rate) {
        return refuse("idle sampling requires a positive integral frame rate");
    }
    const auto position = idle_position(playback, start_frame, player.playable_frames(),
                                        static_cast<std::uint32_t>(rate), tick, ticks_per_second);
    if (!position) return refuse("idle clip position does not fit 64 bits or the tick rate is zero");
    return player.sample_position(position->position, position->subdivisions, output);
}

} // namespace eawr::presentation::animation
