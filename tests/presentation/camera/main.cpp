// P1-09 tactical camera contracts.
//
// The constants below are transcribed from plan/inventories/camera-constants.json,
// which is generated from the effective data/xml/tacticalcameras.xml and
// data/xml/gameconstants.xml of the EaW, FoC and Remake profiles. The Python
// contract test tests/presentation/renderer/test_camera_contract.py re-reads
// that inventory and fails if these transcriptions drift from it, so this file
// cannot silently diverge from the committed XML evidence.
//
// Expectations are stated at three zoom levels per mode: minimum (fully zoomed
// in), midpoint and maximum (fully zoomed out), which is the shape the ticket's
// acceptance asks for. Agreement with the original game at those levels still
// requires original captures; see docs/camera.md.

#include "eawr/presentation/camera/camera.hpp"
#include "eawr/presentation/camera/input.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <limits>
#include <vector>

namespace {
namespace camera = eawr::presentation::camera;

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] bool close(const float left, const float right, const float tolerance = 1e-3F) {
    return std::abs(left - right) <= tolerance;
}

void expect_close(const float actual, const float expected, const std::string_view message,
                  const float tolerance = 1e-3F) {
    if (!close(actual, expected, tolerance)) {
        std::cerr << "FAILED: " << message << " (expected " << expected
                  << ", got " << actual << ")\n";
        ++failures;
    }
}

// Global tactical scroll constants. Identical in all three profiles; see the
// inventory's scroll_constants arrays.
void apply_scroll_constants(camera::Constants& constants) {
    constants.tactical_min_scroll_speed = 1000.0F;   // Tactical_Min_Scroll_Speed
    constants.tactical_max_scroll_speed = 4000.0F;   // Tactical_Max_Scroll_Speed
    constants.tactical_edge_scroll_region = 2.0F;    // Tactical_Edge_Scroll_Region
    constants.tactical_offscreen_scroll_region = 50.0F;  // Tactical_Offscreen_Scroll_Region
    constants.push_scroll_speed_modifier = 2.0F;     // Push_Scroll_Speed_Modifier
    constants.scroll_acceleration_factor = 0.08F;    // Scroll_Acceleration_Factor
    constants.scroll_deceleration_factor = 1.5F;     // Scroll_Deceleration_Factor
}

// EaW base data/xml/tacticalcameras.xml, <TacticalCamera Name="Land_Mode">.
[[nodiscard]] camera::Constants eaw_land() {
    camera::Constants constants;
    constants.distance_min = 170.0F;
    constants.distance_max = 460.0F;
    constants.distance_default = 460.0F;
    constants.distance_per_mouse_unit = 100.0F;
    constants.distance_smooth_time = 0.21F;
    constants.pitch_min = 50.0F;
    constants.pitch_max = 50.0F;
    constants.pitch_default = 50.0F;
    constants.pitch_per_mouse_unit = 0.0F;
    constants.pitch_per_zoom_unit = -30.0F;
    constants.pitch_when_zoomed_in = 15.0F;
    constants.pitch_zoom_begin_fraction = 0.1F;
    constants.yaw_min = -1000.0F;
    constants.yaw_max = 1000.0F;
    constants.yaw_default = 0.0F;
    constants.yaw_per_mouse_unit = 1.5F;
    constants.fov_min = 25.0F;
    constants.fov_max = 55.0F;
    constants.fov_default = 55.0F;
    constants.fov_per_mouse_unit = 0.0F;
    constants.near_clip = 10.0F;
    constants.far_clip = 7000.0F;
    constants.use_splines = true;  // Use_Splines = yes
    constants.distance_spline = {{0.0F, 170.0F}, {0.3F, 220.0F}, {1.0F, 460.0F}};
    constants.pitch_spline = {{0.0F, 17.0F}, {0.35F, 45.0F}, {1.0F, 55.0F}, {1.1F, 55.0F}};
    apply_scroll_constants(constants);
    return constants;
}

// FoC expansion Land_Mode differs from EaW only in the first pitch control
// point: 27 degrees instead of 17.
[[nodiscard]] camera::Constants foc_land() {
    camera::Constants constants = eaw_land();
    constants.pitch_spline = {{0.0F, 27.0F}, {0.35F, 45.0F}, {1.0F, 55.0F}, {1.1F, 55.0F}};
    return constants;
}

// EaW base and FoC expansion <TacticalCamera Name="Space_Mode"> are identical.
[[nodiscard]] camera::Constants eaw_space() {
    camera::Constants constants;
    constants.distance_min = 200.0F;
    constants.distance_max = 1900.0F;
    constants.distance_default = 1000.0F;
    constants.distance_per_mouse_unit = 500.0F;
    constants.distance_smooth_time = 0.1F;
    constants.pitch_min = -10.0F;
    constants.pitch_max = 85.0F;
    constants.pitch_default = 50.0F;
    constants.pitch_per_mouse_unit = -1.5F;
    constants.pitch_per_zoom_unit = 0.0F;
    constants.pitch_when_zoomed_in = 50.0F;
    constants.pitch_zoom_begin_fraction = -1.0F;  // disabled
    constants.yaw_min = -1000.0F;
    constants.yaw_max = 1000.0F;
    constants.yaw_default = 0.0F;
    constants.yaw_per_mouse_unit = 1.5F;
    constants.fov_min = 55.0F;
    constants.fov_max = 55.0F;
    constants.fov_default = 55.0F;
    constants.fov_per_mouse_unit = 0.0F;
    constants.near_clip = 10.0F;
    constants.far_clip = 7000.0F;
    constants.use_splines = false;
    apply_scroll_constants(constants);
    return constants;
}

// Remake mod Land_Mode. It ships spline text but sets Use_Splines = no, so the
// linear distance range and the constant pitch apply.
[[nodiscard]] camera::Constants remake_land() {
    camera::Constants constants;
    constants.distance_min = 200.0F;
    constants.distance_max = 1050.0F;
    constants.distance_default = 900.0F;
    constants.distance_per_mouse_unit = 120.0F;
    constants.distance_smooth_time = 0.15F;
    constants.pitch_min = -90.0F;
    constants.pitch_max = 90.0F;
    constants.pitch_default = 50.0F;
    constants.pitch_per_mouse_unit = -0.45F;
    constants.pitch_per_zoom_unit = 0.0F;
    constants.pitch_when_zoomed_in = 0.0F;
    constants.pitch_zoom_begin_fraction = 0.0F;
    constants.yaw_min = -1000.0F;
    constants.yaw_max = 1000.0F;
    constants.yaw_default = 0.0F;
    constants.yaw_per_mouse_unit = 0.45F;
    constants.fov_min = 35.0F;
    constants.fov_max = 60.0F;
    constants.fov_default = 60.0F;
    constants.fov_per_mouse_unit = 5.0F;
    constants.near_clip = 1.0F;
    constants.far_clip = 45000.0F;
    constants.use_splines = false;
    apply_scroll_constants(constants);
    return constants;
}

// Remake mod Space_Mode.
[[nodiscard]] camera::Constants remake_space() {
    camera::Constants constants = remake_land();
    constants.distance_min = 1000.0F;
    constants.distance_max = 2000.0F;
    constants.distance_default = 2000.0F;
    constants.distance_per_mouse_unit = 200.0F;
    constants.distance_smooth_time = 0.1F;
    constants.pitch_default = 60.0F;
    constants.fov_min = 25.0F;
    constants.fov_default = 60.0F;
    constants.fov_per_mouse_unit = 10.0F;
    constants.near_clip = 1.0F;
    constants.far_clip = 200000.0F;
    return constants;
}

struct ZoomExpectation final {
    float zoom{};
    float distance{};
    float pitch_degrees{};
    float pan_speed{};
};

void check_curve(const camera::Constants& constants, const std::string_view label,
                 const std::array<ZoomExpectation, 3>& expectations) {
    for (const ZoomExpectation& expectation : expectations) {
        auto state = camera::solve(constants, expectation.zoom);
        const std::string where =
            std::string(label) + " @ zoom " + std::to_string(expectation.zoom);
        if (!state) {
            expect(false, where + ": solve failed: " + state.error().message);
            continue;
        }
        expect_close(state.value().distance, expectation.distance, where + " distance");
        expect_close(state.value().pitch_degrees, expectation.pitch_degrees, where + " pitch");
        expect_close(state.value().pan_speed, expectation.pan_speed, where + " pan speed", 0.05F);
        expect(state.value().zoom == expectation.zoom, where + " echoes the clamped zoom");
    }
}

void test_three_zoom_levels_per_mode() {
    // Land, EaW: the distance spline is (0,170) (0.3,220) (1,460), so the
    // midpoint lies on the second segment: 220 + 240 * (0.2/0.7).
    check_curve(eaw_land(), "eaw land", {{
        {0.0F, 170.0F, 17.0F, 1000.0F},
        {0.5F, 288.571429F, 47.307692F, 2226.600983F},
        {1.0F, 460.0F, 55.0F, 4000.0F},
    }});
    // FoC land shares the distance spline and differs only at zoom 0.
    check_curve(foc_land(), "foc land", {{
        {0.0F, 170.0F, 27.0F, 1000.0F},
        {0.5F, 288.571429F, 47.307692F, 2226.600983F},
        {1.0F, 460.0F, 55.0F, 4000.0F},
    }});
    // Space is linear in distance and holds Pitch_Default, because
    // Pitch_Per_Zoom_Unit is 0 and Pitch_Zoom_Begin_Fraction is disabled.
    check_curve(eaw_space(), "eaw/foc space", {{
        {0.0F, 200.0F, 50.0F, 1000.0F},
        {0.5F, 1050.0F, 50.0F, 2500.0F},
        {1.0F, 1900.0F, 50.0F, 4000.0F},
    }});
    check_curve(remake_land(), "remake land", {{
        {0.0F, 200.0F, 50.0F, 1000.0F},
        {0.5F, 625.0F, 50.0F, 2500.0F},
        {1.0F, 1050.0F, 50.0F, 4000.0F},
    }});
    check_curve(remake_space(), "remake space", {{
        {0.0F, 1000.0F, 60.0F, 1000.0F},
        {0.5F, 1500.0F, 60.0F, 2500.0F},
        {1.0F, 2000.0F, 60.0F, 4000.0F},
    }});
}

void test_zoom_limits_and_clamping() {
    const camera::Constants land = eaw_land();
    auto below = camera::solve(land, -5.0F);
    auto above = camera::solve(land, 42.0F);
    expect(below && close(below.value().distance, land.distance_min),
           "zoom below range clamps to Distance_Min");
    expect(above && close(above.value().distance, land.distance_max),
           "zoom above range clamps to Distance_Max");
    expect(below && below.value().zoom == camera::min_zoom, "clamped zoom is reported");
    expect(above && above.value().zoom == camera::max_zoom, "clamped zoom is reported");
    expect(!camera::solve(land, std::nanf("")), "a non-finite zoom is rejected");
    expect(!camera::clamp_zoom(std::numeric_limits<float>::infinity()),
           "an infinite zoom is rejected");

    // Every shipped mode stays inside its own authored distance range across
    // the whole parameter, which is the zoom-limit contract.
    const std::array<camera::Constants, 5> modes{
        eaw_land(), foc_land(), eaw_space(), remake_land(), remake_space()};
    for (const camera::Constants& constants : modes) {
        for (int step = 0; step <= 20; ++step) {
            const float zoom = static_cast<float>(step) / 20.0F;
            auto state = camera::solve(constants, zoom);
            expect(state && state.value().distance >= constants.distance_min - 1e-3F
                       && state.value().distance <= constants.distance_max + 1e-3F,
                   "distance stays inside the authored range");
            expect(state && state.value().pan_speed >= constants.tactical_min_scroll_speed - 0.05F
                       && state.value().pan_speed
                              <= constants.tactical_max_scroll_speed + 0.05F,
                   "pan speed stays inside the authored scroll range");
        }
    }
}

void test_invalid_constants_are_rejected() {
    expect(!camera::validate(camera::Constants{}),
           "a default-constructed Constants is inert and rejected");
    camera::Constants inverted = eaw_space();
    inverted.distance_min = 2000.0F;
    expect(!camera::validate(inverted), "an inverted distance range is rejected");
    camera::Constants bad_clip = eaw_space();
    bad_clip.near_clip = 0.0F;
    expect(!camera::validate(bad_clip), "a non-positive near clip is rejected");
    camera::Constants bad_scroll = eaw_space();
    bad_scroll.tactical_max_scroll_speed = 10.0F;
    expect(!camera::validate(bad_scroll), "an inverted scroll-speed range is rejected");
    camera::Constants missing_spline = eaw_space();
    missing_spline.use_splines = true;
    expect(!camera::validate(missing_spline),
           "Use_Splines without usable curves is rejected rather than defaulted");
    camera::Constants decreasing = eaw_land();
    decreasing.pitch_spline = {{0.5F, 10.0F}, {0.2F, 20.0F}};
    expect(!camera::validate(decreasing), "a decreasing spline fraction is rejected");
}

void test_spline_parsing_and_evaluation() {
    // The shipped land lists mix ',' and ' ' separators inside one value.
    auto distance = camera::parse_spline("0.0 170.0, 0.3, 220.0, 1.0, 460.0");
    expect(distance.has_value(), "the shipped mixed-separator distance spline parses");
    if (distance) {
        const std::vector<camera::SplinePoint> expected{
            {0.0F, 170.0F}, {0.3F, 220.0F}, {1.0F, 460.0F}};
        expect(distance.value() == expected, "mixed separators produce the documented points");
    }
    auto pitch = camera::parse_spline("0.0,17.0, 0.35,45.0, 1.0, 55.0, 1.1,55");
    expect(pitch.has_value() && pitch.value().size() == 4, "the shipped pitch spline parses");
    expect(camera::parse_spline("770.0f, 1.0, 1000.0f, 2.0").has_value(),
           "a trailing f suffix is accepted");
    expect(!camera::parse_spline("0.0, 1.0, 0.5").has_value(),
           "an odd token count is rejected");
    expect(!camera::parse_spline("0.0, one, 1.0, 2.0").has_value(),
           "a non-numeric token is rejected");
    expect(!camera::parse_spline("0.5, 1.0, 0.2, 2.0").has_value(),
           "a decreasing fraction is rejected");
    expect(!camera::parse_spline("nan, 1.0, 1.0, 2.0").has_value(),
           "a non-finite token is rejected");
    expect(!camera::parse_spline("0.0, 1.0").has_value(),
           "a single control point is rejected");

    const std::array<camera::SplinePoint, 3> points{{{0.0F, 10.0F}, {0.5F, 20.0F},
                                                     {1.0F, 30.0F}}};
    const std::span<const camera::SplinePoint> span{points};
    expect_close(camera::evaluate_spline(span, 0.25F).value(), 15.0F, "spline midsegment");
    expect_close(camera::evaluate_spline(span, -1.0F).value(), 10.0F, "spline clamps below");
    expect_close(camera::evaluate_spline(span, 9.0F).value(), 30.0F, "spline clamps above");
    expect_close(camera::evaluate_spline(span, 0.5F).value(), 20.0F, "spline hits a knot");
}

void test_zoom_step_input_mapping() {
    const camera::Constants land = eaw_land();
    // Distance_Per_Mouse_Unit 100 over a 290-unit range is 0.3448 of the
    // parameter per detent.
    auto out = camera::apply_zoom_step(land, 0.0F, 1.0F);
    expect(out.has_value(), "one zoom-out detent succeeds");
    expect_close(out.value(), 100.0F / 290.0F, "land detent step");
    auto back = camera::apply_zoom_step(land, out.value(), -1.0F);
    expect_close(back.value(), 0.0F, "the reverse detent returns to the minimum");
    expect_close(camera::apply_zoom_step(land, 0.0F, -3.0F).value(), 0.0F,
                 "zooming in past the minimum clamps");
    expect_close(camera::apply_zoom_step(land, 1.0F, 3.0F).value(), 1.0F,
                 "zooming out past the maximum clamps");
    const camera::Constants space = eaw_space();
    expect_close(camera::apply_zoom_step(space, 0.0F, 1.0F).value(), 500.0F / 1700.0F,
                 "space detent step");
    expect(!camera::apply_zoom_step(land, 0.0F, std::nanf("")),
           "a non-finite detent is rejected");
}

void test_pan_input_mapping() {
    const camera::Constants land = eaw_land();
    auto zoomed_in = camera::solve(land, 0.0F);
    auto zoomed_out = camera::solve(land, 1.0F);
    expect(zoomed_in && zoomed_out, "land solves at both zoom limits");

    auto slow = camera::apply_pan(land, zoomed_in.value(), {1.0F, 0.0F, false}, 0.5F);
    expect_close(slow.value().x, 500.0F, "pan at minimum zoom is Tactical_Min_Scroll_Speed * dt");
    expect_close(slow.value().y, 0.0F, "a pure x input produces no y displacement");
    auto fast = camera::apply_pan(land, zoomed_out.value(), {1.0F, 0.0F, false}, 0.5F);
    expect_close(fast.value().x, 2000.0F,
                 "pan at maximum zoom is Tactical_Max_Scroll_Speed * dt");
    expect(fast.value().x > slow.value().x, "panning is faster when zoomed out");

    auto pushed = camera::apply_pan(land, zoomed_in.value(), {1.0F, 0.0F, true}, 0.5F);
    expect_close(pushed.value().x, 500.0F * land.push_scroll_speed_modifier,
                 "push scrolling applies Push_Scroll_Speed_Modifier");

    // A diagonal input must not exceed the authored speed.
    auto diagonal = camera::apply_pan(land, zoomed_out.value(), {1.0F, 1.0F, false}, 1.0F);
    const float length = std::sqrt(diagonal.value().x * diagonal.value().x
        + diagonal.value().y * diagonal.value().y);
    expect_close(length, 4000.0F, "a diagonal pan is normalised to the authored speed", 0.05F);

    auto idle = camera::apply_pan(land, zoomed_out.value(), {0.0F, 0.0F, false}, 1.0F);
    expect(idle && idle.value() == camera::PanDisplacement{},
           "no input produces no displacement");
    expect(!camera::apply_pan(land, zoomed_out.value(), {1.0F, 0.0F, false}, -1.0F),
           "a negative delta is rejected");
}

void test_input_two_controls_and_repeats() {
    camera::InputReducer reducer;
    const camera::Event first{1U, 10U, camera::EventType::press};
    const camera::Event second{2U, 10U, camera::EventType::press};
    const camera::Event repeat{1U, 10U, camera::EventType::repeat};
    const camera::Event release_first{1U, 10U, camera::EventType::release};
    const camera::Event release_second{2U, 10U, camera::EventType::release};

    expect(reducer.process(first).has_value(), "first control press succeeds");
    expect(reducer.held_actions().size() == 1U
               && reducer.held_actions().front() == camera::HeldAction{10U, 1U},
           "one pressed control exposes one held action");
    expect(reducer.process(repeat).has_value(), "repeat succeeds after a press");
    expect(reducer.held_actions().front() == camera::HeldAction{10U, 1U},
           "repeat does not multiply a held action");

    expect(reducer.process(second).has_value(), "second control press succeeds");
    expect(reducer.held_actions().front() == camera::HeldAction{10U, 2U},
           "two controls for one action expose one action with two contributors");
    expect(reducer.process(release_first).has_value(), "first control release succeeds");
    expect(reducer.held_actions().front() == camera::HeldAction{10U, 1U},
           "releasing one contributor keeps the action held");
    expect(reducer.process(release_first).has_value(), "duplicate release is inert");
    expect(reducer.held_actions().front() == camera::HeldAction{10U, 1U},
           "duplicate release does not remove the other contributor");
    expect(reducer.process(release_second).has_value(), "last control release succeeds");
    expect(reducer.held_actions().empty(), "releasing the last contributor clears the action");
}

void test_input_opposing_controls_are_independent_and_ordered() {
    camera::InputReducer reducer;
    // Press in reverse action order to prove that output ordering is stable.
    expect(reducer.process({11U, 201U, camera::EventType::press}).has_value(),
           "opposing control press succeeds");
    expect(reducer.process({10U, 200U, camera::EventType::press}).has_value(),
           "second opposing control press succeeds");
    expect(reducer.held_actions().size() == 2U
               && reducer.held_actions()[0] == camera::HeldAction{200U, 1U}
               && reducer.held_actions()[1] == camera::HeldAction{201U, 1U},
           "opposing actions remain independent in action-ID order");
    expect(reducer.is_held(200U) && reducer.is_held(201U),
           "both opposing actions are held");

    expect(reducer.process({10U, 200U, camera::EventType::release}).has_value(),
           "one opposing control release succeeds");
    expect(reducer.held_actions().size() == 1U
               && reducer.held_actions().front() == camera::HeldAction{201U, 1U},
           "releasing one opposing action leaves the other held");
}

void test_input_focus_loss_requires_fresh_press() {
    camera::InputReducer reducer;
    expect(reducer.process({20U, 300U, camera::EventType::press}).has_value(),
           "focus fixture press succeeds");
    expect(reducer.pressed_control_count() == 1U, "focus fixture has one pressed control");

    // Simulate a missing release: focus loss is the authoritative cancellation.
    reducer.set_focus_eligible(false);
    expect(!reducer.focus_eligible() && !reducer.input_eligible(),
           "focus loss makes input ineligible");
    expect(reducer.held_actions().empty() && reducer.pressed_control_count() == 0U,
           "focus loss clears held actions and pressed controls");
    expect(reducer.process({20U, 300U, camera::EventType::release}).has_value(),
           "late release after focus loss is inert");

    reducer.set_focus_eligible(true);
    expect(reducer.focus_eligible() && reducer.held_actions().empty(),
           "focus regain grants eligibility without resurrecting state");
    expect(reducer.process({20U, 300U, camera::EventType::repeat}).has_value(),
           "stale repeat after focus regain is inert");
    expect(reducer.held_actions().empty(), "stale repeat cannot recreate a held action");
    expect(reducer.process({20U, 300U, camera::EventType::press}).has_value(),
           "fresh press after focus regain succeeds");
    expect(reducer.is_held(300U), "fresh press is the only post-focus activation");
}

void test_input_capture_lock_cancels_and_invalid_events_are_atomic() {
    camera::InputReducer reducer;
    expect(reducer.process({30U, 400U, camera::EventType::press}).has_value(),
           "capture fixture press succeeds");
    reducer.set_capture_locked(true);
    expect(reducer.capture_locked() && reducer.held_actions().empty(),
           "capture lock cancels held actions");
    expect(reducer.process({30U, 400U, camera::EventType::press}).has_value(),
           "valid input while locked is ignored, not rejected");
    expect(reducer.held_actions().empty(), "locked input cannot reactivate the reducer");

    reducer.set_capture_locked(false);
    expect(!reducer.capture_locked(), "capture unlock restores eligibility");
    expect(reducer.process({30U, 400U, camera::EventType::repeat}).has_value(),
           "stale repeat after capture unlock is inert");
    expect(reducer.process({30U, 400U, camera::EventType::press}).has_value(),
           "fresh press after capture unlock succeeds");
    expect(reducer.is_held(400U), "capture unlock requires and accepts a fresh press");

    const auto invalid_control = reducer.process(
        {camera::invalid_control_id, 400U, camera::EventType::press});
    const auto invalid_action = reducer.process(
        {30U, camera::invalid_action_id, camera::EventType::press});
    const auto invalid_kind = reducer.process(
        {30U, 400U, static_cast<camera::EventType>(255U)});
    expect(!invalid_control.has_value() && !invalid_action.has_value() && !invalid_kind.has_value(),
           "zero IDs and unknown event kinds are rejected");
    expect(reducer.is_held(400U), "invalid events leave existing state unchanged");

    const auto mismatched_release = reducer.process(
        {30U, 401U, camera::EventType::release});
    expect(!mismatched_release.has_value() && reducer.is_held(400U),
           "a mismatched action release is rejected atomically");
}

void test_edge_scroll_regions() {
    const camera::Constants land = eaw_land();
    const float width = 1280.0F;
    const float height = 720.0F;
    auto centre = camera::edge_scroll(land, 640.0F, 360.0F, width, height);
    expect(centre && centre.value() == camera::EdgeScroll{0, 0}, "the centre does not scroll");
    auto left = camera::edge_scroll(land, 1.0F, 360.0F, width, height);
    expect(left && left.value().axis_x == -1, "the left edge region scrolls left");
    auto right = camera::edge_scroll(land, width - 1.0F, 360.0F, width, height);
    expect(right && right.value().axis_x == 1, "the right edge region scrolls right");
    auto top = camera::edge_scroll(land, 640.0F, 0.0F, width, height);
    expect(top && top.value().axis_y == -1, "the top edge region scrolls up");
    auto corner = camera::edge_scroll(land, 0.0F, 0.0F, width, height);
    expect(corner && corner.value() == camera::EdgeScroll{-1, -1},
           "a corner scrolls on both axes");
    // Tactical_Offscreen_Scroll_Region keeps a slightly offscreen cursor
    // scrolling; further out has left the window and stops.
    auto just_off = camera::edge_scroll(land, -20.0F, 360.0F, width, height);
    expect(just_off && just_off.value().axis_x == -1,
           "a cursor inside the offscreen region keeps scrolling");
    auto far_off = camera::edge_scroll(land, -200.0F, 360.0F, width, height);
    expect(far_off && far_off.value().axis_x == 0,
           "a cursor beyond the offscreen region stops scrolling");
    expect(!camera::edge_scroll(land, 0.0F, 0.0F, 0.0F, height),
           "a zero viewport extent is rejected");
}

void test_rotate_and_pitch_input_mapping() {
    const camera::Constants space = eaw_space();
    // Yaw_Per_Mouse_Unit is 1.5 and the authored range spans a full turn, so
    // dragging wraps rather than sticking at the sentinel limit.
    expect_close(camera::apply_rotate(space, 0.0F, 10.0F).value(), 15.0F, "yaw from mouse units");
    auto wrapped = camera::apply_rotate(space, 0.0F, 10000.0F);
    expect(wrapped && wrapped.value() >= space.yaw_min && wrapped.value() <= space.yaw_max,
           "a large drag wraps inside the authored yaw range");
    camera::Constants narrow = eaw_space();
    narrow.yaw_min = -30.0F;
    narrow.yaw_max = 30.0F;
    expect_close(camera::apply_rotate(narrow, 0.0F, 1000.0F).value(), 30.0F,
                 "a narrow authored yaw range clamps instead of wrapping");

    // Pitch_Per_Mouse_Unit is -1.5 in space and 0 in EaW land.
    expect_close(camera::apply_pitch(space, 50.0F, 10.0F).value(), 35.0F,
                 "pitch from mouse units");
    expect_close(camera::apply_pitch(space, 50.0F, -1000.0F).value(), space.pitch_max,
                 "free-look pitch clamps to Pitch_Max");
    expect_close(camera::apply_pitch(space, 50.0F, 1000.0F).value(), space.pitch_min,
                 "free-look pitch clamps to Pitch_Min");
    expect_close(camera::apply_pitch(eaw_land(), 50.0F, 100.0F).value(), 50.0F,
                 "land free-look pitch is inert because Pitch_Per_Mouse_Unit is 0");
    expect(!camera::apply_rotate(space, 0.0F, std::nanf("")),
           "a non-finite rotate input is rejected");
}

void test_smoothing_is_deterministic() {
    // Distance_Smooth_Time 0.21 with a 0.21 second step leaves 1/e of the error.
    auto once = camera::smooth_toward(0.0F, 100.0F, 0.21F, 0.21F);
    expect_close(once.value(), 100.0F * (1.0F - std::exp(-1.0F)), "one time constant decays 1/e",
                 0.01F);
    expect_close(camera::smooth_toward(0.0F, 100.0F, 0.0F, 0.016F).value(), 100.0F,
                 "a non-positive smooth time snaps");
    expect_close(camera::smooth_toward(5.0F, 100.0F, 0.2F, 0.0F).value(), 5.0F,
                 "a zero delta holds");
    auto first = camera::smooth_toward(3.0F, 90.0F, 0.15F, 0.016F);
    auto second = camera::smooth_toward(3.0F, 90.0F, 0.15F, 0.016F);
    expect(first.value() == second.value(), "smoothing is deterministic for equal inputs");
    expect(!camera::smooth_toward(0.0F, 1.0F, 0.2F, -0.1F), "a negative delta is rejected");
}

void test_eye_position_geometry() {
    const std::array<float, 3> target{0.0F, 0.0F, 0.0F};
    const std::span<const float, 3> at{target};
    // Straight down: the eye sits directly above the target.
    auto overhead = camera::eye_position(at, 100.0F, 90.0F, 0.0F);
    expect(overhead && close(overhead.value()[1], 100.0F, 0.01F)
               && close(overhead.value()[0], 0.0F, 0.01F)
               && close(overhead.value()[2], 0.0F, 0.01F),
           "a 90 degree pitch places the eye directly overhead");
    // Horizon: the eye sits at target height, offset along +z at yaw 0.
    auto horizon = camera::eye_position(at, 100.0F, 0.0F, 0.0F);
    expect(horizon && close(horizon.value()[1], 0.0F, 0.01F)
               && close(horizon.value()[2], 100.0F, 0.01F),
           "a zero pitch places the eye on the horizon behind the target");
    // The shipped land pitch of 50 degrees puts the eye above and behind.
    auto land = camera::eye_position(at, 460.0F, 55.0F, 0.0F);
    expect(land && land.value()[1] > 0.0F && land.value()[2] > 0.0F,
           "the shipped land framing is above and behind the target");
    expect(!camera::eye_position(at, -1.0F, 50.0F, 0.0F),
           "a negative distance is rejected");
}

// #515 (tactical-camera-input "Field of view", debug build): the XML angle is horizontal on a
// 4:3 screen; the renderer draws with that screen's vertical angle, which FoC keeps on 16:9 and
// the remake on every aspect (#195): 42.66 degrees for Fov_Default 55.
void test_field_of_view_is_foc_horizontal_on_4_3() {
    constexpr float degrees = 3.14159265F / 180.0F;
    const auto space = camera::vertical_fov_degrees(55.0F);
    expect(space && close(space.value(), 42.66F, 0.01F), "55 across a 4:3 screen is 42.66 up");
    // At 4:3 the horizontal angle is the XML one again.
    expect(space && close(2.0F * std::atan(std::tan(space.value() * 0.5F * degrees) * 4.0F / 3.0F) / degrees, 55.0F, 1e-3F),
           "4:3 draws exactly the XML angle across");
    // FoC on 16:9 widens the horizontal half-angle tangent by 0.75 * 16 / 9, keeping the vertical.
    const float foc_16_9 = 2.0F * std::atan(std::tan(27.5F * degrees) * 0.75F * (16.0F / 9.0F) / (16.0F / 9.0F)) / degrees;
    expect(space && close(space.value(), foc_16_9, 1e-3F), "16:9 keeps FoC's 4:3 vertical angle");
    const auto overview = camera::vertical_fov_degrees(70.0F);
    expect(overview && close(overview.value(), 2.0F * std::atan(0.75F * std::tan(35.0F * degrees)) / degrees, 1e-3F),
           "Tactical_Overview_FOV2 follows the same rule");
    expect(!camera::vertical_fov_degrees(0.0F) && !camera::vertical_fov_degrees(180.0F)
               && !camera::vertical_fov_degrees(std::numeric_limits<float>::quiet_NaN()),
           "a degenerate angle is refused");
    // The drag moves the vertical frustum at the target distance per viewport height.
    camera::State state{};
    state.distance = 100.0F;
    state.fov_degrees = 55.0F;
    const auto moved = camera::drag_displacement(state, 0.0F, 1.0F);
    expect(moved && space && close(moved.value().y, 2.0F * 100.0F * std::tan(space.value() * 0.5F * degrees), 1e-2F),
           "a full-height drag moves the vertical frustum height");
}

void test_mode_names_match_the_xml_definitions() {
    expect(camera::definition_name(camera::Mode::land) == "Land_Mode", "land definition name");
    expect(camera::definition_name(camera::Mode::space) == "Space_Mode", "space definition name");
    expect(camera::definition_name(camera::Mode::unlocked) == "Unlocked",
           "unlocked definition name");
    expect(camera::to_string(camera::Mode::land) == "land", "land mode string");
    expect(camera::to_string(camera::Mode::space) == "space", "space mode string");
}

} // namespace

int main() {
    test_three_zoom_levels_per_mode();
    test_zoom_limits_and_clamping();
    test_invalid_constants_are_rejected();
    test_spline_parsing_and_evaluation();
    test_zoom_step_input_mapping();
    test_pan_input_mapping();
    test_input_two_controls_and_repeats();
    test_input_opposing_controls_are_independent_and_ordered();
    test_input_focus_loss_requires_fresh_press();
    test_input_capture_lock_cancels_and_invalid_events_are_atomic();
    test_edge_scroll_regions();
    test_rotate_and_pitch_input_mapping();
    test_smoothing_is_deterministic();
    test_eye_position_geometry();
    test_field_of_view_is_foc_horizontal_on_4_3();
    test_mode_names_match_the_xml_definitions();
    if (failures != 0) {
        std::cerr << failures << " camera contract failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "camera contracts passed\n";
    return EXIT_SUCCESS;
}
