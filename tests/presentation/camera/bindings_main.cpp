// P1-09 camera binding-table loader and lifecycle adapter contracts.
//
// Exercises apps/viewer/src/camera_input.{hpp,cpp}, which is engine
// independent, without Godot. Constants are the wholly synthetic camera
// fixture values (tests/assets/fixtures/camera_fixture.py), never retail data,
// and every binding below is project-authored test data.

#include "camera_input.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "camera_binding_test_support.hpp"

#ifndef EAWR_CAMERA_BINDINGS_PATH
#error "EAWR_CAMERA_BINDINGS_PATH must name apps/viewer/project/config/camera-bindings.json"
#endif

namespace eawr_camera_binding_test {

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void expect_close(const float actual, const float expected, const std::string_view message,
                  const float tolerance) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << "FAILED: " << message << " (expected " << expected << ", got " << actual
                  << ")\n";
        ++failures;
    }
}

// Synthetic fixture Land_Mode (camera_fixture.LAND + SCROLL); not retail data.
camera::Constants fixture_constants() {
    camera::Constants constants;
    constants.distance_min = 100.0F;
    constants.distance_max = 500.0F;
    constants.distance_default = 300.0F;
    constants.distance_per_mouse_unit = 80.0F;
    constants.distance_smooth_time = 0.2F;
    constants.pitch_min = 20.0F;
    constants.pitch_max = 60.0F;
    constants.pitch_default = 40.0F;
    constants.pitch_when_zoomed_in = 20.0F;
    constants.pitch_zoom_begin_fraction = -1.0F;
    constants.yaw_min = -1000.0F;
    constants.yaw_max = 1000.0F;
    constants.yaw_default = 0.0F;
    constants.yaw_per_mouse_unit = 1.25F;
    constants.fov_min = 30.0F;
    constants.fov_max = 50.0F;
    constants.fov_default = 50.0F;
    constants.near_clip = 5.0F;
    constants.far_clip = 8000.0F;
    constants.use_splines = true;
    constants.distance_spline = {{0.0F, 100.0F}, {0.25F, 200.0F}, {1.0F, 500.0F}};
    constants.pitch_spline = {{0.0F, 20.0F}, {0.5F, 40.0F}, {1.0F, 60.0F}};
    constants.tactical_min_scroll_speed = 600.0F;
    constants.tactical_max_scroll_speed = 2400.0F;
    constants.tactical_edge_scroll_region = 3.0F;
    constants.tactical_offscreen_scroll_region = 40.0F;
    constants.push_scroll_speed_modifier = 2.5F;
    constants.scroll_acceleration_factor = 0.1F;
    constants.scroll_deceleration_factor = 1.25F;
    return constants;
}

std::string table_with(const std::string_view bindings, const std::string_view head) {
    return "{" + std::string(head) + ", \"bindings\": [" + std::string(bindings) + "]}";
}

// A compact project-authored test table, independent of the committed file.
std::string test_table() {
    return table_with(std::string(pan_right_d) + R"(,
      {"id": "land.right.arrow", "action": "pan_right", "context": "land", "device": "keyboard", "control": "Right", "trigger": "held", "scale": 1},
      {"id": "land.left.a", "action": "pan_left", "context": "land", "device": "keyboard", "control": "a", "trigger": "held", "scale": 1},
      {"id": "land.forward.w", "action": "pan_forward", "context": "land", "device": "keyboard", "control": "W", "trigger": "held", "scale": 1},
      {"id": "land.push.shift-d", "action": "push_scroll", "context": "land", "device": "keyboard", "control": "D", "modifiers": ["shift"], "trigger": "held", "scale": 1},
      {"id": "land.zoom.wheel-up", "action": "zoom", "context": "land", "device": "mouse_wheel", "control": "up", "trigger": "delta", "scale": -1},
      {"id": "land.zoom.pagedown", "action": "zoom", "context": "land", "device": "keyboard", "control": "PageDown", "trigger": "pressed", "scale": 2},
      {"id": "land.grab.middle", "action": "rotate_grab", "context": "land", "device": "mouse_button", "control": "middle", "trigger": "held", "scale": 1},
      {"id": "land.rotate.x", "action": "rotate", "context": "land", "device": "mouse_motion", "control": "x", "trigger": "delta", "scale": 0.5},
      {"id": "land.pan.alt-x", "action": "pan_motion_x", "context": "land", "device": "mouse_motion", "control": "x", "modifiers": ["alt"], "trigger": "delta", "scale": 1},
      {"id": "land.pan.alt-y", "action": "pan_motion_y", "context": "land", "device": "mouse_motion", "control": "y", "modifiers": ["alt"], "trigger": "delta", "scale": -1},
      {"id": "land.reset.home", "action": "reset_view", "context": "land", "device": "keyboard", "control": "Home", "trigger": "pressed", "scale": 1},
      {"id": "space.right.d", "action": "pan_right", "context": "space", "device": "keyboard", "control": "D", "trigger": "held", "scale": 1},
      {"id": "space.pan.alt-x", "action": "pan_motion_x", "context": "space", "device": "mouse_motion", "control": "x", "modifiers": ["alt"], "trigger": "delta", "scale": 1},
      {"id": "space.pan.alt-y", "action": "pan_motion_y", "context": "space", "device": "mouse_motion", "control": "y", "modifiers": ["alt"], "trigger": "delta", "scale": -1})");
}

input::RawEvent key(const std::string_view name, const bool pressed, const bool echo,
                    const std::uint8_t modifiers) {
    input::RawEvent event;
    event.kind = input::RawKind::key;
    event.code = input::key_code(name).value_or(0U);
    event.pressed = pressed;
    event.echo = echo;
    event.modifiers = modifiers;
    return event;
}

input::RawEvent button(const std::uint32_t code, const bool pressed) {
    input::RawEvent event;
    event.kind = input::RawKind::mouse_button;
    event.code = code;
    event.pressed = pressed;
    return event;
}

input::RawEvent wheel(const std::uint32_t code, const float factor) {
    input::RawEvent event;
    event.kind = input::RawKind::mouse_wheel;
    event.code = code;
    event.pressed = true;
    event.factor = factor;
    return event;
}

input::RawEvent motion(const float x, const float y, const float dx, const float dy) {
    input::RawEvent event;
    event.kind = input::RawKind::mouse_motion;
    event.position_x = x;
    event.position_y = y;
    event.relative_x = dx;
    event.relative_y = dy;
    event.has_position = true;
    return event;
}

input::Adapter ready_adapter() {
    input::Adapter adapter(input::Context::land, true);
    expect(adapter.activate(test_table()).has_value(), "test table activates");
    expect(adapter.set_viewport(1280.0F, 720.0F).has_value(), "viewport publishes");
    return adapter;
}

input::StepIntent step(input::Adapter& adapter) {
    static const camera::Constants constants = fixture_constants();
    auto intent = adapter.take_step(constants);
    expect(intent.has_value(), "take_step succeeds");
    return intent ? intent.value() : input::StepIntent{};
}

void send(input::Adapter& adapter, const input::RawEvent& event, const std::string_view what) {
    expect(adapter.handle(event).has_value(), what);
}

void expect_code(const std::string& json, const std::string_view code,
                 const std::string_view message) {
    auto parsed = input::parse_binding_table(json);
    if (parsed) {
        std::cerr << "FAILED: " << message << " (unexpectedly accepted)\n";
        ++failures;
        return;
    }
    if (parsed.error().code != code) {
        std::cerr << "FAILED: " << message << " (expected " << code << ", got "
                  << parsed.error().code << ": " << parsed.error().message << ")\n";
        ++failures;
    }
}

std::string v2_table() {
    return table_with(std::string(pan_right_d) + "," + std::string(free_toggle_land) + ","
        + std::string(free_toggle_free) + R"(,
      {"id": "free.left.a", "action": "free_move_left", "context": "free", "device": "keyboard", "control": "A", "trigger": "held", "scale": 1},
      {"id": "free.right.d", "action": "free_move_right", "context": "free", "device": "keyboard", "control": "D", "trigger": "held", "scale": 1},
      {"id": "free.forward.w", "action": "free_move_forward", "context": "free", "device": "keyboard", "control": "W", "trigger": "held", "scale": 1},
      {"id": "free.back.s", "action": "free_move_back", "context": "free", "device": "keyboard", "control": "S", "trigger": "held", "scale": 1},
      {"id": "free.rise.e", "action": "free_rise", "context": "free", "device": "keyboard", "control": "E", "trigger": "held", "scale": 1},
      {"id": "free.descend.q", "action": "free_descend", "context": "free", "device": "keyboard", "control": "Q", "trigger": "held", "scale": 1},
      {"id": "free.grab.right", "action": "free_look_grab", "context": "free", "device": "mouse_button", "control": "right", "trigger": "held", "scale": 1},
      {"id": "free.yaw.x", "action": "free_look_yaw", "context": "free", "device": "mouse_motion", "control": "x", "trigger": "delta", "scale": 2},
      {"id": "free.pitch.y", "action": "free_look_pitch", "context": "free", "device": "mouse_motion", "control": "y", "trigger": "delta", "scale": -1})",
        v2_head);
}

} // namespace eawr_camera_binding_test

int main() {
    using namespace eawr_camera_binding_test;
    test_committed_project_table_is_valid_and_labelled();
    test_schema_and_version_rejections();
    test_json_parser_limits_and_escaped_duplicate_keys();
    test_report_encoder_escapes_accepted_control_characters();
    test_malformed_utf8_is_rejected();
    test_unicode_escapes_match_raw_utf8();
    test_binding_field_rejections();
    test_ambiguous_chords();
    test_activation_is_atomic();
    test_held_repeat_and_pressed_triggers();
    test_focus_loss_requires_fresh_press();
    test_capture_lock_suppresses_everything();
    test_viewport_generations();
    test_wheel_and_rotate_deltas();
    test_alt_mouse_pan_in_both_tactical_modes();
    test_alt_drag_travel_is_frame_rate_independent();
    test_context_switch_cancels();
    test_advance_pose();
    test_schema_v1_keeps_rejecting_free();
    test_schema_v2_accepts_free_flight();
    test_schema_v2_rejections();
    test_adapter_free_toggle_and_flight();
    test_adapter_capture_lock_ignores_free_toggle();
    test_pan_speed_scale_key();
    test_orbit_pitch_bindings();
    test_orbit_cancellation();
    test_mouse_units_follow_the_screen();
    test_click_reset();
    test_input_default_ledger();
    if (failures != 0) {
        std::cerr << failures << " camera binding contract failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "camera binding contracts passed\n";
    return EXIT_SUCCESS;
}
