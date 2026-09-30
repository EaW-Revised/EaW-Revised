#include "eawr/presentation/camera/camera.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <utility>

namespace eawr::presentation::camera {
namespace {

constexpr float degrees_to_radians = 0.017453292519943295F;
// A yaw range spanning at least one full turn is treated as free rotation and
// wraps; a narrower authored range is a real limit and clamps.
constexpr float full_turn_degrees = 360.0F;

[[nodiscard]] core::Diagnostic failure(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool finite(const float value) noexcept { return std::isfinite(value); }

[[nodiscard]] float lerp(const float from, const float to, const float t) noexcept {
    return from + (to - from) * t;
}

[[nodiscard]] bool spline_is_usable(const std::span<const SplinePoint> points) noexcept {
    if (points.size() < 2) return false;
    for (const SplinePoint& point : points) {
        if (!finite(point.fraction) || !finite(point.value)) return false;
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (points[index].fraction < points[index - 1].fraction) return false;
    }
    return true;
}

[[nodiscard]] bool parse_scalar_token(std::string_view token, float& out) noexcept {
    if (!token.empty() && (token.back() == 'f' || token.back() == 'F')) {
        token.remove_suffix(1);
    }
    if (token.empty()) return false;
    // from_chars rejects a leading '+' and accepts hex/inf/nan spellings, so
    // screen the token before converting.
    std::string_view digits = token;
    if (digits.front() == '+' || digits.front() == '-') digits.remove_prefix(1);
    if (digits.empty()) return false;
    for (const char character : digits) {
        const bool allowed = (character >= '0' && character <= '9') || character == '.'
            || character == 'e' || character == 'E' || character == '+' || character == '-';
        if (!allowed) return false;
    }
    if (token.front() == '+') token.remove_prefix(1);
    float parsed{};
    const char* const begin = token.data();
    const char* const end = begin + token.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) return false;
    if (!finite(parsed)) return false;
    out = parsed;
    return true;
}

} // namespace

core::Result<void> validate(const Constants& constants) {
    const std::array<float, 25> scalars{
        constants.distance_min, constants.distance_max, constants.distance_default,
        constants.distance_per_mouse_unit, constants.distance_smooth_time,
        constants.pitch_min, constants.pitch_max, constants.pitch_default,
        constants.pitch_per_mouse_unit, constants.pitch_per_zoom_unit,
        constants.pitch_when_zoomed_in, constants.pitch_zoom_begin_fraction,
        constants.yaw_min, constants.yaw_max, constants.yaw_default,
        constants.yaw_per_mouse_unit,
        constants.fov_min, constants.fov_max, constants.fov_default,
        constants.fov_per_mouse_unit,
        constants.near_clip, constants.far_clip,
        constants.tactical_min_scroll_speed, constants.tactical_max_scroll_speed,
        constants.push_scroll_speed_modifier,
    };
    if (!std::all_of(scalars.begin(), scalars.end(),
            [](const float value) { return finite(value); })) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_constants,
            "camera constants contain a non-finite value"));
    }
    if (!finite(constants.tactical_edge_scroll_region)
        || !finite(constants.tactical_offscreen_scroll_region)
        || !finite(constants.scroll_acceleration_factor)
        || !finite(constants.scroll_deceleration_factor)) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_constants,
            "tactical scroll constants contain a non-finite value"));
    }
    if (!(constants.distance_max > constants.distance_min)) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_constants,
            "Distance_Max must exceed Distance_Min"));
    }
    if (constants.pitch_max < constants.pitch_min) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_constants,
            "Pitch_Max must not be below Pitch_Min"));
    }
    if (constants.yaw_max < constants.yaw_min) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_constants,
            "Yaw_Max must not be below Yaw_Min"));
    }
    if (constants.fov_max < constants.fov_min) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_constants,
            "Fov_Max must not be below Fov_Min"));
    }
    if (!(constants.near_clip > 0.0F) || !(constants.far_clip > constants.near_clip)) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_constants,
            "Near_Clip must be positive and below Far_Clip"));
    }
    if (constants.tactical_max_scroll_speed < constants.tactical_min_scroll_speed) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_constants,
            "Tactical_Max_Scroll_Speed must not be below Tactical_Min_Scroll_Speed"));
    }
    if (constants.use_splines) {
        if (!spline_is_usable(constants.distance_spline)) {
            return core::Result<void>::failure(failure(diagnostic_codes::invalid_spline,
                "Use_Splines is set but Distance_Spline is not a usable curve"));
        }
        if (!spline_is_usable(constants.pitch_spline)) {
            return core::Result<void>::failure(failure(diagnostic_codes::invalid_spline,
                "Use_Splines is set but Pitch_Spline is not a usable curve"));
        }
    }
    return core::Result<void>::success();
}

core::Result<float> evaluate_spline(
    const std::span<const SplinePoint> points, const float fraction) {
    if (!spline_is_usable(points)) {
        return core::Result<float>::failure(failure(diagnostic_codes::invalid_spline,
            "spline needs at least two finite, non-decreasing control points"));
    }
    if (!finite(fraction)) {
        return core::Result<float>::failure(failure(diagnostic_codes::invalid_request,
            "spline fraction must be finite"));
    }
    if (fraction <= points.front().fraction) {
        return core::Result<float>::success(points.front().value);
    }
    if (fraction >= points.back().fraction) {
        return core::Result<float>::success(points.back().value);
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        const SplinePoint& previous = points[index - 1];
        const SplinePoint& current = points[index];
        if (fraction > current.fraction) continue;
        const float span = current.fraction - previous.fraction;
        // Coincident fractions are a step; take the later value rather than
        // dividing by zero.
        if (!(span > 0.0F)) return core::Result<float>::success(current.value);
        const float t = (fraction - previous.fraction) / span;
        return core::Result<float>::success(lerp(previous.value, current.value, t));
    }
    return core::Result<float>::success(points.back().value);
}

core::Result<float> parse_scalar(const std::string_view text) {
    // Petroglyph pads values with surrounding whitespace in places; trim it
    // before the strict whole-token conversion.
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto space = [](const char character) {
        return character == ' ' || character == '\t' || character == '\r'
            || character == '\n';
    };
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    float value{};
    if (!parse_scalar_token(text.substr(begin, end - begin), value)) {
        return core::Result<float>::failure(failure(diagnostic_codes::invalid_request,
            "value is not a finite decimal number"));
    }
    return core::Result<float>::success(value);
}

core::Result<std::vector<SplinePoint>> parse_spline(const std::string_view text) {
    using Points = std::vector<SplinePoint>;
    std::vector<float> values;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size()
            && (text[index] == ',' || text[index] == ' ' || text[index] == '\t'
                || text[index] == '\r' || text[index] == '\n')) {
            ++index;
        }
        if (index >= text.size()) break;
        const std::size_t start = index;
        while (index < text.size() && text[index] != ',' && text[index] != ' '
            && text[index] != '\t' && text[index] != '\r' && text[index] != '\n') {
            ++index;
        }
        float value{};
        if (!parse_scalar_token(text.substr(start, index - start), value)) {
            return core::Result<Points>::failure(failure(diagnostic_codes::invalid_spline,
                "spline contains a token that is not a finite decimal number"));
        }
        values.push_back(value);
    }
    if (values.size() < 4 || values.size() % 2 != 0) {
        return core::Result<Points>::failure(failure(diagnostic_codes::invalid_spline,
            "spline needs an even token count describing at least two control points"));
    }
    Points points;
    points.reserve(values.size() / 2);
    for (std::size_t pair = 0; pair + 1 < values.size(); pair += 2) {
        points.push_back(SplinePoint{values[pair], values[pair + 1]});
    }
    if (!spline_is_usable(points)) {
        return core::Result<Points>::failure(failure(diagnostic_codes::invalid_spline,
            "spline control-point fractions must not decrease"));
    }
    return core::Result<Points>::success(std::move(points));
}

core::Result<float> clamp_zoom(const float zoom) {
    if (!finite(zoom)) {
        return core::Result<float>::failure(failure(diagnostic_codes::invalid_request,
            "zoom must be finite"));
    }
    return core::Result<float>::success(std::clamp(zoom, min_zoom, max_zoom));
}

core::Result<State> solve(const Constants& constants, const float zoom) {
    if (auto valid = validate(constants); !valid) {
        return core::Result<State>::failure(valid.error());
    }
    auto clamped = clamp_zoom(zoom);
    if (!clamped) return core::Result<State>::failure(clamped.error());
    const float t = clamped.value();

    State state;
    state.zoom = t;

    if (constants.use_splines) {
        auto distance = evaluate_spline(constants.distance_spline, t);
        if (!distance) return core::Result<State>::failure(distance.error());
        state.distance = distance.value();
    } else {
        state.distance = lerp(constants.distance_min, constants.distance_max, t);
    }
    state.distance = std::clamp(state.distance, constants.distance_min, constants.distance_max);

    if (constants.use_splines) {
        // Deliberately unclamped: see the `solve` contract in the header.
        auto pitch = evaluate_spline(constants.pitch_spline, t);
        if (!pitch) return core::Result<State>::failure(pitch.error());
        state.pitch_degrees = pitch.value();
    } else {
        const float begin = constants.pitch_zoom_begin_fraction;
        float pitch{};
        if (begin > 0.0F && begin <= max_zoom && t < begin) {
            pitch = lerp(constants.pitch_when_zoomed_in, constants.pitch_default, t / begin);
        } else {
            pitch = constants.pitch_default
                + constants.pitch_per_zoom_unit * (t - std::max(begin, 0.0F));
        }
        state.pitch_degrees = std::clamp(pitch, constants.pitch_min, constants.pitch_max);
    }

    state.yaw_degrees = std::clamp(constants.yaw_default, constants.yaw_min, constants.yaw_max);
    state.fov_degrees = std::clamp(constants.fov_default, constants.fov_min, constants.fov_max);

    // Pan speed follows the normalised distance rather than the raw zoom, so
    // the spline land mode accelerates along the curve the camera actually
    // flies. For the linear modes the two are identical.
    const float span = constants.distance_max - constants.distance_min;
    const float normalized_distance =
        std::clamp((state.distance - constants.distance_min) / span, 0.0F, 1.0F);
    state.pan_speed = lerp(constants.tactical_min_scroll_speed,
        constants.tactical_max_scroll_speed, normalized_distance);
    return core::Result<State>::success(state);
}

core::Result<float> apply_zoom_step(
    const Constants& constants, const float zoom, const float detents) {
    if (auto valid = validate(constants); !valid) {
        return core::Result<float>::failure(valid.error());
    }
    if (!finite(detents)) {
        return core::Result<float>::failure(failure(diagnostic_codes::invalid_request,
            "wheel detents must be finite"));
    }
    auto current = clamp_zoom(zoom);
    if (!current) return current;
    // Distance_Per_Mouse_Unit is world units per detent; the zoom parameter is
    // normalised over the mode's own distance range, so the step converts.
    const float span = constants.distance_max - constants.distance_min;
    const float step = constants.distance_per_mouse_unit / span;
    return clamp_zoom(current.value() + step * detents);
}

core::Result<PanDisplacement> apply_pan(
    const Constants& constants, const State& state, const PanInput& input,
    const float delta_seconds) {
    if (auto valid = validate(constants); !valid) {
        return core::Result<PanDisplacement>::failure(valid.error());
    }
    if (!finite(input.x) || !finite(input.y) || !finite(delta_seconds)
        || !finite(state.pan_speed) || delta_seconds < 0.0F) {
        return core::Result<PanDisplacement>::failure(failure(
            diagnostic_codes::invalid_request,
            "pan input, pan speed and delta seconds must be finite and non-negative"));
    }
    float scale = state.pan_speed * delta_seconds;
    if (input.push_scroll) scale *= constants.push_scroll_speed_modifier;
    // A diagonal input is normalised so pressing two keys does not pan faster
    // than the authored speed along one axis.
    const float length = std::sqrt(input.x * input.x + input.y * input.y);
    if (!(length > 0.0F)) return core::Result<PanDisplacement>::success(PanDisplacement{});
    const float inverse = scale / length;
    return core::Result<PanDisplacement>::success(
        PanDisplacement{input.x * inverse, input.y * inverse});
}

core::Result<float> vertical_fov_degrees(const float foc_fov_degrees) {
    if (!finite(foc_fov_degrees) || !(foc_fov_degrees > 0.0F) || !(foc_fov_degrees < 180.0F)) {
        return core::Result<float>::failure(failure(diagnostic_codes::invalid_request,
            "field of view must be in (0, 180) degrees"));
    }
    // The 4:3 screen's vertical half-angle: three quarters of the horizontal one's tangent.
    const float half = std::atan(0.75F * std::tan(foc_fov_degrees * 0.5F * degrees_to_radians));
    return core::Result<float>::success(2.0F * half / degrees_to_radians);
}

core::Result<PanDisplacement> drag_displacement(
    const State& state, const float screen_x, const float screen_y) {
    if (!finite(screen_x) || !finite(screen_y) || !finite(state.distance)
        || !finite(state.fov_degrees) || state.distance < 0.0F
        || !(state.fov_degrees > 0.0F) || !(state.fov_degrees < 180.0F)) {
        return core::Result<PanDisplacement>::failure(failure(
            diagnostic_codes::invalid_request,
            "drag, distance and field of view must be finite and in range"));
    }
    auto vertical = vertical_fov_degrees(state.fov_degrees);
    if (!vertical) return core::Result<PanDisplacement>::failure(vertical.error());
    const float half_fov = vertical.value() * 0.5F * degrees_to_radians;
    const float ground_per_screen = 2.0F * state.distance * std::tan(half_fov);
    const PanDisplacement moved{screen_x * ground_per_screen, screen_y * ground_per_screen};
    if (!finite(moved.x) || !finite(moved.y)) {
        return core::Result<PanDisplacement>::failure(failure(
            diagnostic_codes::invalid_request, "drag displacement overflows"));
    }
    return core::Result<PanDisplacement>::success(moved);
}

core::Result<EdgeScroll> edge_scroll(
    const Constants& constants, const float cursor_x, const float cursor_y,
    const float extent_x, const float extent_y) {
    if (auto valid = validate(constants); !valid) {
        return core::Result<EdgeScroll>::failure(valid.error());
    }
    if (!finite(cursor_x) || !finite(cursor_y) || !finite(extent_x) || !finite(extent_y)
        || !(extent_x > 0.0F) || !(extent_y > 0.0F)) {
        return core::Result<EdgeScroll>::failure(failure(diagnostic_codes::invalid_request,
            "edge scroll needs a finite cursor and a positive viewport extent"));
    }
    const float edge = constants.tactical_edge_scroll_region;
    const float offscreen = constants.tactical_offscreen_scroll_region;
    const auto axis = [edge, offscreen](const float cursor, const float extent) {
        if (cursor < -offscreen || cursor > extent + offscreen) return 0;
        if (cursor <= edge) return -1;
        if (cursor >= extent - 1.0F - edge) return 1;
        return 0;
    };
    return core::Result<EdgeScroll>::success(
        EdgeScroll{axis(cursor_x, extent_x), axis(cursor_y, extent_y)});
}

core::Result<float> apply_rotate(
    const Constants& constants, const float yaw_degrees, const float mouse_units) {
    if (auto valid = validate(constants); !valid) {
        return core::Result<float>::failure(valid.error());
    }
    if (!finite(yaw_degrees) || !finite(mouse_units)) {
        return core::Result<float>::failure(failure(diagnostic_codes::invalid_request,
            "yaw and mouse units must be finite"));
    }
    const float updated = yaw_degrees + constants.yaw_per_mouse_unit * mouse_units;
    const float span = constants.yaw_max - constants.yaw_min;
    if (span >= full_turn_degrees) {
        // A range of a full turn or more is free rotation; wrap so continuous
        // dragging never sticks at an authored sentinel such as +/-1000.
        const float wrapped = std::fmod(updated - constants.yaw_min, span);
        return core::Result<float>::success(
            constants.yaw_min + (wrapped < 0.0F ? wrapped + span : wrapped));
    }
    return core::Result<float>::success(
        std::clamp(updated, constants.yaw_min, constants.yaw_max));
}

core::Result<float> apply_pitch(
    const Constants& constants, const float pitch_degrees, const float mouse_units) {
    if (auto valid = validate(constants); !valid) {
        return core::Result<float>::failure(valid.error());
    }
    if (!finite(pitch_degrees) || !finite(mouse_units)) {
        return core::Result<float>::failure(failure(diagnostic_codes::invalid_request,
            "pitch and mouse units must be finite"));
    }
    const float updated = pitch_degrees + constants.pitch_per_mouse_unit * mouse_units;
    return core::Result<float>::success(
        std::clamp(updated, constants.pitch_min, constants.pitch_max));
}

core::Result<float> smooth_toward(
    const float current, const float target, const float smooth_time,
    const float delta_seconds) {
    if (!finite(current) || !finite(target) || !finite(smooth_time) || !finite(delta_seconds)
        || delta_seconds < 0.0F) {
        return core::Result<float>::failure(failure(diagnostic_codes::invalid_request,
            "smoothing needs finite values and a non-negative delta"));
    }
    if (!(smooth_time > 0.0F) || !(delta_seconds > 0.0F)) {
        return core::Result<float>::success(smooth_time > 0.0F ? current : target);
    }
    // Exponential approach: the remaining error decays by 1/e per smooth_time,
    // which is frame-rate independent for a fixed delta.
    const float blend = 1.0F - std::exp(-delta_seconds / smooth_time);
    return core::Result<float>::success(lerp(current, target, blend));
}

core::Result<std::array<float, 3>> eye_position(
    const std::span<const float, 3> target, const float distance,
    const float pitch_degrees, const float yaw_degrees) {
    using Position = std::array<float, 3>;
    if (!finite(target[0]) || !finite(target[1]) || !finite(target[2])
        || !finite(distance) || !finite(pitch_degrees) || !finite(yaw_degrees)
        || distance < 0.0F) {
        return core::Result<Position>::failure(failure(diagnostic_codes::invalid_request,
            "eye position needs a finite target, angles and a non-negative distance"));
    }
    const float pitch = pitch_degrees * degrees_to_radians;
    const float yaw = yaw_degrees * degrees_to_radians;
    const float horizontal = distance * std::cos(pitch);
    return core::Result<Position>::success(Position{
        target[0] + horizontal * std::sin(yaw),
        target[1] + distance * std::sin(pitch),
        target[2] + horizontal * std::cos(yaw),
    });
}

} // namespace eawr::presentation::camera
