"""S28/S30: native modal input over copied observer snapshot fixtures."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support

ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticHeroActionsRuntime(unittest.TestCase):
    layouts = {}

    def initial(self, fixture, seat=1):
        key = fixture, seat
        if key not in self.layouts:
            data, capture = support.GalacticCommandBarRuntime.capture(self, seat, fixture)
            self.layouts[key] = data
            menu = data["hero_action_hud"]
            self.assertTrue(menu["active"])
            self.assertEqual(menu["source"], "controlled-observer-snapshot")
            self.assertEqual((menu["actor"], menu["planet"]), (10, 101))
            self.assertTrue(capture.is_file())
            for control in menu["controls"]:
                x, y, width, height = control["hit_rect"]
                self.assertGreater(width, 0, control)
                self.assertGreater(height, 0, control)
                self.assertGreaterEqual(x, 0, control)
                self.assertGreaterEqual(y, 0, control)
                self.assertLessEqual(x + width, 1280, control)
                self.assertLessEqual(y + height, 720, control)
        return self.layouts[key]

    def native(self, fixture, actions, seat=1):
        initial = self.initial(fixture, seat)
        points = {row["name"]: [row["hit_rect"][0] + row["hit_rect"][2] / 2,
            row["hit_rect"][1] + row["hit_rect"][3] / 2]
            for row in initial["hero_action_hud"]["controls"]}
        points["world"] = [100, 100]
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hero-menu-"))
        driver, report = directory / "input.gd", directory / "report.json"
        driver.write_text('''extends SceneTree
var frame = 0
var actions = JSON.parse_string(''' + repr(json.dumps(actions)) + ''')
var points = JSON.parse_string(''' + repr(json.dumps(points)) + ''')
var capture_path = ''' + json.dumps((directory / "native.png").as_posix()) + '''
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(name, down):
    if name != "world":
        var target = root.find_child(name, true, false)
        assert(target != null and target.is_visible_in_tree())
    var point = Vector2(points[name][0], points[name][1])
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = point
    event.global_position = point
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    for index in range(actions.size()):
        if frame == 40 + index * 20: click(actions[index], true)
        if frame == 41 + index * 20: click(actions[index], false)
    if frame == 80 + actions.size() * 20:
        root.get_texture().get_image().save_png(capture_path)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", str(seat),
            "--eawr-galactic-hud-fixture", fixture, "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)], cwd=ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["map_input"]["selected"], initial["map_input"]["selected"])
        self.assertEqual(data["map_input"]["selection_invalidations"], 0)
        self.assertEqual(data["live_session"]["submitted_orders"], 1)
        self.assertLessEqual(data["hero_action_hud"]["rebuilds"], 2)
        return data["hero_action_hud"]

    def test_theft_prepared_choices_and_native_acceptance(self):
        menu = self.initial("theft")["hero_action_hud"]
        controls = {row["name"]: row for row in menu["controls"]}
        self.assertTrue(controls["bm_0000"]["enabled"])
        self.assertFalse(controls["bm_0001"]["enabled"])
        self.assertEqual([row["advance"] for row in menu["options"]], [False, False])
        self.assertEqual([row["affordable"] for row in menu["options"]], [True, False])
        accepted = self.native("theft", ["world", "bm_0001", "bm_0000"])
        self.assertEqual(accepted["choices_accepted"], 1)
        self.assertEqual(accepted["accepted_index"], 0)
        self.assertFalse(accepted["active"])

    def test_single_theft_advance_and_native_close(self):
        menu = self.initial("theft-single")["hero_action_hud"]
        self.assertEqual(len(menu["options"]), 1)
        self.assertTrue(menu["options"][0]["advance"])
        closed = self.native("theft-single", ["bm_close"])
        self.assertEqual(closed["closes"], 1)
        self.assertEqual(closed["choices_accepted"], 0)
        self.assertFalse(closed["active"])

    def test_neutralization_valid_disabled_and_siphon_native(self):
        menu = self.initial("neutralization")["hero_action_hud"]
        controls = {row["name"]: row for row in menu["controls"]}
        self.assertEqual([controls[name]["enabled"] for name in ["h_0000", "h_0001", "h_0100"]], [True, False, False])
        self.assertEqual([row["valid"] for row in menu["options"]], [True, False, True])
        self.assertTrue(menu["options"][2]["siphon"])
        self.assertEqual(menu["options"][2]["source_type"], 1004)
        self.assertGreater(menu["options"][0]["target_absence_raw"], menu["options"][0]["actor_absence_raw"])
        accepted = self.native("neutralization", ["world", "h_0001", "h_0100", "h_0000"])
        self.assertEqual(accepted["choices_accepted"], 1)
        self.assertEqual(accepted["accepted_index"], 0)
        self.assertFalse(accepted["active"])

    def test_neutralization_native_close_each_faction(self):
        for seat in [1, 3, 4]:
            with self.subTest(seat=seat):
                closed = self.native("neutralization", ["h_close"], seat)
                self.assertEqual(closed["closes"], 1)
                self.assertEqual(closed["choices_accepted"], 0)
                self.assertFalse(closed["active"])


if __name__ == "__main__":
    unittest.main()
