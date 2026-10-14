"""GHUD-S10: native radar/zoom admission, fixed scale and no world leak."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticRadarRuntime(unittest.TestCase):
    def capture(self, focused=False, fixture="fleet-count"):
        extra = ["--eawr-galactic-hud-fixture", fixture]
        if focused:
            extra += ["--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750"]
        return support.PlanetHudRuntime.capture(self, "synthetic", *extra)

    @staticmethod
    def control(data, name, u=.5, v=.5):
        row = next(row for row in data["radar_hud"]["controls"] if row["name"] == name)
        x, y, width, height = row["rect"]
        return [x + width * u, y + height * v]

    def native_run(self, events, focused=False, fixture="fleet-count", sample_times=()):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-radar-clicks-"))
        report, capture, driver = directory / "report.json", directory / "capture.png", directory / "driver.gd"
        # Wall-time scheduling lets the real camera finish its 0.5-second flight
        # even when the GPU renders faster than the old frame-count driver.
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var next_event = 0
var events = %s
var sample_times = %s
var next_sample = 0
var sample_root = %s
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if next_sample < sample_times.size() and elapsed >= sample_times[next_sample]:
        root.get_texture().get_image().save_png(sample_root + "/sample_" + str(next_sample) + ".png")
        next_sample += 1
    if next_event < events.size() and elapsed >= events[next_event][0]:
        var row = events[next_event]
        var position = Vector2(row[1][0], row[1][1])
        var motion = InputEventMouseMotion.new()
        motion.position = position
        motion.global_position = position
        Input.parse_input_event(motion)
        var event = InputEventMouseButton.new()
        event.position = position
        event.global_position = position
        event.button_index = row[2]
        event.pressed = row[3]
        Input.parse_input_event(event)
        next_event += 1
    if elapsed >= %s:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(events), json.dumps(sample_times), json.dumps(str(directory).replace("\\", "/")),
       max(events[-1][0] + 1.0, sample_times[-1] + .2 if sample_times else 0),
       json.dumps(str(capture).replace("\\", "/"))), encoding="utf-8")
        # JSON booleans are valid GDScript literals.
        extra = ["--eawr-galactic-focus", "101"] if focused else []
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report), *extra], cwd=support.ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"], 101, "HUD pointer leaked into world picking")
        self.assertEqual(data["live_session"]["submitted_orders"], 1, "radar/zoom must not issue simulation orders")
        self.assertTrue(capture.is_file())
        if sample_times:
            from tests.presentation.renderer.space_map_test_support import decode_png
            selected = [row for row in data["radar_hud"]["blips"]
                        if row["planet"] == 101 and row["radar"] == 0]
            self.assertEqual(len(selected), 1)
            row = selected[0]
            self.assertTrue(row["visible"])
            self.assertTrue(row["base_icon"], "selection must retain the planet icon")
            self.assertEqual(row["overlay"], "selected")
            self.assertEqual(row["alpha"], 255)
            x, y, width, height = row["rect"]
            sx, sy, sw, sh = row["selected_rect"]
            # Mounted stock atlas: the 16-texel selected quad surrounds the
            # retained 12-texel dot; each quad keeps its own natural dimensions.
            self.assertAlmostEqual(sw / width, 16 / 12, places=4)
            self.assertAlmostEqual(sh / height, 16 / 12, places=4)
            self.assertAlmostEqual(sx + sw / 2, x + width / 2, places=4)
            self.assertAlmostEqual(sy + sh / 2, y + height / 2, places=4)
            # The ring can be translucent; the retained centre glyph must stay
            # bright throughout the hold, including while simulation is paused.
            channel = max(range(3), key=lambda channel: row["colour"][channel])
            for index, when in enumerate(sample_times):
                sample = directory / f"sample_{index}.png"
                self.assertTrue(sample.is_file(), f"missing sample at {when}s")
                _, _, pixels = decode_png(sample.read_bytes())
                centre = [pixel for line in pixels[int(y + height * .25):int(y + height * .75)]
                          for pixel in line[int(x + width * .25):int(x + width * .75)]]
                self.assertGreaterEqual(sum(pixel[channel] >= row["colour"][channel] * .7
                                            for pixel in centre), 4,
                                        f"selected radar centre faded at {when}s: {row}")
                # Stock selected art contains opaque (0,120,232) ring texels.
                # Its blue highlight must retain its RGB instead of receiving
                # the base dot's faction tint, including outside the base box.
                ring = [pixels[py][px]
                        for py in range(int(sy), int(sy + sh))
                        for px in range(int(sx), int(sx + sw))
                        if not (x <= px < x + width and y <= py < y + height)]
                self.assertGreaterEqual(sum(pixel[1] >= 100 and pixel[2] >= 190 for pixel in ring), 4,
                                        f"selected overlay lost authored brightness at {when}s")
        return data

    def test_selected_planet_retains_bright_base_icon_over_time(self):
        initial, _ = self.capture(fixture="known")
        point = self.control(initial, "radar_galactic", .75, .25)
        self.native_run([[.2, point, 1, True], [.25, point, 1, False]], fixture="known",
                        sample_times=[.5, .75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0])

    def test_discovery_filtered_blips_and_fixed_native_mapping(self):
        initial, _ = self.capture()
        rows = {row["planet"]: row for row in initial["radar_hud"]["blips"]}
        self.assertTrue(rows[101]["visible"])
        self.assertTrue(rows[103]["visible"])
        self.assertEqual(rows[103]["alpha"], 128)
        point = self.control(initial, "radar_galactic", .75, .25)
        data = self.native_run([[1.0, point, 1, True], [1.05, point, 1, False]])
        self.assertEqual(data["radar_hud"]["positions"], 1)
        for actual, expected in zip(data["radar_hud"]["last_position"], [190, 230, 10]):
            self.assertAlmostEqual(actual, expected, delta=.01)
        self.assertFalse(data["radar_hud"]["advanced"])
        self.assertEqual(data["map_input"]["selection_invalidations"], initial["map_input"]["selection_invalidations"])

    def test_native_zoom_toggle_focus_then_outward(self):
        initial, _ = self.capture()
        point = self.control(initial, "b_zoom")
        data = self.native_run([[1.0, point, 1, True], [1.05, point, 1, False],
            [2.0, point, 1, True], [2.05, point, 1, False]])
        self.assertEqual(data["radar_hud"]["zoom_requests"], 2)
        self.assertEqual(data["flights"][0]["state"], "overview")
        self.assertEqual(data["map_input"]["selection_invalidations"], initial["map_input"]["selection_invalidations"])

    def test_discovery_alpha_and_ownership_tint_are_independent(self):
        for focused in (False, True):
            for fixture in ("known", "masked"):
                initial, _ = self.capture(focused=focused, fixture=fixture)
                for row in initial["radar_hud"]["blips"]:
                    self.assertTrue(row["visible"])
                    self.assertEqual(row["alpha"], 128 if row["planet"] == 103 else 255)
                    expected = [100, 100, 100]
                    if fixture == "known" and row["planet"] != 103:
                        fact = next(fact for fact in initial["hud"]["intelligence"] if fact["planet"] == row["planet"])
                        self.assertIsNotNone(fact["owner"])
                        # Fixture owner 1 resolves through the mounted metadata,
                        # independently of each planet's actual placement owner.
                        expected = next(planet["colour"] for planet in initial["planets"] if planet["id"] == 101)
                    self.assertEqual(row["colour"], expected)
                blip = next(row for row in initial["radar_hud"]["blips"]
                            if row["planet"] == 103 and row["radar"] == 0)
                x, y, width, height = blip["rect"]
                point = [x + width / 2, y + height / 2]
                data = self.native_run([[1.0, point, 1, True], [1.05, point, 1, False]],
                                       focused=focused, fixture=fixture)
                self.assertEqual(data["map_input"]["selection_invalidations"], initial["map_input"]["selection_invalidations"])
                self.assertEqual(data["radar_hud"]["positions"], 1, "dim glyph retains background positioning")

    def test_native_advanced_radar_retains_overlay_then_zoom_closes_it(self):
        initial, _ = self.capture(focused=True)
        normal = self.control(initial, "radar_galactic")
        advanced = self.control(initial, "radar_galactic_adv_map", .25, .75)
        zoom = self.control(initial, "b_zoom_adv_map")
        # A focused wheel-in opens the advanced map; click positions it and the
        # authored outward button closes only the overlay, retaining focus.
        data = self.native_run([[1.0, normal, 4, True], [1.05, normal, 4, False],
            [1.4, advanced, 1, True], [1.45, advanced, 1, False],
            [1.8, zoom, 1, True], [1.85, zoom, 1, False]], focused=True)
        self.assertEqual(data["radar_hud"]["positions"], 1)
        self.assertEqual(data["radar_hud"]["zoom_requests"], 2)
        self.assertFalse(data["radar_hud"]["advanced"])
        self.assertEqual(data["flights"][0]["state"], "focused")
        for actual, expected in zip(data["radar_hud"]["last_position"], [-190, -230, 10]):
            self.assertAlmostEqual(actual, expected, delta=.01)

    def test_tutorial_lock_consumes_radar_and_zoom_clicks(self):
        initial, _ = self.capture(fixture="radar-locked")
        radar, zoom = self.control(initial, "radar_galactic"), self.control(initial, "b_zoom")
        data = self.native_run([[1.0, radar, 1, True], [1.05, radar, 1, False],
            [1.2, radar, 4, True], [1.25, radar, 4, False],
            [1.4, zoom, 1, True], [1.45, zoom, 1, False]], fixture="radar-locked")
        self.assertEqual(data["radar_hud"]["positions"], 0)
        self.assertEqual(data["radar_hud"]["zoom_requests"], 0)
        self.assertEqual(data["map_input"]["selection_invalidations"], initial["map_input"]["selection_invalidations"])

    def test_wheel_is_suppressed_during_held_scroll_and_release_clears_it(self):
        initial, _ = self.capture()
        radar = self.control(initial, "radar_galactic")
        data = self.native_run([[1.0, [640, 100], 2, True],
            [1.2, radar, 4, True], [1.25, radar, 4, False], [1.4, radar, 2, False],
            [1.6, radar, 4, True], [1.65, radar, 4, False]])
        self.assertEqual(data["radar_hud"]["zoom_requests"], 1)
        self.assertEqual(data["flights"][0]["state"], "focused")


if __name__ == "__main__":
    unittest.main()
