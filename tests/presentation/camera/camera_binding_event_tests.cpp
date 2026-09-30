#include "camera_binding_test_support.hpp"

namespace eawr_camera_binding_test {

void test_ambiguous_chords() {
    const std::string other_action = R"({"id": "land.left.d", "action": "pan_left",
        "context": "land", "device": "keyboard", "control": "d", "trigger": "held", "scale": 1})";
    expect_code(table_with(std::string(pan_right_d) + "," + other_action),
                input::diagnostic_codes::ambiguous_binding_chord,
                "one chord bound to two actions is rejected (case-insensitive key)");
    const std::string same_action = R"({"id": "land.right.d2", "action": "pan_right",
        "context": "land", "device": "keyboard", "control": "D", "trigger": "held", "scale": 1})";
    expect_code(table_with(std::string(pan_right_d) + "," + same_action),
                input::diagnostic_codes::ambiguous_binding_chord,
                "a duplicated chord is rejected even for the same action");
    const std::string other_context = R"({"id": "space.left.d", "action": "pan_left",
        "context": "space", "device": "keyboard", "control": "D", "trigger": "held", "scale": 1})";
    expect(input::parse_binding_table(table_with(std::string(pan_right_d) + "," + other_context))
               .has_value(),
           "the same chord in another context is not ambiguous");
    const std::string with_modifier = R"({"id": "land.left.ctrl-d", "action": "pan_left",
        "context": "land", "device": "keyboard", "control": "D", "modifiers": ["ctrl"],
        "trigger": "held", "scale": 1})";
    expect(input::parse_binding_table(table_with(std::string(pan_right_d) + "," + with_modifier))
               .has_value(),
           "a distinct modifier set is a distinct chord");
}

void test_activation_is_atomic() {
    input::Adapter adapter = ready_adapter();
    const input::BindingTable before = *adapter.table();
    send(adapter, key("D", true), "press D");
    const auto failed = adapter.activate(table_with(R"({"id": "x"})"));
    expect(!failed.has_value(), "invalid replacement table is rejected");
    expect(adapter.table() && *adapter.table() == before, "rejected table keeps the prior table");
    expect(adapter.is_held(input::Action::pan_right),
           "rejected activation does not disturb held input");
    expect(adapter.activate(test_table()).has_value(), "valid replacement activates");
    expect(adapter.held_actions().empty(), "successful activation cancels held input");

    input::Adapter empty(input::Context::land, true);
    expect(!empty.activate("{}").has_value(), "startup activation failure is reported");
    expect(!empty.has_table(), "startup failure leaves the adapter inactive");
    const auto routed = empty.handle(key("D", true));
    expect(!routed.has_value()
               && routed.error().code == input::diagnostic_codes::no_active_bindings,
           "an inactive adapter refuses routing with a diagnostic");
}

void test_held_repeat_and_pressed_triggers() {
    input::Adapter adapter = ready_adapter();
    send(adapter, key("D", true), "press D");
    send(adapter, key("Right", true), "press Right");
    expect(adapter.held_actions().size() == 1U
               && adapter.held_actions().front().control_count == 2U,
           "two controls hold one pan_right action");
    send(adapter, key("D", true, true), "echo D");
    expect(adapter.held_actions().front().control_count == 2U, "echo adds no contribution");
    send(adapter, key("D", false), "release D");
    expect_close(step(adapter).pan_x, 1.0F, "remaining control keeps pan_right");
    send(adapter, key("Right", false), "release Right");
    expect_close(step(adapter).pan_x, 0.0F, "releasing both stops panning");

    send(adapter, key("A", true), "press A");
    send(adapter, key("D", true), "press D");
    expect_close(step(adapter).pan_x, 0.0F, "opposing controls cancel");
    send(adapter, key("A", false), "release A");
    send(adapter, key("D", false), "release D");

    send(adapter, key("PageDown", true), "press PageDown");
    send(adapter, key("PageDown", true, true), "echo PageDown");
    expect_close(step(adapter).zoom_detents, 2.0F, "pressed zoom fires once despite repeat");
    expect_close(step(adapter).zoom_detents, 0.0F, "pressed zoom is consumed once");
    send(adapter, key("PageDown", false), "release PageDown");
    send(adapter, key("PageDown", true), "fresh PageDown");
    expect_close(step(adapter).zoom_detents, 2.0F, "a fresh press fires again");
    send(adapter, key("PageDown", false), "release PageDown");

    send(adapter, key("Home", true), "press Home");
    expect(step(adapter).reset_requests == 1U, "reset fires once");
    send(adapter, key("Home", false), "release Home");

    send(adapter, key("D", true, false, input::modifier::shift), "press shift+D");
    input::StepIntent pushed = step(adapter);
    expect(pushed.push_scroll && pushed.pan_x == 0.0F,
           "shift+D resolves to its own exact chord, not plain D");
    send(adapter, key("D", false), "release D without shift");
    expect(!step(adapter).push_scroll, "release after a modifier change releases the pressed chord");

    send(adapter, key("Q", true), "press unbound Q");
    expect(adapter.counters().ignored_unbound >= 1U, "unbound key is counted and ignored");
    input::RawEvent bad = key("D", true);
    bad.modifiers = 0x80U;
    const std::uint64_t delivered = adapter.counters().delivered;
    expect(!adapter.handle(bad).has_value(), "unknown modifier bits are rejected");
    expect(adapter.counters().delivered == delivered, "rejected event does not mutate counters");
}

void test_focus_loss_requires_fresh_press() {
    input::Adapter adapter = ready_adapter();
    send(adapter, key("D", true), "press D");
    send(adapter, button(input::mouse_code::middle, true), "grab");
    send(adapter, motion(640.0F, 360.0F, 10.0F, 0.0F), "drag");
    adapter.set_focus(false);
    input::StepIntent intent = step(adapter);
    expect(intent.empty(), "focus loss cancels held, drag and pending deltas");
    send(adapter, key("W", true), "press W while unfocused");
    send(adapter, wheel(input::mouse_code::wheel_up, 1.0F), "wheel while unfocused");
    expect(step(adapter).empty(), "unfocused input is ignored");
    send(adapter, key("D", false), "late release while unfocused");
    adapter.set_focus(true);
    expect(step(adapter).empty(), "regaining focus resurrects nothing");
    send(adapter, key("W", false), "stale release after regain");
    expect(step(adapter).empty(), "a stale release after regain is inert");
    send(adapter, key("D", true, true), "stale echo after regain");
    expect(step(adapter).empty(), "a stale echo does not press");
    send(adapter, key("D", true), "fresh press after regain");
    expect_close(step(adapter).pan_x, 1.0F, "a fresh press after regain pans");
}

void test_capture_lock_suppresses_everything() {
    input::Adapter adapter = ready_adapter();
    send(adapter, key("D", true), "press D");
    adapter.set_capture_locked(true);
    expect(step(adapter).empty(), "capture lock cancels held input");
    send(adapter, key("W", true), "press W while locked");
    send(adapter, key("Home", true), "reset while locked");
    send(adapter, key("PageDown", true), "zoom while locked");
    send(adapter, wheel(input::mouse_code::wheel_up, 3.0F), "wheel while locked");
    send(adapter, button(input::mouse_code::middle, true), "grab while locked");
    send(adapter, motion(0.0F, 0.0F, 50.0F, 0.0F), "motion to a corner while locked");
    adapter.set_focus(false);
    adapter.set_focus(true);
    expect(adapter.set_viewport(800.0F, 600.0F).has_value(), "resize while locked");
    expect(step(adapter).empty(), "every hostile event during capture lock is ignored");
    expect(!adapter.pointer_valid(), "no pointer sample survives capture lock");
    adapter.set_capture_locked(false);
    expect(step(adapter).empty(), "unlocking resurrects nothing");
    send(adapter, key("W", true), "fresh press after unlock");
    expect_close(step(adapter).pan_y, 1.0F, "fresh press after unlock pans forward");
}

void test_viewport_generations() {
    input::Adapter adapter = ready_adapter();
    const std::uint64_t generation = adapter.counters().viewport_generation;
    expect(!adapter.set_viewport(-1.0F, 720.0F).has_value(), "negative extent is rejected");
    expect(!adapter.set_viewport(std::numeric_limits<float>::quiet_NaN(), 720.0F).has_value(),
           "NaN extent is rejected");
    expect(adapter.counters().viewport_generation == generation,
           "rejected extents publish no generation");
    send(adapter, key("D", true), "press D");
    expect(adapter.set_viewport(0.0F, 0.0F).has_value(), "zero extent (minimised) publishes");
    expect(!adapter.eligible(), "zero extent suspends input");
    send(adapter, key("W", true), "press W while minimised");
    expect(step(adapter).empty(), "zero extent yields no movement");
    expect(adapter.set_viewport(1280.0F, 720.0F).has_value(), "restore publishes");
    expect(step(adapter).empty(), "restoring the viewport does not restore held movement");

    send(adapter, motion(1.0F, 360.0F, 0.0F, 0.0F), "pointer at left edge");
    expect_close(step(adapter).pan_x, -1.0F, "left edge pans left");
    send(adapter, motion(640.0F, 0.0F, 0.0F, 0.0F), "pointer at top edge");
    expect_close(step(adapter).pan_y, 1.0F, "top edge pans up the screen");
    expect(adapter.set_viewport(1024.0F, 768.0F).has_value(), "resize publishes");
    expect(step(adapter).empty(), "resize invalidates the pointer sample");
    send(adapter, motion(1023.0F, 400.0F, 0.0F, 0.0F), "pointer at right edge");
    expect_close(step(adapter).pan_x, 1.0F, "right edge pans right in the new generation");
    adapter.pointer_left();
    expect(step(adapter).empty(), "leaving the window stops edge scrolling");
}

void test_wheel_and_rotate_deltas() {
    input::Adapter adapter = ready_adapter();
    send(adapter, wheel(input::mouse_code::wheel_up, 0.0F), "wheel with no factor");
    send(adapter, wheel(input::mouse_code::wheel_up, 2.0F), "wheel with factor 2");
    send(adapter, wheel(input::mouse_code::wheel_down, 1.0F), "unbound wheel down");
    expect_close(step(adapter).zoom_detents, -3.0F, "wheel notches accumulate once with scale");
    expect_close(step(adapter).zoom_detents, 0.0F, "wheel deltas are consumed once");

    send(adapter, motion(640.0F, 360.0F, 20.0F, 5.0F), "motion without grab");
    expect_close(step(adapter).rotate_units, 0.0F, "motion without a grab does not rotate");
    send(adapter, button(input::mouse_code::middle, true), "grab");
    send(adapter, motion(650.0F, 360.0F, 20.0F, 5.0F), "drag");
    send(adapter, motion(660.0F, 360.0F, 4.0F, 5.0F), "drag");
    // Without screen_mouse_units (the host and free-fixture tables) a scaled
    // pixel is one unit: 24 px at scale 0.5.
    expect_close(step(adapter).rotate_units, 12.0F, "grabbed x motion rotates with scale");
    expect(adapter.set_viewport(1920.0F, 1080.0F).has_value(), "republish viewport");
    send(adapter, motion(650.0F, 360.0F, 20.0F, 0.0F), "drag after resize");
    expect_close(step(adapter).rotate_units, 0.0F, "resize cancels the drag anchor");
    send(adapter, button(input::mouse_code::middle, false), "release after resize");
    send(adapter, button(input::mouse_code::middle, true), "grab after resize");
    send(adapter, motion(650.0F, 360.0F, 24.0F, 5.0F), "drag at 1920 wide");
    expect_close(step(adapter).rotate_units, 12.0F, "pixel units ignore the viewport size");
}

void test_alt_mouse_pan_in_both_tactical_modes() {
    for (const input::Context context : {input::Context::land, input::Context::space}) {
        input::Adapter adapter(context, true);
        expect(adapter.activate(test_table()).has_value(), "Alt pan table activates");
        expect(adapter.set_viewport(1280.0F, 720.0F).has_value(), "Alt pan viewport publishes");
        auto ordinary = motion(640.0F, 360.0F, 20.0F, 10.0F);
        send(adapter, ordinary, "ordinary pointer motion");
        expect(step(adapter).empty(), "ordinary motion does not pan");
        auto alt = ordinary;
        alt.modifiers = input::modifier::alt;
        send(adapter, alt, "Alt pointer motion");
        auto moved = step(adapter);
        expect_close(moved.drag_x, 20.0F / 720.0F, "Alt horizontal motion drags right in viewport heights");
        expect_close(moved.drag_y, -10.0F / 720.0F, "Alt downward motion drags backward");
        expect(moved.pan_x == 0.0F && moved.pan_y == 0.0F, "Alt motion is not a pan velocity");
        expect(step(adapter).empty(), "Alt motion is consumed once");
        send(adapter, alt, "pending Alt motion before focus loss");
        adapter.set_focus(false);
        adapter.set_focus(true);
        expect(step(adapter).empty(), "focus loss cancels Alt motion");
    }
}

// The same physical Alt drag (pixels per second times seconds) travels the
// same ground at 30, 60 and 144 frames per second.
void test_alt_drag_travel_is_frame_rate_independent() {
    const camera::Constants constants = fixture_constants();
    auto initial = camera::solve(constants, 0.5F);
    expect(initial.has_value(), "drag fixture solves");
    const input::TacticalPose pose{initial.value(), {0.0F, 0.0F, 0.0F}};
    // #515: the frustum of the XML angle's 4:3 vertical angle (tangent 3/4 of the XML one's).
    const float screen = 2.0F * pose.state.distance * 0.75F
        * std::tan(pose.state.fov_degrees * std::numbers::pi_v<float> / 360.0F);
    constexpr float pixels_per_second = 400.0F;
    for (const int rate : {30, 60, 144}) {
        input::Adapter adapter = ready_adapter();
        input::TacticalPose current = pose;
        const float seconds = 1.0F / static_cast<float>(rate);
        for (int frame = 0; frame < rate; ++frame) {
            auto drag = motion(640.0F, 360.0F, pixels_per_second * seconds, 0.0F);
            drag.modifiers = input::modifier::alt;
            send(adapter, drag, "Alt drag frame");
            current = input::advance_pose(constants, current, step(adapter), 0.5F, seconds).value();
        }
        expect_close(current.target[0], pixels_per_second / 720.0F * screen,
                     "Alt drag travel matches one screen per viewport height", 1e-2F);
        expect_close(current.target[2], 0.0F, "horizontal Alt drag leaves Z");
    }
}

void test_context_switch_cancels() {
    input::Adapter adapter = ready_adapter();
    send(adapter, key("D", true), "press land D");
    adapter.set_context(input::Context::space);
    expect(step(adapter).empty(), "context switch cancels the old context");
    send(adapter, key("W", true), "W is unbound in the space test context");
    expect(step(adapter).empty(), "unbound space key does nothing");
    send(adapter, key("D", true), "space D");
    expect_close(step(adapter).pan_x, 1.0F, "space context uses its own binding");
    expect(input::context_for(camera::Mode::land) == input::Context::land
               && input::context_for(camera::Mode::space) == input::Context::space
               && !input::context_for(camera::Mode::unlocked).has_value(),
           "only land and space modes have an interactive context; free is toggled, not a mode");
}

input::Adapter free_adapter() {
    input::Adapter adapter(input::Context::land, true);
    expect(adapter.activate(v2_table()).has_value(), "v2 test table activates");
    expect(adapter.set_viewport(1280.0F, 720.0F).has_value(), "viewport publishes");
    return adapter;
}

void test_adapter_free_toggle_and_flight() {
    input::Adapter adapter = free_adapter();
    send(adapter, key("F", true), "press F in land");
    send(adapter, key("F", true, true), "echo F");
    input::StepIntent entering = step(adapter);
    expect(entering.free_toggle_requests == 1U, "one fresh toggle press requests one transition");
    expect(entering.pan_x == 0.0F && entering.free_move_z == 0.0F, "the toggle moves nothing");

    const std::uint64_t cancellations = adapter.counters().cancellations;
    send(adapter, key("D", true), "hold D before switching");
    adapter.set_context(input::Context::free);
    expect(adapter.counters().cancellations == cancellations + 1U,
           "entering free is a cancelling context change");
    expect(adapter.held_actions().empty(), "no tactical hold survives entering free");
    send(adapter, key("F", false), "late release of the entry toggle");
    send(adapter, key("D", false), "late release of the tactical pan");
    expect(step(adapter).empty(), "stale releases after entering free are inert");

    send(adapter, key("W", true), "press W in free");
    send(adapter, key("D", true), "press D in free");
    input::StepIntent flying = step(adapter);
    expect(flying.free_move_z == 1.0F && flying.free_move_x == 1.0F,
           "held free keys produce forward and strafe axes");
    expect(flying.pan_x == 0.0F && flying.pan_y == 0.0F, "free keys never pan");
    send(adapter, key("S", true), "press S in free");
    expect(step(adapter).free_move_z == 0.0F, "opposing free keys cancel");
    send(adapter, key("E", true), "press E");
    expect(step(adapter).free_move_y == 1.0F, "rise is +Y");
    send(adapter, key("Q", true), "press Q");
    expect(step(adapter).free_move_y == 0.0F, "rise and descend cancel");

    // Edge scrolling never applies in free flight, even at an edge.
    send(adapter, motion(0.0F, 0.0F, 0.0F, 0.0F), "pointer at the corner");
    const input::StepIntent cornered = step(adapter);
    expect(cornered.pan_x == 0.0F && cornered.pan_y == 0.0F, "free flight does not edge scroll");

    // Look needs the grab; its axes route to yaw and pitch with their scales.
    send(adapter, motion(640.0F, 360.0F, 10.0F, 4.0F), "motion without the look grab");
    expect(step(adapter).free_look_yaw_units == 0.0F, "motion without the grab does not look");
    send(adapter, button(input::mouse_code::right, true), "look grab");
    send(adapter, motion(650.0F, 364.0F, 10.0F, 4.0F), "look drag");
    send(adapter, motion(655.0F, 360.0F, 5.0F, -4.0F), "look drag");
    const input::StepIntent looked = step(adapter);
    expect_close(looked.free_look_yaw_units, 30.0F, "yaw look accumulates x with scale 2");
    expect_close(looked.free_look_pitch_units, 0.0F, "pitch look accumulates y with scale -1");
    expect(looked.rotate_units == 0.0F, "free look never feeds tactical rotate");
    send(adapter, motion(655.0F, 362.0F, 0.0F, 3.0F), "look drag down");
    expect_close(step(adapter).free_look_pitch_units, -3.0F, "pitch scale is applied");
    expect_close(step(adapter).free_look_pitch_units, 0.0F, "look deltas are consumed once");

    // Focus loss and resize cancel held flight, the look grab and pending look.
    send(adapter, motion(655.0F, 360.0F, 7.0F, 0.0F), "pending look before focus loss");
    adapter.set_focus(false);
    expect(step(adapter).empty(), "focus loss cancels free flight and pending look");
    adapter.set_focus(true);
    send(adapter, motion(655.0F, 360.0F, 7.0F, 0.0F), "motion after regain");
    expect(step(adapter).empty(), "regaining focus resurrects neither flight nor grab");
    send(adapter, key("W", true), "fresh W after regain");
    send(adapter, button(input::mouse_code::right, true), "fresh grab after regain");
    send(adapter, motion(655.0F, 360.0F, 1.0F, 0.0F), "fresh look");
    expect(adapter.set_viewport(1024.0F, 768.0F).has_value(), "resize while flying");
    expect(step(adapter).empty(), "resize cancels flight, grab and pending look");

    send(adapter, key("F", true), "press F in free");
    expect(step(adapter).free_toggle_requests == 1U, "the free-context toggle requests exit");
    adapter.set_context(input::Context::land);
    send(adapter, key("W", true), "W in land is unbound in this table");
    expect(step(adapter).empty(), "leaving free cancels and routes land bindings again");
    send(adapter, key("D", true), "fresh D in land");
    expect_close(step(adapter).pan_x, 1.0F, "land pan works after leaving free");
}

void test_adapter_capture_lock_ignores_free_toggle() {
    input::Adapter adapter = free_adapter();
    adapter.set_capture_locked(true);
    send(adapter, key("F", true), "toggle while locked");
    send(adapter, button(input::mouse_code::right, true), "grab while locked");
    send(adapter, motion(10.0F, 10.0F, 40.0F, 40.0F), "motion while locked");
    expect(step(adapter).empty(), "capture lock swallows the free toggle");
    expect(adapter.counters().routed == 0U, "nothing was routed under capture lock");
    adapter.set_capture_locked(false);
    expect(step(adapter).empty(), "unlocking does not replay the toggle");
}

void test_advance_pose() {
    const camera::Constants constants = fixture_constants();
    auto initial = camera::solve(constants, 0.5F);
    expect(initial.has_value(), "fixture solves");
    input::TacticalPose pose{initial.value(), {0.0F, 0.0F, 0.0F}};
    const float speed = pose.state.pan_speed;

    auto moved = input::advance_pose(constants, pose, {1.0F, 0.0F}, 0.5F, 0.5F);
    expect(moved.has_value(), "pan right advances");
    expect_close(moved.value().target[0], speed * 0.5F, "yaw 0 screen-right is +X");
    expect_close(moved.value().target[2], 0.0F, "yaw 0 screen-right leaves Z");
    auto forward = input::advance_pose(constants, pose, {0.0F, 1.0F}, 0.5F, 0.5F);
    expect_close(forward.value().target[2], -speed * 0.5F, "yaw 0 screen-up is -Z");

    input::TacticalPose turned = pose;
    turned.state.yaw_degrees = 90.0F;
    auto rotated_pan = input::advance_pose(constants, turned, {1.0F, 0.0F}, 0.5F, 1.0F);
    expect_close(rotated_pan.value().target[0], 0.0F, "yaw 90 screen-right leaves X", 1e-2F);
    expect_close(rotated_pan.value().target[2], -speed, "yaw 90 screen-right is -Z", 1e-1F);

    input::StepIntent zoom;
    zoom.zoom_detents = 1.0F;
    auto zoomed = input::advance_pose(constants, pose, zoom, 0.5F, 0.0F);
    expect(zoomed.value().state.zoom > pose.state.zoom, "positive detent zooms out");
    expect_close(zoomed.value().state.zoom, 0.5F + 80.0F / 400.0F, "detent uses Distance_Per_Mouse_Unit");

    input::StepIntent rotate;
    rotate.rotate_units = 8.0F;
    auto yawed = input::advance_pose(constants, pose, rotate, 0.5F, 0.0F);
    expect_close(yawed.value().state.yaw_degrees, 10.0F, "rotate uses Yaw_Per_Mouse_Unit");

    input::StepIntent reset;
    reset.reset_requests = 1U;
    auto restored = input::advance_pose(constants, yawed.value(), reset, 0.25F, 0.0F);
    expect_close(restored.value().state.zoom, 0.25F, "reset restores the default zoom");
    expect_close(restored.value().state.yaw_degrees, 0.0F, "reset restores Yaw_Default");

    expect(!input::advance_pose(constants, pose, {}, 0.5F, -1.0F).has_value(),
           "negative duration is rejected");
    expect(!input::advance_pose(constants, pose, {},
                                0.5F, std::numeric_limits<float>::infinity()).has_value(),
           "infinite duration is rejected");
    expect(input::advance_pose(constants, pose, {}, 0.5F, 1.0F).value() == pose,
           "an empty intent leaves the pose unchanged");

    // Replaying one scripted sequence twice is reproducible on one build.
    const auto replay = [&]() {
        input::Adapter adapter = ready_adapter();
        input::TacticalPose current = pose;
        send(adapter, key("D", true), "script D");
        send(adapter, key("W", true), "script W");
        for (int frame = 0; frame < 5; ++frame) {
            current = input::advance_pose(constants, current, step(adapter), 0.5F, 1.0F / 60.0F)
                          .value();
        }
        send(adapter, wheel(input::mouse_code::wheel_up, 1.0F), "script wheel");
        current = input::advance_pose(constants, current, step(adapter), 0.5F, 1.0F / 60.0F)
                      .value();
        return current;
    };
    expect(replay() == replay(), "identical scripted events reproduce the same pose");
}

// The committed map tables' middle-button chords in a compact test table:
// the FoC law (a plain drag translates at x4, Ctrl rotates and tilts at x1).
constexpr std::string_view orbit_bindings =
    R"({"id": "land.right.d", "action": "pan_right", "context": "land", "device": "keyboard", "control": "D", "trigger": "held", "scale": 1},
      {"id": "land.grab.middle", "action": "rotate_grab", "context": "land", "device": "mouse_button", "control": "middle", "trigger": "held", "scale": 1},
      {"id": "land.translate.x", "action": "translate_x", "context": "land", "device": "mouse_motion", "control": "x", "trigger": "delta", "scale": 4},
      {"id": "land.translate.y", "action": "translate_y", "context": "land", "device": "mouse_motion", "control": "y", "trigger": "delta", "scale": -4},
      {"id": "land.orbit-grab.ctrl-middle", "action": "rotate_grab", "context": "land", "device": "mouse_button", "control": "middle", "modifiers": ["ctrl"], "trigger": "held", "scale": 1},
      {"id": "land.orbit.ctrl-x", "action": "rotate", "context": "land", "device": "mouse_motion", "control": "x", "modifiers": ["ctrl"], "trigger": "delta", "scale": -1},
      {"id": "land.orbit.ctrl-y", "action": "orbit_pitch", "context": "land", "device": "mouse_motion", "control": "y", "modifiers": ["ctrl"], "trigger": "delta", "scale": -1})";

// The committed map tables' units: FoC mouse units, screen fractions x 100.
constexpr std::string_view law_head =
    R"("schema": "eawr-camera-bindings", "version": 1, "provenance": "project-authored",
       "notice": "test-only project-authored bindings", "edge_scroll": true,
       "screen_mouse_units": true)";

constexpr std::string_view click_reset_head =
    R"("schema": "eawr-camera-bindings", "version": 1, "provenance": "project-authored",
       "notice": "test-only project-authored bindings", "edge_scroll": true, "click_reset": true,
       "screen_mouse_units": true)";

input::RawEvent with_modifiers(input::RawEvent event, const std::uint8_t modifiers) {
    event.modifiers = modifiers;
    return event;
}

void test_orbit_pitch_bindings() {
    input::Adapter adapter(input::Context::land, true);
    expect(adapter.activate(table_with(orbit_bindings, law_head)).has_value(), "orbit table activates in v1");
    expect(adapter.set_viewport(1280.0F, 720.0F).has_value(), "orbit viewport publishes");
    constexpr std::uint8_t ctrl = input::modifier::ctrl;
    const auto ctrl_drag = with_modifiers(motion(640.0F, 360.0F, 10.0F, 6.0F), ctrl);
    send(adapter, ctrl_drag, "Ctrl motion without a grab");
    expect(step(adapter).empty(), "Ctrl motion without a grab does nothing");
    send(adapter, with_modifiers(button(input::mouse_code::middle, true), ctrl), "Ctrl+middle press");
    expect(adapter.is_held(input::Action::rotate_grab), "Ctrl+middle holds the rotate grab");
    send(adapter, ctrl_drag, "Ctrl drag");
    auto orbited = step(adapter);
    expect_close(orbited.rotate_units, -10.0F * 100.0F / 1280.0F,
                 "Ctrl horizontal motion rotates by width fractions, reversed");
    expect_close(orbited.orbit_pitch_units, -6.0F * 100.0F / 720.0F,
                 "Ctrl downward motion is a negative (screen-up positive) height fraction");
    expect(orbited.pan_x == 0.0F && orbited.drag_x == 0.0F && orbited.drag_y == 0.0F
               && orbited.translate_x == 0.0F && orbited.translate_y == 0.0F,
           "a Ctrl drag does not pan");
    send(adapter, motion(640.0F, 360.0F, 4.0F, 6.0F), "motion after Ctrl is released");
    orbited = step(adapter);
    expect_close(orbited.rotate_units, 0.0F, "without Ctrl the grab does not rotate");
    expect_close(orbited.orbit_pitch_units, 0.0F, "without Ctrl there is no orbit pitch");
    expect_close(orbited.translate_x, 4.0F * 4.0F * 100.0F / 1280.0F,
                 "without Ctrl the grab translates at four times the width fraction");
    expect_close(orbited.translate_y, -4.0F * 6.0F * 100.0F / 720.0F,
                 "a downward plain drag translates back");
    send(adapter, button(input::mouse_code::middle, false), "middle release without Ctrl");
    expect(!adapter.is_held(input::Action::rotate_grab), "a release ends the Ctrl grab");
    expect(step(adapter).view_reset_requests == 0U, "the table without click_reset never click-resets");
    send(adapter, ctrl_drag, "Ctrl motion after release");
    expect(step(adapter).empty(), "released orbit does nothing");
    send(adapter, button(input::mouse_code::middle, true), "plain middle press");
    send(adapter, ctrl_drag, "Ctrl pressed during a plain drag");
    expect_close(step(adapter).orbit_pitch_units, -6.0F * 100.0F / 720.0F, "Ctrl during a middle drag orbits");
    send(adapter, button(input::mouse_code::middle, false), "plain middle release");

    // Vocabulary: tactical only, pointer motion delta only, in either schema.
    const auto binding = [](const std::string_view fields) {
        return table_with(std::string("{") + std::string(fields) + "}");
    };
    expect_code(binding(R"("id": "x", "action": "orbit_pitch", "context": "land",
        "device": "keyboard", "control": "W", "trigger": "pressed", "scale": 1)"),
        input::diagnostic_codes::incompatible_binding, "orbit pitch needs pointer motion");
    expect_code(binding(R"("id": "x", "action": "orbit_pitch", "context": "land",
        "device": "mouse_motion", "control": "y", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::incompatible_binding, "orbit pitch is a delta");
    expect_code(table_with(R"({"id": "x", "action": "orbit_pitch", "context": "free",
        "device": "mouse_motion", "control": "y", "trigger": "delta", "scale": 1})", v2_head),
        input::diagnostic_codes::incompatible_binding, "orbit pitch cannot be bound in free flight");
    expect(input::parse_binding_table(table_with(orbit_bindings, v2_head)).has_value(),
           "a v2 table accepts the orbit chords");
    expect(input::to_string(input::Action::orbit_pitch) == "orbit_pitch", "orbit pitch has a name");
    expect(input::to_string(input::Action::translate_x) == "translate_x"
               && input::to_string(input::Action::translate_y) == "translate_y",
           "the grab translate has names");
    expect_code(binding(R"("id": "x", "action": "translate_x", "context": "land",
        "device": "mouse_button", "control": "left", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::incompatible_binding, "translate needs pointer motion");
    expect_code(table_with(R"({"id": "x", "action": "translate_y", "context": "free",
        "device": "mouse_motion", "control": "y", "trigger": "delta", "scale": 1})", v2_head),
        input::diagnostic_codes::incompatible_binding, "translate cannot be bound in free flight");
    expect_code(table_with(pan_right_d, R"("schema": "eawr-camera-bindings", "version": 1,
        "provenance": "project-authored", "notice": "n", "edge_scroll": true, "click_reset": 1)"),
        input::diagnostic_codes::invalid_bindings, "click_reset must be a boolean");
    expect_code(table_with(pan_right_d, R"("schema": "eawr-camera-bindings", "version": 1,
        "provenance": "project-authored", "notice": "n", "edge_scroll": true,
        "screen_mouse_units": "yes")"),
        input::diagnostic_codes::invalid_bindings, "screen_mouse_units must be a boolean");
    const auto plain = input::parse_binding_table(table_with(orbit_bindings));
    expect(plain.has_value() && !plain.value().screen_mouse_units, "screen_mouse_units is off when absent");
}

// RO-7: the FoC mouse unit is a screen fraction times 100, so the same share
// of the screen gives the same units at 1280 x 720 and 1920 x 1080.
void test_mouse_units_follow_the_screen() {
    constexpr std::uint8_t ctrl = input::modifier::ctrl;
    for (const auto& [width, height] : {std::pair{1280.0F, 720.0F}, std::pair{1920.0F, 1080.0F}}) {
        const std::string size = std::to_string(static_cast<int>(width)) + "x"
            + std::to_string(static_cast<int>(height));
        input::Adapter adapter(input::Context::space, true);
        std::string table(orbit_bindings);
        for (std::size_t at = table.find("land"); at != std::string::npos; at = table.find("land", at)) {
            table.replace(at, 4, "space");
        }
        expect(adapter.activate(table_with(table, law_head)).has_value(), "space law table activates " + size);
        expect(adapter.set_viewport(width, height).has_value(), "law viewport publishes " + size);
        send(adapter, with_modifiers(button(input::mouse_code::middle, true), ctrl), "Ctrl grab " + size);
        send(adapter, with_modifiers(motion(0.0F, 0.0F, width, 0.0F), ctrl), "full-width Ctrl drag " + size);
        expect_close(step(adapter).rotate_units, -100.0F, "a full-width Ctrl drag is 100 mouse units " + size);
        send(adapter, with_modifiers(motion(0.0F, 0.0F, -0.5F * width, 0.0F), ctrl), "half-width back " + size);
        expect_close(step(adapter).rotate_units, 50.0F, "a half-width Ctrl drag is 50 units " + size);
        send(adapter, with_modifiers(motion(0.0F, 0.0F, 0.0F, height), ctrl), "full-height Ctrl drag down " + size);
        expect_close(step(adapter).orbit_pitch_units, -100.0F, "a full-height drag down is -100 units " + size);
        send(adapter, motion(0.0F, 0.0F, 0.4F * width, 0.0F), "plain drag of 0.4 widths " + size);
        const auto translated = step(adapter);
        expect_close(translated.translate_x, 160.0F, "0.4 of the width translates 1.6 screens (x4) " + size);
        expect(translated.rotate_units == 0.0F, "a plain drag never rotates " + size);

        // The same chords without screen_mouse_units stay in scaled pixels:
        // the conversion belongs to the map tables only.
        input::Adapter pixels(input::Context::space, true);
        expect(pixels.activate(table_with(table)).has_value(), "pixel table activates " + size);
        expect(pixels.set_viewport(width, height).has_value(), "pixel viewport publishes " + size);
        send(pixels, with_modifiers(button(input::mouse_code::middle, true), ctrl), "pixel grab " + size);
        send(pixels, with_modifiers(motion(0.0F, 0.0F, 40.0F, 30.0F), ctrl), "pixel Ctrl drag " + size);
        const auto raw = step(pixels);
        expect_close(raw.rotate_units, -40.0F, "without screen units 40 px rotate 40 units " + size);
        expect_close(raw.orbit_pitch_units, -30.0F, "without screen units 30 px tilt 30 units " + size);
    }
}

// FoC: a middle release that moved at most 1% of the screen resets the view,
// unless Ctrl is held at the release.
void test_click_reset() {
    constexpr std::uint8_t ctrl = input::modifier::ctrl;
    input::Adapter adapter(input::Context::land, true);
    expect(adapter.activate(table_with(orbit_bindings, click_reset_head)).has_value(),
           "click reset table activates");
    expect(adapter.table()->click_reset, "the table carries click_reset");
    expect(adapter.set_viewport(1280.0F, 720.0F).has_value(), "click viewport publishes");
    const auto middle = [](const bool pressed) { return button(input::mouse_code::middle, pressed); };

    send(adapter, middle(true), "click press");
    send(adapter, middle(false), "click release");
    auto clicked = step(adapter);
    expect(clicked.view_reset_requests == 1U && clicked.reset_requests == 0U,
           "a still middle click requests a view reset");
    expect(step(adapter).view_reset_requests == 0U, "the request is consumed once");

    send(adapter, middle(true), "small drag press");
    send(adapter, motion(0.0F, 0.0F, 8.0F, 4.0F), "drag inside 1% of the screen");
    send(adapter, middle(false), "small drag release");
    expect(step(adapter).view_reset_requests == 1U, "a drag within 1% still clicks");

    send(adapter, middle(true), "drag press");
    send(adapter, motion(0.0F, 0.0F, 20.0F, 0.0F), "drag beyond 1% of the width");
    expect(step(adapter).view_reset_requests == 0U, "a step mid-drag requests nothing");
    send(adapter, motion(0.0F, 0.0F, -18.0F, 0.0F), "drag back near the start");
    send(adapter, middle(false), "drag release near the start");
    expect(step(adapter).view_reset_requests == 1U,
           "the net travel decides, across steps, as in FoC");

    send(adapter, middle(true), "long drag press");
    send(adapter, motion(0.0F, 0.0F, 0.0F, 10.0F), "drag beyond 1% of the height");
    send(adapter, middle(false), "long drag release");
    expect(step(adapter).view_reset_requests == 0U, "a real drag does not reset");

    send(adapter, with_modifiers(middle(true), ctrl), "Ctrl click press");
    send(adapter, with_modifiers(middle(false), ctrl), "Ctrl click release");
    expect(step(adapter).view_reset_requests == 0U, "a Ctrl click does not reset");
    send(adapter, middle(true), "plain press, Ctrl release");
    send(adapter, with_modifiers(middle(false), ctrl), "release with Ctrl");
    expect(step(adapter).view_reset_requests == 0U, "Ctrl at the release decides");
    send(adapter, with_modifiers(middle(true), ctrl), "Ctrl press, plain release");
    send(adapter, middle(false), "release without Ctrl");
    expect(step(adapter).view_reset_requests == 1U, "a release without Ctrl resets");

    send(adapter, middle(true), "press before pointer exit");
    adapter.pointer_left();
    send(adapter, middle(false), "release after pointer exit");
    expect(step(adapter).view_reset_requests == 0U, "a grab ended by pointer exit is no click");
    send(adapter, middle(true), "press before focus loss");
    adapter.set_focus(false);
    adapter.set_focus(true);
    send(adapter, middle(false), "release after focus loss");
    expect(step(adapter).view_reset_requests == 0U, "a cancelled grab is no click");
}

// Focus loss and pointer exit end an orbit drag; only a fresh press resumes it.
void test_orbit_cancellation() {
    constexpr std::uint8_t ctrl = input::modifier::ctrl;
    const auto ctrl_drag = with_modifiers(motion(640.0F, 360.0F, 10.0F, 6.0F), ctrl);
    const auto ctrl_press = with_modifiers(button(input::mouse_code::middle, true), ctrl);
    input::Adapter adapter(input::Context::land, true);
    expect(adapter.activate(table_with(orbit_bindings, law_head)).has_value(), "orbit table activates");
    expect(adapter.set_viewport(1280.0F, 720.0F).has_value(), "orbit viewport publishes");

    send(adapter, ctrl_press, "Ctrl+middle before focus loss");
    send(adapter, ctrl_drag, "pending orbit before focus loss");
    adapter.set_focus(false);
    adapter.set_focus(true);
    expect(step(adapter).empty(), "focus loss drops the pending orbit");
    send(adapter, ctrl_drag, "Ctrl motion after focus regain");
    expect(step(adapter).empty(), "focus regain does not resurrect the orbit grab");
    send(adapter, button(input::mouse_code::middle, false), "stale release");
    send(adapter, ctrl_press, "fresh Ctrl+middle");
    send(adapter, ctrl_drag, "fresh orbit");
    expect_close(step(adapter).orbit_pitch_units, -6.0F * 100.0F / 720.0F, "a fresh press orbits again");

    send(adapter, key("D", true), "held key through pointer exit");
    send(adapter, ctrl_drag, "pending orbit before pointer exit");
    adapter.pointer_left();
    auto after_exit = step(adapter);
    expect(after_exit.rotate_units == 0.0F && after_exit.orbit_pitch_units == 0.0F,
           "pointer exit drops the pending orbit");
    expect_close(after_exit.pan_x, 1.0F, "pointer exit keeps a held key");
    expect(!adapter.is_held(input::Action::rotate_grab), "pointer exit releases the grab");
    send(adapter, ctrl_drag, "Ctrl motion after pointer exit");
    auto no_orbit = step(adapter);
    expect(no_orbit.rotate_units == 0.0F && no_orbit.orbit_pitch_units == 0.0F,
           "an orbit after pointer exit needs a fresh press");
    send(adapter, button(input::mouse_code::middle, false), "late release outside");
    send(adapter, ctrl_press, "fresh Ctrl+middle after pointer exit");
    send(adapter, ctrl_drag, "orbit after pointer exit");
    expect_close(step(adapter).orbit_pitch_units, -6.0F * 100.0F / 720.0F,
                 "a fresh press after pointer exit orbits");
}

} // namespace eawr_camera_binding_test
