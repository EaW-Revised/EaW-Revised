"""WGP-41/42: the stock campaign's first rendered overview uses its authored home."""
import json
import math
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the GPU route and installed FoC data")
class GalaxyStartRuntime(unittest.TestCase):
    def test_equal_footing_starts_at_hypori_on_the_first_rendered_frame(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-galaxy-start-"))
        report = directory / "startup.json"
        capture = directory / "startup.png"
        driver = directory / "first-frame.gd"
        driver.write_text('''extends SceneTree
func _initialize():
    call_deferred("_start")
func _start():
    var scene = load(ProjectSettings.get_setting("application/run/main_scene"))
    var instance = scene.instantiate()
    root.add_child(instance)
    current_scene = instance
    RenderingServer.frame_post_draw.connect(_first_frame, CONNECT_ONE_SHOT)
func _first_frame():
    var arguments = OS.get_cmdline_user_args()
    for index in range(arguments.size() - 1):
        if arguments[index] == "--startup-frame-capture":
            root.get_texture().get_image().save_png(arguments[index + 1])
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
''', encoding="utf-8")
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
            str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "Sandbox_Equal_Footing_Underworld", "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report),
            "--startup-frame-capture", str(capture)], cwd=ROOT, capture_output=True, text=True,
            timeout=300, check=False)
        log = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue(report.is_file(), log)
        self.assertTrue(capture.is_file(), log)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["status"], "closed")
        self.assertEqual(data["snapshot"]["ownership"], "authored-starting-forces")
        self.assertTrue(any(planet.get("owner") for planet in data["planets"]))
        self.assertTrue(data["fleets"])
        self.assertEqual(data["flights"][0]["state"], "overview")
        self.assertEqual(data["camera"]["target"], [160, 10, 320])
        for actual, expected in zip(data["camera"]["eye"], [160, 360, 320 + math.sqrt(3) * 350]):
            self.assertAlmostEqual(actual, expected, delta=0.002)
        camera = data["cameras"][0]
        self.assertEqual(camera["target"], [160, -320, 10])
        self.assertEqual((camera["yaw"], camera["pitch"], camera["distance"]), (0, 30, 700))
        expected_fov = math.degrees(2 * math.atan(math.tan(math.atan(0.5) / 2) / (16 / 9)))
        self.assertAlmostEqual(data["camera"]["vertical_fov"], expected_fov, delta=0.0001)
        self.assertEqual((data["camera"]["near"], data["camera"]["far"]), (10, 10000))
        from space_map_test_support import decode_png
        width, height, rows = decode_png(capture.read_bytes())
        self.assertEqual((width, height), (1280, 720))
        self.assertGreater(len({pixel for row in rows for pixel in row}), 500)
        self.assertNotIn("ERROR:", log)


if __name__ == "__main__":
    unittest.main()
