#pragma once

// Private support for the camera binding contract runner (bindings_main.cpp).
// Helper definitions live in bindings_main.cpp; the cases are grouped into
// bindings_json_tests.cpp and camera_binding_event_tests.cpp.

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

namespace eawr_camera_binding_test {

namespace camera = eawr::presentation::camera;
namespace input = eawr::viewer::camera_input;

extern int failures;

void expect(const bool condition, const std::string_view message);
void expect_close(const float actual, const float expected, const std::string_view message,
                  const float tolerance = 1e-3F);
camera::Constants fixture_constants();
std::string table_with(const std::string_view bindings, const std::string_view head =
    R"("schema": "eawr-camera-bindings", "version": 1, "provenance": "project-authored",
       "notice": "test-only project-authored bindings", "edge_scroll": true)");

constexpr std::string_view pan_right_d =
    R"({"id": "land.right.d", "action": "pan_right", "context": "land", "device": "keyboard",
        "control": "D", "trigger": "held", "scale": 1})";

std::string test_table();
input::RawEvent key(const std::string_view name, const bool pressed, const bool echo = false,
                    const std::uint8_t modifiers = 0);
input::RawEvent button(const std::uint32_t code, const bool pressed);
input::RawEvent wheel(const std::uint32_t code, const float factor);
input::RawEvent motion(const float x, const float y, const float dx, const float dy);
input::Adapter ready_adapter();
input::StepIntent step(input::Adapter& adapter);
void send(input::Adapter& adapter, const input::RawEvent& event, const std::string_view what);
void expect_code(const std::string& json, const std::string_view code,
                 const std::string_view message);

// --- Schema v2: free context ------------------------------------------------

constexpr std::string_view v2_head =
    R"("schema": "eawr-camera-bindings", "version": 2, "provenance": "project-authored",
       "notice": "test-only project-authored bindings", "edge_scroll": true,
       "free_camera": {"move_speed": 100, "vertical_speed": 50, "look_degrees_per_unit": 0.5,
                       "pitch_min_degrees": -80, "pitch_max_degrees": 80})";

constexpr std::string_view free_toggle_land =
    R"({"id": "land.free.f", "action": "free_toggle", "context": "land", "device": "keyboard",
        "control": "F", "trigger": "pressed", "scale": 1})";
constexpr std::string_view free_toggle_free =
    R"({"id": "free.free.f", "action": "free_toggle", "context": "free", "device": "keyboard",
        "control": "F", "trigger": "pressed", "scale": 1})";

std::string v2_table();

// bindings_json_tests.cpp
void test_committed_project_table_is_valid_and_labelled();
void test_schema_and_version_rejections();
void test_json_parser_limits_and_escaped_duplicate_keys();
void test_report_encoder_escapes_accepted_control_characters();
void test_malformed_utf8_is_rejected();
void test_unicode_escapes_match_raw_utf8();
void test_binding_field_rejections();
void test_schema_v1_keeps_rejecting_free();
void test_schema_v2_accepts_free_flight();
void test_schema_v2_rejections();
void test_pan_speed_scale_key();
void test_input_default_ledger();

// camera_binding_event_tests.cpp
void test_ambiguous_chords();
void test_activation_is_atomic();
void test_held_repeat_and_pressed_triggers();
void test_focus_loss_requires_fresh_press();
void test_capture_lock_suppresses_everything();
void test_viewport_generations();
void test_wheel_and_rotate_deltas();
void test_alt_mouse_pan_in_both_tactical_modes();
void test_alt_drag_travel_is_frame_rate_independent();
void test_context_switch_cancels();
void test_adapter_free_toggle_and_flight();
void test_adapter_capture_lock_ignores_free_toggle();
void test_advance_pose();
void test_orbit_pitch_bindings();
void test_orbit_cancellation();
void test_mouse_units_follow_the_screen();
void test_click_reset();

} // namespace eawr_camera_binding_test
