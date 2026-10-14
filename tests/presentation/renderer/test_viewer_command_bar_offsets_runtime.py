"""GHUDL-01..05: focused shell and advanced radar placement, two resolutions."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class CommandBarOffsetsRuntime(unittest.TestCase):
    def capture(self, width, height, advanced=False):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-command-bar-offsets-"))
        report, capture = directory / "report.json", directory / "capture.png"
        # Windows can clamp a 1080p window even when it is borderless.
        # Pin its render viewport before scene startup, as capture_viewport.hpp
        # does for production captures; the OS window may display it scaled.
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", f"{width}x{height}",
                "--path", str(ROOT / "apps/viewer/project")]
        driver = directory / "driver.gd"
        initialize = '''func _initialize():
    root.borderless = true
    root.position = Vector2i(0, 0)
    root.size = Vector2i(%s, %s)
    root.content_scale_factor = 1.0
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_size = Vector2i(%s, %s)
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
''' % (width, height, width, height)
        if advanced:
            # The independently pinned normal radar position lies inside the
            # real native control. Wheel-in opens the focused advanced map.
            driver.write_text('''extends SceneTree
var elapsed = 0.0
var sent = false
''' + initialize + '''func _process(delta):
    elapsed += delta
    if elapsed >= 1.0 and not sent:
        sent = true
        var position = Vector2(%s, %s)
        var motion = InputEventMouseMotion.new()
        motion.position = position
        motion.global_position = position
        root.push_input(motion, true)
        var event = InputEventMouseButton.new()
        event.position = position
        event.global_position = position
        event.button_index = MOUSE_BUTTON_WHEEL_UP
        event.pressed = true
        root.push_input(event, true)
    if elapsed >= 2.0:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (96 * height / 720, 626 * height / 720,
       json.dumps(str(capture).replace("\\", "/"))), encoding="utf-8")
        else:
            driver.write_text("extends SceneTree\n" + initialize, encoding="utf-8")
        args += ["--script", str(driver)]
        args += ["--", "--eawr-galactic", "synthetic", "--eawr-game-root", GAME_ROOT,
                 "--eawr-galactic-paused", "yes", "--eawr-galactic-hud-fixture", "fleet-count",
                 "--eawr-galactic-focus", "101", "--eawr-report", str(report)]
        if not advanced:
            args += ["--eawr-galactic-flight-ms", "750", "--eawr-capture", str(capture)]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        self.assertTrue(capture.is_file())
        png = capture.read_bytes()
        self.assertEqual((int.from_bytes(png[16:20], "big"), int.from_bytes(png[20:24], "big")),
                         (width, height), "acceptance capture must use the exact render viewport")
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["flights"][0]["state"], "focused")
        return data, capture

    def assert_rect(self, actual, expected, width, height):
        x, y, w, h = actual
        self.assertGreaterEqual(x, 0)
        self.assertGreaterEqual(y, 0)
        self.assertLessEqual(x + w, width)
        self.assertLessEqual(y + h, height)
        # Two pixels at 720p cover raster snapping in the retained retail
        # focused frames. The higher-resolution target is their uniform scale.
        for value, target in zip(actual, expected):
            self.assertAlmostEqual(value, target * height / 720, delta=2 * height / 720)

    def test_focused_slots_and_panel_geometry(self):
        for width, height in [(1280,720), (1920,1080)]:
            with self.subTest(resolution=(width,height)):
                data, _ = self.capture(width,height)
                controls = {row["name"]: row for row in data["fleet_hud"]["controls"]}
                for name, target in [("q1_0001", [213,64,40,40]), ("q2_0001", [539,64,40,40])]:
                    self.assertTrue(controls[name]["visible"])
                    self.assert_rect(controls[name]["rect"],target,width,height)
                panels = {row["name"]: row for row in data["fleet_hud"]["shell_geometry"]}
                # Full authored quads include transparent texture padding;
                # their opaque frames align with GHUD-R10's three panels.
                for name, target in [("q1_frame", [151,41,328,254]),
                                     ("q2_frame", [477,41,328,254]),
                                     ("q3_frame", [799,41,328,254])]:
                    self.assert_rect(panels[name]["rect"],target,width,height)
                radar = {row["name"]: row for row in data["radar_hud"]["controls"]}
                self.assertTrue(radar["radar_galactic"]["visible"])
                normal_rect = radar["radar_galactic"]["rect"]
                self.assert_rect(normal_rect,[14,545,166,165],width,height)
                for actual, target in zip(normal_rect,[13.675,544.554,165.775,165.266]):
                    self.assertAlmostEqual(actual,target * height / 720,delta=.01 * height / 720,
                                           msg="normal radar moved from its unchanged command-bar anchor")

    def test_advanced_radar_and_zoom_geometry(self):
        for width, height in [(1280,720), (1920,1080)]:
            with self.subTest(resolution=(width,height)):
                data, _ = self.capture(width,height,advanced=True)
                self.assertTrue(data["radar_hud"]["advanced"])
                controls = {row["name"]: row for row in data["radar_hud"]["controls"]}
                # GHUDL-01/04 debug-build/model projection targets, distinct
                # from the focused retail frames which show the normal radar.
                for name, target in [("radar_galactic_adv_map", [174,545,166,165]),
                                     ("b_zoom_adv_map", [175,688,19,19])]:
                    self.assertTrue(controls[name]["visible"])
                    self.assert_rect(controls[name]["rect"],target,width,height)


if __name__ == "__main__":
    unittest.main()
