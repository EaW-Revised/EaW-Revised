#include "eawr/presentation/camera/retreat_camera.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace eawr::presentation::camera {
namespace {

constexpr float degrees_to_radians = 0.017453292519943295F;
constexpr float full_turn_degrees = 360.0F;

[[nodiscard]] core::Diagnostic failure(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool finite(const std::array<float, 3>& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

[[nodiscard]] std::array<float, 3> add(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

[[nodiscard]] std::array<float, 3> scaled(const std::array<float, 3>& a, const float s) noexcept {
    return {a[0] * s, a[1] * s, a[2] * s};
}

[[nodiscard]] float length(const std::array<float, 3>& a) noexcept {
    return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

// The ship shot's fixed camera and target (FS-R08): the camera sits beyond the destination by a quarter of the
// entry/destination span, offset sideways by 150 on the map plane and 70 up; the target sits the same quarter span
// on the other side of the destination.
[[nodiscard]] RetreatCameraPose ship_shot(const RetreatCameraInput& input) noexcept {
    const std::array<float, 3> along{input.destination[0] - input.entry[0], input.destination[1] - input.entry[1],
        input.destination[2] - input.entry[2]};
    const float quarter = length(along) * retreat_rules::ship_shot_span_fraction;
    // The sideways axis is the departure direction turned a quarter turn on the map plane (left of travel).
    float side_x = -input.departure_direction[1];
    float side_y = input.departure_direction[0];
    const float side_length = std::sqrt(side_x * side_x + side_y * side_y);
    if (side_length > 0.0F) { side_x /= side_length; side_y /= side_length; }
    const std::array<float, 3> side{side_x * retreat_rules::ship_shot_perpendicular,
        side_y * retreat_rules::ship_shot_perpendicular, retreat_rules::ship_shot_height};
    RetreatCameraPose pose;
    pose.shot = RetreatShot::ship;
    pose.eye = add(add(input.destination, scaled(input.departure_direction, quarter)), side);
    pose.target = add(input.destination, scaled(input.departure_direction, -quarter));
    return pose;
}

} // namespace

std::array<float, 3> retreat_departure_direction(const float pitch_degrees, const float yaw_degrees) noexcept {
    const float pitch = pitch_degrees * degrees_to_radians;
    const float yaw = yaw_degrees * degrees_to_radians;
    return {-std::cos(pitch) * std::cos(yaw), -std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
}

float retreat_orbit_degrees(const float frames_after_last_key) noexcept {
    if (!(frames_after_last_key > 0.0F)) return 0.0F;
    const float u = std::min(frames_after_last_key / retreat_rules::orbit_period_frames, 1.0F);
    // Trapezoid velocity profile: speed ramps up over the first `e` of the period, holds, and ramps down over the
    // last `e`; with e = 0.5 it never holds. The profile covers exactly one turn per period.
    const float e = std::clamp(retreat_rules::orbit_ease, 1.0e-3F, 0.5F);
    const float peak = 1.0F / (1.0F - e);
    float progress{};
    if (u < e) progress = peak * u * u / (2.0F * e);
    else if (u <= 1.0F - e) progress = peak * (e * 0.5F + (u - e));
    else progress = 1.0F - peak * (1.0F - u) * (1.0F - u) / (2.0F * e);
    return progress * full_turn_degrees;
}

RetreatShot retreat_shot_at(const RetreatCameraInput& input, const std::uint32_t k) noexcept {
    if (!input.station) return RetreatShot::ship;
    if (input.ship_shot) {
        if (k < retreat_rules::station_cut_frame) return RetreatShot::ship;
        return k <= retreat_rules::station_hold_last_frame ? RetreatShot::station_hold : RetreatShot::station_orbit;
    }
    return k <= retreat_rules::station_only_hold_last_frame ? RetreatShot::station_hold : RetreatShot::station_orbit;
}

core::Result<RetreatCameraPose> retreat_camera(
    const RetreatCameraInput& input, const std::uint32_t k, const Constants& unlocked, const float fraction) {
    using Result = core::Result<RetreatCameraPose>;
    if (!finite(input.entry) || !finite(input.destination) || !finite(input.departure_direction)
        || (input.station && !finite(*input.station))) {
        return Result::failure(failure(diagnostic_codes::invalid_retreat_camera_input,
            "retreat camera geometry must be finite"));
    }
    if (!input.ship_shot && !input.station) {
        return Result::failure(failure(diagnostic_codes::invalid_retreat_camera_input,
            "a retreat cinematic without a ship shot needs a station"));
    }
    if (!(unlocked.near_clip > 0.0F) || !(unlocked.far_clip > unlocked.near_clip)
        || !std::isfinite(unlocked.object_fade_begin) || !std::isfinite(unlocked.object_fade_end)
        || unlocked.object_fade_end < unlocked.object_fade_begin) {
        return Result::failure(failure(diagnostic_codes::invalid_retreat_camera_clip,
            "the Unlocked camera clip and fade values cannot describe a camera"));
    }
    const RetreatShot shot = retreat_shot_at(input, k);
    RetreatCameraPose pose;
    if (shot == RetreatShot::ship) {
        pose = ship_shot(input);
    } else {
        // FS-R09: the attached target is the station point and the camera sits at its fixed offset, ignoring the
        // station's facing. The orbit turns that offset about Z around the target.
        pose.shot = shot;
        pose.target = *input.station;
        std::array<float, 3> offset = retreat_rules::station_camera_offset;
        if (shot == RetreatShot::station_orbit) {
            const std::uint32_t last_key = input.ship_shot ? retreat_rules::station_hold_last_frame
                                                           : retreat_rules::station_only_hold_last_frame;
            const float radians = retreat_orbit_degrees(static_cast<float>(k - last_key) + std::clamp(fraction, 0.0F, 1.0F)) * degrees_to_radians;
            const float c = std::cos(radians);
            const float s = std::sin(radians);
            offset = {offset[0] * c - offset[1] * s, offset[0] * s + offset[1] * c, offset[2]};
        }
        pose.eye = add(pose.target, offset);
    }
    pose.near_clip = unlocked.near_clip;
    pose.far_clip = unlocked.far_clip;
    pose.object_fade_begin = unlocked.object_fade_begin;
    pose.object_fade_end = unlocked.object_fade_end;
    return Result::success(pose);
}

} // namespace eawr::presentation::camera
