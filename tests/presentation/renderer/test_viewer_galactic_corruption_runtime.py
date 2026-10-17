"""WGP-19..21: lit particles, spreading paths, replacement and teardown."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the GPU route and installed FoC data")
class GalacticCorruptionRuntime(unittest.TestCase):
    def capture_fixture(self, fixture):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-corruption-"))
        report, capture = directory / (fixture + ".json"), directory / (fixture + ".png")
        driver = directory / (fixture + ".gd")
        driver.write_text('''extends SceneTree
var started: int = 0
var captured: bool = false
func _initialize() -> void:
    call_deferred("_start_preview")
func _start_preview() -> void:
    var scene: PackedScene = load(ProjectSettings.get_setting("application/run/main_scene"))
    var viewer := scene.instantiate()
    root.add_child(viewer)
    current_scene = viewer
    started = Time.get_ticks_msec()
    RenderingServer.frame_post_draw.connect(_frame_drawn)
func _frame_drawn() -> void:
    if captured or Time.get_ticks_msec() - started < 3000:
        return
    captured = true
    root.get_texture().get_image().save_png(CAPTURE)
    call_deferred("_close_preview")
func _close_preview() -> void:
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
'''.replace("CAPTURE", json.dumps(capture.as_posix())), encoding="utf-8")
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
            str(ROOT / "apps/viewer/project"), "--script", str(driver), "--", "--eawr-galactic", "synthetic",
            "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
            "--eawr-galactic-hud-fixture", fixture, "--eawr-galactic-paused", "no",
            *([] if fixture.startswith("corruption-spread") else ["--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750"]),
            "--eawr-hud", "off", "--eawr-lighting", "sh", "--eawr-environment", "map",
            "--eawr-shadows", "on", "--eawr-report", str(report)],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (directory / (fixture + ".log")).write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["status"], "closed", data.get("failure"))
        self.assertNotIn("ERROR:", log)
        return data, capture

    def test_admitted_particle_draws_in_midground(self):
        from PIL import Image, ImageChops, ImageStat
        images = []
        for fixture in ("corruption-clean", "corruption"):
            data, capture = self.capture_fixture(fixture)
            facts = data["corruption"]
            self.assertEqual(facts["refused_effects"], 0)
            self.assertEqual(facts["line_refused_effects"], 0)
            self.assertEqual(facts["attachments"], int(fixture == "corruption"))
            self.assertEqual(facts["effects"], int(fixture == "corruption"))
            if fixture == "corruption":
                self.assertGreater(facts["resources"], 0)
                self.assertGreater(facts["particles"], 0)
            images.append(Image.open(capture).convert("RGB"))
        self.assertIsNotNone(ImageChops.difference(*images).getbbox(), "particle must change the drawn image")
        # This sky patch excludes the planet's animated surface and the HUD.
        # A changed cloud pose on the planet cannot stand in for particle output.
        sky = [ImageStat.Stat(image.crop((0, 32, 160, 300))).mean for image in images]
        delta = [corrupted - clean for clean, corrupted in zip(*sky)]
        self.assertGreater(sum(delta), 3.0, "settled corruption haze must draw in the surrounding sky")
        # Stock V1 RGB keys warm from grey to gold. C06's focused sky is olive/gold;
        # the former birth-order stream measured only R/B 1.4 and G/B 1.3 here.
        # Subtract the clean sky so a warm background cannot satisfy this check.
        self.assertGreater(delta[2], 1.0, "hue sample must contain drawn haze")
        self.assertGreater(delta[0] / delta[2], 1.8, "authored haze must retain warm red/blue ratio")
        self.assertGreater(delta[1] / delta[2], 1.6, "authored haze must retain warm green/blue ratio")


    def test_spreading_lines_paths_type_replacement_and_cleanup(self):
        from PIL import Image, ImageChops
        captures = {}
        for fixture in ("corruption-spread", "corruption-spread-settled", "corruption-spread-replaced", "corruption-spread-removed"):
            data, capture = self.capture_fixture(fixture)
            facts = data["corruption"]
            self.assertEqual(facts["refused_effects"], 0)
            self.assertEqual(facts["line_refused_effects"], 0)
            captures[fixture] = Image.open(capture).convert("RGB")
            if fixture.endswith("removed"):
                for key in ("attachments", "connections", "effects", "resources", "particles", "line_effects", "line_resources", "line_particles", "path_quads"):
                    self.assertEqual(facts[key], 0, (fixture, key))
                continue
            self.assertEqual(facts["attachments"], 3)
            self.assertEqual(facts["effects"], 3, "type replacement must release the old effect")
            self.assertGreater(facts["line_particles"], 0)
            self.assertGreater(facts["line_resources"], 0)
            self.assertEqual(facts["line_effects"], 2 if fixture.endswith("settled") else 1)
            self.assertEqual(facts["path_quads"], 0 if fixture.endswith("settled") else 1)
            if fixture == "corruption-spread":
                planet = next(row for row in facts["parameters"] if row["planet"] == 102)
                self.assertGreater(planet["fraction"], 0)
                self.assertLess(planet["fraction"], 1)
                self.assertAlmostEqual(planet["brightness"], 0.3+0.7*planet["fraction"], places=4)
                self.assertEqual(planet["size"], 1)
                self.assertEqual(planet["energy"], 1)
        self.assertIsNotNone(ImageChops.difference(captures["corruption-spread"], captures["corruption-spread-removed"]).getbbox(),
                             "spreading corruption must draw visible output")



try:
    from . import test_viewer_planet_hud_runtime as support
    from . import test_viewer_galactic_radar_runtime as native
except ImportError:
    import test_viewer_planet_hud_runtime as support
    import test_viewer_galactic_radar_runtime as native


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticCorruptionHudRuntime(unittest.TestCase):
    def capture(self, fixture="corruption"):
        return support.PlanetHudRuntime.capture(self, "synthetic", "--eawr-galactic-hud-fixture", fixture)

    @staticmethod
    def point(data, name):
        row = next(row for row in data["corruption_hud"]["controls"] if row["name"] == name)
        x, y, width, height = row["rect"]
        return [x + width / 2, y + height / 2]

    def click(self, points, fixture="corruption"):
        events = []
        for index, point in enumerate(points):
            events.extend([[1.0 + index * .3, point, 1, True], [1.05 + index * .3, point, 1, False]])
        return native.GalacticRadarRuntime.native_run(self, events, fixture=fixture)

    def test_authored_slots_independent_requirements_and_all_native_buttons(self):
        initial, _ = self.capture()
        hud = initial["corruption_hud"]
        self.assertTrue(hud["active"])
        self.assertEqual(hud["source"], "controlled-observer-snapshot")
        self.assertEqual([row["slot"] for row in hud["options"]], [1, 2, 3])
        first, expensive, unmet = hud["options"]
        self.assertTrue(all(first[key] for key in ["affordable", "units_satisfied", "missions_satisfied", "planets_satisfied"]))
        self.assertFalse(expensive["affordable"])
        self.assertTrue(expensive["missions_satisfied"])
        self.assertTrue(unmet["affordable"])
        self.assertFalse(unmet["units_satisfied"])
        self.assertFalse(unmet["missions_satisfied"])
        self.assertTrue(unmet["planets_satisfied"], "ANY global owned alternative passes")
        controls = {row["name"]: row for row in hud["controls"]}
        for name in ["corrupt_button_icon_a", "corrupt_button_icon_b", "corrupt_button_icon_c", "corrupt_close"]:
            self.assertIn(name, controls)
            x, y, width, height = controls[name]["rect"]
            self.assertGreater(width, 0)
            self.assertGreater(height, 0)
            self.assertGreaterEqual(x, 0)
            self.assertGreaterEqual(y, 0)
            self.assertLessEqual(x + width, 1280)
            self.assertLessEqual(y + height, 720)
        self.assertTrue(controls["corrupt_button_icon_a"]["enabled"])
        self.assertFalse(controls["corrupt_button_icon_b"]["enabled"])
        self.assertFalse(controls["corrupt_button_icon_c"]["enabled"])
        labels = {row["name"]: row["text"] for row in hud["labels"]}
        self.assertEqual(labels["corrupt_cost_a"], "$ 250")
        self.assertEqual(labels["corrupt_cost_b"], "$ 2000")
        self.assertTrue(labels["corrupt_title_a"])
        self.assertTrue(labels["corrupt_text00a"])
        self.assertTrue(labels["corrupt_text01b"], "copied briefing ID resolves through the fixture text-key map")
        self.assertTrue(labels["corrupt_text01c"])
        label_rows = {row["name"]: row for row in hud["labels"]}
        for slot in ["b", "c"]:
            benefit = label_rows["corrupt_text00" + slot]["rect"]
            briefing = label_rows["corrupt_text01" + slot]["rect"]
            self.assertGreater(benefit[3], 0)
            self.assertGreater(briefing[3], 0)
            self.assertGreaterEqual(briefing[1], benefit[1] + benefit[3], "localized paragraphs overlap")
        disabled = self.click([self.point(initial, "corrupt_button_icon_b"),
            self.point(initial, "corrupt_button_icon_c"), self.point(initial, "corrupt_icon_00a"), [640, 30]])
        self.assertTrue(disabled["corruption_hud"]["active"])
        self.assertEqual(disabled["corruption_hud"]["choices_accepted"], 0)
        self.assertEqual(disabled["map_input"]["selection_invalidations"], initial["map_input"]["selection_invalidations"])
        self.assertEqual(disabled["corruption_hud"]["rebuilds"], hud["rebuilds"], "paused redraws reuse facts")
        closed = self.click([self.point(initial, "corrupt_close")])
        self.assertFalse(closed["corruption_hud"]["active"])
        self.assertEqual(closed["corruption_hud"]["closes"], 1)
        accepted = self.click([self.point(initial, "corrupt_button_icon_a")])
        self.assertFalse(accepted["corruption_hud"]["active"])
        self.assertEqual(accepted["corruption_hud"]["choices_accepted"], 1)
        self.assertEqual(accepted["corruption_hud"]["accepted_slot"], 1)
        self.assertEqual(accepted["map_input"]["selection_invalidations"], initial["map_input"]["selection_invalidations"])

    def test_slot_gap_and_tutorial_gate(self):
        gap, _ = self.capture("corruption-gaps")
        self.assertEqual([row["slot"] for row in gap["corruption_hud"]["options"]], [1, 3])
        self.assertNotIn("corrupt_button_icon_b", {row["name"] for row in gap["corruption_hud"]["controls"]})
        locked, _ = self.capture("corruption-locked")
        self.assertFalse(locked["corruption_hud"]["active"])
        self.assertEqual(locked["corruption_hud"]["choices_accepted"], 0)


if __name__ == "__main__":
    unittest.main()
