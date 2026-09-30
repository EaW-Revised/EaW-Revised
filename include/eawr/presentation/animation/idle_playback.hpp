#pragma once

#include "eawr/core/result.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// Idle playback of a placed object's first idle clip (Idle_Anim_00), as the
// retail game starts it when the object's model is created, on land and in
// space (#145, #157): the clip
// plays at Idle_Anim_00_Rate_Mod from a random frame and loops when
// Loop_Idle_Anim_00 is set. A clip that does not loop is restarted from frame
// 0 at rate 1 by the IDLE behaviour once it has finished; an object without
// that behaviour holds the last frame. A space DUMMY_STARSHIP does not take
// the random start. The facts are recorded in
// docs/asset-formats.md#idle-clip-playback. Engine-free; presentation only.
namespace eawr::presentation::animation {

// Idle_Anim_00_Rate_Mod as the exact decimal fraction it spells, reduced.
struct RateMod final {
    std::uint32_t numerator{1};
    std::uint32_t denominator{1};

    friend bool operator==(const RateMod&, const RateMod&) = default;
};

// Decimal text such as "1.0", " 0.25 " or "8": digits with an optional
// fraction of at most six digits, surrounding whitespace allowed. Nullopt for
// anything else (sign, exponent, empty, or a value past the uint32 range).
[[nodiscard]] std::optional<RateMod> parse_rate_mod(std::string_view text) noexcept;

struct IdlePlayback final {
    bool loop{};              // Loop_Idle_Anim_00
    bool restarts{};          // the object has the IDLE behaviour
    bool random_start{true};  // false for a space DUMMY_STARSHIP
    RateMod rate{};           // Idle_Anim_00_Rate_Mod, 1 when absent

    friend bool operator==(const IdlePlayback&, const IdlePlayback&) = default;
};

// The stand-in for retail's random start frame: uniform over [0, frames - 1],
// taken from a hash of `identity` (the placement's map path and TED record),
// so every capture of a placement starts its clip at the same frame. 0 when
// `frames` is 0.
[[nodiscard]] std::uint32_t idle_start_frame(std::string_view identity, std::uint32_t frames) noexcept;

// A clip position in 1/subdivisions frames, for Player::sample_position.
struct ClipPosition final {
    std::uint64_t position{};
    std::uint32_t subdivisions{1};

    friend bool operator==(const ClipPosition&, const ClipPosition&) = default;
};

// The position at tick `tick` of a clock running at `ticks_per_second`, for
// a clip of `playable_frames` at the integral `frames_per_second` that started
// at `start_frame` (taken modulo the playable frames). The result lies in
// [0, playable_frames * subdivisions]; the end is reached only by a held clip.
// A zero Rate_Mod freezes the clip at its start frame. Nullopt when the frame
// rate or tick rate is zero or the arithmetic does not fit 64 bits.
[[nodiscard]] std::optional<ClipPosition> idle_position(
    const IdlePlayback& playback, std::uint32_t start_frame, std::uint32_t playable_frames,
    std::uint32_t frames_per_second, std::uint64_t tick, std::uint32_t ticks_per_second) noexcept;

// The pose of `player`'s clip at that tick: idle_position for the clip's own
// playable frames and frame rate, sampled by Player::sample_position. It
// fails with EAWR-ANIMATION-0004 for a frame rate that is not a positive
// whole number or when idle_position has no result.
[[nodiscard]] core::Result<Pose> sample_idle(const Player& player, const IdlePlayback& playback,
                                             std::uint32_t start_frame, std::uint64_t tick,
                                             std::uint32_t ticks_per_second);

} // namespace eawr::presentation::animation
