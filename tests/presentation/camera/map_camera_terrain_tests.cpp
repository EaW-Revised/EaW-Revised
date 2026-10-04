#include "map_camera_movement_support.hpp"

namespace eawr_map_camera_test {

void test_terrain_ground_sampling() {
    using eawr::viewer::TerrainGround;
    // A 3x3 grid, 10 units apart; one raised corner makes the surface
    // non-planar, so the triangle split is observable.
    const TerrainGround ground{3, 3, 10.0F, {0.0F, 10.0F, 20.0F, 100.0F, 200.0F, 120.0F, 200.0F, 210.0F, 220.0F}};
    close(*ground.at(0.0F, 0.0F), 0.0F, "a grid sample is exact");
    close(*ground.at(5.0F, 0.0F), 5.0F, "an edge midpoint interpolates");
    close(*ground.at(7.5F, 2.5F), 55.0F, "lower-right triangle (00, 10, 11)");
    close(*ground.at(2.5F, 7.5F), 100.0F, "upper-left triangle (00, 11, 01)");
    close(*ground.at(-50.0F, -50.0F), 0.0F, "outside the grid clamps to the edge");
    close(*ground.at(1e6F, 1e6F), 220.0F, "far outside clamps to the last sample");
    expect(!TerrainGround{}.at(0.0F, 0.0F), "an empty grid has no height");
    expect(!TerrainGround{3, 3, 10.0F, {0.0F}}.at(0.0F, 0.0F), "an inconsistent grid has no height");
    expect(!ground.at(std::numeric_limits<float>::quiet_NaN(), 0.0F), "a non-finite point has no height");
}

struct FollowingLand final {
    MapCameraBridge bridge{true, input::Context::land};
    bool ok{};
};
void activate_following(FollowingLand& land, const std::string_view min_height,
                        const bool follows = true,
                        const std::optional<camera::TacticalFrame> fixed = std::nullopt,
                        const bool with_coast = false) {
    const std::string following = replaced(std::string(tactical_xml), "<Use_Splines>yes</Use_Splines>",
        std::string(follows ? "<Location_Follows_Terrain>1</Location_Follows_Terrain>" : "")
        + "<Location_Height_Up_Smooth_Time>0.3</Location_Height_Up_Smooth_Time>"
          "<Location_Height_Down_Smooth_Time>1.0</Location_Height_Down_Smooth_Time>"
          "<Min_Height_Above_Terrain>" + std::string(min_height) + "</Min_Height_Above_Terrain>"
          "<Use_Splines>yes</Use_Splines>");
    const auto config = land_config(read("map-camera.xml"));
    auto constants = load(camera::Mode::land, following);
    expect(config.has_value() && constants.has_value(), "following config and constants load");
    if (!config || !constants) return;
    if (with_coast) {
        constants->constants.scroll_acceleration_factor = 0.1F;
        constants->constants.scroll_deceleration_factor = 0.2F;
    }
    eawr::viewer::TerrainGround ramp{17, 13, 40.0F, {}};
    for (std::uint32_t row = 0; row < ramp.height; ++row) {
        for (std::uint32_t column = 0; column < ramp.width; ++column) {
            ramp.heights.push_back(static_cast<float>(column) * ramp.spacing);
        }
    }
    land.bridge.set_ground(std::move(ramp));
    auto active = land.bridge.activate(*config, constants->constants,
        read("map-camera-bindings.json"), 1280, 720, fixed);
    expect(active.has_value(), "following land bridge activates");
    land.ok = active.has_value();
}

void test_terrain_following() {
    FollowingLand land;
    activate_following(land, "20.0");
    if (!land.ok) return;
    auto& bridge = land.bridge;
    expect(bridge.follows_ground(), "Location_Follows_Terrain with a ground source follows");
    close(bridge.frame().target[1], 320.0F, "activation snaps the target onto the ground");
    const float eye_above = bridge.frame().eye[1] - bridge.frame().target[1];
    expect(handled(bridge, key("D", true)) && bridge.step(0.1F).has_value(), "pan onto higher ground");
    const float ground = bridge.frame().target[0];
    expect(ground > 320.0F, "pan moved the target up the ramp");
    close(bridge.frame().target[1], 320.0F + (ground - 320.0F) * (1.0F - std::exp(-0.1F / 0.3F)),
          "rising ground eases with Location_Height_Up_Smooth_Time", 1e-2F);
    close(bridge.frame().eye[1] - bridge.frame().target[1], eye_above, "the eye keeps its offset");
    expect(handled(bridge, key("D", false)) && handled(bridge, key("A", true))
           && bridge.step(10.0F).has_value(), "pan back down the ramp");
    const float lower = bridge.frame().target[0];
    const float before = ground > 320.0F ? 320.0F + (ground - 320.0F) * (1.0F - std::exp(-0.1F / 0.3F)) : 0.0F;
    close(bridge.frame().target[1], before + (lower - before) * (1.0F - std::exp(-10.0F / 1.0F)),
          "falling ground eases with Location_Height_Down_Smooth_Time", 1e-2F);
    expect(handled(bridge, key("A", false)) && handled(bridge, key("Home", true))
           && bridge.step(0.0F).has_value(), "reset");
    close(bridge.frame().target[1], 320.0F, "reset snaps onto the ground again");

    FollowingLand floored;
    activate_following(floored, "10000.0");
    if (floored.ok) {
        close(floored.bridge.frame().eye[1], 320.0F + 10000.0F, "the eye stays Min_Height_Above_Terrain above ground", 1e-2F);
        const auto held = floored.bridge.frame();
        expect(floored.bridge.step(0.5F).has_value() && floored.bridge.frame() == held,
               "an idle step at the eye floor is exactly stable");
    }

    FollowingLand inert;
    activate_following(inert, "20.0", false);
    if (inert.ok) {
        expect(!inert.bridge.follows_ground(), "without Location_Follows_Terrain the ground is inert");
        close(inert.bridge.frame().target[1], 0.0F, "the authored target height stands");
    }

    const camera::TacticalFrame fixed{1280, 720, 60.0F, 1.0F, 5000.0F, {320.0F, 900.0F, -240.0F},
                                      {320.0F, 0.0F, -240.0F}, {0.0F, 0.0F, -1.0F}};
    FollowingLand locked;
    activate_following(locked, "20.0", true, fixed);
    if (locked.ok) {
        expect(locked.bridge.frame() == fixed && locked.bridge.step(1.0F).has_value()
               && locked.bridge.frame() == fixed, "a fixed capture is never moved by the ground");
    }
}


// FoC's middle click resets pitch, yaw, distance and field of view and leaves
// the camera location alone, its smoothed height included (debug build,
// the tactical camera reset without the home flag). A land click
// while the height is still easing up a ramp keeps that height exactly.
void test_land_click_reset_keeps_the_target_height() {
    FollowingLand land;
    activate_following(land, "20.0");
    if (!land.ok) return;
    auto& bridge = land.bridge;
    expect(handled(bridge, key("D", true)) && bridge.step(0.1F).has_value()
           && handled(bridge, key("D", false)) && bridge.step(0.0F).has_value(), "pan up the ramp");
    expect(handled(bridge, wheel(true)) && bridge.step(0.0F).has_value(), "zoom before the click");
    const auto before = bridge.frame();
    const float ground = before.target[0];  // The ramp's height equals source X.
    expect(before.target[1] < ground - 1.0F, "the target height is still easing below the ground");
    expect(handled(bridge, middle(true)) && handled(bridge, middle(false))
           && bridge.step(0.0F).has_value(), "land middle click");
    expect(bridge.view_resets() == 1U, "the click is a view reset");
    close(bridge.controller().state().zoom, bridge.config().zoom, "the click restores the zoom");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        close(bridge.frame().target[axis], before.target[axis], "the click keeps the target", 1e-4F);
    }
    const auto& entry = bridge.trace().back();
    expect(entry.view_resets == 1U && entry.target_after == entry.target_before,
           "the reset step's trace keeps all three target axes");
    expect(bridge.frame().eye[1] >= bridge.frame().eye[0] + 20.0F - 1e-2F,
           "the eye floor still holds after the click");
    expect(bridge.step(0.1F).has_value() && bridge.frame().target[1] > before.target[1],
           "terrain following resumes easing after the click");

    FollowingLand floored;
    activate_following(floored, "10000.0");
    if (!floored.ok) return;
    const auto held = floored.bridge.frame();
    expect(handled(floored.bridge, middle(true)) && handled(floored.bridge, middle(false))
           && floored.bridge.step(0.0F).has_value(), "click at the eye floor");
    close(floored.bridge.frame().eye[1], 320.0F + 10000.0F, "the eye floor still lifts a click reset", 1e-2F);
    close(floored.bridge.frame().target[1], held.target[1], "at the floor the height is unchanged", 1e-3F);
}

// RO-7: a full-width Ctrl drag is 100 mouse units (150 degrees with FoC's
// Yaw_Per_Mouse_Unit 1.5) at 1280 x 720 and 1920 x 1080 alike, through the
// committed table and the bridge. The synthetic fixture turns 1.25 per unit.
void test_orbit_keeps_terrain_clearance() {
    FollowingLand land;
    activate_following(land, "20.0");
    if (!land.ok) return;
    auto& bridge = land.bridge;
    const auto initial = bridge.frame();
    // Turn a quarter so the eye sits up the ramp: 72 mouse units at 1.25 degrees,
    // 921.6 of 1280 px, purely horizontal, so only clearance may raise the pitch.
    const float start_pitch = bridge.controller().state().pitch_degrees;
    expect(handled(bridge, ctrl(middle(true))) && handled(bridge, ctrl(pointer(-921.6F, 0.0F)))
           && bridge.step(0.1F).has_value(), "Ctrl rotate over the ramp");
    close(bridge.controller().yaw_degrees(), 90.0F, "the eye is turned toward higher ground", 1e-3F);
    expect(bridge.controller().state().pitch_degrees >= start_pitch - 1e-3F,
           "terrain clearance only ever raises the land pitch");
    const auto& frame = bridge.frame();
    const float ground = frame.eye[0];  // The ramp's height equals source X.
    orbit_about_focus(initial, frame, "terrain orbit keeps focus");
    expect(frame.eye[0] > frame.target[0], "the eye is up the ramp");
    expect(frame.eye[1] >= ground + 20.0F - 1e-2F, "the eye stays Min_Height_Above_Terrain above ground");
    expect(handled(bridge, wheel(true)) && bridge.step(0.0F).has_value(), "zoom after terrain orbit");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        close(bridge.frame().target[axis], initial.target[axis], "terrain zoom keeps the focus");
    }
    expect(bridge.frame().eye[1] >= bridge.frame().eye[0] + 20.0F - 1e-2F,
           "terrain zoom keeps eye clearance");
    expect(handled(bridge, ctrl(middle(false))) && bridge.step(0.5F).has_value(),
           "terrain orbit settles after release");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        close(bridge.frame().target[axis], initial.target[axis], "release keeps the terrain focus");
    }
}

void test_orbit_during_pan_coast_follows_terrain() {
    FollowingLand land;
    activate_following(land, "20.0", true, std::nullopt, true);
    if (!land.ok) return;
    auto& bridge = land.bridge;
    expect(handled(bridge, key("D", true)) && bridge.step(0.1F).has_value(),
           "pan starts up the ramp before orbit");
    const auto before_coast = bridge.frame();
    expect(handled(bridge, key("D", false)) && handled(bridge, ctrl(middle(true)))
           && handled(bridge, ctrl(pointer(8.0F, 0.0F))) && bridge.step(0.1F).has_value(),
           "released pan coasts during Ctrl orbit");
    const auto during_coast = bridge.frame();
    expect(during_coast.target[0] > before_coast.target[0],
           "released pan translates the orbit focus");
    close(during_coast.target[1],
          before_coast.target[1] + (during_coast.target[0] - before_coast.target[1])
              * (1.0F - std::exp(-0.1F / 0.3F)),
          "coasting orbit follows the uphill target", 1e-2F);
    expect(during_coast.target[1] > before_coast.target[1],
           "coasting orbit does not hold the old target height");
    expect(handled(bridge, ctrl(pointer(8.0F, 0.0F))) && bridge.step(0.1F).has_value(),
           "orbit continues during the coast");
    expect(bridge.frame().target[0] > during_coast.target[0]
           && bridge.frame().target[1] > during_coast.target[1],
           "subsequent coasting orbit steps keep following the ramp");
}

// The same physical Ctrl drag reaches the same yaw and pitch at any frame rate.

} // namespace eawr_map_camera_test
