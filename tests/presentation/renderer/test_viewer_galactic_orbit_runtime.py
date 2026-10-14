"""GHUD-S78/S79: focused station levels and authored orbital pads."""
import unittest
import math
import json
import os
import pathlib
import subprocess
import tempfile

try:
    from . import test_viewer_planet_hud_runtime as support
    from . import test_viewer_galactic_fleet_runtime as fleets
except ImportError:
    import test_viewer_planet_hud_runtime as support
    import test_viewer_galactic_fleet_runtime as fleets


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticOrbitRuntime(unittest.TestCase):
    def test_station_rotation_uses_admitted_display_frames_and_mod_rate(self):
        import xml.etree.ElementTree as ET
        from PIL import Image, ImageChops
        from tools.inventory.corpus import Corpus
        source = Corpus(support.GAME_ROOT).read_effective("foc", "data/xml/gameconstants.xml").data
        samples = []
        # A 20-second hitch admits one frame. Zero and 1ms calls afterward
        # admit none; the next 32ms and 33ms calls each admit one. The large
        # authored rates make the three small display deltas visible in pixels.
        for rate, zero in ((20.0, False), (-20.0, False), (0.0, False), (20.0, True)):
            with self.subTest(rate=rate, zero_delta=zero):
                with tempfile.TemporaryDirectory(prefix="eawr-station-rate-") as temporary:
                    mod = pathlib.Path(temporary)
                    xml = mod / "Data" / "XML"
                    xml.mkdir(parents=True)
                    (xml.parent / "MegaFiles.xml").write_text(
                        "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
                    constants = ET.fromstring(source)
                    constants.find("Galactic_Zoom_In_Station_Rotation").text = str(rate)
                    (xml / "GameConstants.xml").write_bytes(ET.tostring(constants))
                    fixture = "orbit-rotation-zero" if zero else "orbit-rotation-advance"
                    data, capture = support.PlanetHudRuntime.capture(self, "synthetic",
                        "--eawr-mod-root", str(mod), "--eawr-galactic-hud-fixture", fixture,
                        "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750")
                orbit = data["orbit_hud"]
                self.assertTrue(orbit["station_visible"])
                self.assertGreater(orbit["station_surfaces"], 0)
                self.assertEqual(orbit["rotation_rate"], rate)
                self.assertEqual(orbit["rotation_probe_advances"], [0]*5 if zero else [1, 0, 0, 1, 1])
                self.assertAlmostEqual(orbit["station_rotation"], 0 if zero else 3 * 30 / 1000 * rate)
                self.assertEqual(orbit["rotation_updates"], 0 if zero else 3)
                self.assertEqual(orbit["display_delta"], 0, "later zero inputs changed the pose")
                samples.append((data, capture))
        self.assertTrue(all(sample[0]["orbit_hud"]["station_position"] ==
                            samples[0][0]["orbit_hud"]["station_position"] for sample in samples))
        # Inspect actual rendered station pixels, not only its report angle.
        rects = [sample[0]["orbit_hud"]["station_rect"] for sample in samples]
        box = (math.floor(min(r[0] for r in rects)), math.floor(min(r[1] for r in rects)),
               math.ceil(max(r[0] + r[2] for r in rects)), math.ceil(max(r[1] + r[3] for r in rects)))
        images = []
        for _, capture in samples:
            with Image.open(capture) as image:
                images.append(image.convert("RGB").crop(box))
        for rotated in images[:2]:
            diff = ImageChops.difference(rotated, images[2])
            self.assertGreater(sum(max(pixel) > 8 for pixel in diff.getdata()), 100)
        self.assertIsNone(ImageChops.difference(images[2], images[3]).getbbox())

    def capture(self, fixture, focused=True):
        data, capture = fleets.GalacticFleetRuntime.capture(self, fixture, focused)
        if fixture.startswith("orbit-placement-"):
            (capture.parent / "placement-case.json").write_text(
                json.dumps({"fixture": fixture, "focused": focused}), encoding="utf-8")
        return data, capture

    def test_station_attachment_inherits_yavin_and_manaan_world_transform(self):
        from PIL import Image, ImageChops
        # One paired overview control uses the exact former planet matrix,
        # keeping camera, assets and clock identical for the eye comparison.
        before, before_path = self.capture("orbit-placement-yavin-overview-before", focused=False)
        after, after_path = self.capture("orbit-placement-yavin-overview", focused=False)
        self.assertEqual(before["camera"], after["camera"])
        self.assertFalse(before["orbit_hud"]["station_visible"])
        self.assertFalse(after["orbit_hud"]["station_visible"])
        self.assertTrue(before_path.is_file() and after_path.is_file())
        # GHUD-S78: both authored model fixtures attach station_ALT1 directly
        # to the root. R-ROT-01's +Z quarter turn maps (x,y,z) to (-y,x,z),
        # then the planet scale applies, while the station mesh stays unit scale.
        anchor = (-1.3940496444702148, -4.506401538848877, 8.296165466308594)
        for name in ("yavin", "manaan"):
            with self.subTest(planet=name):
                empty, empty_path = self.capture(f"orbit-placement-{name}-empty")
                data, capture = self.capture(f"orbit-placement-{name}")
                planet = next(row for row in data["planets"] if row["id"] == 101)
                orbit = data["orbit_hud"]
                self.assertFalse(empty["orbit_hud"]["station_visible"])
                self.assertTrue(orbit["station_visible"])
                for actual, origin, offset in zip(orbit["station_position"], planet["position"], anchor):
                    # The viewer report uses six significant digits. Compare
                    # the exact serialized expectation rather than inventing
                    # precision that this report does not publish.
                    expected = float(format(origin + offset * planet["scale"], ".6g"))
                    self.assertEqual(actual, expected)
                self.assertIsNotNone(orbit["station_rect"])
                x, y, w, h = orbit["station_rect"]
                self.assertGreater(w, 0)
                self.assertGreater(h, 0)
                self.assertGreaterEqual(x, 0)
                self.assertGreaterEqual(y, 0)
                self.assertLessEqual(x + w, 1280)
                self.assertLessEqual(y + h, 720)
                box = (math.floor(x), math.floor(y), math.ceil(x + w), math.ceil(y + h))
                with Image.open(empty_path) as image:
                    background = image.convert("RGB").crop(box)
                with Image.open(capture) as image:
                    difference = ImageChops.difference(image.convert("RGB").crop(box), background)
                    if name == "yavin":
                        # The authored focused moon belongs at the central
                        # planet region seen in native, ahead of the gas giant.
                        # An unrotated planet instance leaves orange gas here.
                        moon = image.convert("RGB").crop((500, 350, 780, 540))
                        self.assertGreater(sum(g > r + 10 and b > r + 5 and max(r, g, b) > 30
                                               for r, g, b in moon.getdata()), 1000)
                self.assertGreater(sum(max(pixel) > 8 for pixel in difference.getdata()), 100)

    def test_current_station_levels_zero_through_five(self):
        from PIL import Image, ImageChops
        models = set()
        empty = None
        for level in range(6):
            with self.subTest(level=level):
                data, capture = self.capture(f"orbit-level-{level}")
                orbit = data["orbit_hud"]
                self.assertEqual(orbit["station_visible"], level != 0)
                self.assertEqual(orbit["station_surfaces"] > 0, level != 0)
                if level:
                    self.assertIn(f"level{level}.alo", orbit["model"].lower())
                    models.add(orbit["model"])
                    self.assertNotEqual(orbit["station_position"], [0, 0, 0])
                    # Submitted surfaces alone can be entirely outside the viewport.
                    # Use projected asset bounds, then require actual station pixels beside
                    # the pads, compared with the identical paused level-zero fixture.
                    self.assertIsNotNone(orbit["station_rect"])
                    x, y, w, h = orbit["station_rect"]
                    self.assertGreater(w, 0)
                    self.assertGreater(h, 0)
                    self.assertGreaterEqual(x, 0)
                    self.assertGreaterEqual(y, 0)
                    self.assertLessEqual(x + w, 1280)
                    self.assertLessEqual(y + h, 720)
                    box = (math.floor(x), math.floor(y), math.ceil(x + w), math.ceil(y + h))
                    with Image.open(capture) as image:
                        difference = ImageChops.difference(image.convert("RGB").crop(box), empty.crop(box))
                    self.assertGreater(sum(max(pixel) > 8 for pixel in difference.getdata()), 100)
                else:
                    with Image.open(capture) as image:
                        empty = image.convert("RGB").copy()
                slots = orbit["slots"]
                self.assertEqual(len(slots), 2)
                for slot in slots:
                    self.assertTrue(fleets.GalacticFleetRuntime.drawn(slot), slot)
                    self.assertEqual(slot["object"], 0)
        self.assertEqual(len(models), 5)
        neutral, _ = self.capture("orbit-neutral")
        self.assertTrue(neutral["orbit_hud"]["station_visible"])
        self.assertGreater(neutral["orbit_hud"]["station_surfaces"], 0)
        self.assertIn("i_pirate_station.alo", neutral["orbit_hud"]["model"].lower())

    def native_station_capture(self, fixture):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-orbit-cursor-"))
        report, capture, driver = directory / "report.json", directory / "capture.png", directory / "driver.gd"
        # Startup --focus skips the overview cursor. Use real navigation and
        # wheel input so the station follows an already tinted cursor instance.
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var phase = 0
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(code):
    for pressed in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = pressed
        Input.parse_input_event(event)
func wheel():
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_WHEEL_UP
        event.pressed = pressed
        event.position = Vector2(1200, 400)
        event.global_position = event.position
        Input.parse_input_event(event)
func _process(delta):
    elapsed += delta
    if phase == 0 and elapsed >= 1.0:
        key(KEY_PAGEUP)
        phase = 1
        elapsed = 0.0
    elif phase == 1 and elapsed >= 0.2:
        key(KEY_PAGEDOWN)
        phase = 2
        elapsed = 0.0
    elif phase == 2 and elapsed >= 0.2:
        wheel()
        phase = 3
        elapsed = 0.0
    elif phase == 3 and elapsed >= 1.5:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % json.dumps(str(capture).replace("\\", "/")), encoding="utf-8")
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report)], cwd=support.ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        self.assertTrue(capture.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"], 101)
        self.assertEqual(data["map_input"]["selection_invalidations"], 2)
        self.assertFalse(data["selection_cursor"]["shown"])
        return data, capture

    def test_station_textures_survive_cursor_tint_for_each_faction(self):
        from PIL import Image, ImageChops
        for planet in ["rebel", "empire", "underworld"]:
            with self.subTest(planet=planet):
                empty, empty_path = self.native_station_capture(f"orbit-look-{planet}-empty")
                red, red_path = self.native_station_capture(f"orbit-look-{planet}-red")
                blue, blue_path = self.native_station_capture(f"orbit-look-{planet}-blue")
                self.assertFalse(empty["orbit_hud"]["station_visible"])
                orbit = red["orbit_hud"]
                self.assertTrue(orbit["station_visible"])
                self.assertEqual(orbit["station_rect"], blue["orbit_hud"]["station_rect"])
                x, y, w, h = orbit["station_rect"]
                box = (max(0, math.floor(x)), max(0, math.floor(y)),
                       min(1280, math.ceil(x + w)), min(720, math.ceil(y + h)))
                self.assertGreater(box[2], box[0])
                self.assertGreater(box[3], box[1])
                with Image.open(empty_path) as image:
                    background = image.convert("RGB").crop(box)
                with Image.open(red_path) as image:
                    hull = image.convert("RGB").crop(box)
                with Image.open(blue_path) as image:
                    other = image.convert("RGB").crop(box)
                changed = ImageChops.difference(hull, background)
                pixels = [rgb for rgb, delta in zip(hull.getdata(), changed.getdata())
                          if max(delta) > 8]
                self.assertGreater(len(pixels), 100, "no station pixels")
                # The hull retains neutral texture detail; red/blue cursor light
                # must not multiply all pixels of a newly inserted station.
                neutral = sum(max(rgb) > 30 and max(rgb) - min(rgb) < 35 for rgb in pixels)
                self.assertGreater(neutral, 30, "hull is entirely faction tinted")
                tinted = ImageChops.difference(hull, other)
                self.assertLess(sum(max(rgb) > 8 for rgb, delta in
                                    zip(tinted.getdata(), changed.getdata()) if max(delta) > 8), 10,
                                "station inherited the cursor's instance light scale")

    def test_orbital_structures_use_creation_order_and_authored_rects(self):
        data, capture = self.capture("orbit-filled")
        slots = data["orbit_hud"]["slots"]
        self.assertEqual([slot["object"] for slot in slots], [9201, 9202])
        self.assertEqual([slot["type"] for slot in slots], [1004, 1005])
        self.assertTrue(capture.is_file())
        self.assertNotEqual(slots[0]["rect"], slots[1]["rect"])
        self.assertTrue(all(fleets.GalacticFleetRuntime.drawn(slot) for slot in slots))

    def test_unknown_base_and_structures_do_not_leak(self):
        data, _ = self.capture("orbit-hidden")
        orbit = data["orbit_hud"]
        self.assertFalse(orbit["station_visible"])
        self.assertEqual(orbit["station_surfaces"], 0)
        self.assertFalse(orbit["structures_known"])
        self.assertTrue(all(slot["object"] == 0 for slot in orbit["slots"]))
        self.assertTrue(all(slot["alpha"] == 1 for slot in orbit["slots"]))

    def test_third_authored_pad_and_overview_lifecycle(self):
        data, _ = self.capture("orbit-extra")
        slots = data["orbit_hud"]["slots"]
        self.assertEqual([slot["index"] for slot in slots], [0, 1, 2])
        self.assertEqual(slots[2]["name"], "space_Pad_03")
        self.assertTrue(all(fleets.GalacticFleetRuntime.drawn(slot) for slot in slots))
        overview, _ = self.capture("orbit-level-3", focused=False)
        self.assertFalse(overview["orbit_hud"]["station_visible"])
        self.assertEqual(overview["orbit_hud"]["station_surfaces"], 0)
        self.assertFalse(any(slot["visible"] for slot in overview["orbit_hud"]["slots"]))


if __name__ == "__main__":
    unittest.main()
