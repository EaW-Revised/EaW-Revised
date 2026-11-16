#pragma once

#include "eawr/core/result.hpp"

#include <array>
#include <cstdint>
#include <string_view>

// Pure model of the campaign space-battle ARRIVAL cinematic camera
// (docs/behaviour/gc-arrival-camera.md, SK-ARR-01..SK-ARR-14).
//
// The shots are built from constants of the debug build, not loaded from a camera XML file or a ship animation
// (SK-ARR-07). The keys are a handful of position keyframes with the debug build's own interpolation (cut, linear,
// Hermite spline, rotation about the target) and its trapezoid ease; this file restates that arithmetic and nothing
// else. Presentation only: a function of the cinematic frame k and the geometry it is given. It owns no engine type,
// keeps no state and reads no clock. Coordinates are source coordinates (X, Y on the map plane, Z up).
namespace eawr::presentation::camera {

namespace arrival_rules {
// SK-ARR-02/SK-ARR-03/SK-ARR-04, debug build. Frame numbers are cinematic frames k, 30 per second.
inline constexpr std::uint32_t attacker_hold_last_frame = 35;     // camera key: end of the fixed opening hold
inline constexpr std::uint32_t attacker_target_hold_frame = 23;   // target key: the target leaves the entry point
inline constexpr std::uint32_t last_key_frame = 120;              // every shot ends on its rest pose here
inline constexpr std::uint32_t duration_frames = 150;             // the 5 s cinematic timer at 30 frames per second
inline constexpr float duration_seconds = 5.0F;
inline constexpr float tension = 0.5F;                            // default spline tension of a key
inline constexpr float ease_half = 0.5F;
inline constexpr float shot_zero_span_fraction = 0.05F;
inline constexpr float shot_zero_perpendicular = 300.0F;
inline constexpr float shot_zero_height = 70.0F;
inline constexpr float shot_one_height = -70.0F;
inline constexpr float defender_height = 20.0F;
inline constexpr float near_clip = 1.0F;       // SK-ARR-05: the frustum set while the cinematic plays
inline constexpr float far_clip = 10000.0F;
// The hyperspace distance table (WR-38) that places the entry point: the full-speed span of frames 45..119 plus the
// slow-to-target span of frames 120..149.
inline constexpr float table_full_speed_per_frame = 8.125F;
inline constexpr std::uint32_t table_full_speed_first_frame = 45;
inline constexpr std::uint32_t table_slow_first_frame = 120;
} // namespace arrival_rules

namespace diagnostic_codes {
inline constexpr std::string_view invalid_arrival_camera_input = "EAWR-CAMERA-0603";
} // namespace diagnostic_codes

enum class ArrivalRole : std::uint8_t {
    attacker, // the local player is the invader: it watches its own fleet leave hyperspace
    defender, // the local player defends: a sweep around its star base
};

enum class ArrivalShot : std::uint8_t {
    attacker_near_destination, // shot 0: close to the destination, off to the side
    attacker_behind_entry,     // shot 1: beyond the entry point, a spline back to the rest pose
    defender_base_sweep,
};

[[nodiscard]] constexpr std::string_view to_string(const ArrivalShot shot) noexcept {
    switch (shot) {
    case ArrivalShot::attacker_near_destination: return "attacker_near_destination";
    case ArrivalShot::attacker_behind_entry: return "attacker_behind_entry";
    case ArrivalShot::defender_base_sweep: return "defender_base_sweep";
    }
    return "invalid";
}

struct ArrivalCameraInput final {
    ArrivalRole role{ArrivalRole::attacker};
    // Attacker only: which of the two shots (SK-ARR-03). 0 or 1; anything else is rejected.
    std::uint8_t shot_choice{0};
    // The arrival destination (the conflict lead's destination) and the unit direction the fleet departs along
    // (the lead's facing reversed). The entry point is destination + direction * arrival_entry_distance().
    std::array<float, 3> destination{};
    std::array<float, 3> departure_direction{-1.0F, 0.0F, 0.0F};
    // The tactical camera's eye when the cinematic starts. Every shot ends on it (SK-ARR-03/04).
    std::array<float, 3> rest_eye{};
    // Defender only: the look-at point (the defender's star base, or the camera's own look-at without one).
    std::array<float, 3> look_at{};
};

struct ArrivalCameraPose final {
    std::array<float, 3> eye{};
    std::array<float, 3> target{};
    ArrivalShot shot{ArrivalShot::attacker_near_destination};
    float near_clip{arrival_rules::near_clip};
    float far_clip{arrival_rules::far_clip};

    friend bool operator==(const ArrivalCameraPose&, const ArrivalCameraPose&) = default;
};

// The shot at cinematic frame `k`. `fraction` in [0, 1) is the presented time between frame k and k+1; the keyed
// motion is continuous in it, and it never changes which segment plays. Rejects a non-finite input, a zero-length
// departure direction and a shot choice other than 0 or 1.
[[nodiscard]] core::Result<ArrivalCameraPose> arrival_camera(
    const ArrivalCameraInput& input, std::uint32_t k, float fraction = 0.0F);

// SK-ARR-02: the distance from the destination back to the entry point, the sum of the full-speed span and the
// slow-to-target span of the hyperspace table (609.375 + the eased last 30 frames).
[[nodiscard]] float arrival_entry_distance() noexcept;

// SK-ARR-02: the entry point of an input: destination + departure direction * arrival_entry_distance().
[[nodiscard]] std::array<float, 3> arrival_entry_point(const ArrivalCameraInput& input) noexcept;

// SK-ARR-09: the attacker's shot choice from a seed. The debug build draws it from a shared cinematic random stream
// that the simulation never reads; this is a presentation-only stand-in, a hash of the battle seed, so a replayed
// battle shows the same shot. Returns 0 or 1.
[[nodiscard]] std::uint8_t arrival_shot_from_seed(std::uint64_t battle_seed) noexcept;

// The trapezoid ease of the debug build's cinematic keys: the fraction of the segment's distance covered at
// segment fraction `t` in [0, 1], speeding up over `ease_out` (of the first key) and slowing down over `ease_in`
// (of the second key). Exposed for tests.
[[nodiscard]] float arrival_ease(float t, float ease_out, float ease_in) noexcept;

// SK-ARR-01: the cinematic belongs to a battle that has not run yet. The debug build starts it from the post-load step of
// a battle that is not a loaded save; a battle restored mid-way (its first ready tick above zero) plays none. A battle
// restored from the prebattle autosave is ready at tick 0 and plays it (WGS-64 restores that presentation).
[[nodiscard]] constexpr bool arrival_admits_battle_ready_at(const std::uint64_t ready_tick) noexcept { return ready_tick == 0; }

// True while the cinematic owns the screen: frames 0..149 (the letterbox, the ignored input and the hidden command
// bar last the whole 5 s timer, the camera keys end earlier, at frame 120).
[[nodiscard]] constexpr bool arrival_cinematic_running(const std::uint32_t k) noexcept {
    return k < arrival_rules::duration_frames;
}

} // namespace eawr::presentation::camera
