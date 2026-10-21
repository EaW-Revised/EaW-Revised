"""GC names use authoritative locations and draw through the HUD canvas."""
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


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class PlanetHudRuntime(unittest.TestCase):
    def capture(self, campaign, *extra, engine_args=()):
        # Imported BASE/HEAD capture helpers keep their original verification policy.
        verify = isinstance(self, PlanetHudRuntime)
        verify_args = ("--eawr-galactic-hud-verify", "yes") if verify else ()
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-hud-"))
        report, capture = directory / "report.json", directory / "capture.png"
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", *engine_args,
            "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-galactic", campaign,
            *verify_args, "--eawr-game-root", GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report), "--eawr-capture", str(capture), *extra],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        if verify:
            self.assertGreater(data["hud_work"]["verify_checks"], 0)
            self.assertEqual(data["hud_work"]["verify_mismatches"], 0, data["hud_work"].get("verify_detail"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["status"], "captured")
        return data, capture

    def check_label_pixels(self, data, capture, expected_minimum=3):
        from tests.presentation.renderer.space_map_test_support import decode_png
        width, height, rows = decode_png(capture.read_bytes())
        tested = 0
        for label in data["hud"]["labels"]:
            x, y, w, h = label["rect"]
            if not label["visible"] or x < 0 or y < 0 or x + w >= width or y + h >= height:
                continue
            rgb = label["colour"]
            pixels = [pixel for row in rows[int(y):int(y + h + 1)]
                      for pixel in row[int(x):int(x + w + 1)]]
            matching = sum(all(abs(pixel[channel] - rgb[channel]) <= 16 for channel in range(3))
                           for pixel in pixels)
            self.assertGreaterEqual(matching, 6, f"name has no coloured glyph pixels: {label}")
            tested += 1
        self.assertGreaterEqual(tested, expected_minimum)

    def test_campaign_names_exclude_art_and_clear_unpublished_facts(self):
        data, capture = self.capture("Sandbox_Equal_Footing_Underworld")
        hud = data["hud"]
        self.assertEqual(hud["planet_source"], "GalacticSnapshot")
        self.assertEqual(hud["production_source"], "unavailable")
        self.assertTrue(hud["selected_income"].startswith("$ "))
        planets = {planet["id"] for planet in data["planets"] if planet["layer"] == "world"}
        self.assertEqual(hud["observer_facts"], len(planets))
        facts = {row["planet"]: row for row in hud["intelligence"]}
        self.assertEqual(set(facts), planets)
        self.assertEqual(data["map_input"]["discovery_source"], "published-local-faction-observer")
        discovered = {planet for planet, fact in facts.items() if fact["discovered"]}
        # WGUI-01/04/107: installed starting owners expand discovery even paused.
        # Retail Equal Footing day-1 captures (GC-DISC-01; out/gc-4164/retail.md) show
        # full nearby planets and grid shells together, with known owner colours.
        self.assertIn(hud["selected"], discovered)
        self.assertGreater(len(discovered), 1)
        self.assertLess(len(discovered), len(planets), "discovery must retain the unscouted systems")
        self.assertNotEqual(hud["selected"], 0)
        # WGUI-38: every named planet keeps its name; undiscovered ones in the neutral colour.
        named_world = {planet["id"] for planet in data["planets"] if planet["layer"] == "world" and planet["show_name"]}
        self.assertEqual({label["planet"] for label in hud["labels"]}, named_world)
        self.assertEqual({label["planet"] for label in hud["labels"] if label["visible"]}, named_world)
        for planet, fact in facts.items():
            if planet not in discovered:
                self.assertEqual(fact["name"], "")
                for field in ["owner", "income", "ground", "space", "structures", "control"]:
                    self.assertIsNone(fact[field], (planet, field))
        hidden = [planet for planet in data["planets"] if not planet["show_name"]]
        self.assertEqual(len(hidden), 1, "stock campaign must contain the unnamed galaxy core")
        self.assertEqual(hidden[0]["name"].upper(), "GALAXY_CORE_ART_MODEL")
        named = {planet["id"] for planet in data["planets"] if planet["show_name"]}
        self.assertTrue(discovered)
        self.assertTrue(discovered <= named, "admitted normal planets must retain their names")
        self.assertNotIn(hidden[0]["id"], {label["planet"] for label in hud["labels"]},
                         "unnamed art must have neither a label nor its native hover control")
        colours = {planet["id"]: planet["colour"] for planet in data["planets"]}
        for label in hud["labels"]:
            self.assertEqual(label["text"], label["text"].upper())
            self.assertNotIn("GALAXY_CORE_ART", label["text"])
            fact = facts[label["planet"]]
            expected = colours[label["planet"]] if fact["discovered"] and fact["owner"] is not None else [100, 100, 100]
            self.assertEqual(label["colour"], expected, "name tint requires discovery and published ownership")
        self.check_label_pixels(data, capture, expected_minimum=1)

        # GC-DISC-01 (out/gc-4164/retail.md) is a Rebel capture. Keep the
        # original Underworld case above, and pin the matching retail setup:
        # Bespin/Mustafar are textured and red; Corulag remains a grey shell.
        retail, _ = self.capture("Sandbox_Equal_Footing_Rebel")
        retail_planets = {planet["name"]: planet for planet in retail["planets"]}
        retail_facts = {row["planet"]: row for row in retail["hud"]["intelligence"]}
        retail_labels = {row["planet"]: row for row in retail["hud"]["labels"]}
        for name, known in (("Bespin", True), ("Mustafar", True), ("Corulag", False)):
            with self.subTest(retail_planet=name):
                planet = retail_planets[name]
                identity = planet["id"]
                self.assertEqual(retail_facts[identity]["discovered"], known)
                self.assertEqual(planet["drawn"], "model")
                self.assertIn(identity, retail["shell_models"])
                self.assertEqual(identity in retail["alternate2_planets"], not known)
                self.assertEqual(identity in retail["shell_drawn_planets"], not known)
                self.assertEqual(retail_labels[identity]["colour"], planet["colour"] if known else [100, 100, 100])
                if known:
                    self.assertEqual(planet["faction"], "Rebel")
                    self.assertEqual(retail_facts[identity]["owner"], planet["owner"])

    def test_name_sits_right_of_the_planet_on_its_centre_row(self):
        # GHUDL-15 / WGUI-96: the name is left justified at one authored X
        # offset from the projected planet point and centred on that row.
        data, _ = self.capture("Sandbox_Equal_Footing_Underworld")
        anchored = [label for label in data["hud"]["labels"] if label["visible"] and "anchor" in label]
        self.assertTrue(anchored)
        offsets = set()
        for label in anchored:
            x, y, _, h = label["rect"]
            anchor_x, anchor_y = label["anchor"]
            self.assertGreater(x, anchor_x, label)
            self.assertLessEqual(abs(y + h / 2 - anchor_y), 1.0, f"name is off the planet's centre row: {label}")
            offsets.add(round(x - anchor_x, 2))
        self.assertEqual(len(offsets), 1, f"names must share one authored offset: {offsets}")

    def test_status_icons_sit_under_the_name(self):
        data, _ = self.capture("Sandbox_Equal_Footing_Underworld")
        labels = {label["planet"]: label for label in data["hud"]["labels"]}
        for icon in data["hud"]["status_icons"]:
            if not icon["visible"] or icon["planet"] not in labels or "anchor" not in icon:
                continue
            x, y, w, h = icon["rect"]
            name = labels[icon["planet"]]
            self.assertLessEqual(abs(x - name["rect"][0]), 1.0, f"icon row starts at the name's left edge: {icon}")
            self.assertGreater(y + h / 2, icon["anchor"][1], f"status icons sit below the planet row: {icon}")
        # Debug build: a planet owned by a corruption-benefiting faction is
        # corrupted, so the information icon never marks the local home.
        facts = {row["planet"]: row for row in data["hud"]["intelligence"]}
        home = data["hud"]["selected"]
        self.assertNotEqual(home, 0)
        self.assertIsNotNone(facts[home]["owner"])
        self.assertFalse(any(icon["planet"] == home and icon["kind"] == "corruption_info" and icon["visible"]
                             for icon in data["hud"]["status_icons"]))
        self.assertTrue(any(icon["kind"] == "corruption_info" and icon["visible"]
                            for icon in data["hud"]["status_icons"]), "foreign planets keep the information icon")

    def test_synthetic_observer_fixture_uses_faction_colours(self):
        data, capture = self.capture("synthetic")
        planets = {planet["id"]: planet for planet in data["planets"]}
        for label in data["hud"]["labels"]:
            self.assertEqual(label["colour"], planets[label["planet"]]["colour"])
        self.check_label_pixels(data, capture)

    def test_only_transit_fleets_have_world_models(self):
        data, _ = self.capture("synthetic", "--eawr-galactic-hud-fixture", "fleet-transit")
        moving = [row for row in data["fleets"] if row["transit"]]
        stationed = [row for row in data["fleets"] if not row["transit"]]
        self.assertEqual(len(moving), 1)
        self.assertTrue(stationed)
        self.assertEqual(moving[0]["fleet"], 601)
        self.assertEqual(moving[0]["destination"], 102)
        self.assertEqual(len(moving[0]["facing"]), 3)
        source = next(row for row in data["planets"] if row["id"] == 101)
        self.assertNotEqual(moving[0]["position"], source["position"])
        # #3071: the model turns to the facing, which points along the hop.
        target = next(row for row in data["planets"] if row["id"] == 102)["position"]
        dx, dy = target[0] - moving[0]["position"][0], target[1] - moving[0]["position"][1]
        fx, fy = moving[0]["facing"][0], moving[0]["facing"][1]
        self.assertGreater((fx * dx + fy * dy) / (math.hypot(fx, fy) * math.hypot(dx, dy)), 0.99)
        self.assertGreater(moving[0]["world_model_instances"], 0)
        self.assertTrue(all(row["world_model_instances"] == 0 for row in stationed))
        self.assertTrue(any(row["visible"] for row in data["fleet_hud"]["presence"]))

    def test_command_bar_opaque_pixels_occlude_late_planet_overlay(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-occlusion-"))
        driver = directory / "driver.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var label
var shell
var output = %s
func _initialize():
    root.borderless = true
    root.position = Vector2i.ZERO
    root.size = Vector2i(1280, 720)
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(_delta):
    frame += 1
    if frame == 120:
        current_scene.set_process(false)
        label = root.find_child("planet_name_101", true, false)
        assert(label != null)
        shell = root.find_child("Text_Credits", true, false).get_parent().get_parent()
        label.text = "MMMMMMMMMMMM"
        label.add_theme_color_override("font_color", Color(1, 0, 1))
        label.position = Vector2(350, 680)
        label.show()
    if frame == 125:
        root.get_texture().get_image().save_png(output + "/overlay.png")
        label.hide()
    if frame == 130:
        root.get_texture().get_image().save_png(output + "/hidden.png")
        label.show()
        shell.hide()
    if frame == 135:
        root.get_texture().get_image().save_png(output + "/exposed.png")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % json.dumps(directory.as_posix()), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "production",
            "--eawr-galactic-hud-verify", "yes", "--eawr-game-root", GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(directory / "report.json")],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        work = json.loads((directory / "report.json").read_text(encoding="utf-8"))["hud_work"]
        self.assertGreater(work["verify_checks"], 0)
        self.assertEqual(work["verify_mismatches"], 0, work.get("verify_detail"))
        from tests.presentation.renderer.space_map_test_support import decode_png
        images = {name: decode_png((directory / (name + ".png")).read_bytes())[2]
                  for name in ["overlay", "hidden", "exposed"]}
        # Keep the map/animation outside this opaque production-shell interior
        # out of the comparison; the exposed control proves the glyphs exist.
        crop = lambda rows: [pixel for row in rows[680:696] for pixel in row[350:445]]
        self.assertEqual(crop(images["overlay"]), crop(images["hidden"]))
        self.assertGreater(sum(pixel[0] > 180 and pixel[1] < 80 and pixel[2] > 180
                               for pixel in crop(images["exposed"])), 20)

    def test_focused_view_hides_overview_labels(self):
        overview, _ = self.capture("Sandbox_Equal_Footing_Underworld")
        home = overview["hud"]["selected"]
        self.assertNotEqual(home, 0)
        self.assertTrue(any(label["planet"] == home and label["visible"]
                            for label in overview["hud"]["labels"]))
        # The neutral layout discovers only its authored home. Focus that
        # published planet so this tests fading an admitted overview label.
        data, _ = self.capture("Sandbox_Equal_Footing_Underworld",
                               "--eawr-galactic-focus", str(home), "--eawr-galactic-flight-ms", "750")
        self.assertEqual(data["flights"][0]["state"], "focused")
        self.assertGreater(len(data["hud"]["labels"]), 0)
        self.assertTrue(all(not label["visible"] and label["alpha"] == 0 for label in data["hud"]["labels"]))
        if data["hud"]["selected"]:
            self.assertTrue(data["hud"]["selected_income"].startswith("$ "))
        else:
            self.assertEqual(data["hud"]["selected_income"], "")


if __name__ == "__main__":
    unittest.main()
