"""#5990: the galactic right-button scroll holds the cursor from its press point and measures the drag by position."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = (ROOT / "apps/viewer/src/galactic/galactic_mode.cpp").read_text(encoding="utf-8")
HANDOFF = (ROOT / "apps/viewer/src/galactic/galactic_handoff.cpp").read_text(encoding="utf-8")
STATE = (ROOT / "apps/viewer/src/galactic/galactic_mode_internal.hpp").read_text(encoding="utf-8")


class GalacticScrollHold(unittest.TestCase):
    def test_no_per_event_warp_or_echo_filter(self):
        # A per-event warp to the pre-move point and its echo filter break when Godot merges the echo with real motion.
        self.assertNotIn("pointer_warp", SOURCE)
        self.assertNotIn("pointer_warp", STATE)
        self.assertNotIn("warp_mouse(old_position)", SOURCE)

    def test_hold_starts_on_press_and_measures_by_position(self):
        self.assertIn("PointerHold scroll_hold;", STATE)
        self.assertIn("state.scroll_hold.begin(press.x, press.y)", SOURCE)
        self.assertIn("state.scroll_hold.delta(position.x, position.y)", SOURCE)

    def test_one_warp_per_frame_and_feed_off_releases(self):
        self.assertEqual(SOURCE.count("warp_mouse("), 1)
        self.assertIn("HoldStep::release", SOURCE)
        self.assertIn("HoldStep::warp", SOURCE)
        # Edge scroll is off only for a live scroll: a hold left behind by a path that cleared `scrolling` must not stop it.
        self.assertIn("(state.scrolling || state.orbiting) && state.scroll_hold.active()", SOURCE)

    def test_hold_only_where_the_warp_works_and_ends_at_the_battle_handoff(self):
        self.assertIn("::eawr::viewer::camera_input::pointer_hold_supported);", SOURCE)
        self.assertGreaterEqual(HANDOFF.count("scroll_hold.end()"), 2)

    # #5990 gaps: the middle drag is held and pans like the right drag, Ctrl switches it to tilt and yaw.
    def test_middle_drag_shares_the_hold(self):
        self.assertIn("if (index == MOUSE_BUTTON_RIGHT || index == MOUSE_BUTTON_MIDDLE) {", SOURCE)
        self.assertIn("(state.scrolling || state.orbiting) && state.scroll_hold.active()", SOURCE)
        self.assertIn("state.orbiting = state.map_input.held_rotate = false;", SOURCE)
        self.assertIn("!drag_held", SOURCE)

    def test_plain_middle_pans_and_ctrl_tilts_from_the_held_delta(self):
        motion = SOURCE[SOURCE.index("InputEventMouseMotion>(event.ptr())"):SOURCE.index("void GalacticMode::cancel_input()")]
        self.assertIn("state.scroll_hold.delta(position.x, position.y)", motion)
        self.assertNotIn("move = motion->get_relative()", motion)
        self.assertIn("flight.tilt(fy)", motion)
        self.assertIn("flight.right_drag(*state.galaxy, fx, fy, true, false)", motion)

    def test_cursor_tracks_middle_hold_ctrl_release_and_handoff(self):
        process = SOURCE[SOURCE.index("GalacticMode::process("):SOURCE.index("void GalacticMode::cancel_input()")]
        self.assertIn("cursor.camera_pan = state.scrolling || (state.orbiting && !ctrl);", process)
        self.assertIn("cursor.camera_rotate = state.orbiting && ctrl;", process)
        self.assertIn("state.mouse_cursor->update(ui::battle_cursor(cursor), delta);", process)
        self.assertIn("state.orbiting = state.map_input.held_rotate = false;", process)
        self.assertIn("state.scrolling = state.map_input.held_scroll = false;", process)
        self.assertIn("state.mouse_cursor->suspend();", process)
        self.assertGreaterEqual(HANDOFF.count("mouse_cursor->suspend()"), 2)
        motion = SOURCE[SOURCE.index("InputEventMouseMotion>(event.ptr())"):SOURCE.index("void GalacticMode::cancel_input()")]
        self.assertIn("flight.right_drag(*state.galaxy, fx, -fy, true, motion->is_ctrl_pressed())", motion)

    def test_stale_hold_and_overview_only_go_through_the_tested_step(self):
        self.assertEqual(SOURCE.count("camera_input::drag_hold_step("), 2)  # process() and the button handler
        self.assertIn("flight.state() == galactic_view::CameraState::overview,", SOURCE)
        self.assertNotIn("if (!state.scrolling && !state.orbiting) state.scroll_hold.end();", SOURCE)

    def test_loading_phase_and_radar_end_the_shared_hold(self):
        self.assertEqual(HANDOFF.count("scroll_hold.end()"), 3)
        radar = (ROOT / "apps/viewer/src/galactic/galactic_radar_hud.cpp").read_text(encoding="utf-8")
        self.assertIn("if (!scrolling && !orbiting) scroll_hold.end();", radar)


if __name__ == "__main__":
    unittest.main()
