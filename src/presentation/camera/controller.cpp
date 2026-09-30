#include "eawr/presentation/camera/controller.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>

namespace eawr::presentation::camera {
namespace {

[[nodiscard]] core::Diagnostic failure(std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool finite_frame(const TacticalFrame& frame) {
    if (frame.width == 0 || frame.height == 0
        || !std::isfinite(frame.vertical_fov_degrees)
        || !std::isfinite(frame.near_plane) || !std::isfinite(frame.far_plane)
        || !(frame.vertical_fov_degrees > 0.0F)
        || !(frame.vertical_fov_degrees < 180.0F)
        || !(frame.near_plane > 0.0F) || !(frame.far_plane > frame.near_plane)) {
        return false;
    }
    for (const float value : frame.eye) if (!std::isfinite(value)) return false;
    for (const float value : frame.target) if (!std::isfinite(value)) return false;
    for (const float value : frame.up) if (!std::isfinite(value)) return false;
    return true;
}

// Invert the authored distance curve so pitch can be evaluated at the live
// distance. If an override has repeated/non-monotone distances, stay on the
// branch nearest the previous zoom instead of jumping across the curve.
[[nodiscard]] core::Result<float> zoom_at_distance(
    const Constants& constants, const float distance, const float previous_zoom) {
    if (!constants.use_splines) {
        return core::Result<float>::success(std::clamp(
            (distance - constants.distance_min) /
                (constants.distance_max - constants.distance_min), 0.0F, 1.0F));
    }
    float best_zoom = previous_zoom;
    float best_error = std::numeric_limits<float>::infinity();
    float best_travel = std::numeric_limits<float>::infinity();
    const auto consider = [&](const float zoom) {
        const auto evaluated = evaluate_spline(constants.distance_spline, zoom);
        if (!evaluated) return;
        const float value = std::clamp(evaluated.value(),
            constants.distance_min, constants.distance_max);
        const float error = std::abs(value - distance);
        const float travel = std::abs(zoom - previous_zoom);
        if (error < best_error || (error == best_error && travel < best_travel)) {
            best_zoom = zoom;
            best_error = error;
            best_travel = travel;
        }
    };
    float left_zoom = 0.0F;
    auto left = evaluate_spline(constants.distance_spline, left_zoom);
    if (!left) return core::Result<float>::failure(left.error());
    // An entire flat segment can map to this distance. Its interior is the
    // closest inverse when the previous zoom already lies on that segment.
    consider(previous_zoom);
    consider(left_zoom);
    for (std::size_t index = 0; index <= constants.distance_spline.size(); ++index) {
        const float right_zoom = index == constants.distance_spline.size() ? 1.0F
            : std::clamp(constants.distance_spline[index].fraction, 0.0F, 1.0F);
        if (right_zoom < left_zoom) continue;
        auto right = evaluate_spline(constants.distance_spline, right_zoom);
        if (!right) return core::Result<float>::failure(right.error());
        const float a = std::clamp(left.value(), constants.distance_min, constants.distance_max);
        const float b = std::clamp(right.value(), constants.distance_min, constants.distance_max);
        if (a != b && distance >= std::min(a, b) && distance <= std::max(a, b)) {
            consider(left_zoom + (right_zoom - left_zoom) * (distance - a) / (b - a));
        }
        consider(right_zoom);
        left_zoom = right_zoom;
        left = right;
    }
    return core::Result<float>::success(best_zoom);
}

// Integrate a pan velocity approaching a target whose speed follows the
// exponential zoom distance. Both the final velocity and travel are exact for
// constant input over this step, including the equal-time-constant limit.
[[nodiscard]] bool integrate_pan(const float current, const float start_target,
    const float goal_target, const float pan_time, const float zoom_time,
    const float seconds, float& next, float& travel) {
    const double t = seconds;
    const double start = start_target;
    const double goal = goal_target;
    const double initial = current;
    double end{};
    double area{};
    if (!(zoom_time > 0.0F) || start == goal) {
        if (!(pan_time > 0.0F)) {
            end = goal;
            area = goal * t;
        } else {
            const double blend = -std::expm1(-t / pan_time);
            end = initial + (goal - initial) * blend;
            area = goal * t + (initial - goal) * pan_time * blend;
        }
    } else if (!(pan_time > 0.0F)) {
        const double zoom_blend = -std::expm1(-t / zoom_time);
        end = goal + (start - goal) * (1.0 - zoom_blend);
        area = goal * t + (start - goal) * zoom_time * zoom_blend;
    } else {
        const double p = pan_time;
        const double z = zoom_time;
        const double pan_decay = std::exp(-t / p);
        const double zoom_decay = std::exp(-t / z);
        const double pan_area = p * -std::expm1(-t / p);
        const double zoom_area = z * -std::expm1(-t / z);
        const double changing = start - goal;
        if (std::abs(z - p) <= 1e-5 * std::max(z, p)) {
            const double ratio = t / p;
            end = goal + (initial - goal) * pan_decay
                + changing * ratio * pan_decay;
            area = goal * t + (initial - goal) * pan_area
                + changing * p * (1.0 - (1.0 + ratio) * pan_decay);
        } else {
            const double coefficient = changing * z / (z - p);
            end = goal + (initial - goal) * pan_decay
                + coefficient * (zoom_decay - pan_decay);
            area = goal * t + (initial - goal) * pan_area
                + coefficient * (zoom_area - pan_area);
        }
    }
    next = static_cast<float>(end);
    travel = static_cast<float>(area);
    return std::isfinite(next) && std::isfinite(travel);
}

[[nodiscard]] core::Result<TacticalFrame> make_frame(
    const Constants& constants, const State& state, float yaw_degrees,
    const std::array<float, 3>& target, std::uint32_t width, std::uint32_t height) {
    if (!std::isfinite(state.zoom) || !std::isfinite(state.distance)
        || !std::isfinite(state.pitch_degrees) || !std::isfinite(state.yaw_degrees)
        || !std::isfinite(state.fov_degrees) || !std::isfinite(state.pan_speed)) {
        return core::Result<TacticalFrame>::failure(failure(
            diagnostic_codes::invalid_controller_request, "solved camera state is not finite"));
    }
    auto eye = eye_position(target, state.distance, state.pitch_degrees, yaw_degrees);
    if (!eye) return core::Result<TacticalFrame>::failure(eye.error());
    // #515: the XML angle is FoC's (horizontal on 4:3); the frame carries the vertical one.
    // An angle outside (0, 180) has none and goes on as it is, for the frame check to refuse.
    const auto vertical = vertical_fov_degrees(state.fov_degrees);
    TacticalFrame frame{width, height, vertical ? vertical.value() : state.fov_degrees, constants.near_clip,
                        constants.far_clip, eye.value(), target};
    if (!finite_frame(frame)) {
        return core::Result<TacticalFrame>::failure(failure(
            diagnostic_codes::invalid_controller_request, "camera frame is not finite and valid"));
    }
    return core::Result<TacticalFrame>::success(frame);
}

} // namespace

OrbitPitchRange space_orbit_pitch_range(const Constants& constants) noexcept {
    return {std::max(constants.pitch_min, -89.0F), std::min(constants.pitch_max, 89.0F)};
}

core::Result<BoundedTacticalController> BoundedTacticalController::create(
    Constants constants, SourceTargetBounds source_bounds,
    std::array<float, 3> render_target, float zoom, float yaw_degrees,
    std::uint32_t width, std::uint32_t height) {
    using Result = core::Result<BoundedTacticalController>;
    if (auto valid = validate(constants); !valid) return Result::failure(valid.error());
    if (source_bounds.logical_path.empty() || source_bounds.source_id.empty()
        || source_bounds.authority.empty()
        || !std::isfinite(source_bounds.min_x) || !std::isfinite(source_bounds.max_x)
        || !std::isfinite(source_bounds.min_y) || !std::isfinite(source_bounds.max_y)
        || !(source_bounds.min_x < source_bounds.max_x)
        || !(source_bounds.min_y < source_bounds.max_y)) {
        return Result::failure(failure(diagnostic_codes::invalid_target_bounds,
            "explicit source target bounds need finite, ordered limits and provenance"));
    }
    RenderTargetBounds bounds{source_bounds.min_x, source_bounds.max_x,
                              -source_bounds.max_y, -source_bounds.min_y};
    if (!std::isfinite(bounds.min_z) || !std::isfinite(bounds.max_z)) {
        return Result::failure(failure(diagnostic_codes::invalid_target_bounds,
            "source target bounds cannot be converted to render coordinates"));
    }
    if (!std::isfinite(render_target[0]) || !std::isfinite(render_target[1])
        || !std::isfinite(render_target[2]) || !std::isfinite(yaw_degrees)
        || width == 0 || height == 0) {
        return Result::failure(failure(diagnostic_codes::invalid_controller_request,
            "initial camera target, yaw and viewport must be valid"));
    }
    auto state = solve(constants, zoom);
    if (!state) return Result::failure(state.error());
    if (yaw_degrees < constants.yaw_min || yaw_degrees > constants.yaw_max) {
        return Result::failure(failure(diagnostic_codes::invalid_controller_request,
            "initial yaw is outside the supplied camera range"));
    }
    state.value().yaw_degrees = yaw_degrees;
    render_target[0] = std::clamp(render_target[0], bounds.min_x, bounds.max_x);
    render_target[2] = std::clamp(render_target[2], bounds.min_z, bounds.max_z);
    auto frame = make_frame(constants, state.value(), yaw_degrees, render_target, width, height);
    if (!frame) return Result::failure(frame.error());
    return Result::success(BoundedTacticalController(std::move(constants),
        std::move(source_bounds), bounds, state.value(), yaw_degrees, frame.value()));
}

core::Result<void> BoundedTacticalController::advance(const TacticalStep& step) {
    if (capture_locked_) return core::Result<void>::success();
    const auto invalid = [] {
        return core::Result<void>::failure(failure(
            diagnostic_codes::invalid_controller_request, "camera step is invalid or overflows"));
    };
    if (!std::isfinite(step.wheel_detents) || !std::isfinite(step.yaw_mouse_units)
        || !std::isfinite(step.delta_seconds) || step.delta_seconds < 0.0F
        || !std::isfinite(step.pan.x) || !std::isfinite(step.pan.y)
        || std::abs(step.pan.x) > 1.0F || std::abs(step.pan.y) > 1.0F
        || !std::isfinite(step.drag.x) || !std::isfinite(step.drag.y)
        || !std::isfinite(step.orbit_pitch_units) || !std::isfinite(step.pitch_adjust_degrees)
        || !std::isfinite(step.translate_units.x) || !std::isfinite(step.translate_units.y)
        || !std::isfinite(step.pan_speed_scale) || !(step.pan_speed_scale > 0.0F)
        || !(step.orbit_pitch_range.min_degrees >= -89.0F)
        || !(step.orbit_pitch_range.min_degrees < step.orbit_pitch_range.max_degrees)
        || !(step.orbit_pitch_range.max_degrees <= 89.0F)
        || (step.view_yaw_degrees && !std::isfinite(*step.view_yaw_degrees))) {
        return invalid();
    }
    const float yaw_delta = constants_.yaw_per_mouse_unit * step.yaw_mouse_units;
    if (!std::isfinite(yaw_delta) || !std::isfinite(yaw_degrees_ + yaw_delta)) return invalid();
    auto zoom = apply_zoom_step(constants_, target_zoom_, step.wheel_detents);
    if (!zoom) return core::Result<void>::failure(zoom.error());
    auto goal = solve(constants_, zoom.value());
    if (!goal) return core::Result<void>::failure(goal.error());
    auto distance = smooth_toward(state_.distance, goal.value().distance,
        constants_.distance_smooth_time, step.delta_seconds);
    if (!distance) return core::Result<void>::failure(distance.error());
    distance.value() = std::clamp(distance.value(), constants_.distance_min,
        constants_.distance_max);
    auto live_zoom = zoom_at_distance(constants_, distance.value(), state_.zoom);
    if (!live_zoom) return core::Result<void>::failure(live_zoom.error());
    auto state = solve(constants_, live_zoom.value());
    if (!state) return core::Result<void>::failure(state.error());
    state.value().distance = distance.value();
    const float fraction = (distance.value() - constants_.distance_min)
        / (constants_.distance_max - constants_.distance_min);
    state.value().pan_speed = constants_.tactical_min_scroll_speed
        + (constants_.tactical_max_scroll_speed - constants_.tactical_min_scroll_speed)
            * fraction;
    auto yaw = apply_rotate(constants_, yaw_degrees_, step.yaw_mouse_units);
    if (!yaw || !std::isfinite(yaw.value())) return invalid();
    state.value().yaw_degrees = yaw.value();
    // Orbit: offset the zoom-linked pitch at this step's live distance.
    const auto orbit_clamp = [&step](const float pitch) {
        return std::clamp(pitch, step.orbit_pitch_range.min_degrees,
                          step.orbit_pitch_range.max_degrees);
    };
    const float zoom_pitch = state.value().pitch_degrees;
    float orbit_offset = orbit_pitch_offset_;
    // FoC tilts by Pitch_Per_Mouse_Unit per mouse unit; zero (FoC land) never tilts.
    const float tilt = constants_.pitch_per_mouse_unit * step.orbit_pitch_units
        + step.pitch_adjust_degrees;
    if (!std::isfinite(tilt)) return invalid();
    if (tilt != 0.0F) {
        const float from = orbit_offset != 0.0F ? orbit_clamp(zoom_pitch + orbit_offset) : zoom_pitch;
        if (!std::isfinite(from + tilt)) return invalid();
        orbit_offset = orbit_clamp(from + tilt) - zoom_pitch;
    }
    if (orbit_offset != 0.0F) state.value().pitch_degrees = orbit_clamp(zoom_pitch + orbit_offset);
    const float length = std::hypot(step.pan.x, step.pan.y);
    const float modifier = step.pan_speed_scale
        * (step.pan.push_scroll ? constants_.push_scroll_speed_modifier : 1.0F);
    const float start_scale = state_.pan_speed * modifier;
    const float goal_fraction = (goal.value().distance - constants_.distance_min)
        / (constants_.distance_max - constants_.distance_min);
    const float goal_speed = constants_.tactical_min_scroll_speed
        + (constants_.tactical_max_scroll_speed - constants_.tactical_min_scroll_speed)
            * goal_fraction;
    const float goal_scale = goal_speed * modifier;
    if (!std::isfinite(start_scale) || !std::isfinite(goal_scale)) return invalid();
    const PanDisplacement desired_start = length > 0.0F
        ? PanDisplacement{step.pan.x * start_scale / length, step.pan.y * start_scale / length}
        : PanDisplacement{};
    const PanDisplacement desired_goal = length > 0.0F
        ? PanDisplacement{step.pan.x * goal_scale / length, step.pan.y * goal_scale / length}
        : PanDisplacement{};
    // FoC's 1.5 deceleration factor is a braking gain, not 1.5 seconds of
    // coast. Scale the acceleration time by its reciprocal on release.
    const float smooth_time = length > 0.0F ? constants_.scroll_acceleration_factor
        : (constants_.scroll_deceleration_factor > 0.0F
            ? constants_.scroll_acceleration_factor / constants_.scroll_deceleration_factor : 0.0F);
    PanDisplacement velocity{};
    PanDisplacement displacement{};
    if (!integrate_pan(pan_velocity_.x, desired_start.x, desired_goal.x, smooth_time,
            constants_.distance_smooth_time, step.delta_seconds, velocity.x, displacement.x)
        || !integrate_pan(pan_velocity_.y, desired_start.y, desired_goal.y, smooth_time,
            constants_.distance_smooth_time, step.delta_seconds, velocity.y, displacement.y)) {
        return invalid();
    }
    // A drag is added once at this step's live distance; it never feeds the
    // velocity, so a release after a drag does not coast.
    auto dragged = drag_displacement(state.value(), step.drag.x, step.drag.y);
    if (!dragged) return invalid();
    displacement.x += dragged.value().x;
    displacement.y += dragged.value().y;
    // FoC translate: a mouse unit moves 1/100 of the live (smoothed) distance.
    const float translate_scale = state.value().distance * 0.01F;
    displacement.x += step.translate_units.x * translate_scale;
    displacement.y += step.translate_units.y * translate_scale;
    if (!std::isfinite(displacement.x) || !std::isfinite(displacement.y)) return invalid();

    const float radians = step.view_yaw_degrees.value_or(yaw.value()) * (std::numbers::pi_v<float> / 180.0F);
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    auto target = tactical_frame_.target;
    const float x = target[0] + cosine * displacement.x - sine * displacement.y;
    const float z = target[2] - sine * displacement.x - cosine * displacement.y;
    if (!std::isfinite(x) || !std::isfinite(z)) return invalid();
    target[0] = std::clamp(x, bounds_.min_x, bounds_.max_x);
    target[2] = std::clamp(z, bounds_.min_z, bounds_.max_z);
    if (target[0] != x || target[2] != z) {
        float world_x = cosine * velocity.x - sine * velocity.y;
        float world_z = -sine * velocity.x - cosine * velocity.y;
        if (target[0] != x) world_x = 0.0F;
        if (target[2] != z) world_z = 0.0F;
        velocity = {cosine * world_x - sine * world_z,
                    -sine * world_x - cosine * world_z};
    }
    auto frame = make_frame(constants_, state.value(), yaw.value(), target,
                            tactical_frame_.width, tactical_frame_.height);
    if (!frame) return core::Result<void>::failure(frame.error());
    state_ = state.value();
    target_zoom_ = zoom.value();
    pan_velocity_ = velocity;
    yaw_degrees_ = yaw.value();
    orbit_pitch_offset_ = orbit_offset;
    tactical_frame_ = frame.value();
    return core::Result<void>::success();
}

core::Result<void> BoundedTacticalController::set_viewport(
    const std::uint32_t width, const std::uint32_t height) {
    if (capture_locked_) return core::Result<void>::success();
    if (width == 0 || height == 0) {
        return core::Result<void>::failure(failure(
            diagnostic_codes::invalid_controller_request, "camera viewport must be nonempty"));
    }
    auto next = make_frame(constants_, state_, yaw_degrees_, tactical_frame_.target, width, height);
    if (!next) return core::Result<void>::failure(next.error());
    tactical_frame_ = next.value();
    return core::Result<void>::success();
}

core::Result<void> BoundedTacticalController::set_target_height(const float height) {
    if (capture_locked_) return core::Result<void>::success();
    if (!std::isfinite(height)) {
        return core::Result<void>::failure(failure(
            diagnostic_codes::invalid_controller_request, "camera target height must be finite"));
    }
    auto target = tactical_frame_.target;
    target[1] = height;
    auto next = make_frame(constants_, state_, yaw_degrees_, target,
                           tactical_frame_.width, tactical_frame_.height);
    if (!next) return core::Result<void>::failure(next.error());
    tactical_frame_ = next.value();
    return core::Result<void>::success();
}

core::Result<void> BoundedTacticalController::set_target(const float x, const float z) {
    if (capture_locked_) return core::Result<void>::success();
    if (!std::isfinite(x) || !std::isfinite(z)) {
        return core::Result<void>::failure(failure(
            diagnostic_codes::invalid_controller_request, "camera target must be finite"));
    }
    auto target = tactical_frame_.target;
    target[0] = std::clamp(x, bounds_.min_x, bounds_.max_x);
    target[2] = std::clamp(z, bounds_.min_z, bounds_.max_z);
    auto next = make_frame(constants_, state_, yaw_degrees_, target,
                           tactical_frame_.width, tactical_frame_.height);
    if (!next) return core::Result<void>::failure(next.error());
    tactical_frame_ = next.value();
    pan_velocity_ = {};
    return core::Result<void>::success();
}

core::Result<void> BoundedTacticalController::lock_capture(const TacticalFrame& fixed_frame) {
    if (capture_locked_) {
        if (fixed_frame == fixed_frame_) return core::Result<void>::success();
        return core::Result<void>::failure(failure(
            diagnostic_codes::invalid_controller_request,
            "an active fixed capture cannot be replaced"));
    }
    if (!finite_frame(fixed_frame)) {
        return core::Result<void>::failure(failure(
            diagnostic_codes::invalid_controller_request, "fixed capture frame is invalid"));
    }
    fixed_frame_ = fixed_frame;
    capture_locked_ = true;
    return core::Result<void>::success();
}

} // namespace eawr::presentation::camera
