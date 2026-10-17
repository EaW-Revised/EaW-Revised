"""WGUI-49/76/78: real Godot input events reach the galactic selection policy."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))


@unittest.skipUnless(RUNTIME, "requires the GPU route")
class GalaxyInputRuntime(unittest.TestCase):
    def run_input(self, events, campaign="synthetic", game_root=None, fixture=None):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-galaxy-input-"))
        report = directory / "input.json"
        capture = directory / "input.png"
        driver = directory / "input.gd"
        driver.write_text('''extends SceneTree
var frame = 0
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(code, ctrl=false):
    var event = InputEventKey.new()
    event.keycode = code
    event.ctrl_pressed = ctrl
    event.pressed = true
    Input.parse_input_event(event)
    event = InputEventKey.new()
    event.keycode = code
    event.ctrl_pressed = ctrl
    event.pressed = false
    Input.parse_input_event(event)
func button(index, down, position, twice = false):
    var event = InputEventMouseButton.new()
    event.button_index = index
    event.pressed = down
    event.position = position
    event.double_click = twice
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
''' + events + '''
    if frame == 100:
        RenderingServer.frame_post_draw.connect(_finish, CONNECT_ONE_SHOT)
    return false
func _finish():
    var args = OS.get_cmdline_user_args()
    for index in range(args.size() - 1):
        if args[index] == "--input-capture":
            root.get_texture().get_image().save_png(args[index + 1])
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
''', encoding="utf-8")
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
                str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                "--eawr-galactic", campaign, "--eawr-galactic-paused", "yes",
                "--eawr-report", str(report), "--input-capture", str(capture)]
        if fixture:
            args += ["--eawr-galactic-hud-fixture", fixture]
        if game_root:
            args += ["--eawr-game-root", game_root]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue(report.is_file(), log)
        self.assertTrue(capture.is_file(), log)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["status"], "closed", log)
        self.assertNotIn("ERROR:", log)
        return data

    def test_navigation_wrap_and_overview_deselection(self):
        data = self.run_input('''    if frame == 30: key(KEY_PAGEDOWN)
    if frame == 35: key(KEY_PAGEDOWN)
    if frame == 40: key(KEY_PAGEUP)
    if frame == 45: button(MOUSE_BUTTON_LEFT, true, Vector2(-10000, -10000))
    if frame == 46: button(MOUSE_BUTTON_LEFT, false, Vector2(-10000, -10000))''')
        self.assertEqual(data["map_input"]["selected"], 0)
        self.assertEqual(data["map_input"]["selection_invalidations"], 4)
        self.assertEqual(data["flights"][0]["state"], "overview")

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"), "requires installed FoC data")
    def test_world_selection_and_wheel_remain_available_beside_the_hud(self):
        data = self.run_input('''    if frame == 30: button(MOUSE_BUTTON_LEFT, true, Vector2(-10000, -10000))
    if frame == 31: button(MOUSE_BUTTON_LEFT, false, Vector2(-10000, -10000))
    if frame == 40: button(MOUSE_BUTTON_LEFT, true, Vector2(640, 360))
    if frame == 41: button(MOUSE_BUTTON_LEFT, false, Vector2(640, 360))
    if frame == 50: button(MOUSE_BUTTON_WHEEL_UP, true, Vector2(640, 360))
    if frame == 51: button(MOUSE_BUTTON_WHEEL_UP, false, Vector2(640, 360))''',
            "Sandbox_Equal_Footing_Underworld", os.environ["EAWR_EAW_GAME_ROOT"])
        home = next(planet["id"] for planet in data["planets"] if planet["name"].lower() == "hypori")
        self.assertEqual(data["map_input"]["selected"], home)
        self.assertEqual(data["map_input"]["selection_invalidations"], 2)
        self.assertEqual(data["flights"][0]["planet"], home)
        self.assertIn(data["flights"][0]["state"], ("inward", "focused"))

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"), "requires installed FoC data")
    def test_held_scroll_and_rotate_consume_wheel(self):
        data = self.run_input('''    if frame == 30: button(MOUSE_BUTTON_MIDDLE, true, Vector2(640, 360))
    if frame == 31: button(MOUSE_BUTTON_WHEEL_UP, true, Vector2(640, 360))
    if frame == 32: button(MOUSE_BUTTON_MIDDLE, false, Vector2(640, 360))
    if frame == 40: button(MOUSE_BUTTON_RIGHT, true, Vector2(640, 360))
    if frame == 41: button(MOUSE_BUTTON_WHEEL_UP, true, Vector2(640, 360))
    if frame == 42: button(MOUSE_BUTTON_RIGHT, false, Vector2(640, 360))''',
            "Sandbox_Equal_Footing_Underworld", os.environ["EAWR_EAW_GAME_ROOT"])
        self.assertEqual(data["flights"][0]["state"], "overview")
        self.assertFalse(data["flights"][0]["advanced_map"])
        self.assertNotEqual(data["map_input"]["selected"], 0)
        self.assertEqual(data["map_input"]["selection_invalidations"], 0)

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"), "requires installed FoC data")
    def test_double_click_selected_home_zooms_and_release_keeps_selection(self):
        data = self.run_input('''    if frame == 30: button(MOUSE_BUTTON_LEFT, true, Vector2(640, 360), true)
    if frame == 31: button(MOUSE_BUTTON_LEFT, false, Vector2(640, 360))''',
            "Sandbox_Equal_Footing_Underworld", os.environ["EAWR_EAW_GAME_ROOT"])
        self.assertIn(data["flights"][0]["state"], ("inward", "focused"))
        self.assertEqual(data["flights"][0]["planet"], data["map_input"]["selected"])
        self.assertEqual(data["map_input"]["selection_invalidations"], 0)
    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"), "requires installed FoC data")
    def test_home_resolves_live_authored_leader_when_configured_home_is_missing(self):
        # Drive the existing project binding; retail profile remapping is separate from this producer.
        data = self.run_input("    if frame == 30: key(KEY_HOME, true)\n    if frame == 40: key(KEY_HOME, true)",
                              game_root=os.environ["EAWR_EAW_GAME_ROOT"], fixture="fleet-drag-home")
        self.assertEqual(data["map_input"]["selected"], 102)
        beta = next(row for row in data["planets"] if row["id"] == 102)
        x, y, z = beta["position"]
        self.assertEqual(data["camera"]["target"], [x, z, -y])
        self.assertEqual(data["map_input"]["discovery_source"], "published-local-faction-observer")


if __name__ == "__main__":
    unittest.main()
