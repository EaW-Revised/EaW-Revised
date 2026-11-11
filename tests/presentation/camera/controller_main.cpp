#include "../../support/check.hpp"
#include "eawr/presentation/camera/controller.hpp"
#include "../../../apps/viewer/src/battlecam_chase.hpp"
#include "../../../apps/viewer/src/battlecam_selection.hpp"
#include "../../../apps/viewer/src/battlecam_shots.hpp"
#include "../../../apps/viewer/src/battlecam_tec.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace camera = eawr::presentation::camera;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(bool condition, std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAILED: ");
}
void close(float actual, float expected, std::string_view message, float tolerance = 1e-3F) {
    expect(std::abs(actual - expected) <= tolerance, message);
}

camera::Constants constants() {
    camera::Constants c;
    c.distance_min = 100.0F;
    c.distance_max = 200.0F;
    c.distance_default = 100.0F;
    c.distance_per_mouse_unit = 50.0F;
    c.pitch_min = 45.0F;
    c.pitch_max = 45.0F;
    c.pitch_default = 45.0F;
    c.pitch_when_zoomed_in = 45.0F;
    c.yaw_min = -180.0F;
    c.yaw_max = 180.0F;
    c.yaw_per_mouse_unit = 90.0F;
    // Negative like FoC's space -1.5: a positive (screen-up) unit tilts toward the horizon.
    c.pitch_per_mouse_unit = -90.0F;
    c.fov_min = 55.0F;
    c.fov_max = 55.0F;
    c.fov_default = 55.0F;
    c.near_clip = 1.0F;
    c.far_clip = 1000.0F;
    c.tactical_min_scroll_speed = 10.0F;
    c.tactical_max_scroll_speed = 20.0F;
    c.push_scroll_speed_modifier = 2.0F;
    return c;
}
camera::SourceTargetBounds bounds() {
    return {-30.0F, 70.0F, -80.0F, 20.0F,
            "maps/explicit-camera-bounds.json", "synthetic-map", "authored target rectangle"};
}
camera::BoundedTacticalController make(std::array<float, 3> target = {0.0F, 5.0F, 0.0F},
                                       float yaw = 0.0F) {
    auto result = camera::BoundedTacticalController::create(
        constants(), bounds(), target, 0.0F, yaw, 1280, 720);
    expect(result.has_value(), "valid bounded controller creates");
    return result.value();
}

void test_bounds_and_basis() {
    auto initially_rotated = make({0.0F, 5.0F, 0.0F}, 45.0F);
    close(initially_rotated.state().yaw_degrees, 45.0F,
          "solved state exposes the requested initial yaw");
    auto c = make();
    expect(c.render_bounds() == camera::RenderTargetBounds{-30.0F, 70.0F, -20.0F, 80.0F},
           "asymmetric source Y is inverted exactly once");
    expect(c.source_bounds().source_id == "synthetic-map", "provenance retained");
    expect(c.advance({{1.0F, 0.0F, false}, 0.0F, 0.0F, 1.0F}).has_value(),
           "yaw zero right pan accepted");
    close(c.frame().target[0], 10.0F, "yaw zero right is +X");
    close(c.frame().target[2], 0.0F, "yaw zero right leaves Z");
    expect(c.advance({{0.0F, 1.0F, false}, 0.0F, 0.0F, 1.0F}).has_value(),
           "yaw zero up pan accepted");
    close(c.frame().target[2], -10.0F, "yaw zero up is -Z");
    expect(c.advance({{1.0F, 0.0F, false}, 0.0F, 1.0F, 1.0F}).has_value(),
           "rotation and pan accepted in same step");
    close(c.yaw_degrees(), 90.0F, "yaw updated first");
    close(c.state().yaw_degrees, 90.0F, "solved state exposes the updated yaw");
    close(c.frame().target[0], 10.0F, "yaw 90 right leaves X");
    close(c.frame().target[2], -20.0F, "yaw 90 right is -Z");
    expect(c.advance({{0.0F, 1.0F, false}, 0.0F, 0.0F, 1.0F}).has_value(),
           "yaw 90 up pan accepted");
    close(c.frame().target[0], 0.0F, "yaw 90 up is -X");
    close(c.frame().target[1], 5.0F, "target elevation preserved");
}

void test_corners_and_cancellation() {
    auto c = make({69.0F, 2.0F, -19.0F});
    expect(c.advance({{1.0F, 1.0F, false}, 0.0F, 0.0F, 100.0F}).has_value(),
           "diagonal reaches corner");
    close(c.frame().target[0], 70.0F, "right edge clamps");
    close(c.frame().target[2], -20.0F, "near edge clamps");
    const auto at_corner = c.frame();
    expect(c.advance({{1.0F, 1.0F, false}, 0.0F, 0.0F, 100.0F}).has_value(),
           "outward pan accepted");
    expect(c.frame() == at_corner, "outward motion has no stored velocity");
    expect(c.advance({{-1.0F, 0.0F, false}, 0.0F, 0.0F, 1.0F}).has_value(),
           "inward pan accepted immediately");
    close(c.frame().target[0], 60.0F, "outward motion does not delay inward motion");
    auto far = make({-100.0F, 0.0F, 1000.0F});
    close(far.frame().target[0], -30.0F, "initial X clamps");
    close(far.frame().target[2], 80.0F, "initial Z clamps once");
    const std::array<std::array<float, 3>, 4> corners{{
        {-30.0F, 0.0F, -20.0F}, {-30.0F, 0.0F, 80.0F},
        {70.0F, 0.0F, -20.0F}, {70.0F, 0.0F, 80.0F},
    }};
    for (const auto& corner : corners) {
        auto at = make(corner);
        const float x_out = corner[0] < 0.0F ? -1.0F : 1.0F;
        const float y_out = corner[2] < 0.0F ? 1.0F : -1.0F;
        expect(at.advance({{x_out, y_out, false}, 0.0F, 0.0F, 1.0F}).has_value(),
               "outward pan at every corner accepted");
        expect(at.frame().target == corner, "all four corners remain clamped");
    }
}

void test_zoom_and_push() {
    auto c = make();
    expect(c.advance({{1.0F, 0.0F, true}, 1.0F, 0.0F, 1.0F}).has_value(),
           "wheel and push pan accepted together");
    close(c.state().zoom, 0.5F, "wheel applies before pan");
    close(c.state().pan_speed, 15.0F, "pan speed uses new zoom");
    close(c.frame().target[0], 30.0F, "push modifier applies to new pan speed");
    const auto before = c.frame();
    expect(c.advance({}).has_value(), "zero input accepted");
    expect(c.frame() == before, "zero input does not drift");
}

void test_atomic_rejections() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    auto b = bounds();
    b.min_x = b.max_x;
    expect(!camera::BoundedTacticalController::create(constants(), b, {0,0,0}, 0, 0, 1, 1),
           "degenerate bounds rejected");
    b = bounds(); b.min_y = 30.0F;
    expect(!camera::BoundedTacticalController::create(constants(), b, {0,0,0}, 0, 0, 1, 1),
           "inverted bounds rejected");
    b = bounds(); b.max_y = inf;
    expect(!camera::BoundedTacticalController::create(constants(), b, {0,0,0}, 0, 0, 1, 1),
           "nonfinite bounds rejected");
    b = bounds(); b.authority.clear();
    expect(!camera::BoundedTacticalController::create(constants(), b, {0,0,0}, 0, 0, 1, 1),
           "unproven bounds rejected");
    expect(!camera::BoundedTacticalController::create(constants(), bounds(), {nan,0,0}, 0, 0, 1, 1),
           "nonfinite initial target rejected");
    auto c = make();
    const auto original_frame = c.frame();
    const auto original_state = c.state();
    const float original_yaw = c.yaw_degrees();
    const camera::TacticalStep bad_steps[]{
        {{1,0,false}, nan, 1, 1}, {{1,0,false}, 0, inf, 1},
        {{1,0,false}, 0, 0, -1}, {{inf,0,false}, 0, 0, 1},
        {{2,0,false}, 0, 0, 1}, {{1,0,false}, 0, 0, inf},
        {{1,0,false}, 0, 0, std::numeric_limits<float>::max()},
    };
    for (const auto& step : bad_steps) {
        expect(!c.advance(step), "invalid or overflowing step rejected");
        expect(c.frame() == original_frame && c.state() == original_state
                   && c.yaw_degrees() == original_yaw,
               "rejected step keeps full controller state");
    }
}

void test_fixed_capture() {
    auto c = make();
    auto fixed = c.frame();
    fixed.width = 640; fixed.height = 480;
    fixed.target = {31.0F, 7.0F, -19.0F};
    fixed.eye = {17.0F, 250.0F, 333.0F};
    fixed.up = {0.0F, 0.5F, 0.5F};
    fixed.vertical_fov_degrees = 37.0F;
    fixed.near_plane = 3.0F; fixed.far_plane = 900.0F;
    auto invalid = fixed; invalid.eye[0] = std::numeric_limits<float>::infinity();
    expect(!c.lock_capture(invalid) && !c.capture_locked(), "bad capture rejected atomically");
    const auto tactical_before = c.tactical_frame();
    expect(c.lock_capture(fixed).has_value(), "fixed capture locks");
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    expect(c.advance({{nan, nan, true}, nan, nan, nan}).has_value(),
           "locked interaction ignored before validation");
    expect(c.frame() == fixed, "fixed frame preserved exactly");
    expect(c.tactical_frame() == tactical_before, "tactical frame paused during capture");
    expect(c.set_viewport(1920, 1080).has_value(), "locked resize is ignored");
    expect(c.frame() == fixed && c.tactical_frame() == tactical_before,
           "locked resize preserves the submitted frame and tactical state exactly");
    auto replacement = fixed; replacement.width = 1920;
    expect(!c.lock_capture(replacement), "active capture cannot be replaced");
    expect(c.frame() == fixed, "rejected replacement preserves fixed frame");
    expect(c.lock_capture(fixed).has_value(), "same fixed capture is idempotent");
    c.unlock_capture();
    expect(c.frame() == tactical_before, "unlock restores tactical frame exactly");
}

void test_viewport_transaction() {
    auto c = make();
    const auto before = c.frame();
    expect(!c.set_viewport(0, 1080), "empty viewport rejected");
    expect(c.frame() == before, "rejected viewport preserves complete frame");
    expect(c.set_viewport(1920, 1080).has_value(), "valid viewport accepted");
    expect(c.frame().width == 1920 && c.frame().height == 1080,
           "new viewport published together");
    expect(c.frame().target == before.target && c.frame().eye == before.eye,
           "resize preserves pose");
}
// #195: a window resize widens or narrows the view and never squashes it.
// The renderer sets a vertical-FOV perspective and Godot takes the aspect from
// the viewport every frame, so a resize must publish only the new size: the
// vertical FOV, clip planes and pose stay, and the horizontal extent follows
// the aspect (ultrawide sees more at the sides, 5:4 less).
void test_viewport_aspect() {
    auto c = make();
    const auto before = c.frame();
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 7> sizes{{
        {1920, 1080}, {2560, 1080}, {3440, 1440}, {5120, 1440}, {1280, 1024}, {1000, 800}, {1280, 720}}};
    for (const auto& [width, height] : sizes) {
        expect(c.set_viewport(width, height).has_value(), "aspect resize accepted");
        const auto& frame = c.frame();
        expect(frame.width == width && frame.height == height, "aspect resize publishes the new size");
        expect(frame.vertical_fov_degrees == before.vertical_fov_degrees && frame.near_plane == before.near_plane &&
                   frame.far_plane == before.far_plane,
               "aspect resize keeps the vertical FOV and clip planes");
        expect(frame.eye == before.eye && frame.target == before.target && frame.up == before.up,
               "aspect resize keeps the pose");
    }
    expect(c.frame() == before, "back at 1280x720 the frame is the original");
}
void test_target_height() {
    auto c = make();
    const auto before = c.frame();
    expect(!c.set_target_height(std::numeric_limits<float>::infinity()), "non-finite height rejected");
    expect(c.frame() == before, "rejected height preserves the frame");
    expect(c.set_target_height(42.0F).has_value(), "finite height accepted");
    close(c.frame().target[1], 42.0F, "target moves to the requested height");
    close(c.frame().eye[1] - c.frame().target[1], before.eye[1] - before.target[1],
          "eye keeps its offset above the target");
    expect(c.frame().target[0] == before.target[0] && c.frame().target[2] == before.target[2]
           && c.frame().eye[0] == before.eye[0] && c.frame().eye[2] == before.eye[2]
           && c.state() == make().state(), "height leaves X/Z, zoom, pitch and yaw unchanged");
    const auto raised = c.frame();
    expect(c.lock_capture(before).has_value() && c.set_target_height(-7.0F).has_value()
           && c.tactical_frame() == raised, "locked capture ignores height changes");
}

void test_zoom_inertia_and_pitch() {
    auto values = constants();
    values.distance_smooth_time = 0.2F;
    values.pitch_min = 20.0F;
    values.pitch_max = 60.0F;
    values.pitch_default = 60.0F;
    values.pitch_when_zoomed_in = 20.0F;
    values.pitch_zoom_begin_fraction = 1.0F;
    const auto create = [&] {
        return camera::BoundedTacticalController::create(values, bounds(),
            {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
    };
    auto once = create();
    expect(once.advance({{}, 1.0F, 0.0F, 0.2F}).has_value(), "eased wheel step accepted");
    const float fraction = 1.0F - std::exp(-1.0F);
    close(once.target_zoom(), 0.5F, "wheel updates target zoom immediately");
    close(once.state().distance, 100.0F + 50.0F * fraction,
          "one smooth time travels 63 percent of the distance step");
    close(once.state().pitch_degrees, 20.0F + 40.0F * once.state().zoom,
          "pitch follows the eased zoom distance");
    for (const int fps : {30, 144}) {
        auto stepped = create();
        expect(stepped.advance({{}, 1.0F, 0.0F, 0.0F}).has_value(),
               "wheel updates target without moving on zero duration");
        for (int frame = 0; frame < fps; ++frame) {
            expect(stepped.advance({{}, 0.0F, 0.0F, 0.2F / fps}).has_value(),
                   "easing frame accepted");
        }
        expect(std::abs(stepped.state().distance - once.state().distance) < 0.01F,
               "30 and 144 fps agree with a single smooth-time step");
    }
    expect(once.advance({{}, 1000.0F, 0.0F, 100.0F}).has_value(),
           "far limit accepted");
    expect(once.target_zoom() == 1.0F && once.state().distance <= values.distance_max,
           "zoom does not overshoot the far limit");
    expect(once.advance({{}, -1000.0F, 0.0F, 100.0F}).has_value(),
           "near limit accepted");
    expect(once.target_zoom() == 0.0F && once.state().distance >= values.distance_min,
           "zoom does not overshoot the near limit");

    values.use_splines = true;
    values.distance_spline = {{0.0F, 100.0F}, {0.25F, 110.0F}, {1.0F, 200.0F}};
    values.pitch_spline = {{0.0F, 20.0F}, {0.25F, 50.0F}, {1.0F, 60.0F}};
    auto spline = create();
    expect(spline.advance({{}, 2.0F, 0.0F, 0.2F}).has_value(),
           "spline zoom step accepted");
    close(spline.state().distance, 100.0F + 100.0F * fraction,
          "spline target distance eases by the same fraction");
    close(spline.state().pitch_degrees,
          camera::evaluate_spline(values.pitch_spline, spline.state().zoom).value(),
          "pitch spline follows the live distance's inverse spline fraction");
}

void test_scroll_ramp() {
    auto values = constants();
    values.scroll_acceleration_factor = 0.2F;
    values.scroll_deceleration_factor = 2.0F;
    auto c = camera::BoundedTacticalController::create(values, bounds(),
        {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
    expect(c.advance({{1.0F, 0.0F, false}, 0.0F, 0.0F, 0.2F}).has_value(),
           "scroll acceleration step accepted");
    const float speed = 10.0F;
    const float rise = 1.0F - std::exp(-1.0F);
    const float first = speed * (0.2F - 0.2F * rise);
    close(c.frame().target[0], first, "scroll accelerates with the authored factor");
    expect(c.advance({{}, 0.0F, 0.0F, 0.4F}).has_value(),
           "scroll release step accepted");
    const float brake_time = 0.2F / 2.0F;
    close(c.frame().target[0], first + speed * rise * brake_time
          * (1.0F - std::exp(-0.4F / brake_time)),
          "scroll braking factor shortens the acceleration time");
    expect(c.frame().target[0] > first, "release does not stop immediately");
    for (const int fps : {30, 144}) {
        auto stepped = camera::BoundedTacticalController::create(values, bounds(),
            {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
        for (int frame = 0; frame < fps; ++frame) {
            expect(stepped.advance({{1.0F, 0.0F, false}, 0.0F, 0.0F, 0.2F / fps}).has_value(),
                   "scroll acceleration substep accepted");
        }
        for (int frame = 0; frame < fps; ++frame) {
            expect(stepped.advance({{}, 0.0F, 0.0F, 0.4F / fps}).has_value(),
                   "scroll deceleration substep accepted");
        }
        expect(std::abs(stepped.frame().target[0] - c.frame().target[0]) < 0.01F,
               "scroll travel agrees at 30 and 144 fps");
    }
    values.scroll_acceleration_factor = 0.08F;
    values.scroll_deceleration_factor = 1.5F;
    auto foc = camera::BoundedTacticalController::create(values, bounds(),
        {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
    expect(foc.advance({{1.0F, 0.0F, false}, 0.0F, 0.0F, 0.2F}).has_value(),
           "FoC pan start accepted");
    const float at_release = foc.frame().target[0];
    expect(foc.advance({{}, 0.0F, 0.0F, 0.2F}).has_value(),
           "FoC pan release accepted");
    const float first_coast = foc.frame().target[0] - at_release;
    expect(first_coast > 0.0F && first_coast < 0.55F,
           "FoC release loses nearly all speed within 0.2 seconds");
    const float after_release = foc.frame().target[0];
    expect(foc.advance({{}, 0.0F, 0.0F, 0.2F}).has_value(),
           "FoC pan later idle step accepted");
    expect(foc.frame().target[0] - after_release < 0.02F,
           "FoC release does not coast for seconds");
}

void test_boundary_diagonal_frame_rate() {
    auto values = constants();
    values.scroll_acceleration_factor = 0.2F;
    const auto travel = [&](int fps) {
        auto c = camera::BoundedTacticalController::create(values, bounds(),
            {70.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
        for (int frame = 0; frame < fps; ++frame) {
            expect(c.advance({{1.0F, -1.0F, false}, 0.0F, 0.0F, 1.0F / fps}).has_value(),
                   "diagonal pan at right boundary accepted");
        }
        close(c.frame().target[0], 70.0F, "blocked X stays on the right boundary");
        return c.frame().target[2];
    };
    const float at_30 = travel(30);
    const float at_60 = travel(60);
    const float at_144 = travel(144);
    expect(std::abs(at_30 - at_60) < 0.02F && std::abs(at_60 - at_144) < 0.02F,
           "unblocked diagonal travel agrees at 30, 60 and 144 fps");
    expect(at_144 > 5.0F, "unblocked diagonal keeps accelerating along Z");
}

void test_concurrent_zoom_pan_frame_rate() {
    auto values = constants();
    values.distance_smooth_time = 0.21F;
    values.scroll_acceleration_factor = 0.08F;
    values.tactical_max_scroll_speed = 100.0F;
    const camera::SourceTargetBounds wide{-1000.0F, 1000.0F, -1000.0F, 1000.0F,
        "maps/wide-camera-bounds.json", "synthetic-map", "authored target rectangle"};
    const auto travel = [&](int fps) {
        auto c = camera::BoundedTacticalController::create(values, wide,
            {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
        expect(c.advance({{}, 2.0F, 0.0F, 0.0F}).has_value(),
               "zoom request starts without elapsed time");
        for (int frame = 0; frame < fps; ++frame) {
            expect(c.advance({{1.0F, 0.0F, false}, 0.0F, 0.0F, 0.2F / fps}).has_value(),
                   "concurrent zoom and pan accepted");
        }
        return c.frame().target[0];
    };
    const float at_30 = travel(30);
    const float at_60 = travel(60);
    const float at_144 = travel(144);
    expect(std::abs(at_30 - at_60) < 0.02F && std::abs(at_60 - at_144) < 0.02F,
           "concurrent zoom and pan travel agrees at 30, 60 and 144 fps");
    expect(std::abs(at_30 - travel(1)) < 0.02F,
           "one long concurrent step agrees with frame steps");
}

void test_flat_distance_spline_idle_pitch() {
    auto values = constants();
    values.use_splines = true;
    values.distance_spline = {{0.0F, 100.0F}, {0.3F, 150.0F},
                              {0.7F, 150.0F}, {1.0F, 200.0F}};
    values.pitch_min = 20.0F;
    values.pitch_max = 60.0F;
    values.pitch_spline = {{0.0F, 20.0F}, {1.0F, 60.0F}};
    auto c = camera::BoundedTacticalController::create(values, bounds(),
        {0.0F, 0.0F, 0.0F}, 0.5F, 0.0F, 1280, 720).value();
    const auto initial = c.state();
    close(initial.distance, 150.0F, "flat spline starts at its plateau distance");
    close(initial.pitch_degrees, 40.0F, "flat spline starts at the requested pitch");
    expect(c.advance({{}, 0.0F, 0.0F, 1.0F / 60.0F}).has_value(),
           "idle flat-spline frame accepted");
    close(c.state().zoom, initial.zoom, "idle flat spline keeps its zoom fraction");
    close(c.state().pitch_degrees, initial.pitch_degrees, "idle flat spline keeps its pitch");
}
} // namespace

// A pointer drag is a displacement in viewport heights: one physical drag
// travels the same at any frame rate, clamps to the bounds and never coasts.
void test_drag_displacement_frame_rate() {
    // #515: Fov_Default 55 is FoC's horizontal angle on 4:3; the frame draws that screen's vertical
    // angle, whose half-angle tangent is 3/4 of tan(27.5 degrees).
    const float screen = 2.0F * 100.0F * 0.75F * std::tan(27.5F * std::numbers::pi_v<float> / 180.0F);
    const float expected = 400.0F / 720.0F * screen;
    for (const int rate : {30, 60, 144}) {
        auto c = make();
        const float seconds = 1.0F / static_cast<float>(rate);
        bool ok = true;
        for (int frame = 0; frame < rate; ++frame) {
            camera::TacticalStep step{};
            step.delta_seconds = seconds;
            step.drag = {400.0F * seconds / 720.0F, 0.0F};
            ok = c.advance(step).has_value() && ok;
        }
        expect(ok, "drag steps accepted");
        expect(std::abs(c.frame().target[0] - expected) <= 1e-2F,
               "same physical drag travels the same at 30, 60 and 144 fps");
        close(c.frame().target[2], 0.0F, "horizontal drag leaves Z");
        const auto released = c.frame();
        expect(c.advance({{}, 0.0F, 0.0F, 1.0F}).has_value() && c.frame() == released,
               "a drag leaves no velocity to coast");
    }
    auto turned = make({0.0F, 5.0F, 0.0F}, 90.0F);
    expect(turned.advance({{}, 0.0F, 0.0F, 0.0F, {0.0F, 0.25F}}).has_value(),
           "zero-duration drag still applies");
    close(turned.frame().target[0], -0.25F * screen, "yaw 90 drag up is -X");
    auto edge = make();
    expect(edge.advance({{}, 0.0F, 0.0F, 0.0F, {10.0F, 0.0F}}).has_value(), "long drag accepted");
    close(edge.frame().target[0], 70.0F, "drag clamps to the target bounds");
    const auto before = edge.frame();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    expect(!edge.advance({{}, 0.0F, 0.0F, 0.0F, {nan, 0.0F}}) && edge.frame() == before,
           "non-finite drag rejected atomically");
}

// The project pan speed scale multiplies the eased pan (held, edge and push)
// but not the drag, and it halves travel at any frame rate.
void test_pan_speed_scale() {
    auto c = make();
    camera::TacticalStep half{{1.0F, 0.0F, false}, 0.0F, 0.0F, 1.0F};
    half.pan_speed_scale = 0.5F;
    expect(c.advance(half).has_value(), "half-speed pan accepted");
    close(c.frame().target[0], 5.0F, "a 0.5 scale halves the XML pan speed");
    close(c.state().pan_speed, 10.0F, "the solved state keeps the XML pan speed");
    half.pan.push_scroll = true;
    expect(c.advance(half).has_value(), "half-speed push pan accepted");
    close(c.frame().target[0], 15.0F, "the scale also applies under the push modifier");
    auto dragged = make();
    camera::TacticalStep drag{};
    drag.drag = {0.25F, 0.0F};
    drag.pan_speed_scale = 0.5F;
    expect(dragged.advance(drag).has_value(), "scaled drag accepted");
    // #515: Fov_Default 55 is FoC's horizontal angle on 4:3; the frame draws that screen's vertical
    // angle, whose half-angle tangent is 3/4 of tan(27.5 degrees).
    const float screen = 2.0F * 100.0F * 0.75F * std::tan(27.5F * std::numbers::pi_v<float> / 180.0F);
    close(dragged.frame().target[0], 0.25F * screen, "a drag keeps its screen-matched scale");

    auto values = constants();
    values.scroll_acceleration_factor = 0.08F;
    values.scroll_deceleration_factor = 1.5F;
    const camera::SourceTargetBounds wide{-1000.0F, 1000.0F, -1000.0F, 1000.0F,
        "maps/wide-camera-bounds.json", "synthetic-map", "authored target rectangle"};
    const auto travel = [&](const int fps, const float scale) {
        auto eased = camera::BoundedTacticalController::create(values, wide,
            {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
        for (int frame = 0; frame < fps; ++frame) {
            camera::TacticalStep step{{1.0F, 0.0F, false}, 0.0F, 0.0F, 0.5F / fps};
            step.pan_speed_scale = scale;
            expect(eased.advance(step).has_value(), "eased scaled pan frame accepted");
        }
        for (int frame = 0; frame < fps; ++frame) {
            camera::TacticalStep step{{}, 0.0F, 0.0F, 0.5F / fps};
            step.pan_speed_scale = scale;
            expect(eased.advance(step).has_value(), "eased scaled release frame accepted");
        }
        return eased.frame().target[0];
    };
    const float full = travel(60, 1.0F);
    for (const int fps : {30, 60, 144}) {
        close(travel(fps, 0.5F), 0.5F * full,
              "a 0.5 scale halves eased pan and coast at 30, 60 and 144 fps", 0.02F);
    }

    const auto before = c.frame();
    for (const float bad : {0.0F, -0.5F, std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity()}) {
        camera::TacticalStep step{{1.0F, 0.0F, false}, 0.0F, 0.0F, 1.0F};
        step.pan_speed_scale = bad;
        expect(!c.advance(step) && c.frame() == before,
               "a nonpositive or non-finite pan scale is rejected atomically");
    }
}

camera::TacticalStep orbit(const float pitch_units, const float yaw_units = 0.0F,
                           const camera::OrbitPitchRange range = camera::land_orbit_pitch_range) {
    camera::TacticalStep step{{}, 0.0F, yaw_units, 0.0F};
    step.orbit_pitch_units = pitch_units;
    step.orbit_pitch_range = range;
    return step;
}

// Orbit pitch: Pitch_Per_Mouse_Unit degrees per unit (FoC), clamped to the
// mode's range, kept after the drag, and cleared only by a new controller.
void test_orbit_pitch_limits_and_persistence() {
    auto c = make();
    close(c.state().pitch_degrees, 45.0F, "the fixture's zoom-linked pitch is 45");
    expect(c.advance(orbit(-0.1F)).has_value(), "orbit pitch accepted");
    close(c.state().pitch_degrees, 54.0F, "one unit tilts Pitch_Per_Mouse_Unit degrees");
    close(c.orbit_pitch_offset(), 9.0F, "the orbit is held as an offset");
    close(c.frame().eye[1] - c.frame().target[1],
          100.0F * std::sin(54.0F * std::numbers::pi_v<float> / 180.0F),
          "the eye follows the orbited pitch");
    expect(c.advance({{}, 0.0F, 0.0F, 1.0F}).has_value(), "idle step after the drag");
    close(c.state().pitch_degrees, 54.0F, "the orbited pitch is kept after release");
    expect(c.advance(orbit(-1.0F)).has_value(), "orbit past overhead accepted");
    close(c.state().pitch_degrees, camera::land_orbit_pitch_range.max_degrees,
          "land orbit stops near overhead");
    expect(c.advance(orbit(0.05F)).has_value(), "orbit back from the limit accepted");
    close(c.state().pitch_degrees, 80.5F, "motion back from the limit responds at once");
    expect(c.advance(orbit(2.0F)).has_value(), "orbit past the horizon accepted");
    close(c.state().pitch_degrees, camera::land_orbit_pitch_range.min_degrees,
          "land orbit stops near horizontal");
    expect(c.frame().eye[1] > c.frame().target[1], "a land orbit never looks up from below");

    // Space: FoC's Space_Mode range, -10..85 with a 45-degree start here.
    auto space_values = constants();
    space_values.pitch_min = -10.0F;
    space_values.pitch_max = 85.0F;
    const auto space_range = camera::space_orbit_pitch_range(space_values);
    expect(space_range == camera::OrbitPitchRange{-10.0F, 85.0F}, "space range is Pitch_Min..Pitch_Max");
    auto space = camera::BoundedTacticalController::create(space_values, bounds(),
        {0.0F, 5.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
    expect(space.advance(orbit(0.6F, 0.0F, space_range)).has_value(),
           "space orbit below the plane accepted");
    close(space.state().pitch_degrees, -9.0F, "space orbit passes under the plane");
    expect(space.frame().eye[1] < space.frame().target[1], "the eye is below the target plane");
    expect(space.advance(orbit(1.0F, 0.0F, space_range)).has_value(),
           "space orbit toward straight up accepted");
    close(space.state().pitch_degrees, -10.0F, "space orbit stops at Pitch_Min");
    expect(space.advance(orbit(-3.0F, 0.0F, space_range)).has_value(),
           "space orbit toward straight down accepted");
    close(space.state().pitch_degrees, 85.0F, "space orbit stops at Pitch_Max");
    auto wide = space_values;
    wide.pitch_min = -90.0F;
    wide.pitch_max = 90.0F;
    expect(camera::space_orbit_pitch_range(wide) == camera::OrbitPitchRange{-89.0F, 89.0F},
           "a wider XML range stops 1 degree short of vertical");

    // FoC Land_Mode: Pitch_Per_Mouse_Unit 0, so the controller itself never tilts
    // (the land map bridge adds the #348 project rate as a host tilt).
    auto flat_values = constants();
    flat_values.pitch_per_mouse_unit = 0.0F;
    auto flat = camera::BoundedTacticalController::create(flat_values, bounds(),
        {0.0F, 5.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
    expect(flat.advance(orbit(-40.0F, 0.0F)).has_value(), "a land vertical drag is accepted");
    close(flat.state().pitch_degrees, 45.0F, "zero Pitch_Per_Mouse_Unit keeps the pitch");
    close(flat.orbit_pitch_offset(), 0.0F, "and holds no orbit");
    auto clearance = flat_values;
    auto lifted = camera::BoundedTacticalController::create(clearance, bounds(),
        {0.0F, 5.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
    auto adjust = orbit(0.0F);
    adjust.pitch_adjust_degrees = 10.0F;
    expect(lifted.advance(adjust).has_value(), "a host pitch adjustment is accepted");
    close(lifted.state().pitch_degrees, 55.0F, "the host adjustment tilts in degrees");

    auto yawed = make();
    expect(yawed.advance(orbit(0.0F, 0.5F)).has_value(), "rotate without orbit accepted");
    close(yawed.state().pitch_degrees, 45.0F, "a plain rotate leaves the pitch");
    close(yawed.orbit_pitch_offset(), 0.0F, "a plain rotate holds no orbit");

    const auto before = c.frame();
    const float offset = c.orbit_pitch_offset();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto& step : {orbit(nan), orbit(0.1F, 0.0F, {10.0F, 10.0F}),
                             orbit(0.1F, 0.0F, {-90.0F, 40.0F}), orbit(0.1F, 0.0F, {0.0F, 90.0F}),
                             orbit(0.1F, 0.0F, {nan, 40.0F}),
                             orbit(std::numeric_limits<float>::max())}) {
        expect(!c.advance(step) && c.frame() == before && c.orbit_pitch_offset() == offset,
               "an invalid orbit step or range is rejected atomically");
    }
}

// A zoom after an orbit changes the pitch by the zoom curve's own change,
// starting from the orbited pitch, so there is no jump back to the curve.
void test_orbit_zoom_resumes_smoothly() {
    auto values = constants();
    values.distance_smooth_time = 0.2F;
    values.pitch_min = 20.0F;
    values.pitch_max = 70.0F;
    values.pitch_default = 30.0F;
    values.pitch_per_zoom_unit = 20.0F;
    values.pitch_zoom_begin_fraction = 0.0F;
    auto c = camera::BoundedTacticalController::create(values, bounds(),
        {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
    close(c.state().pitch_degrees, 30.0F, "zoom-linked pitch at zoom 0");
    expect(c.advance(orbit(-0.1F)).has_value(), "orbit up nine degrees");
    close(c.state().pitch_degrees, 39.0F, "orbited pitch");
    expect(c.advance({{}, 1.0F, 0.0F, 1.0F / 60.0F}).has_value(), "wheel detent starts easing");
    const float first = c.state().pitch_degrees;
    // Eased distance after one frame: zoom 0.04, so the curve adds 0.8 degrees.
    close(first, 39.0F + 20.0F * c.state().zoom, "the first eased frame moves on from the orbited pitch");
    expect(first < 40.0F, "a zoom does not jump back to the zoom-linked pitch");
    for (int frame = 0; frame < 120; ++frame) {
        expect(c.advance({{}, 0.0F, 0.0F, 1.0F / 60.0F}).has_value(), "easing frame accepted");
    }
    close(c.state().pitch_degrees, 30.0F + 20.0F * c.state().zoom + 9.0F,
          "the zoom curve's change is added to the orbited pitch", 1e-2F);
}

// The same physical Ctrl drag, with a concurrent zoom, reaches the same yaw
// and pitch at 30, 60 and 144 fps.
void test_orbit_frame_rate_independence() {
    auto values = constants();
    values.distance_smooth_time = 0.2F;
    values.pitch_min = 20.0F;
    values.pitch_max = 70.0F;
    values.pitch_default = 30.0F;
    values.pitch_per_zoom_unit = 20.0F;
    values.pitch_zoom_begin_fraction = 0.0F;
    values.yaw_per_mouse_unit = 1.5F;
    values.pitch_per_mouse_unit = -1.5F;
    const auto run = [&](const int fps) {
        auto c = camera::BoundedTacticalController::create(values, bounds(),
            {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1280, 720).value();
        expect(c.advance({{}, 1.0F, 0.0F, 0.0F}).has_value(), "zoom starts with the drag");
        const float seconds = 1.0F / static_cast<float>(fps);
        for (int frame = 0; frame < fps; ++frame) {
            // 128 px/s right and 72 px/s down on 1280 x 720 with the committed
            // Ctrl scales of -1: -10 and -10 mouse units per second.
            auto step = orbit(-10.0F * seconds, -10.0F * seconds);
            step.delta_seconds = seconds;
            expect(c.advance(step).has_value(), "orbit frame accepted");
        }
        return std::pair{c.yaw_degrees(), c.state().pitch_degrees};
    };
    const auto at_30 = run(30);
    for (const int fps : {60, 144}) {
        const auto at = run(fps);
        close(at.first, at_30.first, "orbit yaw agrees at 30, 60 and 144 fps");
        close(at.second, at_30.second, "orbit pitch agrees at 30, 60 and 144 fps", 2e-3F);
    }
    close(at_30.first, -15.0F, "10 mouse units at Yaw_Per_Mouse_Unit 1.5");
    close(at_30.second, 30.0F + 10.0F * (1.0F - std::exp(-5.0F)) + 15.0F,
          "-10 units at Pitch_Per_Mouse_Unit -1.5 on top of the eased zoom pitch", 1e-2F);
}

// RO-7 and the FoC debug build: 100 mouse units (a full screen width) turn
// Yaw_Per_Mouse_Unit x 100 degrees; a translate unit moves 1/100 of the live
// distance, the same share at every zoom.
void test_foc_mouse_unit_law() {
    auto values = constants();
    values.yaw_min = -1000.0F;
    values.yaw_max = 1000.0F;
    values.yaw_per_mouse_unit = 1.5F;
    auto c = camera::BoundedTacticalController::create(values, bounds(),
        {0.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 1920, 1080).value();
    expect(c.advance({{}, 0.0F, 100.0F, 0.0F}).has_value(), "a full-width rotate is accepted");
    close(c.yaw_degrees(), 150.0F, "a full-width drag turns 150 degrees");
    expect(c.advance({{}, 0.0F, -50.0F, 0.0F}).has_value(), "a half-width rotate back is accepted");
    close(c.yaw_degrees(), 75.0F, "a half-width drag turns 75 degrees");

    auto moved = make();
    camera::TacticalStep translate{};
    translate.translate_units = {20.0F, -10.0F};
    expect(moved.advance(translate).has_value(), "a translate is accepted");
    close(moved.frame().target[0], 20.0F, "20 units right move 20% of the distance (100)");
    close(moved.frame().target[2], 10.0F, "10 units down the screen move back (+Z at yaw 0)");
    translate.translate_units = {std::numeric_limits<float>::infinity(), 0.0F};
    const auto before = moved.frame();
    expect(!moved.advance(translate) && moved.frame() == before, "a non-finite translate is rejected");
}

// #82 FoC's map overview draws yaw 0 while the tactical yaw is kept for its
// return: pan, drag and translate read their view axes at the drawn yaw, and
// the controller's own yaw does not change.
void test_view_yaw_pan() {
    auto turned = make({0.0F, 5.0F, 0.0F}, 90.0F);
    camera::TacticalStep step{};
    step.view_yaw_degrees = 0.0F;
    step.translate_units = {20.0F, 0.0F};
    expect(turned.advance(step).has_value(), "a translate at view yaw 0 is accepted");
    close(turned.frame().target[0], 20.0F, "view yaw 0: right is +X on a yaw 90 camera");
    close(turned.frame().target[2], 0.0F, "view yaw 0: right leaves Z on a yaw 90 camera");
    step.translate_units = {0.0F, 10.0F};
    expect(turned.advance(step).has_value(), "an upward translate at view yaw 0 is accepted");
    close(turned.frame().target[0], 20.0F, "view yaw 0: up leaves X");
    close(turned.frame().target[2], -10.0F, "view yaw 0: up is -Z");
    close(turned.yaw_degrees(), 90.0F, "the controller keeps its own yaw");

    auto keyed = make({0.0F, 5.0F, 0.0F}, 90.0F);
    camera::TacticalStep key{};
    key.pan = {1.0F, 0.0F, false};
    key.delta_seconds = 1.0F;
    key.view_yaw_degrees = 0.0F;
    expect(keyed.advance(key).has_value(), "a held-key pan at view yaw 0 is accepted");
    close(keyed.frame().target[0], 10.0F, "a held right key pans +X at view yaw 0");
    close(keyed.frame().target[2], 0.0F, "and leaves Z");

    auto level = make();
    step.view_yaw_degrees = 90.0F;
    step.translate_units = {10.0F, 0.0F};
    expect(level.advance(step).has_value(), "a translate at view yaw 90 is accepted");
    close(level.frame().target[0], 0.0F, "view yaw 90: right leaves X on a yaw 0 camera");
    close(level.frame().target[2], -10.0F, "view yaw 90: right is -Z on a yaw 0 camera");

    step.view_yaw_degrees = std::numeric_limits<float>::quiet_NaN();
    const auto before = level.frame();
    expect(!level.advance(step) && level.frame() == before, "a non-finite view yaw is rejected");
}

int main() {
    {
        using namespace eawr::presentation::godot_backend;
        struct PreviewUnit { std::uint64_t entity; bool own; };
        const std::vector<PreviewUnit> units{{1, true}, {2, false}};
        const std::vector<std::uint64_t> alive{1, 2}, seen{1, 2}, fogged{1}, dead{1}, unseen{};
        expect(find_preview_subject(units, alive, seen, 2U, false) != units.end(), "visible enemy can be previewed");
        expect(find_preview_subject(units, alive, fogged, 2U, false) == units.end(), "fogged enemy cannot be previewed");
        expect(find_preview_subject(units, alive, unseen, 1U, false) != units.end(), "own unit bypasses sensor visibility");
        expect(find_preview_subject(units, alive, fogged, 2U, true) != units.end(), "reveal allows displayed enemy");
        expect(!preview_subject_lost(units, alive, seen, 2U, false), "visible enemy preview continues");
        expect(preview_subject_lost(units, alive, fogged, 2U, false), "preview exits when enemy leaves vision");
        expect(preview_subject_lost(units, dead, seen, 2U, false), "preview exits when subject dies");
        expect(!preview_subject_lost(units, alive, fogged, 2U, true), "revealed enemy preview continues");
        expect(preview_subject_lost(units, dead, seen, 2U, true), "reveal cannot preserve a dead subject");
        expect(battlecam_advance(false, false, false, false) == BattlecamAdvance::exit,
            "battle end exits the movie view");
        expect(battlecam_advance(true, true, true, false) == BattlecamAdvance::hold,
            "pause freezes even an expired movie shot");
        expect(battlecam_advance(true, false, true, true) == BattlecamAdvance::update,
            "a held movie key delays the next shot");
        expect(battlecam_advance(true, false, true, false) == BattlecamAdvance::next,
            "an expired unheld shot advances while playing");
        bool camera_movie = true, fog_hidden = true, hud_hidden = true;
        bool key = true, tab = true;
        std::uint64_t subject = 42;
        restore_battlecam([&] { camera_movie = false; }, [&] { fog_hidden = false; },
            [&] { hud_hidden = false; }, subject, key, tab);
        expect(!camera_movie && !fog_hidden && !hud_hidden && subject == 0 && !key && !tab,
            "movie exit restores the camera, fog and HUD and clears held state");
        using Bytes = std::vector<std::byte>;
        const auto word = [](Bytes& out, std::uint32_t v) {
            for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8U*i)) & 255U));
        };
        const auto chunk = [&](std::uint32_t id, bool group, const Bytes& body) {
            Bytes out; word(out, id); word(out, static_cast<std::uint32_t>(body.size()) | (group ? 0x80000000U : 0U));
            out.insert(out.end(), body.begin(), body.end()); return out;
        };
        const auto position = [&](std::uint32_t frame, float x, std::uint32_t transition) {
            Bytes fields{std::byte{0}, std::byte{4}}; word(fields, frame);
            fields.insert(fields.end(), {std::byte{3}, std::byte{4}}); word(fields, transition);
            fields.insert(fields.end(), {std::byte{10}, std::byte{12}});
            word(fields, std::bit_cast<std::uint32_t>(x)); word(fields, 0); word(fields, 0);
            return chunk(4, false, fields);
        };
        const auto scalar = [&](std::uint32_t frame, float value) {
            Bytes fields{std::byte{0}, std::byte{4}}; word(fields, frame);
            fields.insert(fields.end(), {std::byte{9}, std::byte{4}}); word(fields, std::bit_cast<std::uint32_t>(value));
            return chunk(5, false, fields);
        };
        const auto join = [](Bytes& into, const Bytes& more) { into.insert(into.end(), more.begin(), more.end()); };
        Bytes camera_keys; join(camera_keys, position(0, 0, 1)); join(camera_keys, position(30, 10, 2));
        Bytes target_keys; join(target_keys, position(0, 2, 2)); join(target_keys, position(30, 4, 1));
        Bytes fov_keys; join(fov_keys, scalar(0, 40)); join(fov_keys, scalar(30, 60));
        Bytes file; join(file, chunk(0, true, camera_keys)); join(file, chunk(1, true, target_keys)); join(file, chunk(2, true, fov_keys));
        const auto parsed = parse_battlecam_tec(file);
        expect(parsed && parsed->duration == 30, "TEC key sections and duration decode");
        if (parsed) {
            close(tec_position(parsed->camera, 15)[0], 5, "TEC linear camera key interpolates");
            close(tec_position(parsed->target, 15)[0], 2, "TEC cut holds its first target key");
            expect(tec_fov(parsed->fov, 15).has_value(), "TEC FOV is available between keys");
            if (const auto fov = tec_fov(parsed->fov, 15)) close(*fov, 50, "TEC FOV interpolates");
        }
        file.pop_back();
        expect(!parse_battlecam_tec(file), "truncated TEC is refused");
        close(tec_detail::ease(0.5F, 0.25F, 0.25F), 0.5F, "symmetric TEC easing reaches its midpoint");
        close(tec_detail::ease(0.125F, 0.25F, 0.25F), 1.0F / 24.0F, "TEC easing integrates the initial acceleration");
    }
    {
        using namespace eawr::presentation::godot_backend;
        bool circle = false, flyby = false, belly = false, floating = false, bomber_chase = false, frigate = false;
        for (std::uint32_t seed = 1; seed < 1024; ++seed) {
            BattlecamRandom fighter_random{seed}, generic_random{seed}, bomber_random{seed};
            const std::array<float, 3> extents{20, 30, 10};
            expect(make_battlecam_shot(extents, {.fighter = true}, fighter_random).kind == BattlecamShotKind::chase,
                "fighter dispatch chooses chase");
            const auto shot = make_battlecam_shot(extents, {}, generic_random);
            expect(shot.duration > 0.0F && std::isfinite(shot.at(shot.duration * 0.5F)[0]), "shot has a finite path");
            circle = circle || shot.kind == BattlecamShotKind::circle;
            flyby = flyby || shot.kind == BattlecamShotKind::flyby;
            belly = belly || shot.kind == BattlecamShotKind::belly;
            floating = floating || shot.kind == BattlecamShotKind::floating;
            bomber_chase = bomber_chase || make_battlecam_shot(extents, {.bomber = true}, bomber_random).kind == BattlecamShotKind::chase;
            BattlecamRandom frigate_random{seed};
            const auto transfer = make_battlecam_shot(extents, {.frigate = true}, frigate_random, std::array<float, 3>{5, 10, 5});
            if (transfer.kind == BattlecamShotKind::frigate) {
                frigate = true;
                expect(transfer.duration >= 6.0F && transfer.duration <= 8.0F, "frigate transfer lasts 180..240 frames");
                close(transfer.end[1], -15.0F, "frigate endpoint scales to the target bounds");
            }
        }
        expect(circle && flyby && belly && floating && bomber_chase, "category dispatch reaches its supported paths");
        expect(frigate, "frigate dispatch reaches target transfer");
        expect(frigate_target_eligible(0, 180, 10, 0), "opposing target ahead admits frigate transfer");
        expect(!frigate_target_eligible(0, 0, 10, 0) && !frigate_target_eligible(0, 180, 0, 10),
            "frigate transfer rejects parallel headings and targets off its forward cone");
        for (const float radius : {10.0F, 100.0F}) {
            close(preview_distance_clamp(0.0F, radius), 1.5F * radius, "preview permits closer zoom");
            close(preview_distance_clamp(20.0F * radius, radius), 10.0F * radius, "preview clamps to 10R");
            close(preview_start_distance(radius), 5.0F * radius, "preview starts farther out for each size");
            expect(preview_distance_clamp(preview_start_distance(radius), radius) == preview_start_distance(radius),
                "preview start is within zoom limits");
        }
        // #6569: the zoom range grows linearly up to a frigate-sized ship and with the square root beyond.
        // Radii are model-bounds half diagonals of stock and mod ships (reference game data).
        struct PreviewSize { const char* name; float radius; };
        const PreviewSize classes[] = {{"fighter", 39.3F}, {"corvette", 94.8F}, {"frigate", 187.5F}, {"cruiser", 348.4F},
                                        {"star destroyer", 492.8F}, {"super (Executor)", 2671.1F}, {"Pride of the Core", 4378.5F}};
        float previous_start = 0.0F;
        float previous_ratio = 1.0e9F;
        for (const PreviewSize& ship : classes) {
            const PreviewZoom zoom = preview_zoom(ship.radius);
            const std::string label = std::string(" (") + ship.name + ")";
            if (ship.radius <= preview_knee_radius) {
                close(zoom.start, 5.0F * ship.radius, "ships up to a frigate keep the 5R start" + label);
                close(zoom.min, 1.5F * ship.radius, "ships up to a frigate keep the 1.5R zoom-in limit" + label);
                close(zoom.max, 10.0F * ship.radius, "ships up to a frigate keep the 10R zoom-out limit" + label);
            } else {
                expect(zoom.start < 5.0F * ship.radius && zoom.min < 1.5F * ship.radius && zoom.max < 10.0F * ship.radius,
                    "a larger ship starts nearer and zooms in closer than the linear range" + label);
            }
            expect(zoom.min <= zoom.start && zoom.start <= zoom.max, "start sits inside the zoom range" + label);
            expect(zoom.start >= previous_start, "a larger ship never starts nearer than a smaller one" + label);
            expect(zoom.start / ship.radius <= previous_ratio + 1.0e-4F, "the start per radius never grows with size" + label);
            previous_start = zoom.start;
            previous_ratio = zoom.start / ship.radius;
            close(preview_distance_clamp(zoom.start, ship.radius), zoom.start, "the start is within the zoom limits" + label, 1.0e-2F);
        }
        // The two large cases the owner reported: no longer starts at 21893 (Pride) / 13355 (Executor) units out.
        close(preview_start_distance(4378.5F), 4679.0F, "Pride of the Core starts ~4.7 thousand units out", 5.0F);
        expect(preview_zoom(4378.5F).min < 1500.0F, "Pride of the Core zooms in to under 1.5 thousand units");
        close(preview_start_distance(2671.1F), 3655.0F, "Executor starts ~3.7 thousand units out", 5.0F);
        // A map-extent cap: the start and the zoom-out limit never show the whole map, and the range stays ordered.
        const PreviewZoom capped = preview_zoom(4378.5F, 12000.0F);
        close(capped.start, 2400.0F, "start capped to 0.2 of the smaller map dimension");
        close(capped.max, 4800.0F, "zoom-out limit capped to 0.4 of the smaller map dimension");
        expect(capped.min <= capped.start, "the map cap never pushes the start inside the zoom-in limit");
        // A map smaller than the ship's own zoom-in limit: the start rests on that limit.
        const PreviewZoom small_map = preview_zoom(4378.5F, 6000.0F);
        close(small_map.start, small_map.min, "a small map leaves the start on the zoom-in limit");
        const PreviewZoom tiny = preview_zoom(4378.5F, 100.0F);
        expect(tiny.min <= tiny.start && tiny.start <= tiny.max, "a tiny map still leaves an ordered zoom range");
        close(preview_start_distance(39.3F, 6000.0F), 5.0F * 39.3F, "a fighter is not capped by a normal map");
        // The wheel: a fixed 25 units for every ship up to a frigate at any distance (unchanged from before the
        // change), proportional beyond.
        for (const PreviewSize& ship : classes) {
            const PreviewZoom zoom = preview_zoom(ship.radius);
            const std::string label = std::string(" (") + ship.name + ")";
            for (const float fraction : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
                const float distance = zoom.min + (zoom.max - zoom.min) * fraction;
                const float step = preview_wheel_step(distance, ship.radius);
                if (ship.radius <= preview_knee_radius) close(step, 25.0F, "up to a frigate every notch stays 25 units" + label);
                else expect(step >= 25.0F && std::abs(step - std::max(25.0F, 0.05F * distance)) < 1.0e-3F,
                    "a larger ship's notch is 5% of the distance (at least 25)" + label);
            }
            // The zoom limits hold for every class: no distance goes inside the zoom-in limit or beyond the zoom-out limit.
            close(preview_distance_clamp(0.0F, ship.radius), zoom.min, "zooming in stops at the zoom-in limit" + label);
            close(preview_distance_clamp(1.0e9F, ship.radius), zoom.max, "zooming out stops at the zoom-out limit" + label);
            // One notch from the start moves it by the step and stays inside the limits.
            const float notch_in = preview_distance_clamp(zoom.start - preview_wheel_step(zoom.start, ship.radius), ship.radius);
            expect(notch_in >= zoom.min && notch_in <= zoom.start, "a notch in from the start stays inside the range" + label);
        }
        // Notches from the start to the zoom-in limit, before (fixed 25 units) and after this change.
        {
            const float old_start = 5.0F * 4378.5F;
            const float old_min = 1.5F * 4378.5F;
            const int before = static_cast<int>(std::ceil((old_start - old_min) / 25.0F));
            expect(before > 600 && before < 620, "before, the Pride of the Core needed about 613 notches");
            float distance = preview_start_distance(4378.5F);
            int notches = 0;
            while (distance > preview_zoom(4378.5F).min + 1.0e-3F && notches < 1000) {
                distance = preview_distance_clamp(distance - preview_wheel_step(distance, 4378.5F), 4378.5F);
                ++notches;
            }
            expect(notches <= 40, "Pride of the Core reaches its closest zoom in a few dozen wheel notches");
        }
        // A frigate-class ship still needs the same notch count as before from its start to its zoom-in limit.
        {
            float distance = preview_start_distance(187.5F);
            int notches = 0;
            while (distance > preview_zoom(187.5F).min + 1.0e-3F && notches < 1000) {
                distance = preview_distance_clamp(distance - preview_wheel_step(distance, 187.5F), 187.5F);
                ++notches;
            }
            expect(notches == static_cast<int>(std::ceil((5.0F * 187.5F - 1.5F * 187.5F) / 25.0F)),
                "a frigate keeps its 25-unit notch count from start to the zoom-in limit");
        }
        const auto first = preview_offset(30.0F, 0.0F, 30.0F);
        const auto next = preview_offset(30.0F, 45.0F, 30.0F);
        close(first[2], 15.0F, "preview starts at 30 degrees elevation");
        expect(first[0] != next[0] && first[1] != next[1], "preview advances its azimuth");
        const auto hardpoint = hardpoint_camera_offset({1, 0, 0});
        expect(hardpoint.has_value(), "horizontal hardpoint has a weapon frame");
        if (hardpoint) close((*hardpoint)[2], 200.0F, "hardpoint camera sits 200 units along frame up");
        expect(!hardpoint_camera_offset({0, 0, 1}), "vertical hardpoint without a defined zero-roll frame falls through");
    }
    {
        using namespace eawr::presentation::godot_backend;
        const std::array<float, 7> size_weight{1, 0, 0, 0, 0, 0, 0};
        std::vector<BattlecamCandidate> candidates{
            {1, 10, 2, {1, 0, 0, 0, 5, 0, 0}, false, 0},
            {2, 20, 0, {4, 0, 0, 0, 5, 0, 0}, true, 0},
            {3, 30, 0, {9, 0, 0, 0, 5, 0, 0}, false, 0}};
        rank_battlecam_candidates(candidates, size_weight);
        expect(candidates.front().entity == 3 && candidates.back().entity == 1, "saliency weights rank size");
        BattlecamRandom opening{5};
        expect(choose_battlecam_subject(candidates, size_weight, 0, true, opening) == 2, "opening prefers the selected subject");
        BattlecamRandom repeat{5};
        const auto picked = choose_battlecam_subject(candidates, size_weight, 3, false, repeat);
        expect(picked != 3, "selection avoids the prior subject when another type is available");
        const std::vector<BattlecamCandidate> shortcut_candidates{
            {1, 10, 2, {}, false, 0}, {2, 10, 0, {}, false, 0}, {3, 30, 0, {}, false, 0}};
        bool followed_target = false;
        for (std::uint32_t seed = 0; seed < 32; ++seed) {
            BattlecamRandom shortcut{seed};
            followed_target = followed_target || choose_battlecam_subject(shortcut_candidates, size_weight, 1, false, shortcut) == 2;
        }
        expect(followed_target, "attack target shortcut can select a target of the same type");
        BattlecamRandom empty{5};
        expect(choose_battlecam_subject({}, size_weight, 0, true, empty) == 0, "empty selection refuses startup");
    }
    {
        using namespace eawr::presentation::godot_backend;
        BattlecamRandom first{12345U}, second{12345U}, scaled{12345U};
        for (int i = 0; i < 32; ++i) {
            const auto a = BattlecamChase::make(5.0F, first);
            const auto b = BattlecamChase::make(5.0F, second);
            const auto c = BattlecamChase::make(50.0F, scaled);
            expect(a.start == b.start && a.end == b.end && a.duration == b.duration, "seeded chase is deterministic");
            expect(a.duration >= 2.0F && a.duration <= 4.0F, "chase duration is within BCAM-10 bounds");
            const auto end = a.at(a.duration);
            expect(a.at(0.0F) == a.start, "chase starts at its first endpoint");
            const auto mid = a.at(a.duration * 0.5F);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                close(end[axis], a.end[axis], "chase reaches its final endpoint");
                close(mid[axis], (a.start[axis] + a.end[axis]) * 0.5F, "chase interpolates linearly");
                close(c.start[axis], a.start[axis] * 10.0F, "chase scales with model bounds");
            }
        }
    }
    {
        const camera::TacticalFrame original{1280, 720, 45, 1, 10000, {0, 100, 200}, {0, 0, 0}, {0, 1, 0}};
        const std::array<float, 3> subject{100, 20, -40};
        auto entry = camera::movie_frame(original, subject, 0);
        auto later = camera::movie_frame(original, subject, 5);
        expect(entry && later && entry.value().target == subject && later.value().target == subject,
            "movie camera keeps the supplied visible subject in view");
        expect(entry && later && entry.value().eye != later.value().eye, "movie camera moves on presentation time");
        expect(entry && entry.value().near_plane == original.near_plane && entry.value().far_plane == original.far_plane,
            "movie director keeps authored clipping");
        const std::array<float, 3> moved{110, 25, -30};
        auto followed = camera::movie_frame(original, moved, 5);
        expect(later && followed, "moving movie subject accepted");
        if (later && followed) for (std::size_t i = 0; i < 3; ++i)
            close(followed.value().eye[i] - later.value().eye[i], moved[i] - subject[i], "movie follows without a camera cut");
        expect(!camera::movie_frame(original, subject, -1), "movie refuses negative elapsed time");
        expect(!camera::movie_frame(original, subject, std::numeric_limits<double>::infinity()), "movie refuses non-finite time");
        expect(original.eye == std::array<float, 3>{0, 100, 200}, "tactical return frame remains intact");
    }
    test_view_yaw_pan();
    test_pan_speed_scale();
    test_orbit_pitch_limits_and_persistence();
    test_orbit_zoom_resumes_smoothly();
    test_orbit_frame_rate_independence();
    test_foc_mouse_unit_law();
    test_drag_displacement_frame_rate();
    test_bounds_and_basis();
    test_corners_and_cancellation();
    test_zoom_and_push();
    test_atomic_rejections();
    test_fixed_capture();
    test_viewport_transaction();
    test_viewport_aspect();
    test_target_height();
    test_zoom_inertia_and_pitch();
    test_scroll_ramp();
    test_boundary_diagonal_frame_rate();
    test_concurrent_zoom_pan_frame_rate();
    test_flat_distance_spline_idle_pitch();
    return failures == 0 ? 0 : 1;
}
