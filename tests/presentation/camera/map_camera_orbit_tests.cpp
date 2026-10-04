#include "map_camera_movement_support.hpp"

namespace eawr_map_camera_test {

void test_ctrl_orbit_through_the_bridge() {
    Land land;
    activate_land(land, "");
    if (!land.ok) return;
    auto& bridge = land.bridge;
    const auto initial = bridge.frame();
    close(bridge.controller().state().pitch_degrees, 40.0F, "land spline pitch at zoom 0.5");
    close(bridge.orbit_pitch_per_mouse_unit(), camera::land_project_pitch_per_mouse_unit,
          "a land XML rate of 0 tilts at the project rate");
    close(camera::land_project_pitch_per_mouse_unit, -1.5F, "the project land rate is FoC Space_Mode's");
    // 128 px right and 72 px down on 1280 x 720: 10 mouse units on each axis.
    expect(handled(bridge, ctrl(middle(true))) && handled(bridge, ctrl(pointer(128.0F, 72.0F)))
           && bridge.step(0.0F).has_value(), "Ctrl drag steps");
    close(bridge.controller().yaw_degrees(), -12.5F, "Ctrl drag right turns 10 units x 1.25, reversed");
    close(bridge.controller().state().pitch_degrees, 55.0F, "land Ctrl drag down tilts 10 units x 1.5 toward overhead");
    close(bridge.controller().orbit_pitch_offset(), 15.0F, "land holds the tilt as an orbit offset");
    orbit_about_focus(initial, bridge.frame(), "land orbit keeps focus and radius");
    expect(handled(bridge, ctrl(pointer(0.0F, 720.0F))) && bridge.step(0.0F).has_value(), "long Ctrl drag down");
    close(bridge.controller().state().pitch_degrees, camera::land_orbit_pitch_range.max_degrees,
          "a full-height land drag down stops at the land maximum");
    expect(handled(bridge, ctrl(pointer(0.0F, -1440.0F))) && bridge.step(0.0F).has_value(), "long Ctrl drag up");
    close(bridge.controller().state().pitch_degrees, camera::land_orbit_pitch_range.min_degrees,
          "a land drag up stops at the land minimum, above the terrain plane");
    expect(bridge.frame().eye[1] > bridge.frame().target[1], "a land tilt never looks up from below");
    expect(handled(bridge, ctrl(pointer(0.0F, 288.0F))) && bridge.step(0.0F).has_value(), "Ctrl drag back down");
    close(bridge.controller().state().pitch_degrees, 65.0F, "motion back from the limit responds at once");
    expect(handled(bridge, ctrl(middle(false))) && bridge.step(0.5F).has_value(), "release and idle");
    close(bridge.controller().state().pitch_degrees, 65.0F, "the land tilt is kept after release");
    expect(handled(bridge, middle(true)) && handled(bridge, middle(false)) && bridge.step(0.0F).has_value(),
           "land middle click");
    close(bridge.controller().state().pitch_degrees, 40.0F, "the click clears the land tilt");
    close(bridge.controller().orbit_pitch_offset(), 0.0F, "the click holds no land orbit offset");
    expect(bridge.orbit_pitch_range() == camera::land_orbit_pitch_range, "land bridge reports its range");

    Space space;
    activate(space, read("space-map-camera.xml"));
    if (!space.ok) return;
    auto& orbiting = space.bridge;
    const auto space_initial = orbiting.frame();
    close(orbiting.controller().state().pitch_degrees, 45.0F, "space pitch default");
    // 288 px up is 40 mouse units: -1.25 x 40 = -50 degrees, under the plane.
    expect(handled(orbiting, ctrl(middle(true))) && handled(orbiting, ctrl(pointer(8.0F, -288.0F)))
           && orbiting.step(0.0F).has_value(), "space Ctrl drag up");
    orbit_about_focus(space_initial, orbiting.frame(), "space orbit keeps focus and radius");
    close(orbiting.controller().state().pitch_degrees, -5.0F, "space orbit passes under the plane");
    expect(orbiting.frame().eye[1] < orbiting.frame().target[1], "space eye looks up from below");
    expect(handled(orbiting, ctrl(middle(false))) && orbiting.step(0.5F).has_value(), "space release and idle");
    close(orbiting.controller().state().pitch_degrees, -5.0F, "the orbited pitch is kept after release");
    expect(handled(orbiting, key("Home", true)) && orbiting.step(0.0F).has_value(), "space reset");
    close(orbiting.controller().state().pitch_degrees, 45.0F, "reset clears the orbit");
    close(orbiting.controller().orbit_pitch_offset(), 0.0F, "reset holds no orbit offset");
    expect(handled(orbiting, key("Home", false)), "reset release");
    expect(handled(orbiting, ctrl(middle(true))) && handled(orbiting, ctrl(pointer(0.0F, -720.0F)))
           && orbiting.step(0.0F).has_value(), "full-height drag up");
    close(orbiting.controller().state().pitch_degrees, -20.0F, "space stops at the XML Pitch_Min");
    expect(handled(orbiting, ctrl(pointer(0.0F, 720.0F))) && orbiting.step(0.0F).has_value(), "all the way down");
    close(orbiting.controller().state().pitch_degrees, 80.0F, "space stops at the XML Pitch_Max");
    expect(orbiting.orbit_pitch_range() == camera::OrbitPitchRange{-20.0F, 80.0F},
           "space bridge reports the XML range");
    const auto& entry = orbiting.trace().back();
    close(entry.orbit_pitch_units, -100.0F, "the trace records the consumed mouse units");
    close(entry.pitch_before, -20.0F, "the trace records the pitch before");
    close(entry.pitch_after, 80.0F, "the trace records the pitch after");
    expect(handled(orbiting, ctrl(middle(false))), "space Ctrl release");
}

// FoC: a middle drag without Ctrl translates the target and never turns.
void test_ctrl_orbit_is_frame_rate_independent() {
    float first_yaw{};
    float first_pitch{};
    for (const int rate : {30, 60, 144}) {
        Space space;
        activate(space, read("space-map-camera.xml"));
        if (!space.ok) return;
        auto& bridge = space.bridge;
        const float seconds = 1.0F / static_cast<float>(rate);
        bool stepped = handled(bridge, ctrl(middle(true)));
        for (int frame = 0; frame < rate; ++frame) {
            stepped = stepped && handled(bridge, ctrl(pointer(40.0F * seconds, 30.0F * seconds)))
                && bridge.step(seconds).has_value();
        }
        expect(stepped, "Ctrl orbit frames step");
        if (rate == 30) {
            first_yaw = bridge.controller().yaw_degrees();
            first_pitch = bridge.controller().state().pitch_degrees;
            close(first_yaw, -3.125F * 1.25F, "40 of 1280 px right: 3.125 mouse units at 1.25, reversed");
            close(first_pitch, 45.0F + 1.25F * 30.0F * 100.0F / 720.0F,
                  "30 of 720 px down at Pitch_Per_Mouse_Unit -1.25 tilts toward overhead");
        } else {
            // Yaw wraps over the +/-1000 sentinel range, so its float step is coarser.
            close(bridge.controller().yaw_degrees(), first_yaw, "orbit yaw agrees at 30, 60 and 144 fps", 1e-2F);
            close(bridge.controller().state().pitch_degrees, first_pitch,
                  "orbit pitch agrees at 30, 60 and 144 fps");
        }
    }
}

// Focus loss and pointer exit end the orbit drag and drop its pending motion.
void test_orbit_cancellation_in_the_bridge() {
    // Space: the tilt runs at the XML rate itself, not the land project rate.
    Space space;
    activate(space, read("space-map-camera.xml"));
    if (!space.ok) return;
    auto& bridge = space.bridge;
    const float pitch = bridge.controller().state().pitch_degrees;
    expect(handled(bridge, ctrl(middle(true))) && handled(bridge, ctrl(pointer(0.0F, 8.0F))), "pending orbit");
    bridge.set_focus(false);
    bridge.set_focus(true);
    expect(bridge.step(0.0F).has_value() && bridge.controller().state().pitch_degrees == pitch,
           "focus loss drops the pending orbit");
    expect(handled(bridge, ctrl(pointer(0.0F, 8.0F))) && bridge.step(0.0F).has_value()
           && bridge.controller().state().pitch_degrees == pitch, "focus regain needs a fresh press");
    expect(handled(bridge, ctrl(middle(true))) && handled(bridge, ctrl(pointer(0.0F, 8.0F))),
           "fresh press and pending orbit");
    bridge.pointer_left();
    expect(bridge.step(0.0F).has_value() && bridge.controller().state().pitch_degrees == pitch,
           "pointer exit drops the pending orbit");
    expect(handled(bridge, ctrl(pointer(0.0F, 8.0F))) && bridge.step(0.0F).has_value()
           && bridge.controller().state().pitch_degrees == pitch, "pointer exit ends the orbit grab");
    expect(handled(bridge, ctrl(middle(false))) && handled(bridge, ctrl(middle(true)))
           && handled(bridge, ctrl(pointer(0.0F, 8.0F))) && bridge.step(0.0F).has_value(),
           "a fresh press after pointer exit");
    // 8 of 720 px down at Pitch_Per_Mouse_Unit -1.25.
    close(bridge.controller().state().pitch_degrees, pitch + 1.25F * 800.0F / 720.0F,
          "a fresh press orbits again");
}

namespace {

// The Ctrl key's own events as the platform sends them: the press and its auto-repeat carry the
// Ctrl bit, the release does not. The key has no v1 code, so no table can bind it by itself.
input::RawEvent ctrl_key(const bool pressed, const bool echo = false) {
    input::RawEvent event = key("Ctrl", pressed);
    event.echo = echo;
    event.modifiers = pressed ? input::modifier::ctrl : std::uint8_t{};
    return event;
}

float turn(const MapCameraBridge& bridge) { return bridge.controller().yaw_degrees(); }
float tilt(const MapCameraBridge& bridge) { return bridge.controller().state().pitch_degrees; }

// One hand on one committed table: Ctrl first, then the button (FoC's documented chord); the
// button first and Ctrl during the drag; a plain drag; a Ctrl click; a plain click.
void play_the_middle_button_law(MapCameraBridge& bridge, const std::string& table) {
    const float start_yaw = turn(bridge);
    const float start_pitch = tilt(bridge);
    const auto start = bridge.frame();
    expect(handled(bridge, ctrl_key(true)) && handled(bridge, ctrl_key(true, true))
               && handled(bridge, ctrl(middle(true))) && handled(bridge, ctrl(pointer(64.0F, 0.0F)))
               && handled(bridge, ctrl_key(true, true)) && handled(bridge, ctrl(pointer(64.0F, 0.0F)))
               && bridge.step(0.0F).has_value(),
           table + ": Ctrl key, Ctrl + middle drag right");
    // 128 of 1280 px is 10 mouse units at the fixture's Yaw_Per_Mouse_Unit 1.25, x reversed.
    close(turn(bridge), start_yaw - 12.5F, table + ": a Ctrl drag turns");
    close(bridge.frame().target[0], start.target[0], table + ": a Ctrl drag keeps the target", 1e-2F);
    close(bridge.frame().target[2], start.target[2], table + ": a Ctrl drag keeps the target", 1e-2F);
    const float pitch = tilt(bridge);
    expect(handled(bridge, ctrl(pointer(0.0F, -72.0F))) && bridge.step(0.0F).has_value(),
           table + ": Ctrl drag up");
    expect(std::abs(tilt(bridge) - pitch) > 1.0F, table + ": a vertical Ctrl drag tilts");
    close(turn(bridge), start_yaw - 12.5F, table + ": a vertical Ctrl drag does not turn");
    expect(handled(bridge, ctrl(middle(false))) && handled(bridge, ctrl_key(false))
               && bridge.step(0.0F).has_value() && bridge.view_resets() == 0U,
           table + ": releases, button before Ctrl, keep the view");

    const float yaw = turn(bridge);
    expect(handled(bridge, middle(true)) && handled(bridge, ctrl_key(true))
               && handled(bridge, ctrl(pointer(128.0F, 0.0F))) && handled(bridge, ctrl_key(false))
               && handled(bridge, middle(false)) && bridge.step(0.0F).has_value(),
           table + ": middle drag with Ctrl pressed after the button");
    close(turn(bridge), yaw - 12.5F, table + ": Ctrl pressed during the drag turns");

    const auto before_pan = bridge.frame();
    const float pan_yaw = turn(bridge);
    expect(handled(bridge, middle(true)) && handled(bridge, pointer(-64.0F, 0.0F))
               && handled(bridge, middle(false)) && bridge.step(0.0F).has_value(),
           table + ": plain middle drag");
    expect(bridge.frame().target != before_pan.target, table + ": a plain middle drag pans");
    close(turn(bridge), pan_yaw, table + ": a plain middle drag does not turn");

    expect(handled(bridge, ctrl_key(true)) && handled(bridge, ctrl(middle(true)))
               && handled(bridge, ctrl(middle(false))) && handled(bridge, ctrl_key(false))
               && bridge.step(0.0F).has_value() && bridge.view_resets() == 0U,
           table + ": a Ctrl click keeps the view");
    const auto target = bridge.frame().target;
    expect(handled(bridge, middle(true)) && handled(bridge, middle(false)) && bridge.step(0.0F).has_value()
               && bridge.view_resets() == 1U,
           table + ": a plain click resets");
    close(turn(bridge), start_yaw, table + ": the click restores the yaw");
    close(tilt(bridge), start_pitch, table + ": the click restores the pitch");
    close(bridge.frame().target[0], target[0], table + ": the click keeps the target", 1e-2F);
    close(bridge.frame().target[2], target[2], table + ": the click keeps the target", 1e-2F);
}

} // namespace

// The ctrl-mmb regression check (owner report 2026-09-27, #348/#342/#328): the middle-button law
// on every committed map table (land, space map, live battle), with the Ctrl key's own events
// around the chord in both hand orders, as the platform delivers them.
void test_middle_button_law_in_every_map_table() {
    Land land;
    activate_land(land, "");
    if (land.ok) play_the_middle_button_law(land.bridge, "land map-camera-bindings.json");
    for (const std::string_view table : {"space-map-camera-bindings.json", "space-live-camera-bindings.json"}) {
        const auto config = space_config(read("space-map-camera.xml"));
        const auto constants = load(camera::Mode::space);
        expect(config.has_value() && constants.has_value(), "space config and constants load");
        if (!config || !constants) return;
        MapCameraBridge bridge{true, input::Context::space};
        const auto active = bridge.activate(*config, constants->constants, read(table), 1280, 720, std::nullopt);
        expect(active.has_value(), std::string(table) + " activates");
        if (active) play_the_middle_button_law(bridge, std::string(table));
    }
}


} // namespace eawr_map_camera_test
