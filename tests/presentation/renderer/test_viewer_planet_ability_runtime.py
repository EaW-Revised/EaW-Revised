"""GHUD-S95/S96: mounted selected-planet bonus and faithful encyclopedia."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class PlanetAbilityRuntime(unittest.TestCase):
    def test_stock_selected_bonus_encyclopedia_lit(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-ability-"))
        report, capture, driver = [directory / name for name in ("report.json", "lit.png", "driver.gd")]
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var stage = 0
func _initialize():
    var instance = load("res://main.tscn").instantiate()
    root.add_child(instance)
    current_scene = instance
func pointer(target):
    assert(target != null and target.is_visible_in_tree())
    var motion = InputEventMouseMotion.new()
    motion.position = target.get_global_rect().get_center()
    motion.global_position = motion.position
    Input.parse_input_event(motion)
    return motion.position
func press(target, button):
    var point = pointer(target)
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = point
        event.global_position = point
        event.button_index = button
        event.pressed = down
        Input.parse_input_event(event)
func _process(delta):
    elapsed += delta
    if stage == 0 and elapsed > 2.0:
        press(root.find_child("b_zoom", true, false), MOUSE_BUTTON_LEFT)
        stage = 1
    if stage == 1 and elapsed > 4.0:
        press(root.find_child("radar_galactic", true, false), MOUSE_BUTTON_WHEEL_UP)
        stage = 2
    if stage == 2 and elapsed > 6.0:
        pointer(root.find_child("planet_bonus", true, false))
        stage = 3
    if elapsed > 12.0:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % json.dumps(capture.as_posix()), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "Sandbox_Equal_Footing_Underworld", "--eawr-game-root", support.GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-lighting", "sh", "--eawr-environment", "map",
            "--eawr-shadows", "on", "--eawr-report", str(report)], cwd=support.ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        selected = next(row for row in data["planets"] if row["id"] == data["map_input"]["selected"])
        bonus, popup = selected["ability"], data["encyclopedia"]
        self.assertTrue(bonus["title"] and bonus["description"] and bonus["icon"])
        self.assertTrue(popup["visible"] and popup["planet_ability"])
        self.assertGreater(popup["alpha"], 0)
        self.assertEqual((popup["title"], popup["body"], popup["icon"]),
                         (bonus["title"], bonus["description"], bonus["icon"]))
        self.assertIsNone(popup["price"])
        self.assertTrue(capture.is_file())
        x, y, w, h = popup["rect"]
        self.assertGreaterEqual(x, 0)
        self.assertGreaterEqual(y, 0)
        self.assertLessEqual(x + w, 1280.01)
        self.assertLessEqual(y + h, 720.01)


if __name__ == "__main__":
    unittest.main()
