#include "eawr/presentation/camera/free_camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace eawr::presentation::camera {
namespace {

constexpr float degrees_to_radians = 0.017453292519943295F;
constexpr float half_turn_degrees = 180.0F;
constexpr float full_turn_degrees = 360.0F;

[[nodiscard]] core::Diagnostic failure(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool finite(const float value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool finite(const std::array<float, 3>& value) noexcept {
    return finite(value[0]) && finite(value[1]) && finite(value[2]);
}

template <typename T>
[[nodiscard]] core::Result<T> reject_request(std::string message) {
    return core::Result<T>::failure(
        failure(diagnostic_codes::invalid_free_request, std::move(message)));
}

[[nodiscard]] bool pitch_in_bounds(const FreeCameraSettings& settings, const float pitch) noexcept {
    return finite(pitch) && pitch >= settings.pitch_min_degrees
        && pitch <= settings.pitch_max_degrees;
}

[[nodiscard]] core::Result<void> validate_pose(
    const FreeCameraSettings& settings, const FreeCameraPose& pose) {
    if (!finite(pose.eye) || !finite(pose.yaw_degrees)
        || !pitch_in_bounds(settings, pose.pitch_degrees)) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_free_request,
            "free camera pose needs a finite eye and yaw and a pitch inside the settings bounds"));
    }
    return core::Result<void>::success();
}

// The basis is computed from the same trigonometric form as `eye_position`,
// negated, so a tactical yaw/pitch pair describes the same view direction.
[[nodiscard]] std::array<float, 3> forward_of(const float yaw_degrees, const float pitch_degrees) {
    const float yaw = yaw_degrees * degrees_to_radians;
    const float pitch = pitch_degrees * degrees_to_radians;
    const float horizontal = std::cos(pitch);
    return {-horizontal * std::sin(yaw), -std::sin(pitch), -horizontal * std::cos(yaw)};
}

[[nodiscard]] std::array<float, 3> right_of(const float yaw_degrees) {
    const float yaw = yaw_degrees * degrees_to_radians;
    return {std::cos(yaw), 0.0F, -std::sin(yaw)};
}

} // namespace

core::Result<void> validate(const FreeCameraSettings& settings) {
    const auto reject = [](std::string message) {
        return core::Result<void>::failure(
            failure(diagnostic_codes::invalid_free_settings, std::move(message)));
    };
    if (!finite(settings.move_speed) || !finite(settings.vertical_speed)
        || !finite(settings.look_degrees_per_unit) || !finite(settings.pitch_min_degrees)
        || !finite(settings.pitch_max_degrees)) {
        return reject("free camera settings must all be finite");
    }
    if (!(settings.move_speed > 0.0F) || !(settings.vertical_speed > 0.0F)) {
        return reject("free camera move_speed and vertical_speed must be positive");
    }
    if (!(settings.look_degrees_per_unit > 0.0F)) {
        return reject("free camera look_degrees_per_unit must be positive");
    }
    if (settings.pitch_min_degrees < -free_pitch_limit_degrees
        || settings.pitch_max_degrees > free_pitch_limit_degrees
        || !(settings.pitch_min_degrees < settings.pitch_max_degrees)) {
        return reject("free camera pitch bounds must satisfy -89 <= pitch_min < pitch_max <= 89");
    }
    return core::Result<void>::success();
}

float wrap_yaw_degrees(const float yaw_degrees) noexcept {
    if (!finite(yaw_degrees)) return yaw_degrees;
    if (yaw_degrees >= -half_turn_degrees && yaw_degrees < half_turn_degrees) return yaw_degrees;
    float wrapped = std::fmod(yaw_degrees + half_turn_degrees, full_turn_degrees);
    if (wrapped < 0.0F) wrapped += full_turn_degrees;
    wrapped -= half_turn_degrees;
    // Rounding in the shifts above can land exactly on the open upper bound.
    if (wrapped >= half_turn_degrees) wrapped -= full_turn_degrees;
    if (wrapped < -half_turn_degrees) wrapped = -half_turn_degrees;
    return wrapped;
}

core::Result<std::array<float, 3>> free_camera_forward(const FreeCameraPose& pose) {
    using Vector = std::array<float, 3>;
    if (!finite(pose.yaw_degrees) || !finite(pose.pitch_degrees)
        || std::abs(pose.pitch_degrees) > free_pitch_limit_degrees) {
        return reject_request<Vector>("free camera forward needs a finite yaw and bounded pitch");
    }
    return core::Result<Vector>::success(forward_of(pose.yaw_degrees, pose.pitch_degrees));
}

core::Result<std::array<float, 3>> free_camera_right(const FreeCameraPose& pose) {
    using Vector = std::array<float, 3>;
    if (!finite(pose.yaw_degrees)) {
        return reject_request<Vector>("free camera right needs a finite yaw");
    }
    return core::Result<Vector>::success(right_of(pose.yaw_degrees));
}

core::Result<FreeCameraPose> enter_free_camera(
    const FreeCameraSettings& settings, const std::span<const float, 3> eye,
    const float yaw_degrees, const float pitch_degrees) {
    if (auto valid = validate(settings); !valid) {
        return core::Result<FreeCameraPose>::failure(valid.error());
    }
    const FreeCameraPose pose{{eye[0], eye[1], eye[2]}, wrap_yaw_degrees(yaw_degrees),
        pitch_degrees};
    if (!finite(pose.eye) || !finite(pose.yaw_degrees)) {
        return reject_request<FreeCameraPose>("free camera entry needs a finite eye and yaw");
    }
    if (!pitch_in_bounds(settings, pose.pitch_degrees)) {
        return reject_request<FreeCameraPose>(
            "free camera entry pitch lies outside the settings bounds; entry would turn the view");
    }
    return core::Result<FreeCameraPose>::success(pose);
}

core::Result<FreeCameraPose> advance_free_camera(
    const FreeCameraSettings& settings, const FreeCameraPose& pose,
    const FreeCameraIntent& intent, const float delta_seconds) {
    if (auto valid = validate(settings); !valid) {
        return core::Result<FreeCameraPose>::failure(valid.error());
    }
    if (auto valid = validate_pose(settings, pose); !valid) {
        return core::Result<FreeCameraPose>::failure(valid.error());
    }
    if (!finite(delta_seconds) || delta_seconds < 0.0F) {
        return reject_request<FreeCameraPose>(
            "free camera step needs a finite non-negative duration");
    }
    if (!finite(intent.strafe) || !finite(intent.forward) || !finite(intent.rise)
        || !finite(intent.look_yaw_units) || !finite(intent.look_pitch_units)) {
        return reject_request<FreeCameraPose>("free camera intent must be finite");
    }
    if (std::abs(intent.strafe) > 1.0F || std::abs(intent.forward) > 1.0F
        || std::abs(intent.rise) > 1.0F) {
        return reject_request<FreeCameraPose>(
            "free camera translation intent axes must lie in [-1, 1]");
    }

    FreeCameraPose next = pose;
    // Positive look yaw turns the view right, which is decreasing yaw in the
    // eye_position convention; positive look pitch tilts the view down.
    const float yaw_step = intent.look_yaw_units * settings.look_degrees_per_unit;
    const float pitch_step = intent.look_pitch_units * settings.look_degrees_per_unit;
    const float yaw = pose.yaw_degrees - yaw_step;
    const float pitch = pose.pitch_degrees + pitch_step;
    if (!finite(yaw_step) || !finite(pitch_step) || !finite(yaw) || !finite(pitch)) {
        return reject_request<FreeCameraPose>("free camera look step overflows");
    }
    next.yaw_degrees = wrap_yaw_degrees(yaw);
    next.pitch_degrees =
        std::clamp(pitch, settings.pitch_min_degrees, settings.pitch_max_degrees);

    const float length_squared = intent.strafe * intent.strafe
        + intent.forward * intent.forward + intent.rise * intent.rise;
    if (delta_seconds > 0.0F && length_squared > 0.0F) {
        const float normaliser = length_squared > 1.0F ? 1.0F / std::sqrt(length_squared) : 1.0F;
        const float planar = settings.move_speed * delta_seconds;
        const float vertical = settings.vertical_speed * delta_seconds;
        if (!finite(planar) || !finite(vertical)) {
            return reject_request<FreeCameraPose>("free camera step distance overflows");
        }
        const std::array<float, 3> forward = forward_of(next.yaw_degrees, next.pitch_degrees);
        const std::array<float, 3> right = right_of(next.yaw_degrees);
        float strafe = intent.strafe * normaliser * planar;
        float ahead = intent.forward * normaliser * planar;
        float rise = intent.rise * normaliser * vertical;
        // Right is orthogonal to both forward and world up, but a pitched
        // forward is not orthogonal to world up. The world displacement is
        // capped at the length it would have if all three axes were
        // orthogonal, sqrt(strafe^2 + ahead^2 + rise^2). Only the
        // forward/up cross term 2*ahead*rise*forward.y can exceed that, so
        // the step is scaled only when that term is positive. The scale is
        // computed in double because the squares can overflow float. A
        // single-axis step has no cross term and is left bit-identical, and
        // negating the whole intent negates the displacement exactly.
        const double cross = 2.0 * static_cast<double>(ahead) * static_cast<double>(rise)
            * static_cast<double>(forward[1]);
        if (cross > 0.0) {
            const double orthogonal = static_cast<double>(strafe) * strafe
                + static_cast<double>(ahead) * ahead + static_cast<double>(rise) * rise;
            const auto scale = static_cast<float>(std::sqrt(orthogonal / (orthogonal + cross)));
            strafe *= scale;
            ahead *= scale;
            rise *= scale;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            next.eye[axis] += right[axis] * strafe + forward[axis] * ahead;
        }
        next.eye[1] += rise;
        if (!finite(next.eye)) {
            return reject_request<FreeCameraPose>("free camera translation overflows");
        }
    }
    return core::Result<FreeCameraPose>::success(next);
}

core::Result<FreeCameraController> FreeCameraController::create(
    const FreeCameraSettings& settings, const std::span<const float, 3> eye,
    const float yaw_degrees, const float pitch_degrees) {
    auto pose = enter_free_camera(settings, eye, yaw_degrees, pitch_degrees);
    if (!pose) return core::Result<FreeCameraController>::failure(pose.error());
    return core::Result<FreeCameraController>::success(
        FreeCameraController(settings, pose.value()));
}

core::Result<void> FreeCameraController::advance(
    const FreeCameraIntent& intent, const float delta_seconds) {
    auto next = advance_free_camera(settings_, pose_, intent, delta_seconds);
    if (!next) return core::Result<void>::failure(next.error());
    pose_ = next.value();
    return core::Result<void>::success();
}

} // namespace eawr::presentation::camera
