#pragma once

#include "eawr/sim/math/fixed.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// Ion stun (#561, docs/behaviour/space-damage.md IS-01 to IS-09): what an ion shot leaves on the
// unit it hits, and what the stun does to that unit's weapons and speed. Pure functions of Q24
// values and frames: no clock, thread or host input.
namespace eawr::sim::tactical {

// The stun a projectile applies when it hits (IS-01). Content: carried by the shot profile and
// by the projectile in flight.
struct IonStunShot {
    std::uint32_t frames{};        // trunc(Projectile_Ion_Stun_Duration x 30), at least 1
    math::Fixed speed_reduction{}; // Projectile_Ion_Stun_Speed_Reduction_Percent, in [0, 1]
    math::Fixed rate_reduction{};  // Projectile_Ion_Stun_Shot_Rate_Reduction_Percent, in [0, 1]
    bool stack{};                  // Projectile_Ion_Stun_Stack_Duration
    friend constexpr bool operator==(const IonStunShot&, const IonStunShot&) noexcept = default;
};

// A unit's ion stun (hashed state while the unit has one). The unit is stunned while the frame
// is before `end_frame` (IS-03).
struct IonStunState {
    std::uint64_t end_frame{};
    math::Fixed speed_reduction{};
    math::Fixed rate_reduction{};
    friend constexpr bool operator==(const IonStunState&, const IonStunState&) noexcept = default;
};

inline constexpr std::uint32_t max_ion_stun_frames = 30U * 3600U;

// A shot's stun is valid when it lasts 1 to max_ion_stun_frames frames and both reductions are
// in [0, 1].
[[nodiscard]] bool valid_ion_stun(const IonStunShot& shot) noexcept;

// IS-03, IS-04: the unit's stun after a hit at `frame`. A stacking shot on a unit whose stun has
// not ended before `frame` adds its frames to the end; any other shot ends the stun its frames
// after `frame`. The reductions are the latest shot's (IS-05).
[[nodiscard]] IonStunState ion_stun(
    const std::optional<IonStunState>& current, const IonStunShot& shot, std::uint64_t frame) noexcept;

[[nodiscard]] bool ion_stunned(const std::optional<IonStunState>& state, std::uint64_t frame) noexcept;

// IS-05: the factor on the unit's maximum speed, 1 minus the speed reduction while stunned.
[[nodiscard]] math::Fixed ion_speed_factor(const std::optional<IonStunState>& state, std::uint64_t frame) noexcept;
// IS-06: the unit's fire rate, 1 minus the shot rate reduction while stunned.
[[nodiscard]] math::Fixed ion_fire_rate(const std::optional<IonStunState>& state, std::uint64_t frame) noexcept;

// IS-06: a hardpoint's recharge after a burst under fire rate `rate`: divided by the rate when it
// is in (0, 1), rounded half up; otherwise unchanged.
[[nodiscard]] std::uint32_t fire_rate_recharge(std::uint32_t frames, math::Fixed rate) noexcept;
// IS-06: the shots of the next burst: the pulse count times the rate (at least 0), rounded half up.
[[nodiscard]] std::uint32_t fire_rate_pulses(std::uint32_t pulses, math::Fixed rate) noexcept;
// IS-06: the gap between two shots of a burst: zero at a rate of zero or less, otherwise divided
// by the rate, rounded half up.
[[nodiscard]] std::uint32_t fire_rate_gap(std::uint32_t frames, math::Fixed rate) noexcept;

// The canonical record of a unit's stun (docs/replay-format.md).
void append_ion_stun(std::vector<std::uint8_t>& bytes, const IonStunState& state);

} // namespace eawr::sim::tactical
