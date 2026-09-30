#pragma once

#include "eawr/presentation/camera/camera.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace eawr::presentation::camera {

// A caller-supplied legal rectangle for the camera TARGET in source X/Y.
// This is an explicit contract, never a terrain or TED volume inferred here.
struct SourceTargetBounds final {
    float min_x{};
    float max_x{};
    float min_y{};
    float max_y{};
    std::string logical_path;
    std::string source_id;
    std::string authority;
};

struct RenderTargetBounds final {
    float min_x{};
    float max_x{};
    float min_z{};
    float max_z{};

    friend bool operator==(const RenderTargetBounds&, const RenderTargetBounds&) = default;
};

// Plain presentation values. The host may copy these to its renderer camera.
// Keeping the full frame permits an exact pinned capture override.
struct TacticalFrame final {
    std::uint32_t width{};
    std::uint32_t height{};
    float vertical_fov_degrees{};
    float near_plane{};
    float far_plane{};
    std::array<float, 3> eye{};
    std::array<float, 3> target{};
    std::array<float, 3> up{0.0F, 1.0F, 0.0F};

    friend bool operator==(const TacticalFrame&, const TacticalFrame&) = default;
};

// Orbit pitch range, degrees down from the horizon (negative looks up from
// below). A valid range satisfies -89 <= min < max <= 89, so the view is never
// vertical. Space uses the XML Pitch_Min..Pitch_Max, as FoC clamps its tilt
// (FoC: -10..85, so the view may look up from under the battle plane). Land is
// local project policy: FoC's Land_Mode pins its pitch range at 50, so the land
// range is FoC's space range with the minimum raised from -10 to 5 (the eye
// stays above the target and never looks up from under the terrain), from
// near-horizontal to near-overhead. It also bounds the terrain-clearance tilt.
struct OrbitPitchRange final {
    float min_degrees{};
    float max_degrees{};

    friend bool operator==(const OrbitPitchRange&, const OrbitPitchRange&) = default;
};
inline constexpr OrbitPitchRange land_orbit_pitch_range{5.0F, 85.0F};
// Project deviation (owner, #337/#348): FoC's Land_Mode tilts 0 degrees per
// mouse unit, so Ctrl + vertical middle drag never tilts FoC land. The land
// map camera tilts at FoC Space_Mode's Pitch_Per_Mouse_Unit (-1.5) instead,
// as it does in space, whenever the loaded Land_Mode rate is 0.
inline constexpr float land_project_pitch_per_mouse_unit = -1.5F;
// Pitch_Min..Pitch_Max limited to +-89 degrees.
[[nodiscard]] OrbitPitchRange space_orbit_pitch_range(const Constants& constants) noexcept;

struct TacticalStep final {
    PanInput pan{};
    float wheel_detents{};
    float yaw_mouse_units{};
    float delta_seconds{};
    // Pointer drag in viewport heights, view axes (+x right, +y up). Applied
    // once as a displacement (see drag_displacement), bypassing the eased pan
    // velocity, so the same physical drag travels the same at any frame rate.
    PanDisplacement drag{};
    // Orbit pitch in mouse units, a displacement like yaw_mouse_units: one
    // unit tilts Pitch_Per_Mouse_Unit degrees, as in FoC (its -1.5 in space
    // turns a positive, upward unit toward the horizon). See the class note.
    float orbit_pitch_units{};
    // A host tilt in degrees on top of orbit_pitch_units (terrain clearance),
    // positive toward overhead; clamped like the orbit. Not an input rate.
    float pitch_adjust_degrees{};
    // FoC grab translate in mouse units, view axes (+x right, +y up): one
    // unit moves the target 1/100 of the live camera distance. Applied once as
    // a displacement, like `drag`.
    PanDisplacement translate_units{};
    // Project scale on the XML pan speed for the eased pan (held keys, edge
    // scroll, push scroll). The drag keeps its screen-matched scale.
    float pan_speed_scale{1.0F};
    // The range the orbited pitch is clamped to (see OrbitPitchRange).
    OrbitPitchRange orbit_pitch_range{land_orbit_pitch_range};
    // #82: the yaw at which the pan, drag and translate view axes are read,
    // when the drawn view is not the controller's own (FoC's map overview
    // draws yaw 0 and keeps the tactical yaw for its return). Absent: the
    // controller's yaw. The controller's yaw itself is unchanged.
    std::optional<float> view_yaw_degrees{};
};

namespace diagnostic_codes {
inline constexpr std::string_view invalid_target_bounds = "EAWR-CAMERA-0401";
inline constexpr std::string_view invalid_controller_request = "EAWR-CAMERA-0402";
} // namespace diagnostic_codes

// Local project policy: solve zoom and yaw, apply view-relative pan, then clamp
// the target. At yaw 0, screen right is +X and screen up is -Z. At yaw 90,
// screen right is -Z and screen up is -X. The target is clamped, not the frustum.
// Pan velocity is retained between steps; a blocked boundary clears only its
// world-axis component so motion along the boundary continues.
//
// Orbit pitch is an offset on the zoom-linked pitch. It persists after the
// drag, so the view keeps its pitch; a later zoom changes the pitch by the
// zoom curve's own change from there, without a jump. While an offset is held
// the pitch is clamped to the orbit range. A new controller (reset) has none.
class BoundedTacticalController final {
public:
    [[nodiscard]] static core::Result<BoundedTacticalController> create(
        Constants constants, SourceTargetBounds source_bounds,
        std::array<float, 3> render_target, float zoom, float yaw_degrees,
        std::uint32_t width, std::uint32_t height);

    // On failure every observable field is unchanged. While capture is locked,
    // interaction (including malformed interaction) is ignored entirely.
    [[nodiscard]] core::Result<void> advance(const TacticalStep& step);
    [[nodiscard]] core::Result<void> set_viewport(std::uint32_t width, std::uint32_t height);
    // Moves the target to render height `height` (terrain following) and
    // rebuilds the frame around it; X/Z, zoom, pitch and yaw are unchanged.
    // Ignored while capture is locked; a non-finite height is refused.
    [[nodiscard]] core::Result<void> set_target_height(float height);
    // #82: looks at render (x, z), clamped into the target bounds, keeping the
    // height, zoom, pitch and yaw; pan velocity stops (a control group's
    // camera focus). Ignored while capture is locked; non-finite is refused.
    [[nodiscard]] core::Result<void> set_target(float x, float z);
    [[nodiscard]] core::Result<void> lock_capture(const TacticalFrame& fixed_frame);
    void unlock_capture() noexcept { capture_locked_ = false; }

    [[nodiscard]] bool capture_locked() const noexcept { return capture_locked_; }
    [[nodiscard]] const TacticalFrame& frame() const noexcept {
        return capture_locked_ ? fixed_frame_ : tactical_frame_;
    }
    [[nodiscard]] const TacticalFrame& tactical_frame() const noexcept { return tactical_frame_; }
    [[nodiscard]] const State& state() const noexcept { return state_; }
    [[nodiscard]] float target_zoom() const noexcept { return target_zoom_; }
    void stop_pan() noexcept { pan_velocity_ = {}; }
    [[nodiscard]] float yaw_degrees() const noexcept { return yaw_degrees_; }
    [[nodiscard]] float orbit_pitch_offset() const noexcept { return orbit_pitch_offset_; }
    [[nodiscard]] const RenderTargetBounds& render_bounds() const noexcept { return bounds_; }
    [[nodiscard]] const SourceTargetBounds& source_bounds() const noexcept { return source_bounds_; }

private:
    BoundedTacticalController(Constants constants, SourceTargetBounds source_bounds,
                              RenderTargetBounds bounds, State state, float yaw_degrees,
                              TacticalFrame frame)
        : constants_(std::move(constants)), source_bounds_(std::move(source_bounds)),
          bounds_(bounds), state_(state), target_zoom_(state.zoom),
          yaw_degrees_(yaw_degrees), tactical_frame_(frame) {}

    Constants constants_;
    SourceTargetBounds source_bounds_;
    RenderTargetBounds bounds_;
    State state_;
    float target_zoom_{};
    PanDisplacement pan_velocity_{};
    float yaw_degrees_{};
    float orbit_pitch_offset_{};
    TacticalFrame tactical_frame_;
    TacticalFrame fixed_frame_;
    bool capture_locked_{};
};

} // namespace eawr::presentation::camera
