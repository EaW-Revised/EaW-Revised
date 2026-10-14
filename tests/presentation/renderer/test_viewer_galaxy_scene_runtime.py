"""Campaign galaxy art, independent layer scenarios and the paused animation clock."""
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
class GalaxySceneRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-galaxy-art-"))

    def capture(self, name, paused, *extra):
        report = self.directory / (name + ".json")
        capture = self.directory / (name + ".png")
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
            str(ROOT / "apps/viewer/project"), "--", "--eawr-galactic", "Sandbox_Equal_Footing_Underworld",
            "--eawr-game-root", GAME_ROOT, "--eawr-galactic-paused", "yes" if paused else "no",
            "--eawr-report", str(report), "--eawr-capture", str(capture), *extra],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertNotIn("Can't change the size of a `SubViewport`", result.stdout + result.stderr)
        self.assertEqual(data["status"], "captured")
        self.assertTrue(capture.is_file())
        self.assertEqual(data["capture"]["png"], {"width": 1280, "height": 720})
        return data, capture

    def test_native_quit_after_closes_live_galaxy(self):
        self.native_quit("synthetic")

    def test_native_quit_after_closes_live_campaign(self):
        self.native_quit("Sandbox_Equal_Footing_Underworld", "--eawr-game-root", GAME_ROOT)

    def test_native_quit_after_releases_retained_child_feed(self):
        data, log = self.native_quit("synthetic", "--eawr-game-root", GAME_ROOT,
                                    "--eawr-galactic-battle", "synthetic",
                                    "--eawr-galactic-battle-frames", "3600", frames=600)
        self.assertIn("EAWR handoff phase: tactical-ready", log)
        self.assertFalse(data["handoff"]["returned"])

    def test_preview_script_starts_synthetic_battle(self):
        # Exercise deferred main-scene instantiation and window-close notification,
        # as the interactive campaign preview does, rather than engine quit-after.
        driver = self.directory / "synthetic-battle-preview.gd"
        driver.write_text('''extends SceneTree
var rendered_frames: int = 0
func _initialize() -> void:
    call_deferred("_start_preview")
func _start_preview() -> void:
    var scene: PackedScene = load(ProjectSettings.get_setting("application/run/main_scene"))
    var viewer := scene.instantiate()
    root.add_child(viewer)
    current_scene = viewer
    RenderingServer.frame_post_draw.connect(_frame_drawn)
func _frame_drawn() -> void:
    rendered_frames += 1
    if rendered_frames == 900:
        RenderingServer.frame_post_draw.disconnect(_frame_drawn)
        call_deferred("_close_preview")
func _close_preview() -> void:
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
''', encoding="utf-8")
        report = self.directory / "synthetic-battle-preview.json"
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
            str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-game-root", GAME_ROOT, "--eawr-map-effects", "on",
            "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
            "--eawr-galactic", "Sandbox_Equal_Footing_Underworld",
            "--eawr-galactic-battle", "synthetic", "--eawr-report", str(report)],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (self.directory / "synthetic-battle-preview.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue(report.is_file(), log)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["status"], "closed", data.get("failure"))
        self.assertEqual(data["local"]["faction"], "Underworld")
        self.assertEqual(data["command_bar"]["pips"], 10)
        self.assertIsNotNone(data["command_bar"]["current_pip"])
        self.assertIn("EAWR handoff phase: galaxy-resident cycle=1", log)
        self.assertIn("EAWR handoff phase: tactical-ready cycle=1", log)
        self.assertTrue(data["handoff"]["requested"])
        self.assertTrue(data["handoff"]["galaxy_scene_resident"])
        self.assertGreater(data["handoff"]["conflict"], 0)
        self.assertEqual(data["handoff"]["child_ready_tick"], 0)
        self.assertTrue(data["campaign_owner_expired"])
        for error in ("ObjectDB instances leaked", "ERROR:"):
            self.assertNotIn(error, log)

    def native_quit(self, campaign, *extra, frames=90):
        report = self.directory / ("native-quit-" + campaign + "-" + str(frames) + ".json")
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--quit-after", str(frames),
            "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-galactic", campaign,
            "--eawr-report", str(report), *extra], cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue(report.is_file(), log)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["status"], "closed")
        self.assertTrue(data["interactive"])
        self.assertTrue(data["campaign_owner_expired"])
        self.assertGreater(data["frames"], 0)
        for error in ("Can't change the size of a `SubViewport`", "ObjectDB instances leaked", "ERROR:"):
            self.assertNotIn(error, log)
        return data, log

    def test_kessel_rotation_is_continuous_across_zoom_alternates(self):
        self.planet_zoom_continuity("Kessel", require_spin=True)

    def test_coruscant_body_rotation_is_continuous_across_zoom_alternates(self):
        data = self.planet_zoom_continuity("Coruscant", require_spin=True)
        self.assertGreater(max(abs(a - b) for sample in data["zoom_samples"]
                               for a, b in zip(sample["authored_before"], sample["authored_after"])), 0.01,
                           "Urban01 probe must expose the original mismatched body keys, not its static glow")

    def planet_zoom_continuity(self, name, require_spin=False):
        initial, _ = self.capture(name + "-initial", True)
        planet_id = next(planet["id"] for planet in initial["planets"] if planet["name"] == name)
        data, _ = self.capture(name + "-zoom", False, "--eawr-galactic-zoom-probe", str(planet_id))
        self.assert_zoom_continuity(data, require_spin)
        return data

    def assert_zoom_continuity(self, data, require_spin=False):
        samples = data["zoom_samples"]
        for layer in data["scene_layers"]:
            self.assertEqual(layer["resident_instances"], layer["instances"])
        self.assertEqual([sample["alternate"] for sample in samples], [1, 0, 1])
        self.assertGreater(samples[0]["animation_time"], 0)
        self.assertGreater(samples[-1]["animation_time"], samples[0]["animation_time"])
        for sample in samples:
            # Detail meshes can have their own translation/scale. Compare
            # rendered spin relative to each authored first pose, preserving
            # compensated mesh-local bases, from the actual GPU palette.
            for before, after in zip(self.orientation(sample["before"]), self.orientation(sample["after"])):
                self.assertAlmostEqual(before, after, delta=1e-5, msg=str(sample))
        if require_spin:
            self.assertTrue(any(abs(a - b) > 1e-3 for sample in samples[1:]
                                for a, b in zip(samples[0]["after"], sample["after"])),
                            "the probe must exercise a spinning planet")

    @staticmethod
    def orientation(matrix):
        result = []
        for column in range(3):
            vector = matrix[column * 3:column * 3 + 3]
            length = math.sqrt(sum(v * v for v in vector))
            if length == 0:
                raise AssertionError("rotation probe has a collapsed basis")
            result.extend(v / length for v in vector)
        return result

    def test_every_stock_planet_model_zoom_continuity(self):
        from tools.inventory.census_catalog import Catalog, value
        catalog = Catalog(GAME_ROOT)
        models = {}
        for key, row in catalog.defs.items():
            if row["class"].lower() != "planet":
                continue
            planet = catalog.resolve(key)
            model = value(planet, "Galactic_Model_Name")
            if model and row["id"] != "Galaxy_Core_Art_Model":
                models.setdefault(model, row["id"])
        # Keep every distinct authored name, including case variants. The
        # backdrop is galaxy art, not a zoomable planet; everything else runs.
        self.assertEqual(len(models), 36)
        xml = self.directory / "rotation-mod" / "Data" / "XML"
        xml.mkdir(parents=True)
        (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        (xml / "CampaignFiles.xml").write_text(
            "<Campaign_Files><File>RotationSweep.xml</File></Campaign_Files>", encoding="utf-8")
        for index, (model, name) in enumerate(sorted(models.items())):
            with self.subTest(model=model, planet=name):
                (xml / "RotationSweep.xml").write_text(
                    '<Campaigns><Campaign Name="RotationSweep"><Locations>' + name + '</Locations>'
                    '<Starting_Active_Player>Underworld</Starting_Active_Player></Campaign></Campaigns>',
                    encoding="utf-8")
                data, _ = self.capture("stock-rotation-" + str(index), False,
                                       "--eawr-mod-root", str(xml.parent.parent),
                                       "--eawr-galactic", "RotationSweep", "--eawr-galactic-zoom-probe", "1")
                planet = next(p for p in data["planets"] if p["name"] == name)
                self.assertEqual(planet["id"], 1)
                self.assertEqual(planet["model"].lower().split("/")[-1], model.lower())
                self.assert_zoom_continuity(data)

    def test_authored_campaign_art_and_independent_scene_layers(self):
        data, capture = self.capture("campaign-art", False)
        self.assertEqual(data["snapshot"]["source"], "LiveGameSession")
        self.assertEqual(data["snapshot"]["ownership"], "authored-starting-forces")
        unassigned = [planet for planet in data["planets"] if planet["owner"] is None]
        self.assertGreater(sum(planet["owner"] is not None for planet in data["planets"]), 10)
        for planet in unassigned:
            self.assertEqual(planet["colour"], [100, 100, 100])
            self.assertEqual(planet["faction"].upper(), "NEUTRAL")
        self.assertEqual([layer["name"] for layer in data["scene_layers"]],
                         ["world", "background", "midground", "midforeground"])
        scenarios = [layer["scenario"] for layer in data["scene_layers"]]
        self.assertTrue(all(scenarios))
        self.assertEqual(len(set(scenarios)), 4)
        self.assertEqual(data["active_planet_alternate"], 0)
        world = data["scene_layers"][0]
        self.assertGreater(world["submitted_instances"], 0)
        self.assertLess(world["submitted_instances"], world["instances"])
        self.assertEqual(data["light_angles"], [0, 160, 58])
        backdrop = next(planet for planet in data["planets"] if planet["name"] == "Galaxy_Core_Art_Model")
        self.assertEqual(backdrop["layer"], "background")
        self.assertEqual(backdrop["position"], [0, 0, 0])
        self.assertEqual(backdrop["scale"], 1)
        self.assertFalse(backdrop["show_name"])
        self.assertEqual(backdrop["drawn"], "model", data["models"])
        self.assertGreater(data["animation_time"], 0)
        self.assertGreater(sum(planet["show_name"] for planet in data["planets"]), 10)
        from space_map_test_support import decode_png
        width, height, rows = decode_png(capture.read_bytes())
        self.assertEqual((width, height), (1280, 720))
        self.assertGreater(len({pixel for row in rows for pixel in row}), 500)

    def test_paused_models_and_material_clocks_freeze(self):
        paused, _ = self.capture("campaign-paused", True)
        self.assertTrue(paused["paused"])
        self.assertFalse(paused["animate_during_pause"])
        self.assertEqual(paused["animation_time"], 0)
        self.assertEqual(paused["animation_updates"], 1)
        self.assertGreater(paused["frames"], 10)

    def test_camera_focus_selects_the_authored_planet_alternate(self):
        focused, _ = self.capture("campaign-focused", True,
                                  "--eawr-galactic-focus", "28", "--eawr-galactic-flight-ms", "750")
        self.assertEqual(focused["active_planet_alternate"], 1)
        camera = focused["flights"][0]
        self.assertEqual(camera["planet"], 28)
        self.assertEqual(camera["state"], "focused")
        world = focused["scene_layers"][0]
        self.assertGreater(world["submitted_instances"], 0)
        self.assertLess(world["submitted_instances"], world["instances"])

    def test_selected_planet_draws_its_faction_cursor_only_when_zoomed_out(self):
        # WGP-09: the start selects the local home; the cursor model sits on it in the
        # overview, light-scaled by the owner's faction Color, and leaves on zoom-in.
        overview, _ = self.capture("cursor-overview", True)
        cursor = overview["selection_cursor"]
        self.assertEqual(cursor["model"], "data/art/models/w_planet_select_neutral.alo")
        self.assertTrue(cursor["loaded"], overview["models"])
        self.assertTrue(cursor["shown"])
        self.assertEqual(cursor["planet"], overview["map_input"]["selected"])
        self.assertEqual(cursor["instances"], 5)
        home = next(planet for planet in overview["planets"] if planet["id"] == cursor["planet"])
        self.assertEqual(home["owner"], overview["local"]["player"])
        self.assertEqual([round(channel * 255) for channel in cursor["tint"]], home["colour"])
        world = overview["scene_layers"][0]
        self.assertEqual(world["resident_instances"], world["instances"])
        focused, _ = self.capture("cursor-focused", True, "--eawr-galactic-focus", str(cursor["planet"]),
                                  "--eawr-galactic-flight-ms", "2000")
        self.assertEqual(focused["flights"][0]["state"], "focused")
        self.assertFalse(focused["selection_cursor"]["shown"])
        self.assertEqual(focused["selection_cursor"]["instances"], 0)
        # The same mod frame with and without an unreadable replacement model: the cursor's pixels.
        frames = {}
        for name, replaced in (("cursor-control", False), ("cursor-absent", True)):
            mod = self.directory / (name + "-mod") / "Data"
            (mod / "Art" / "Models").mkdir(parents=True)
            (mod / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            if replaced:
                (mod / "Art" / "Models" / "w_planet_select_neutral.alo").write_bytes(b"")
            data, png = self.capture(name, True, "--eawr-mod-root", str(mod.parent))
            self.assertEqual(data["selection_cursor"]["loaded"], not replaced)
            self.assertEqual(data["camera"], overview["camera"])
            frames[name] = png
        from space_map_test_support import decode_png
        first = decode_png(frames["cursor-control"].read_bytes())[2]
        second = decode_png(frames["cursor-absent"].read_bytes())[2]
        changed = sum(any(abs(a[c] - b[c]) > 8 for c in range(3))
                      for row_a, row_b in zip(first, second) for a, b in zip(row_a, row_b))
        self.assertGreater(changed, 2000, "the selection cursor draws nothing")

    def test_additive_planet_glow_preserves_the_scene_beneath_its_quad(self):
        import xml.etree.ElementTree as ET
        from tools.inventory.census_catalog import Catalog

        xml = self.directory / "backdrop-mod" / "Data" / "XML"
        xml.mkdir(parents=True)
        (xml.parent / "MegaFiles.xml").write_text(
            "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        (xml / "CampaignFiles.xml").write_text(
            "<Campaign_Files><File>SceneBackdrop.xml</File></Campaign_Files>", encoding="utf-8")
        # WGS-06/WGP-42: the local home is discovered by observer startup. Keep
        # that home at the origin so its initial camera matches the empty scene.
        planets, _ = Catalog(GAME_ROOT).read("data/xml/planets.xml")
        home = next(row for row in planets if row.get("Name", "").lower() == "abregado_rae")
        home.find("Galactic_Position").text = "0, 0, 0"
        (xml / "Planets.xml").write_text(ET.tostring(planets, encoding="unicode"), encoding="utf-8")
        campaigns = "".join(
            f'<Campaign Name="{name}"><Locations>{locations}</Locations>'
            + ('<Home_Location>Underworld, Abregado_Rae</Home_Location>' if name == "WithPlanet" else '')
            + '<Starting_Active_Player>Underworld</Starting_Active_Player></Campaign>'
            for name, locations in (("Backdrop", "Galaxy_Core_Art_Model"),
                                    ("WithPlanet", "Galaxy_Core_Art_Model,Abregado_Rae")))
        (xml / "SceneBackdrop.xml").write_text(f"<Campaigns>{campaigns}</Campaigns>", encoding="utf-8")
        # The selected home's cursor (WGP-09) darkens the backdrop on purpose; the mod's
        # unreadable cursor model keeps this comparison about the planet's own glow.
        models = xml.parent / "Art" / "Models"
        models.mkdir(parents=True)
        (models / "w_planet_select_neutral.alo").write_bytes(b"")
        extra = ("--eawr-mod-root", str(xml.parent.parent))
        empty, backdrop = self.capture("backdrop-only", True, *extra, "--eawr-galactic", "Backdrop")
        background_probe = empty["scene_layers"][1]["colour_probe"]
        final_probe = empty["scene_layers"][0]["colour_probe"]
        self.assertGreater(max(background_probe[:3]), 0.2, "backdrop probe must exercise colour conversion")
        for source, result in zip(background_probe[:3], final_probe[:3]):
            self.assertAlmostEqual(source, result, delta=3 / 255,
                                   msg="empty scene changes the accumulated backdrop colour")
        data, planet = self.capture("backdrop-planet", True, *extra, "--eawr-galactic", "WithPlanet")
        self.assertEqual(data["camera"], empty["camera"], "glow comparison requires matched camera views")
        self.assertTrue(data["selection_cursor"]["shown"])
        self.assertFalse(data["selection_cursor"]["loaded"])
        self.assertEqual(data["selection_cursor"]["instances"], 0)
        from space_map_test_support import decode_png
        background = decode_png(backdrop.read_bytes())[2]
        image = decode_png(planet.read_bytes())[2]
        centre_pixel = background[empty["camera"]["height"] // 2][empty["camera"]["width"] // 2]
        for value, probe in zip(centre_pixel[:3], final_probe[:3]):
            self.assertAlmostEqual(value / 255, probe, delta=3 / 255,
                                   msg="outer hand-off viewport changes the composed scene colour")
        camera = data["camera"]
        art = next(p for p in data["planets"] if p["name"] == "Abregado_Rae")
        facts = next(row for row in data["hud"]["intelligence"] if row["planet"] == art["id"])
        self.assertTrue(facts["discovered"], "glow fixture home must be published as discovered")
        self.assertEqual(art["model"], "data/art/models/w_planet_temperate01.alo")
        # Name glyphs and status icons legitimately darken the backdrop. Isolate
        # their reported screen rectangles, retaining every other pixel outside
        # the opaque sphere in the original comparison region.
        label_rects = [label["global_rect"] for label in data["hud"]["labels"]
                       if label["planet"] == art["id"] and label["visible"] and label["alpha"] > 0]
        self.assertEqual(len(label_rects), 1, "fixture must isolate only the planet name")
        icon_rects = [icon["rect"] for icon in data["hud"]["status_icons"]
                      if icon["planet"] == art["id"] and icon["visible"]]
        self.assertEqual(len(icon_rects), 1, "fixture must isolate its corruption information icon")
        hud_rects = label_rects + icon_rects
        for left, top, width, height in hud_rects:
            self.assertGreater(width, 0)
            self.assertGreater(height, 0)
            self.assertGreaterEqual(left, 0)
            self.assertGreaterEqual(top, 0)
            self.assertLessEqual(left + width, camera["width"])
            self.assertLessEqual(top + height, camera["height"])
        eye = camera["eye"]
        def dot(a, b):
            return sum(x * y for x, y in zip(a, b))
        def unit(a):
            length = math.sqrt(dot(a, a))
            return [x / length for x in a]
        forward = unit([a - b for a, b in zip(camera["target"], eye)])
        right = unit([-forward[2], 0, forward[0]])
        up = [right[1] * forward[2] - right[2] * forward[1],
              right[2] * forward[0] - right[0] * forward[2],
              right[0] * forward[1] - right[1] * forward[0]]
        source = art["position"]
        offset = [a - b for a, b in zip([source[0], source[2], -source[1]], eye)]
        depth = dot(offset, forward)
        focal = camera["height"] / (2 * math.tan(math.radians(camera["vertical_fov"]) / 2))
        centre = (camera["width"] / 2 + focal * dot(offset, right) / depth,
                  camera["height"] / 2 - focal * dot(offset, up) / depth)
        # WGUI-60's model radius bounds the opaque planet; its larger glow quad
        # must only add RGB to the backdrop outside that circle.
        radius = 4.92 * art["scale"]
        screen_radius = focal * radius / (depth - radius) + 2
        colour_tolerance = 8
        darkened = 0
        compared = 0
        hud_pixels = 0
        for y in range(max(0, int(centre[1] - 2 * screen_radius)),
                       min(camera["height"], int(centre[1] + 2 * screen_radius) + 1)):
            for x in range(max(0, int(centre[0] - 2 * screen_radius)),
                           min(camera["width"], int(centre[0] + 2 * screen_radius) + 1)):
                if math.hypot(x - centre[0], y - centre[1]) <= screen_radius:
                    continue
                if any(left <= x < left + width and top <= y < top + height
                       for left, top, width, height in hud_rects):
                    hud_pixels += 1
                    continue
                compared += 1
                darkened += any(image[y][x][c] + colour_tolerance < background[y][x][c] for c in range(3))
        print(f"additive glow: compared={compared}, darkened={darkened}, "
              f"tolerance={colour_tolerance}, darkened_limit=4, hud_pixels={hud_pixels}, "
              f"label_rect={label_rects[0]}, icon_rects={icon_rects}", flush=True)
        self.assertGreater(hud_pixels, 0, "HUD rectangles must overlap the comparison region")
        self.assertGreater(compared, 1000, "glow comparison must exercise the backdrop")
        self.assertLessEqual(darkened, 4, "additive quad darkens the previous scene")

    def test_campaign_layers_survive_battle_handoff_with_conflict_player_ids(self):
        before, original = self.capture("handoff-campaign-before", True)
        after, returned = self.capture("handoff-campaign-return", True,
                                       "--eawr-galactic-battle", "synthetic",
                                       "--eawr-galactic-battle-cycles", "2")
        # The loaded Underworld display player is not either fixture battle
        # player (1/2). A completed return therefore exercises the seat mapping.
        self.assertNotIn(before["local"]["player"], (1, 2))
        self.assertEqual(after["local"], before["local"])
        self.assertTrue(after["handoff"]["returned"])
        self.assertEqual(after["battle_dialog"]["phase"], "hidden")
        self.assertEqual(after["battle_dialog"]["choices"], 2,
                         "each synthetic battle must acknowledge its summary through the normal action")
        self.assertTrue(after["campaign_owner_expired"])
        self.assertEqual(len(after["handoff_cycles"]), 2)
        self.assertEqual(after["live_session"]["submitted_orders"], 1,
                         "synthetic handoff must submit its initial pause, not pre-set the viewer flag")
        self.assertEqual(after["live_session"]["pause_depth"], 1,
                         "battle return must retain the published initial galaxy pause")
        self.assertTrue(after["paused"], "post-return animation must read the published pause")
        for cycle in after["handoff_cycles"]:
            self.assertEqual(cycle["ready_tick"], 0)
            for field in ("child_expired", "feed_expired", "scene_expired", "galaxy_same_instance"):
                self.assertTrue(cycle[field], cycle)
        self.assertEqual(after["models"], before["models"])
        self.assertEqual(after["animation_time"], 0)
        self.assertEqual(after["animation_updates"], 1)
        self.assertEqual(after["camera"], before["camera"])
        self.assertEqual(len({layer["scenario"] for layer in after["scene_layers"]}), 4)
        self.assertEqual([layer["submitted_instances"] for layer in after["scene_layers"]],
                         [layer["submitted_instances"] for layer in before["scene_layers"]])
        from space_map_test_support import decode_png
        first = decode_png(original.read_bytes())[2]
        last = decode_png(returned.read_bytes())[2]
        changed = sum(any(abs(a[c] - b[c]) > 3 for c in range(3))
                      for row_a, row_b in zip(first, last) for a, b in zip(row_a, row_b))
        self.assertLessEqual(changed, 4, "returned scene loses or changes paused campaign art")


if __name__ == "__main__":
    unittest.main()
