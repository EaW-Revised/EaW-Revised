#include "map_camera_movement_support.hpp"

namespace eawr_map_camera_test {

void test_pan_basis_and_clamp() {
    const float expected = pan_speed(0.5F) * 0.05F;
    close(pan_speed(0.5F), 1500.0F, "linear pan speed at the midpoint", 1e-2F);
    Space yaw0;
    activate(yaw0, read("space-map-camera.xml"));
    if (!yaw0.ok) return;
    auto& bridge = yaw0.bridge;
    expect(handled(bridge, key("D", true)), "D press routes");
    expect(bridge.step(0.05F).has_value(), "yaw 0 held pan steps");
    close(bridge.frame().target[0], 160.0F + expected, "yaw 0 right is +X");
    close(bridge.frame().target[2], -400.0F, "yaw 0 right leaves Z");
    expect(handled(bridge, key("D", false)) && handled(bridge, key("W", true)), "W press routes");
    expect(bridge.step(0.05F).has_value(), "yaw 0 forward steps");
    close(bridge.frame().target[2], -400.0F - expected, "yaw 0 forward is -Z");
    expect(bridge.step(10.0F).has_value(), "long forward step");
    close(bridge.frame().target[2], -800.0F, "forward clamps to source max_y");
    expect(handled(bridge, key("W", false)) && handled(bridge, key("D", true)), "D press again");
    expect(bridge.step(10.0F).has_value(), "long right step");
    close(bridge.frame().target[0], 400.0F, "right clamps to source max_x");
    const auto clamped = bridge.frame();
    expect(bridge.step(10.0F).has_value() && bridge.frame() == clamped,
           "outward motion at the clamp does not accumulate");
    close(bridge.frame().target[1], 60.0F, "target height is preserved by pan");

    Space yaw90;
    activate(yaw90, replaced(read("space-map-camera.xml"), "yaw_degrees=\"0\"", "yaw_degrees=\"90\""));
    if (!yaw90.ok) return;
    auto& rotated = yaw90.bridge;
    close(rotated.controller().yaw_degrees(), 90.0F, "initial yaw 90");
    expect(handled(rotated, key("D", true)) && rotated.step(0.05F).has_value(), "yaw 90 right steps");
    close(rotated.frame().target[0], 160.0F, "yaw 90 right leaves X");
    close(rotated.frame().target[2], -400.0F - expected, "yaw 90 right is -Z");
    expect(handled(rotated, key("D", false)) && handled(rotated, key("W", true))
        && rotated.step(0.05F).has_value(), "yaw 90 forward steps");
    close(rotated.frame().target[0], 160.0F - expected, "yaw 90 forward is -X");
    expect(handled(rotated, key("W", false)) && handled(rotated, key("A", true))
        && rotated.step(10.0F).has_value(), "yaw 90 long left step");
    close(rotated.frame().target[2], 0.0F, "yaw 90 left clamps to source min_y");
}

void test_zoom_yaw_and_reset() {
    Space space;
    activate(space, read("space-map-camera.xml"));
    if (!space.ok) return;
    auto& bridge = space.bridge;
    const auto initial = bridge.frame();
    expect(handled(bridge, wheel(true)) && handled(bridge, wheel(true)) && bridge.step(0.0F).has_value(),
           "two wheel-down detents step");
    close(bridge.controller().state().zoom, 1.0F, "wheel down reaches the far endpoint");
    close(bridge.controller().state().distance, 1200.0F, "far endpoint distance");
    expect(bridge.frame().target == initial.target && bridge.controller().yaw_degrees() == 0.0F,
           "wheel changes only zoom");
    expect(handled(bridge, wheel(true)) && bridge.step(0.0F).has_value(), "further wheel-down steps");
    close(bridge.controller().state().zoom, 1.0F, "zoom clamps at the far endpoint");
    for (int index = 0; index < 4; ++index) expect(handled(bridge, wheel(false)), "wheel-up routes");
    expect(bridge.step(0.0F).has_value(), "four wheel-up detents step");
    close(bridge.controller().state().zoom, 0.0F, "wheel up reaches the near endpoint");
    close(bridge.controller().state().distance, 400.0F, "near endpoint distance");
    expect(handled(bridge, wheel(false)) && bridge.step(0.0F).has_value(), "further wheel-up steps");
    close(bridge.controller().state().zoom, 0.0F, "zoom clamps at the near endpoint");
    expect(handled(bridge, wheel(true)) && handled(bridge, wheel(true)) && bridge.step(0.0F).has_value(),
           "back to the midpoint");
    close(bridge.controller().state().distance, 800.0F, "midpoint distance");

    const float zoom = bridge.controller().state().zoom;
    expect(handled(bridge, middle(true)) && handled(bridge, motion(640.0F, 360.0F, -64.0F))
        && bridge.step(0.0F).has_value(), "grabbed plain drag steps");
    // FoC: a plain middle drag translates 4 x the width fraction x 100 mouse
    // units, each 1/100 of the live distance: 4 * -64/1280 * 800.
    close(bridge.frame().target[0], initial.target[0] - 160.0F, "a plain drag translates by the live distance");
    expect(bridge.controller().yaw_degrees() == 0.0F && bridge.controller().state().zoom == zoom,
           "a plain drag does not turn or zoom");
    expect(handled(bridge, middle(false)) && handled(bridge, motion(640.0F, 360.0F, 8.0F))
        && bridge.step(0.0F).has_value(), "ungrabbed drag steps");
    close(bridge.frame().target[0], initial.target[0] - 160.0F, "released grab stops translating");
    expect(bridge.view_resets() == 0U, "a drag beyond 1% of the screen is no click");
    expect(handled(bridge, ctrl(middle(true))) && handled(bridge, ctrl(motion(640.0F, 360.0F, 128.0F)))
        && bridge.step(0.0F).has_value(), "Ctrl drag steps");
    // 128 px of 1280 is 10 mouse units; the table reverses them, 1.25 degrees each.
    close(bridge.controller().yaw_degrees(), -12.5F, "a Ctrl drag turns Yaw_Per_Mouse_Unit per mouse unit");
    close(bridge.frame().target[0], initial.target[0] - 160.0F, "a Ctrl drag does not translate");
    expect(handled(bridge, ctrl(middle(false))) && bridge.step(0.0F).has_value(), "Ctrl release steps");

    expect(handled(bridge, key("D", true)) && bridge.step(0.05F).has_value(), "pan before reset");
    expect(handled(bridge, key("D", false)) && handled(bridge, key("Home", true))
        && bridge.step(0.0F).has_value(), "reset steps");
    expect(bridge.resets() == 1U && bridge.frame() == initial, "reset restores the authored pose exactly");
}

void test_bridge_uses_motion_constants() {
    const auto config = space_config(read("space-map-camera.xml"));
    auto loaded = load(camera::Mode::space);
    expect(config.has_value() && loaded.has_value(), "motion fixture loads");
    if (!config || !loaded) return;
    loaded->constants.distance_smooth_time = 0.1F;
    loaded->constants.scroll_acceleration_factor = 0.1F;
    loaded->constants.scroll_deceleration_factor = 0.2F;
    MapCameraBridge bridge(true, input::Context::space);
    expect(bridge.activate(*config, loaded->constants,
        read("space-map-camera-bindings.json"), 1280, 720, std::nullopt).has_value(),
        "motion bridge activates");
    if (!bridge.active()) return;
    expect(handled(bridge, wheel(true)) && bridge.step(0.1F).has_value(),
        "wheel reaches the bounded controller");
    close(bridge.controller().target_zoom(), 0.75F, "bridge records the wheel target");
    close(bridge.controller().state().distance,
        800.0F + 200.0F * (1.0F - std::exp(-1.0F)),
        "bridge eases distance using Distance_Smooth_Time", 0.01F);
    const float before_pan = bridge.frame().target[0];
    expect(handled(bridge, key("D", true)) && bridge.step(0.1F).has_value(),
        "held pan accelerates in the bridge");
    const float after_pan = bridge.frame().target[0];
    expect(after_pan > before_pan
        && after_pan - before_pan < bridge.controller().state().pan_speed * 0.1F,
        "bridge pan starts below full speed");
    expect(handled(bridge, key("D", false)) && bridge.step(0.1F).has_value(),
        "released pan decelerates in the bridge");
    expect(bridge.frame().target[0] > after_pan, "bridge pan coasts on release");
}

// One physical Alt drag must travel the same ground at any frame rate: the
// pointer delta is a displacement, not a pan velocity integrated over time.
void test_alt_drag_is_frame_rate_independent() {
    const auto config = space_config(read("space-map-camera.xml"));
    auto loaded = load(camera::Mode::space);
    expect(config.has_value() && loaded.has_value(), "drag fixture loads");
    if (!config || !loaded) return;
    // FoC-like easing, so a velocity path would visibly ramp and coast.
    loaded->constants.distance_smooth_time = 0.1F;
    loaded->constants.scroll_acceleration_factor = 0.08F;
    loaded->constants.scroll_deceleration_factor = 1.5F;
    // 150 px/s right and 60 px/s down for one second in a 720 px viewport.
    constexpr float speed_x = 150.0F;
    constexpr float speed_y = 60.0F;
    // #515: Fov_Default 50 drawn as its 4:3 vertical angle (tangent 3/4 of the XML one's).
    const float screen = 2.0F * 800.0F * 0.75F * std::tan(25.0F * std::numbers::pi_v<float> / 180.0F);
    const float expected_x = 160.0F + speed_x / 720.0F * screen;
    const float expected_z = -400.0F + speed_y / 720.0F * screen;
    for (const int rate : {30, 60, 144}) {
        MapCameraBridge bridge(true, input::Context::space);
        expect(bridge.activate(*config, loaded->constants,
            read("space-map-camera-bindings.json"), 1280, 720, std::nullopt).has_value(),
            "drag bridge activates");
        if (!bridge.active()) return;
        const float seconds = 1.0F / static_cast<float>(rate);
        bool stepped = true;
        for (int frame = 0; frame < rate; ++frame) {
            input::RawEvent drag = motion(640.0F, 360.0F, speed_x * seconds);
            drag.relative_y = speed_y * seconds;
            drag.modifiers = input::modifier::alt;
            stepped = stepped && handled(bridge, drag) && bridge.step(seconds).has_value();
        }
        expect(stepped, "Alt drag frames step");
        close(bridge.frame().target[0], expected_x, "Alt drag x travel is frame-rate independent", 0.05F);
        close(bridge.frame().target[2], expected_z, "Alt drag down pans backward (+Z) at any rate", 0.05F);
        const auto released = bridge.frame();
        expect(bridge.step(0.5F).has_value() && bridge.frame() == released,
            "an Alt drag leaves no pan velocity to coast");
    }
}

// The synthetic land config over a ramp whose height equals source X.
// The committed tables carry the owner's feel check (land pan at half the XML
// speed) and the FoC middle-button law (RO-7, #295): a plain middle drag
// translates at x4, Ctrl + middle drag rotates and tilts at x1 (x reversed to
// the owner's direction check, y so that screen-up is positive, as in FoC),
// and a middle click resets the view.
void test_committed_tables_carry_the_feel_check() {
    for (const auto& [name, context, pan_scale] : {
             std::tuple{"map-camera-bindings.json", input::Context::land, 0.5F},
             std::tuple{"space-map-camera-bindings.json", input::Context::space, 1.0F},
             std::tuple{"space-live-camera-bindings.json", input::Context::space, 1.0F}}) {
        auto table = input::parse_binding_table(read(name));
        expect(table.has_value(), std::string(name) + " parses");
        if (!table) continue;
        expect(table.value().pan_speed_scale == pan_scale,
               std::string(name) + ": land pan is halved, space pan is unchanged");
        expect(table.value().click_reset, std::string(name) + ": a middle click resets the view");
        expect(table.value().screen_mouse_units, std::string(name) + ": drags are in FoC mouse units");
        std::size_t rotates{};
        std::size_t grabs{};
        bool orbit_pitch{};
        bool translate_x{};
        bool translate_y{};
        for (const auto& binding : table.value().bindings) {
            expect(binding.context == context, std::string(name) + " stays in its context");
            if (binding.action == input::Action::rotate) {
                ++rotates;
                expect(binding.scale == -1.0F && binding.modifiers == input::modifier::ctrl,
                       std::string(name) + ": only Ctrl rotates, at x1, reversed");
            }
            if (binding.action == input::Action::rotate_grab && binding.code == input::mouse_code::middle
                && (binding.modifiers == 0U || binding.modifiers == input::modifier::ctrl)) ++grabs;
            if (binding.action == input::Action::orbit_pitch) {
                orbit_pitch = binding.modifiers == input::modifier::ctrl && binding.scale == -1.0F
                    && binding.code == input::mouse_code::motion_y;
            }
            if (binding.action == input::Action::translate_x) {
                translate_x = binding.modifiers == 0U && binding.scale == 4.0F
                    && binding.code == input::mouse_code::motion_x;
            }
            if (binding.action == input::Action::translate_y) {
                translate_y = binding.modifiers == 0U && binding.scale == -4.0F
                    && binding.code == input::mouse_code::motion_y;
            }
        }
        expect(rotates == 1U && grabs == 2U && orbit_pitch && translate_x && translate_y,
               std::string(name) + " binds the FoC middle-button law");
    }
}

// The FoC mouse units belong to the map tables only (PI-9, #295). The host and
// free-fixture tables keep their feel: a 40 px middle drag is 40 rotate units
// at 1280 and at 1920 wide, as before the law.
void test_host_and_free_tables_keep_pixel_units() {
    const auto constants = load(camera::Mode::land);
    expect(constants.has_value(), "land constants load for the host tables");
    if (!constants) return;
    for (const auto name : {"camera-bindings.json", "map-camera-free-bindings.json"}) {
        const auto table = input::parse_binding_table(read(name));
        expect(table.has_value() && !table.value().screen_mouse_units && !table.value().click_reset,
               std::string(name) + " keeps pixel units and no click reset");
        for (const float width : {1280.0F, 1920.0F}) {
            const std::string what = std::string(name) + " at " + std::to_string(static_cast<int>(width));
            input::Adapter adapter(input::Context::land, true);
            expect(adapter.activate(read(name)).has_value(), what + " activates");
            expect(adapter.set_viewport(width, width * 0.5625F).has_value(), what + " viewport");
            expect(adapter.handle(middle(true)).has_value()
                   && adapter.handle(motion(680.0F, 360.0F, 40.0F)).has_value(), what + " drag");
            const auto intent = adapter.take_step(constants->constants);
            expect(intent.has_value(), what + " steps");
            if (intent) close(intent.value().rotate_units, 40.0F, what + ": 40 px rotate 40 units");
        }
    }
}
void test_ctrl_rotate_is_resolution_independent() {
    for (const auto& [width, height] : {std::pair{1280.0F, 720.0F}, std::pair{1920.0F, 1080.0F}}) {
        const std::string size = std::to_string(static_cast<int>(width)) + "x"
            + std::to_string(static_cast<int>(height));
        Space space;
        activate(space, read("space-map-camera.xml"));
        if (!space.ok) return;
        auto& bridge = space.bridge;
        expect(bridge.set_viewport(width, height).has_value(), "viewport " + size);
        const float yaw = bridge.controller().yaw_degrees();
        expect(handled(bridge, ctrl(middle(true))) && handled(bridge, ctrl(pointer(width, 0.0F)))
               && bridge.step(0.0F).has_value(), "full-width Ctrl drag " + size);
        close(bridge.controller().yaw_degrees(), yaw - 125.0F,
              "a full-width Ctrl drag turns 100 x Yaw_Per_Mouse_Unit " + size, 1e-2F);
        expect(handled(bridge, ctrl(pointer(0.0F, 0.2F * height))) && bridge.step(0.0F).has_value(),
               "a fifth of the height down " + size);
        close(bridge.controller().state().pitch_degrees, 45.0F + 25.0F,
              "20 units down tilt 20 x 1.25 toward overhead " + size);
        expect(handled(bridge, ctrl(middle(false))) && bridge.step(0.0F).has_value(), "release " + size);
        const float distance = bridge.controller().state().distance;
        expect(handled(bridge, middle(true)) && handled(bridge, pointer(-0.1F * width, 0.0F))
               && bridge.step(0.0F).has_value(), "a tenth of the width, plain " + size);
        // A tenth of the width is 10 units x4 = 40, each 1/100 of the distance,
        // along the turned screen axis (unclamped inside the fixture bounds).
        const auto& entry = bridge.trace().back();
        close(std::hypot(entry.target_after[0] - entry.target_before[0],
                         entry.target_after[2] - entry.target_before[2]),
              0.4F * distance, "a plain drag moves 0.4 x the distance " + size, 1e-1F);
        expect(handled(bridge, middle(false)) && bridge.step(0.0F).has_value(), "plain release " + size);
    }
}

void test_land_pan_is_halved_space_is_not() {
    Land land;
    activate_land(land, "");
    Space space;
    activate(space, read("space-map-camera.xml"));
    if (!land.ok || !space.ok) return;
    const float land_x = land.bridge.frame().target[0];
    const float space_x = space.bridge.frame().target[0];
    expect(handled(land.bridge, key("D", true)) && land.bridge.step(0.05F).has_value()
           && handled(space.bridge, key("D", true)) && space.bridge.step(0.05F).has_value(),
           "held pan steps on land and in space");
    // Both synthetic modes pan 1500 units/s at zoom 0.5 from the XML.
    close(land.bridge.controller().state().pan_speed, 1500.0F, "land XML pan speed", 1e-2F);
    close(land.bridge.frame().target[0] - land_x, 0.5F * 1500.0F * 0.05F, "land pans at half speed", 1e-2F);
    close(space.bridge.frame().target[0] - space_x, 1500.0F * 0.05F, "space pans at full speed", 1e-2F);
    // Edge pan shares the halved speed; the Alt drag keeps its screen scale.
    expect(handled(land.bridge, key("D", false)) && handled(land.bridge, motion(1279.0F, 360.0F, 0.0F))
           && land.bridge.step(0.0F).has_value(), "pointer reaches the right edge");
    const float edge_x = land.bridge.frame().target[0];
    expect(land.bridge.step(0.05F).has_value(), "edge pan steps");
    close(land.bridge.frame().target[0] - edge_x, 0.5F * 1500.0F * 0.05F, "edge pan is halved too", 1e-2F);
    land.bridge.pointer_left();
    input::RawEvent drag = pointer(72.0F, 0.0F);
    drag.modifiers = input::modifier::alt;
    const float before_drag = land.bridge.frame().target[0];
    expect(handled(land.bridge, drag) && land.bridge.step(0.0F).has_value(), "Alt drag steps");
    const float screen = 2.0F * 300.0F * 0.75F * std::tan(25.0F * std::numbers::pi_v<float> / 180.0F);
    close(land.bridge.frame().target[0] - before_drag, 0.1F * screen,
          "the Alt drag is not scaled by the land pan scale", 1e-2F);
}

// Ctrl + middle drag (FoC law): yaw at Yaw_Per_Mouse_Unit per mouse unit;
// pitch at Pitch_Per_Mouse_Unit. FoC's land rate is 0; the project tilts land
// at FoC space's -1.5 instead (#348 owner deviation), clamped to the land range.
// The tilt is kept after release, shifted by later zoom, cleared by reset; space
// clamps it to the XML Pitch_Min..Pitch_Max.
void test_plain_middle_drag_translates() {
    Land land;
    activate_land(land, "");
    if (land.ok) {
        const auto before = land.bridge.frame();
        const float distance = land.bridge.controller().state().distance;
        expect(handled(land.bridge, middle(true)) && handled(land.bridge, pointer(-64.0F, 0.0F))
               && land.bridge.step(0.0F).has_value(), "land plain middle drag steps");
        close(land.bridge.frame().target[0], before.target[0] - 0.2F * distance,
              "land: 64 of 1280 px x4 is 20 units, a fifth of the distance", 1e-2F);
        close(land.bridge.controller().yaw_degrees(), 0.0F, "land plain drag does not turn");
    }
    Space space;
    activate(space, read("space-map-camera.xml"));
    if (space.ok) {
        const auto before = space.bridge.frame();
        expect(handled(space.bridge, middle(true)) && handled(space.bridge, pointer(0.0F, 36.0F))
               && space.bridge.step(0.0F).has_value(), "space plain middle drag steps");
        // 36 of 720 px down x4 is 20 units back (+Z at yaw 0) of the 800 distance.
        close(space.bridge.frame().target[2], before.target[2] + 160.0F, "space plain drag down moves back");
        close(space.bridge.controller().state().pitch_degrees, 45.0F, "space plain drag does not tilt");
    }
}

// FoC: a middle click resets zoom, yaw and pitch around the current target;
// a Ctrl click does not; Home still restores the authored pose.
void test_middle_click_resets_the_view() {
    Space space;
    activate(space, read("space-map-camera.xml"));
    if (!space.ok) return;
    auto& bridge = space.bridge;
    expect(handled(bridge, key("A", true)) && bridge.step(0.05F).has_value()
           && handled(bridge, key("A", false)) && bridge.step(0.0F).has_value(), "pan away first");
    expect(handled(bridge, wheel(true)) && handled(bridge, ctrl(middle(true)))
           && handled(bridge, ctrl(pointer(128.0F, -72.0F))) && bridge.step(0.0F).has_value(),
           "zoom, turn and tilt");
    expect(handled(bridge, ctrl(middle(false))) && bridge.step(0.0F).has_value(), "Ctrl release");
    const auto moved = bridge.frame();
    expect(bridge.controller().yaw_degrees() != 0.0F && bridge.controller().orbit_pitch_offset() != 0.0F
           && bridge.view_resets() == 0U, "the view is changed and nothing reset");
    expect(handled(bridge, ctrl(middle(true))) && handled(bridge, ctrl(middle(false)))
           && bridge.step(0.0F).has_value(), "Ctrl click");
    expect(bridge.view_resets() == 0U && bridge.frame() == moved, "a Ctrl click keeps the view");
    expect(handled(bridge, middle(true)) && handled(bridge, middle(false))
           && bridge.step(0.0F).has_value(), "middle click");
    expect(bridge.view_resets() == 1U && bridge.resets() == 0U, "a middle click is a view reset");
    close(bridge.controller().yaw_degrees(), bridge.config().yaw_degrees, "the click resets yaw");
    close(bridge.controller().state().zoom, bridge.config().zoom, "the click resets zoom");
    close(bridge.controller().state().pitch_degrees, 45.0F, "the click resets pitch to the default");
    close(bridge.controller().orbit_pitch_offset(), 0.0F, "the click clears the orbit");
    close(bridge.frame().target[0], moved.target[0], "the click keeps the target X");
    close(bridge.frame().target[2], moved.target[2], "the click keeps the target Z");
    const auto& entry = bridge.trace().back();
    expect(entry.view_resets == 1U && entry.resets == 0U, "the trace records the view reset");
}

// An orbit never takes the land eye below the terrain: the eye floor still holds.
namespace {

// FoC's effective Space_Mode and scroll constants (retail tacticalcameras.xml and
// gameconstants.xml, plan/inventories/camera-constants.json), trimmed to the tags the
// loader reads, so the committed Coruscant overrides are checked against the real range.
constexpr std::string_view foc_space_xml = R"XML(<?xml version="1.0" encoding="utf-8"?>
<TacticalCameras>
  <TacticalCamera Name="Space_Mode">
    <Distance_Min>200.0</Distance_Min>
    <Distance_Max>1900.0</Distance_Max>
    <Distance_Default>1000.0</Distance_Default>
    <Distance_Per_Mouse_Unit>500.0</Distance_Per_Mouse_Unit>
    <Distance_Smooth_Time>0.1</Distance_Smooth_Time>
    <Pitch_Min>-10.0</Pitch_Min>
    <Pitch_Max>85.0</Pitch_Max>
    <Pitch_Default>50</Pitch_Default>
    <Pitch_Per_Mouse_Unit>-1.5</Pitch_Per_Mouse_Unit>
    <Pitch_Per_Zoom_Unit>0.0</Pitch_Per_Zoom_Unit>
    <Pitch_When_Zoomed_In>50</Pitch_When_Zoomed_In>
    <Pitch_Zoom_Begin_Fraction>-1.0</Pitch_Zoom_Begin_Fraction>
    <Yaw_Min>-1000</Yaw_Min>
    <Yaw_Max>1000</Yaw_Max>
    <Yaw_Default>0.0</Yaw_Default>
    <Yaw_Per_Mouse_Unit>1.5</Yaw_Per_Mouse_Unit>
    <Fov_Min>55.0</Fov_Min>
    <Fov_Max>55.0</Fov_Max>
    <Fov_Default>55.0</Fov_Default>
    <Fov_Per_Mouse_Unit>0.0</Fov_Per_Mouse_Unit>
    <Near_Clip>10.0</Near_Clip>
    <Far_Clip>7000.0</Far_Clip>
  </TacticalCamera>
</TacticalCameras>
)XML";

constexpr std::string_view foc_scroll_xml = R"XML(<?xml version="1.0" encoding="utf-8"?>
<Game_Constants>
  <Tactical_Min_Scroll_Speed>1000</Tactical_Min_Scroll_Speed>
  <Tactical_Max_Scroll_Speed>4000</Tactical_Max_Scroll_Speed>
  <Tactical_Edge_Scroll_Region>2</Tactical_Edge_Scroll_Region>
  <Tactical_Offscreen_Scroll_Region>50</Tactical_Offscreen_Scroll_Region>
  <Push_Scroll_Speed_Modifier>2.0</Push_Scroll_Speed_Modifier>
  <Scroll_Acceleration_Factor>0.08</Scroll_Acceleration_Factor>
  <Scroll_Deceleration_Factor>1.5</Scroll_Deceleration_Factor>
</Game_Constants>
)XML";

constexpr std::string_view coruscant_map = "data/art/maps/_mp_space_coruscant.ted";
constexpr std::string_view coruscant_sha =
    "91a1fd50ae8ac521e43773f60a3743d64eceb3a00736d80f0ae2234d95108286";

[[nodiscard]] std::optional<camera::LoadedConstants> foc_space() {
    auto loaded = camera::load_constants({bytes(foc_space_xml), "data/xml/tacticalcameras.xml", "foc-sha"},
        {bytes(foc_scroll_xml), "data/xml/gameconstants.xml", "foc-scroll-sha"}, camera::Mode::space);
    expect(loaded.has_value(), "FoC Space_Mode constants load");
    if (!loaded) return std::nullopt;
    return std::move(loaded.value());
}

[[nodiscard]] float eye_distance(const camera::TacticalFrame& frame) {
    const float dx = frame.eye[0] - frame.target[0];
    const float dy = frame.eye[1] - frame.target[1];
    const float dz = frame.eye[2] - frame.target[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

// #390 (owner): both committed space cameras zoom in to 100 instead of FoC's
// Space_Mode Distance_Min 200, keep 1900 and the 1200 opening, and keep FoC's
// pan-speed line; the Ctrl-drag tilt goes down to -60 instead of FoC's Pitch_Min -10,
// and a plain middle drag never changes the depth.
void test_space_close_zoom_and_depth_floor() {
    const auto foc = foc_space();
    if (!foc) return;
    const float degrees = std::numbers::pi_v<float> / 180.0F;
    for (const auto& [name, bindings] : {
             std::pair{std::string_view("coruscant-space-map-camera.xml"),
                       std::string_view("space-map-camera-bindings.json")},
             std::pair{std::string_view("coruscant-live-session-camera.xml"),
                       std::string_view("space-live-camera-bindings.json")}}) {
        const std::string label(name);
        auto parsed = eawr::viewer::parse_map_camera_config(read(name), coruscant_map, coruscant_sha,
                                                            camera::Mode::space);
        expect(parsed.has_value(), label + " parses");
        if (!parsed) continue;
        const MapCameraConfig& config = parsed.value();
        expect(config.constant_overrides.size() == 3
               && config.overview_clicks == 5U
               && config.constant_overrides[0].tag == "Distance_Min"
               && config.constant_overrides[0].value == "100"
               && config.constant_overrides[1].tag == "Tactical_Min_Scroll_Speed"
               && config.constant_overrides[2].tag == "Pitch_Min"
               && config.constant_overrides[2].value == "-60"
               && config.overrides_source_id == "space-camera-owner-deviations",
               label + " carries the #390 and #413 overrides");
        auto resolved = eawr::viewer::resolve_map_constants(config, *foc, name, config_sha);
        expect(resolved.has_value(), label + " resolves over FoC Space_Mode");
        if (!resolved) continue;
        const camera::Constants& constants = resolved.value().constants;
        close(constants.distance_min, 100.0F, label + ": minimum distance 100");
        close(constants.distance_max, 1900.0F, label + ": FoC maximum kept");
        close(constants.pitch_min, -60.0F, label + ": tilt floor -60");
        close(constants.pitch_max, 85.0F, label + ": FoC Pitch_Max kept");
        // Pan speed keeps FoC's line (1000 at 200, 4000 at 1900) and extends it below 200.
        const auto speed_at = [](const camera::Constants& values, const float distance) {
            auto solved = camera::solve(values,
                (distance - values.distance_min) / (values.distance_max - values.distance_min));
            return solved ? solved.value().pan_speed : std::numeric_limits<float>::quiet_NaN();
        };
        for (const float distance : {200.0F, 1000.0F, 1200.0F, 1900.0F}) {
            close(speed_at(constants, distance), speed_at(foc->constants, distance),
                  label + ": FoC pan speed kept", 0.05F);
        }
        close(speed_at(constants, 100.0F), 1000.0F - 3000.0F / 17.0F, label + ": FoC's pan line at 100", 0.05F);

        MapCameraBridge bridge{true, input::Context::space};
        auto active = bridge.activate(config, constants, read(bindings), 1280, 720, std::nullopt);
        expect(active.has_value(), label + " activates");
        if (!active) continue;
        close(bridge.controller().state().distance, 1200.0F, label + ": still opens at 1200", 0.05F);
        close(bridge.controller().state().pitch_degrees, 50.0F, label + ": Space_Mode pitch 50");

        // Wheel all the way in: the distance stops at 100, the pitch stays 50 (Space_Mode has
        // no zoom-linked pitch: Pitch_Per_Zoom_Unit 0, Pitch_Zoom_Begin_Fraction -1).
        for (int detent = 0; detent < 6; ++detent) {
            expect(handled(bridge, wheel(false)) && bridge.step(0.0F).has_value(), label + ": wheel in");
        }
        expect(bridge.step(5.0F).has_value(), label + ": zoom settles");
        close(bridge.controller().state().distance, 100.0F, label + ": zooms in to 100");
        close(bridge.controller().state().pitch_degrees, 50.0F, label + ": pitch unchanged at 100");
        close(eye_distance(bridge.frame()), 100.0F, label + ": the eye is 100 from the target");
        close(bridge.frame().near_plane, 10.0F, label + ": FoC Near_Clip kept");
        const auto closest = bridge.frame();

        // Plain middle drag translates in the battle plane: no tilt, no depth change.
        expect(handled(bridge, middle(true)) && handled(bridge, pointer(0.0F, 360.0F))
               && bridge.step(0.0F).has_value(), label + ": plain middle drag down");
        close(bridge.controller().state().pitch_degrees, 50.0F, label + ": plain drag does not tilt");
        close(bridge.frame().target[1], closest.target[1], label + ": plain drag keeps the plane");
        close(bridge.frame().eye[1], closest.eye[1], label + ": plain drag keeps the eye height");
        // 360 of 720 px x4 is 200 units, each 1/100 of the 100 distance: 200 back (+Z at yaw 0).
        close(bridge.frame().target[2], closest.target[2] + 200.0F, label + ": plain drag moves back", 0.05F);
        expect(handled(bridge, middle(false)) && bridge.step(0.0F).has_value(), label + ": plain release");

        // Ctrl + middle drag up: FoC would stop at Space_Mode Pitch_Min -10, the floor the owner
        // reported; the project floor is -60 (#390). A full-height drag down stops at Pitch_Max 85.
        expect(handled(bridge, ctrl(middle(true))) && handled(bridge, ctrl(pointer(0.0F, -480.0F)))
               && bridge.step(0.0F).has_value(), label + ": Ctrl drag up 66.7 units");
        close(bridge.controller().state().pitch_degrees, -50.0F,
              label + ": the tilt passes FoC's -10 without stopping", 0.01F);
        expect(handled(bridge, ctrl(pointer(0.0F, -1440.0F))) && bridge.step(0.0F).has_value(),
               label + ": Ctrl drag up past the floor");
        close(bridge.controller().state().pitch_degrees, -60.0F, label + ": tilt floor is the project -60");
        close(bridge.frame().eye[1] - bridge.frame().target[1], -100.0F * std::sin(60.0F * degrees),
              label + ": lowest eye is 100 sin 60 below the plane", 0.01F);
        expect(bridge.orbit_pitch_range() == camera::OrbitPitchRange{-60.0F, 85.0F},
               label + ": space orbit range is -60..85");
        expect(handled(bridge, ctrl(pointer(0.0F, 1440.0F))) && bridge.step(0.0F).has_value(),
               label + ": Ctrl drag down");
        close(bridge.controller().state().pitch_degrees, 85.0F, label + ": tilt ceiling is FoC's Pitch_Max");
        expect(handled(bridge, ctrl(middle(false))), label + ": Ctrl release");

        // The middle click still resets to the 1200 opening around the current target.
        expect(handled(bridge, middle(true)) && handled(bridge, middle(false)) && bridge.step(0.0F).has_value(),
               label + ": middle click");
        close(bridge.controller().state().distance, 1200.0F, label + ": click reset returns to 1200", 0.05F);
        close(bridge.controller().state().pitch_degrees, 50.0F, label + ": click reset clears the tilt");
    }
}


} // namespace eawr_map_camera_test
