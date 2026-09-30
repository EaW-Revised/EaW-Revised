#pragma once

// Private support for the map camera bridge contract runner
// (map_camera_bridge_main.cpp), which keeps the helper definitions; the cases
// are grouped into map_camera_{lifecycle,movement,config}_tests.cpp.

#include "map_camera.hpp"

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

namespace eawr_map_camera_test {

namespace camera = eawr::presentation::camera;
namespace input = eawr::viewer::camera_input;
using eawr::viewer::MapCameraBridge;
using eawr::viewer::MapCameraConfig;

extern int failures;

void expect(const bool condition, const std::string_view message);
void close(const float actual, const float expected, const std::string_view message,
           const float tolerance = 1e-3F);
void orbit_about_focus(const camera::TacticalFrame& before, const camera::TacticalFrame& after,
                       const std::string_view label);

constexpr std::string_view space_map = "data/art/maps/eawr_space_synthetic.ted";
constexpr std::string_view space_sha =
    "35377e070c521b37edd679fd1a092f9b77028914531eff1bcbe330dc1ff87202";
constexpr std::string_view land_map = "data/art/maps/eawr_scene_synthetic.ted";
constexpr std::string_view land_sha =
    "3ae3c3379a1b52af246f9f397db7c724937ccc6f848c3eb11637d5149c1ae9bd";

constexpr std::string_view tactical_xml = R"XML(<?xml version="1.0" encoding="utf-8"?>
<TacticalCameras>
  <TacticalCamera Name="Land_Mode">
    <Distance_Min>100.0</Distance_Min>
    <Distance_Max>500.0</Distance_Max>
    <Distance_Default>300.0</Distance_Default>
    <Distance_Per_Mouse_Unit>80.0</Distance_Per_Mouse_Unit>
    <Distance_Smooth_Time>0</Distance_Smooth_Time>
    <Pitch_Min>20.0</Pitch_Min>
    <Pitch_Max>60.0</Pitch_Max>
    <Pitch_Default>40.0</Pitch_Default>
    <Pitch_Per_Mouse_Unit>0.0</Pitch_Per_Mouse_Unit>
    <Pitch_Per_Zoom_Unit>0.0</Pitch_Per_Zoom_Unit>
    <Pitch_When_Zoomed_In>20.0</Pitch_When_Zoomed_In>
    <Pitch_Zoom_Begin_Fraction>-1.0</Pitch_Zoom_Begin_Fraction>
    <Yaw_Min>-1000.0</Yaw_Min>
    <Yaw_Max>1000.0</Yaw_Max>
    <Yaw_Default>0.0</Yaw_Default>
    <Yaw_Per_Mouse_Unit>1.25</Yaw_Per_Mouse_Unit>
    <Fov_Min>30.0</Fov_Min>
    <Fov_Max>50.0</Fov_Max>
    <Fov_Default>50.0</Fov_Default>
    <Fov_Per_Mouse_Unit>0.0</Fov_Per_Mouse_Unit>
    <Near_Clip>5.0</Near_Clip>
    <Far_Clip>8000.0</Far_Clip>
    <Use_Splines>yes</Use_Splines>
    <Distance_Spline>0.0 100.0, 0.25, 200.0, 1.0, 500.0</Distance_Spline>
    <Pitch_Spline>0.0,20.0, 0.5,40.0, 1.0, 60.0</Pitch_Spline>
  </TacticalCamera>
  <TacticalCamera Name="Space_Mode">
    <Distance_Min>400.0</Distance_Min>
    <Distance_Max>1200.0</Distance_Max>
    <Distance_Default>800.0</Distance_Default>
    <Distance_Per_Mouse_Unit>200.0</Distance_Per_Mouse_Unit>
    <Distance_Smooth_Time>0</Distance_Smooth_Time>
    <Pitch_Min>-20.0</Pitch_Min>
    <Pitch_Max>80.0</Pitch_Max>
    <Pitch_Default>45.0</Pitch_Default>
    <Pitch_Per_Mouse_Unit>-1.25</Pitch_Per_Mouse_Unit>
    <Pitch_Per_Zoom_Unit>0.0</Pitch_Per_Zoom_Unit>
    <Pitch_When_Zoomed_In>45.0</Pitch_When_Zoomed_In>
    <Pitch_Zoom_Begin_Fraction>-1.0</Pitch_Zoom_Begin_Fraction>
    <Yaw_Min>-1000.0</Yaw_Min>
    <Yaw_Max>1000.0</Yaw_Max>
    <Yaw_Default>0.0</Yaw_Default>
    <Yaw_Per_Mouse_Unit>1.25</Yaw_Per_Mouse_Unit>
    <Fov_Min>50.0</Fov_Min>
    <Fov_Max>50.0</Fov_Max>
    <Fov_Default>50.0</Fov_Default>
    <Fov_Per_Mouse_Unit>0.0</Fov_Per_Mouse_Unit>
    <Near_Clip>2.0</Near_Clip>
    <Far_Clip>9000.0</Far_Clip>
  </TacticalCamera>
</TacticalCameras>
)XML";

constexpr std::string_view constants_xml = R"XML(<?xml version="1.0" encoding="utf-8"?>
<Game_Constants>
  <Tactical_Min_Scroll_Speed>600.0</Tactical_Min_Scroll_Speed>
  <Tactical_Max_Scroll_Speed>2400.0</Tactical_Max_Scroll_Speed>
  <Tactical_Edge_Scroll_Region>3</Tactical_Edge_Scroll_Region>
  <Tactical_Offscreen_Scroll_Region>40</Tactical_Offscreen_Scroll_Region>
  <Push_Scroll_Speed_Modifier>2.5</Push_Scroll_Speed_Modifier>
  <Scroll_Acceleration_Factor>0</Scroll_Acceleration_Factor>
  <Scroll_Deceleration_Factor>0</Scroll_Deceleration_Factor>
</Game_Constants>
)XML";

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text);
[[nodiscard]] std::optional<camera::LoadedConstants> load(const camera::Mode mode,
                                                           const std::string_view tactical = tactical_xml);
[[nodiscard]] std::string read(const std::string_view name);
[[nodiscard]] std::string replaced(std::string text, const std::string_view from, const std::string_view to);
[[nodiscard]] std::optional<MapCameraConfig> space_config(const std::string& xml);
input::RawEvent key(const std::string_view name, const bool pressed);
input::RawEvent wheel(const bool down);
input::RawEvent middle(const bool pressed);
input::RawEvent motion(const float x, const float y, const float relative_x);

// A space bridge activated from the committed fixtures, optionally with a
// modified config text.
struct Space final {
    MapCameraBridge bridge{true, input::Context::space};
    bool ok{};
};
void activate(Space& space, const std::string& config_xml,
              const std::optional<camera::TacticalFrame> fixed = std::nullopt);
[[nodiscard]] bool handled(MapCameraBridge& bridge, const input::RawEvent& event);
[[nodiscard]] float pan_speed(const float zoom);

constexpr std::string_view config_sha =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

[[nodiscard]] std::string with_overrides(const std::string& config_xml, const std::string_view block);
[[nodiscard]] std::optional<MapCameraConfig> land_config(const std::string& xml);
[[nodiscard]] eawr::core::Result<camera::LoadedConstants> resolve(
    const MapCameraConfig& config, const camera::Mode mode);

// A land bridge activated from the committed land config plus an optional
// override block.
struct Land final {
    MapCameraBridge bridge{true, input::Context::land};
    std::optional<camera::LoadedConstants> constants;
    bool ok{};
};
void activate_land(Land& land, const std::string_view overrides,
                   const std::optional<camera::TacticalFrame> fixed = std::nullopt);

// map_camera_lifecycle_tests.cpp
void test_space_activation_and_provenance();
void test_incompatible_configs_and_bindings_reject();
void test_lifecycle_clears_stale_input();
void test_failures_do_not_partially_mutate();
void test_bounded_trace();

// map_camera_movement_tests.cpp
void test_pan_basis_and_clamp();
void test_zoom_yaw_and_reset();
void test_terrain_ground_sampling();
void test_bridge_uses_motion_constants();
void test_alt_drag_is_frame_rate_independent();
void test_terrain_following();
void test_committed_tables_carry_the_feel_check();
void test_land_pan_is_halved_space_is_not();
void test_ctrl_orbit_through_the_bridge();
void test_plain_middle_drag_translates();
void test_middle_click_resets_the_view();
void test_host_and_free_tables_keep_pixel_units();
void test_land_click_reset_keeps_the_target_height();
void test_ctrl_rotate_is_resolution_independent();
void test_orbit_keeps_terrain_clearance();
void test_orbit_during_pan_coast_follows_terrain();
void test_ctrl_orbit_is_frame_rate_independent();
void test_orbit_cancellation_in_the_bridge();
void test_space_close_zoom_and_depth_floor();
void test_middle_button_law_in_every_map_table();

// map_camera_config_tests.cpp
void test_committed_configs_carry_no_overrides();
void test_config_override_parsing_rejects();
void test_overrides_drive_the_bridge_through_lifecycle();
void test_fixed_capture_outranks_overrides();
void test_report_members();
void test_resolve_dry_runs_the_controller();
void test_override_effect_markers();
void test_committed_m1_configs_parse();

} // namespace eawr_map_camera_test
