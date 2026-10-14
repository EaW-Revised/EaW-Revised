"""GHUD-S82..S83: focused object hovers reuse the timed encyclopedia."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_fleet_runtime as fleets
from tests.presentation.renderer import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticPlanetTooltipsRuntime(unittest.TestCase):
    capture = fleets.GalacticFleetRuntime.capture

    def hover(self, fixture, name, ground=True, rearrange=False):
        initial, _ = self.capture(fixture)
        section = "ground_hud" if ground else "fleet_hud"
        row = next(row for row in initial[section]["controls"] if row["name"] == name)
        self.assertTrue(row["visible"])
        x, y, w, h = row["rect"]
        source = next(row for row in initial["ground_hud"]["controls"] if row["name"] == "Pad_00")
        sx, sy, sw, sh = source["rect"]
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-tooltip-"))
        driver, report, capture = [directory / name for name in ("driver.gd", "report.json", "capture.png")]
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var moved = false
var viewport
var stage = 0
var rearrange = %s
var source = Vector2(%s, %s)
var target = Vector2(%s, %s)
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.relative = Vector2(24, 0)
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    viewport.push_input(event, true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.position = point
    event.global_position = point
    event.pressed = down
    viewport.push_input(event, true)
func _initialize():
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(load("res://main.tscn").instantiate())
    viewport.notify_mouse_entered()
    current_scene = viewport
func _process(delta):
    elapsed += delta
    if rearrange and elapsed > 2.0 and stage == 0:
        motion(source, false)
        mouse(source, true)
        stage = 1
    if rearrange and elapsed > 2.2 and stage == 1:
        motion(target, true)
        stage = 2
    if rearrange and elapsed > 2.4 and stage == 2:
        mouse(target, false)
        stage = 3
    if elapsed > 2.6 and not moved:
        motion(target, false)
        moved = true
    if elapsed > 7.0:
        viewport.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % ("true" if rearrange else "false", sx + sw / 2, sy + sh / 2,
       x + w / 2, y + h / 2, json.dumps(capture.as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
            "--eawr-galactic-paused", "yes", "--eawr-game-root", support.GAME_ROOT,
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True,
            timeout=180, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"], initial["map_input"]["selected"])
        self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"])
        if rearrange:
            self.assertGreater(data["ground_hud"]["reorders"], 0)
        return data["encyclopedia"]

    def assert_popup(self, popup, title):
        self.assertTrue(popup["visible"])
        self.assertGreater(popup["alpha"], 0)
        self.assertEqual(popup["title"], title)
        self.assertIsNone(popup["price"], "existing objects must not inherit a build-card price")
        self.assertTrue(popup["icon"])
        x, y, w, h = popup["rect"]
        self.assertGreaterEqual(x, 0)
        self.assertGreaterEqual(y, 0)
        self.assertLessEqual(x + w, 1280.01)
        self.assertLessEqual(y + h, 720.01)

    def test_ground_unit_structure_and_orbit_unit_titles(self):
        for fixture, name, ground, title in [
            ("ground-structures", "Pad_00", True, "Stormtroopers"),
            ("ground-structures", "Planet_structure_00", True, "Barracks"),
            ("fleet-contents", "q1_0001", False, "Imperial Star Destroyer"),
        ]:
            with self.subTest(target=name):
                self.assert_popup(self.hover(fixture, name, ground), title)

    def test_empty_and_unknown_ground_cells_do_not_disclose_types(self):
        for fixture, name in [("ground-structures", "Pad_09"),
                              ("ground-count", "Pad_00"),
                              ("ground-structures-hidden", "Planet_structure_00")]:
            with self.subTest(fixture=fixture, target=name):
                popup = self.hover(fixture, name)
                self.assertFalse(popup["visible"])
                self.assertEqual(popup["type"], 0)

    def test_rearranged_company_hover_follows_displayed_pad_identity(self):
        self.assert_popup(self.hover("ground-structures", "Pad_04", rearrange=True), "Stormtroopers")


if __name__ == "__main__":
    unittest.main()
