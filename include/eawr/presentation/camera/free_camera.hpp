#pragma once

#include "eawr/core/result.hpp"

#include <array>
#include <span>
#include <string_view>

// Deterministic, engine-independent cinematic free camera.
//
// This is local project policy, not a reconstruction of the original game.
// The `Unlocked` XML camera definition does not describe free-flight
// bindings, speeds, look sensitivity or collision, so every tunable below is
// supplied by a project-authored settings block (see
// docs/behaviour/tactical-camera-input.md). Nothing here clamps the eye to a
// map: legal map bounds belong to the typed #26 contract, which is not
// available to this module.
//
// Numeric policy and purity match camera.hpp: presentation-only `float`, no
// engine type, no I/O, no global state, and no value ever re-enters the
// simulation.
//
// Orientation uses the same convention as `eye_position`: Y is up, pitch is
// measured in degrees *down* from the horizon, and at yaw 0 the view looks
// along -Z with screen-right along +X. A tactical camera's yaw and pitch can
// therefore be adopted unchanged, so entering free flight does not turn the
// view.
namespace eawr::presentation::camera {

// Pitch is kept strictly inside this magnitude so the view never becomes
// parallel to the world up axis, where a look-at basis is undefined.
inline constexpr float free_pitch_limit_degrees = 89.0F;

// Project-authored free-flight tuning. No field has a retail source.
struct FreeCameraSettings final {
    // World units per second along the view plane (forward/back, strafe).
    float move_speed{};
    // World units per second along world up (rise/descend).
    float vertical_speed{};
    // Degrees of yaw or pitch per look unit (one pointer pixel for the
    // viewer's mouse-motion bindings, before the binding's own scale).
    float look_degrees_per_unit{};
    // Inclusive pitch bounds, degrees down from the horizon.
    float pitch_min_degrees{};
    float pitch_max_degrees{};

    friend bool operator==(const FreeCameraSettings&, const FreeCameraSettings&) = default;
};

struct FreeCameraPose final {
    std::array<float, 3> eye{};
    // Always wrapped into [-180, 180).
    float yaw_degrees{};
    // Always inside [pitch_min_degrees, pitch_max_degrees].
    float pitch_degrees{};

    friend bool operator==(const FreeCameraPose&, const FreeCameraPose&) = default;
};

// One step of intent. Translation axes are dimensionless and each must lie in
// [-1, 1]: +strafe moves screen-right, +forward moves along the view
// direction, +rise moves along world up. Look units are unbounded finite
// deltas: +look_yaw turns the view right, +look_pitch tilts it down.
struct FreeCameraIntent final {
    float strafe{};
    float forward{};
    float rise{};
    float look_yaw_units{};
    float look_pitch_units{};

    friend bool operator==(const FreeCameraIntent&, const FreeCameraIntent&) = default;
};

namespace diagnostic_codes {
inline constexpr std::string_view invalid_free_settings = "EAWR-CAMERA-0301";
inline constexpr std::string_view invalid_free_request = "EAWR-CAMERA-0302";
} // namespace diagnostic_codes

// Rejects settings that cannot drive a free camera: any non-finite value, a
// non-positive speed or look rate, or pitch bounds that are inverted, empty
// or outside [-free_pitch_limit_degrees, free_pitch_limit_degrees].
[[nodiscard]] core::Result<void> validate(const FreeCameraSettings& settings);

// Wraps a finite yaw into [-180, 180). Non-finite input is returned unchanged;
// callers validate before wrapping.
[[nodiscard]] float wrap_yaw_degrees(float yaw_degrees) noexcept;

// Unit view direction and unit screen-right direction for a pose. Right is
// always horizontal, so it stays orthogonal to the view direction.
[[nodiscard]] core::Result<std::array<float, 3>> free_camera_forward(const FreeCameraPose& pose);
[[nodiscard]] core::Result<std::array<float, 3>> free_camera_right(const FreeCameraPose& pose);

// Builds the starting pose from an existing camera: the eye is copied, yaw is
// wrapped, and pitch is copied. A pitch outside the settings' bounds is
// rejected rather than clamped, because clamping would visibly turn the view
// at the moment of entry.
[[nodiscard]] core::Result<FreeCameraPose> enter_free_camera(
    const FreeCameraSettings& settings, std::span<const float, 3> eye, float yaw_degrees,
    float pitch_degrees);

// Advances one step. Look is applied first (yaw wraps, pitch clamps into the
// settings' bounds), then translation along the updated basis. Translation
// has two steps:
//   1. The intent vector is scaled down to unit length when longer; shorter
//      (analog) intent is kept. Strafe and forward are then multiplied by
//      move_speed and rise by vertical_speed.
//   2. A pitched view direction is not orthogonal to world up, so forward
//      and a vertical axis can reinforce. The world displacement is capped
//      at the length it would have on orthogonal axes,
//      sqrt((strafe^2 + forward^2) * move_speed^2 + rise^2 * vertical_speed^2)
//      times the duration, by scaling the whole step uniformly. Axes that
//      oppose (a downward forward plus rise) are left shorter, not boosted.
// So one axis alone is never scaled by the cap, no step exceeds
// max(move_speed, vertical_speed), and negating the intent negates the
// displacement. Look deltas are not time scaled, so a zero duration still
// applies look but never translates.
//
// Every input is validated before any arithmetic is kept: a non-finite or
// negative duration, a non-finite intent, a translation axis outside [-1, 1],
// an invalid pose or settings, or a result that would overflow to a
// non-finite value is rejected with no new pose.
[[nodiscard]] core::Result<FreeCameraPose> advance_free_camera(
    const FreeCameraSettings& settings, const FreeCameraPose& pose,
    const FreeCameraIntent& intent, float delta_seconds);

// Stateful wrapper for hosts. It owns validated settings and the current
// pose, and `advance` commits a new pose only when the whole step succeeds.
class FreeCameraController final {
public:
    [[nodiscard]] static core::Result<FreeCameraController> create(
        const FreeCameraSettings& settings, std::span<const float, 3> eye,
        float yaw_degrees, float pitch_degrees);

    [[nodiscard]] core::Result<void> advance(const FreeCameraIntent& intent,
                                             float delta_seconds);

    [[nodiscard]] const FreeCameraSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] const FreeCameraPose& pose() const noexcept { return pose_; }

private:
    FreeCameraController(const FreeCameraSettings& settings, const FreeCameraPose& pose) noexcept
        : settings_(settings), pose_(pose) {}

    FreeCameraSettings settings_;
    FreeCameraPose pose_;
};

} // namespace eawr::presentation::camera
