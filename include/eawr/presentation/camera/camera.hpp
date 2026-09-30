#pragma once

#include "eawr/core/result.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Deterministic, engine-independent tactical camera model.
//
// Numeric policy: this is presentation code, so it uses `float`, matching the
// documented boundary in docs/simulation.md ("presentation performs any float
// conversion"). The Q24 fixed-point contract in docs/fixed-point.md governs
// simulation arithmetic and is deliberately not used here: no value produced by
// this module ever re-enters the simulation, and no simulation state is read.
//
// The module is pure. It owns no engine type, performs no I/O, keeps no global
// state, and every entry point is a function of its arguments alone, so the
// same inputs always produce the same outputs on every target.
namespace eawr::presentation::camera {

// The zoom parameter is normalised: 0 is fully zoomed in (Distance_Min) and 1
// is fully zoomed out (Distance_Max). Petroglyph's splines are authored in the
// same parameter, which is why their first control point carries Distance_Min.
inline constexpr float min_zoom = 0.0F;
inline constexpr float max_zoom = 1.0F;

enum class Mode : std::uint8_t { land, space, unlocked };

[[nodiscard]] constexpr std::string_view to_string(const Mode mode) noexcept {
    switch (mode) {
    case Mode::land: return "land";
    case Mode::space: return "space";
    case Mode::unlocked: return "unlocked";
    }
    return "invalid";
}

// The XML definition name each mode is read from.
[[nodiscard]] constexpr std::string_view definition_name(const Mode mode) noexcept {
    switch (mode) {
    case Mode::land: return "Land_Mode";
    case Mode::space: return "Space_Mode";
    case Mode::unlocked: return "Unlocked";
    }
    return "";
}

// One control point of a Petroglyph camera spline: a zoom fraction and the
// value at that fraction. Petroglyph mixes ',' and ' ' separators inside one
// list, so the separator carries no structure; a spline is a flat numeric
// sequence read two at a time.
struct SplinePoint final {
    float fraction{};
    float value{};

    friend bool operator==(const SplinePoint&, const SplinePoint&) = default;
};

// Every field below is an XML-sourced constant. Names match the source tags so
// a reader can trace each value back to plan/inventories/camera-constants.json.
// No value is defaulted to a guessed original; an unset struct is inert and
// `validate` rejects it.
struct Constants final {
    // data/xml/tacticalcameras.xml, per <TacticalCamera Name=...>
    float distance_min{};
    float distance_max{};
    float distance_default{};
    float distance_per_mouse_unit{};
    float distance_smooth_time{};

    float pitch_min{};
    float pitch_max{};
    float pitch_default{};
    float pitch_per_mouse_unit{};
    float pitch_per_zoom_unit{};
    float pitch_when_zoomed_in{};
    float pitch_zoom_begin_fraction{};

    float yaw_min{};
    float yaw_max{};
    float yaw_default{};
    float yaw_per_mouse_unit{};

    float fov_min{};
    float fov_max{};
    float fov_default{};
    float fov_per_mouse_unit{};

    float near_clip{};
    float far_clip{};

    // Terrain following (Land_Mode). Nonzero location_follows_terrain makes a
    // map camera with a ground source keep its target on the terrain, easing
    // up and down with the two smooth times, and its eye at least
    // min_height_above_terrain above the ground. Absent fields stay inert.
    float location_follows_terrain{};
    float location_height_up_smooth_time{};
    float location_height_down_smooth_time{};
    float min_height_above_terrain{};

    // Use_Splines. When set, distance_spline and pitch_spline are consumed and
    // the pitch spline supersedes pitch_min/pitch_max. See the header note on
    // `solve` for why, and what still has to confirm it.
    bool use_splines{};
    std::vector<SplinePoint> distance_spline;
    std::vector<SplinePoint> pitch_spline;

    // data/xml/gameconstants.xml, global tactical scroll constants.
    float tactical_min_scroll_speed{};
    float tactical_max_scroll_speed{};
    float tactical_edge_scroll_region{};
    float tactical_offscreen_scroll_region{};
    float push_scroll_speed_modifier{};
    float scroll_acceleration_factor{};
    float scroll_deceleration_factor{};
};

// The camera values a consumer needs for one frame. All angles are degrees.
struct State final {
    float zoom{};
    float distance{};
    float pitch_degrees{};
    float yaw_degrees{};
    float fov_degrees{};
    // World units per second at this zoom, before any per-input modifier.
    float pan_speed{};

    friend bool operator==(const State&, const State&) = default;
};

// A pan request in view-relative screen axes: +x pans right, +y pans "up" the
// screen (away from the viewer along the ground plane).
struct PanInput final {
    float x{};
    float y{};
    // Right-button push scrolling applies push_scroll_speed_modifier.
    bool push_scroll{};
};

// A world-space ground-plane displacement. The caller owns how this is applied
// to its own camera basis and its own map bounds; this module never clamps to a
// map, because map bounds are not a camera constant.
struct PanDisplacement final {
    float x{};
    float y{};

    friend bool operator==(const PanDisplacement&, const PanDisplacement&) = default;
};

namespace diagnostic_codes {
inline constexpr std::string_view invalid_constants = "EAWR-CAMERA-0001";
inline constexpr std::string_view invalid_spline = "EAWR-CAMERA-0002";
inline constexpr std::string_view invalid_request = "EAWR-CAMERA-0003";
} // namespace diagnostic_codes

// Rejects constants that cannot describe a camera: a non-finite value, an
// inverted or empty distance range, an inverted pitch/yaw/FOV range, a
// non-positive near clip, a far clip at or below the near clip, an inverted
// scroll-speed range, or (when use_splines is set) a spline with fewer than two
// control points or a decreasing fraction. It never substitutes a default.
[[nodiscard]] core::Result<void> validate(const Constants& constants);

// Piecewise-linear evaluation of a Petroglyph camera spline. Fractions below
// the first control point clamp to its value and fractions above the last clamp
// to the last, which is why the shipped land pitch spline carries a redundant
// trailing point at fraction 1.1.
[[nodiscard]] core::Result<float> evaluate_spline(
    std::span<const SplinePoint> points, float fraction);

// Parses one Petroglyph scalar lexeme. A trailing 'f'/'F' suffix is accepted
// because the shipped data carries values such as "770.0f". The conversion is
// exception-free, locale-independent, must consume the whole token, and rejects
// hexadecimal, infinity and NaN spellings.
[[nodiscard]] core::Result<float> parse_scalar(std::string_view text);

// Parses a flat Petroglyph spline lexeme such as "0.0,17.0, 0.35,45.0, 1.0, 55.0".
// ',' and whitespace are interchangeable separators, a trailing 'f' suffix is
// accepted, an odd token count is rejected, and fractions must be
// non-decreasing. This is a pure string function; it reads no file.
[[nodiscard]] core::Result<std::vector<SplinePoint>> parse_spline(std::string_view text);

// Clamps a zoom request into [min_zoom, max_zoom]. A non-finite request is an
// error rather than a silently substituted value.
[[nodiscard]] core::Result<float> clamp_zoom(float zoom);

// Resolves the camera state at one zoom.
//
// distance: use_splines evaluates distance_spline, otherwise it interpolates
// linearly from distance_min to distance_max. The result is clamped into
// [distance_min, distance_max] either way.
//
// pitch: use_splines evaluates pitch_spline, and that spline **supersedes**
// pitch_min/pitch_max. The shipped EaW and FoC Land_Mode set
// Pitch_Min = Pitch_Max = Pitch_Default = 50 while authoring a pitch spline
// spanning 17 to 55 degrees; applying the clamp would make the spline,
// Pitch_When_Zoomed_In and Pitch_Zoom_Begin_Fraction all dead data. This is a
// derived reading of the shipped constants, not a measured engine fact, and it
// is the one rule in this module that the original-capture comparison still has
// to confirm. Without splines, pitch below pitch_zoom_begin_fraction blends
// pitch_when_zoomed_in toward pitch_default, and above it follows
// pitch_default + pitch_per_zoom_unit * (zoom - max(begin, 0)); that branch is
// then clamped into [pitch_min, pitch_max].
//
// pan_speed: linear interpolation from tactical_min_scroll_speed to
// tactical_max_scroll_speed on the **normalised distance**, not directly on the
// zoom parameter. For the linear modes the two agree; for the spline land mode
// the speed follows the curve the camera actually flies. Also derived; the
// timed measurement against the original is outstanding.
[[nodiscard]] core::Result<State> solve(const Constants& constants, float zoom);

// One mouse-wheel detent. Petroglyph stores Distance_Per_Mouse_Unit in world
// units, so a detent is converted into a zoom-parameter step against the mode's
// own distance range. `detents` is positive to zoom out and negative to zoom in.
[[nodiscard]] core::Result<float> apply_zoom_step(
    const Constants& constants, float zoom, float detents);

// Ground-plane displacement for one pan input over `delta_seconds`.
[[nodiscard]] core::Result<PanDisplacement> apply_pan(
    const Constants& constants, const State& state, const PanInput& input,
    float delta_seconds);

// The vertical field of view a renderer draws a camera angle with (#515,
// docs/behaviour/tactical-camera-input.md "Field of view"). FoC reads the XML angle
// (Fov_Default, Tactical_Overview_FOV) as the horizontal angle of a 4:3 screen and keeps that
// screen's vertical angle on 16:9, 2 * atan(0.75 * tan(angle / 2)); the remake keeps it on every
// aspect (#195). Refuses an angle outside (0, 180).
[[nodiscard]] core::Result<float> vertical_fov_degrees(float foc_fov_degrees);

// Ground-plane displacement for a pointer drag, in view axes (+x screen right,
// +y screen up). `screen_x`/`screen_y` are the drag in viewport heights, so a
// drag of one full viewport height moves the frustum height at the target
// distance, 2 * distance * tan(vertical fov / 2) with the state angle's
// vertical_fov_degrees, whatever the zoom. This is a displacement per event,
// never a velocity, so it does not depend on the frame rate. Local project
// policy, not a claim about the original drag.
[[nodiscard]] core::Result<PanDisplacement> drag_displacement(
    const State& state, float screen_x, float screen_y);

// Edge scrolling. `cursor` and `extent` are in the same pixel space. The result
// is -1, 0 or +1 per axis: inside tactical_edge_scroll_region of an edge scrolls
// toward it, and a cursor outside the viewport keeps scrolling while it remains
// within tactical_offscreen_scroll_region. A cursor further out than that has
// left the window, which stops the scroll rather than accelerating it.
struct EdgeScroll final {
    int axis_x{};
    int axis_y{};

    friend bool operator==(const EdgeScroll&, const EdgeScroll&) = default;
};

[[nodiscard]] core::Result<EdgeScroll> edge_scroll(
    const Constants& constants, float cursor_x, float cursor_y,
    float extent_x, float extent_y);

// Yaw from horizontal mouse motion, wrapped into [yaw_min, yaw_max] when that
// range spans a full turn and clamped otherwise.
[[nodiscard]] core::Result<float> apply_rotate(
    const Constants& constants, float yaw_degrees, float mouse_units);

// Pitch from vertical mouse motion, always clamped into [pitch_min, pitch_max].
// This is the free-look pitch input, independent of the zoom pitch curve; the
// shipped Land_Mode sets pitch_per_mouse_unit to 0, which makes it inert there.
[[nodiscard]] core::Result<float> apply_pitch(
    const Constants& constants, float pitch_degrees, float mouse_units);

// First-order smoothing toward a target over `delta_seconds`, using a
// Petroglyph *_Smooth_Time as the time constant. A non-positive smooth time
// snaps. Deterministic and frame-rate independent for a fixed delta.
[[nodiscard]] core::Result<float> smooth_toward(
    float current, float target, float smooth_time, float delta_seconds);

// Eye position for a target point, distance, pitch and yaw. Y is up, matching
// the viewer's FixedCamera basis. Pitch is measured down from the horizon, so
// the shipped 50-degree land pitch places the eye above and behind the target.
[[nodiscard]] core::Result<std::array<float, 3>> eye_position(
    std::span<const float, 3> target, float distance, float pitch_degrees,
    float yaw_degrees);

} // namespace eawr::presentation::camera
