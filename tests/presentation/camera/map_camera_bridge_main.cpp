// CPU contracts for the viewer's MapCameraBridge in land and space context.
//
// The tactical XML below is synthetic test data from
// tests/assets/fixtures/camera_fixture.py. The committed map configs are
// project-authored FoC map pins and bounds; these tests make no claims about
// original camera constants.
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

#include "map_camera_test_support.hpp"

namespace eawr_map_camera_test {

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
void close(const float actual, const float expected, const std::string_view message,
           const float tolerance) {
    expect(std::abs(actual - expected) <= tolerance, message);
}

void orbit_about_focus(const camera::TacticalFrame& before, const camera::TacticalFrame& after,
                       const std::string_view label) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        close(after.target[axis], before.target[axis], label, 1e-3F);
    }
    const auto radius = [](const camera::TacticalFrame& frame) {
        return std::hypot(frame.eye[0] - frame.target[0],
                          frame.eye[1] - frame.target[1], frame.eye[2] - frame.target[2]);
    };
    close(radius(after), radius(before), label, 1e-2F);
    expect(after.eye != before.eye, label);
}

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] std::optional<camera::LoadedConstants> load(const camera::Mode mode,
                                                           const std::string_view tactical) {
    auto loaded = camera::load_constants(
        {bytes(tactical), "data/xml/tacticalcameras.xml", "tactical-sha"},
        {bytes(constants_xml), "data/xml/gameconstants.xml", "constants-sha"}, mode);
    if (!loaded) return std::nullopt;
    return std::move(loaded.value());
}

[[nodiscard]] std::string read(const std::string_view name) {
    std::ifstream file(std::string(EAWR_VIEWER_CONFIG_DIR) + "/" + std::string(name), std::ios::binary);
    expect(static_cast<bool>(file), "committed config fixture is readable");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string replaced(std::string text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    expect(at != std::string::npos, "fixture substitution anchor exists");
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

[[nodiscard]] std::optional<MapCameraConfig> space_config(const std::string& xml) {
    auto parsed = eawr::viewer::parse_map_camera_config(xml, space_map, space_sha, camera::Mode::space);
    if (!parsed) return std::nullopt;
    return parsed.value();
}

input::RawEvent key(const std::string_view name, const bool pressed) {
    input::RawEvent event;
    event.kind = input::RawKind::key;
    event.code = input::key_code(name).value_or(0U);
    event.pressed = pressed;
    return event;
}
input::RawEvent wheel(const bool down) {
    input::RawEvent event;
    event.kind = input::RawKind::mouse_wheel;
    event.code = down ? input::mouse_code::wheel_down : input::mouse_code::wheel_up;
    event.pressed = true;
    return event;
}
input::RawEvent middle(const bool pressed) {
    input::RawEvent event;
    event.kind = input::RawKind::mouse_button;
    event.code = input::mouse_code::middle;
    event.pressed = pressed;
    event.position_x = 640.0F;
    event.position_y = 360.0F;
    event.has_position = true;
    return event;
}
input::RawEvent motion(const float x, const float y, const float relative_x) {
    input::RawEvent event;
    event.kind = input::RawKind::mouse_motion;
    event.position_x = x;
    event.position_y = y;
    event.relative_x = relative_x;
    event.has_position = true;
    return event;
}

void activate(Space& space, const std::string& config_xml,
              const std::optional<camera::TacticalFrame> fixed) {
    const auto config = space_config(config_xml);
    const auto constants = load(camera::Mode::space);
    expect(config.has_value() && constants.has_value(), "space config and constants load");
    if (!config || !constants) return;
    auto active = space.bridge.activate(*config, constants->constants,
        read("space-map-camera-bindings.json"), 1280, 720, fixed);
    expect(active.has_value(), "space bridge activates");
    space.ok = active.has_value();
}

[[nodiscard]] bool handled(MapCameraBridge& bridge, const input::RawEvent& event) {
    return bridge.handle(event).has_value();
}

[[nodiscard]] float pan_speed(const float zoom) {
    const auto constants = load(camera::Mode::space);
    auto solved = camera::solve(constants->constants, zoom);
    return solved ? solved.value().pan_speed : std::numeric_limits<float>::quiet_NaN();
}

[[nodiscard]] std::string with_overrides(const std::string& config_xml, const std::string_view block) {
    return replaced(config_xml, "</map_camera>", std::string(block) + "\n</map_camera>");
}

[[nodiscard]] std::optional<MapCameraConfig> land_config(const std::string& xml) {
    auto parsed = eawr::viewer::parse_map_camera_config(xml, land_map, land_sha);
    if (!parsed) return std::nullopt;
    return parsed.value();
}

[[nodiscard]] eawr::core::Result<camera::LoadedConstants> resolve(
    const MapCameraConfig& config, const camera::Mode mode) {
    auto xml = load(mode);
    expect(xml.has_value(), "override base constants load");
    return eawr::viewer::resolve_map_constants(config, *xml, "map-camera.xml", config_sha);
}

void activate_land(Land& land, const std::string_view overrides,
                   const std::optional<camera::TacticalFrame> fixed) {
    const std::string xml = overrides.empty() ? read("map-camera.xml")
                                              : with_overrides(read("map-camera.xml"), overrides);
    const auto config = land_config(xml);
    expect(config.has_value(), "land override config parses");
    if (!config) return;
    auto resolved = resolve(*config, camera::Mode::land);
    expect(resolved.has_value(), "land override layer resolves");
    if (!resolved) return;
    land.constants = resolved.value();
    auto active = land.bridge.activate(*config, resolved.value().constants,
        read("map-camera-bindings.json"), 1280, 720, fixed);
    expect(active.has_value(), "land bridge activates with overrides");
    land.ok = active.has_value();
}

} // namespace eawr_map_camera_test

int main() {
    using namespace eawr_map_camera_test;
    test_space_activation_and_provenance();
    test_incompatible_configs_and_bindings_reject();
    test_pan_basis_and_clamp();
    test_zoom_yaw_and_reset();
    test_lifecycle_clears_stale_input();
    test_failures_do_not_partially_mutate();
    test_bounded_trace();
    test_committed_configs_carry_no_overrides();
    test_config_override_parsing_rejects();
    test_overrides_drive_the_bridge_through_lifecycle();
    test_fixed_capture_outranks_overrides();
    test_report_members();
    test_resolve_dry_runs_the_controller();
    test_override_effect_markers();
    test_terrain_ground_sampling();
    test_bridge_uses_motion_constants();
    test_alt_drag_is_frame_rate_independent();
    test_terrain_following();
    test_committed_tables_carry_the_feel_check();
    test_land_pan_is_halved_space_is_not();
    test_ctrl_orbit_through_the_bridge();
    test_plain_middle_drag_translates();
    test_middle_click_resets_the_view();
    test_host_and_free_tables_keep_pixel_units();
    test_land_click_reset_keeps_the_target_height();
    test_ctrl_rotate_is_resolution_independent();
    test_orbit_keeps_terrain_clearance();
    test_orbit_during_pan_coast_follows_terrain();
    test_ctrl_orbit_is_frame_rate_independent();
    test_orbit_cancellation_in_the_bridge();
    test_space_close_zoom_and_depth_floor();
    test_middle_button_law_in_every_map_table();
    test_committed_m1_configs_parse();
    if (failures == 0) std::cout << "map camera bridge contracts passed\n";
    return failures == 0 ? 0 : 1;
}
