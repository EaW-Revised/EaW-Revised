"""The viewer's galactic mode skeleton (FT-12, docs/galactic-architecture.md section 9).

`--eawr-galactic synthetic` draws the synthetic galaxy for one local seat:
planets with their models, trade-route lines, fleet markers and ownership
colours under a basic orbit camera, then reports and captures.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE; EAWR_EAW_GAME_ROOT
adds the run that draws the stock planet models.
"""

import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import MODE_SOURCES, SOURCE_BUNDLES, mode_source, source_text  # noqa: E402

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
GALACTIC = ROOT / "apps/viewer/src/galactic"
# Stock faction `Color` values the synthetic galaxy carries, and the Neutral colour.
FACTIONS = {"Rebel": (185, 40, 39), "Empire": (54, 134, 242), "Underworld": (247, 201, 13)}
NEUTRAL = (100, 100, 100)


class GalacticModeStructure(unittest.TestCase):
    def test_the_mode_is_wired_into_the_host_and_its_own_source_list(self):
        host = source_text("apps/viewer/src/viewer_host.cpp")
        self.assertIn("GalacticMode::requested()", host)
        self.assertIn("galactic_mode_->process(delta)", host)
        camera = source_text("apps/viewer/src/viewer_host_camera.cpp")
        self.assertIn("galactic_mode_->input(event)", camera)
        manifest = (ROOT / "apps/viewer/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(manifest.count('include("${EAWR_ROOT}/apps/viewer/src/galactic/sources.cmake")'), 1)
        self.assertEqual(manifest.count("${EAWR_VIEWER_SOURCES}"), 1)
        viewer_sources = (ROOT / "apps/viewer/src/sources.cmake").read_text(encoding="utf-8")
        self.assertEqual(viewer_sources.count("${EAWR_VIEWER_GALACTIC_SOURCES}"), 1)
        listed = re.findall(r'"\$\{CMAKE_CURRENT_LIST_DIR\}/([^"]+)"',
                            (GALACTIC / "sources.cmake").read_text(encoding="utf-8"))
        self.assertEqual(sorted(listed), sorted(path.name for path in GALACTIC.glob("*.cpp")))
        build = source_text("apps/viewer/CMakeLists.txt")
        for name in listed:
            self.assertIn(f"apps/viewer/src/galactic/{name}", build)
        for name in MODE_SOURCES["galactic_mode"]:
            self.assertTrue((ROOT / "apps/viewer/src" / name).is_file(), name)

    def test_my_seat_and_my_faction_stay_separate(self):
        # ADR-020: colours and relations come from the faction, the camera from the seat.
        mode = mode_source("galactic_mode")
        self.assertIn("view.local.player", mode)
        self.assertIn("std::map<galactic_view::SeatId, galactic_view::OrbitCamera> cameras",
                      (GALACTIC / "galactic_mode_internal.hpp").read_text(encoding="utf-8"))
        self.assertIn("cameras.try_emplace(galaxy->local.seat", mode)
        self.assertIn('\\"camera_owner\\": \\"seat\\", \\"colour_owner\\": \\"faction\\"', mode)
        # The relation is decided against the seat's player, never the seat ID.
        self.assertIn("*owner == view.local.player", mode)
        # Synthetic observer setup must populate its WGUI-12 player input.
        # ADR-020's name guard still covers every runtime source and private
        # state header; only these two explicit fixture headers are exempt.
        fixture_headers = {
            "apps/viewer/src/galactic/hud_clock_fixture.hpp",
            "apps/viewer/src/galactic/fleet_hud_fixture.hpp",
        }
        members = (
            member
            for name in MODE_SOURCES["galactic_mode"]
            for member in SOURCE_BUNDLES.get(f"apps/viewer/src/{name}", (f"apps/viewer/src/{name}",))
        )
        runtime_mode = "".join(
            (ROOT / member).read_text(encoding="utf-8")
            for member in dict.fromkeys(members) if member not in fixture_headers
        )
        # Observer publications carry a local_player field; that field is not
        # a replacement for the seat-derived view.local.player ownership key.
        self.assertNotRegex(runtime_mode, r"(?<!->)(?<!\.)\blocal_player\b")
        self.assertNotRegex(mode, r"==\s*(?:view\.)?local\.seat")

    def test_the_view_never_owns_or_steps_a_simulation(self):
        mode = mode_source("galactic_mode")
        for name in ("GalacticSession", "TacticalSession", "GameSession::create", "LiveSession::start"):
            self.assertNotIn(name, mode)
        self.assertNotRegex(mode, r'res://[^"\n]*\.(?:tga|dds|alo)')


def run_viewer(test, directory, name, *extra, expected_exit=0):
    executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
    test.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
    report = directory / f"{name}.json"
    capture = directory / f"{name}.png"
    completed = subprocess.run(
        [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
         "--eawr-galactic", "synthetic", *extra, "--eawr-report", str(report), "--eawr-capture", str(capture)],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=300)
    test.assertTrue(report.is_file(), completed.stdout)
    result = json.loads(report.read_text(encoding="utf-8"))
    test.assertEqual(completed.returncode, expected_exit, result.get("failure") or completed.stdout)
    if expected_exit:
        test.assertEqual(result["status"], "failed")
        test.assertTrue(result["failure"])
        test.assertFalse(capture.exists())
        return result, []
    test.assertEqual(result["status"], "captured", result.get("failure"))
    test.assertEqual(result["capture"]["png"], {"width": 1280, "height": 720})
    test.assertTrue(capture.is_file())
    from space_map_test_support import decode_png  # noqa: E402
    width, height, rows = decode_png(capture.read_bytes())
    test.assertEqual((width, height), (1280, 720))
    return result, rows


def near(pixel, colour, tolerance=3):
    return all(abs(a - b) <= tolerance for a, b in zip(pixel, colour))


def assert_galaxy(test, result, rows, seat, faction, faction_seats):
    test.assertEqual(result["mode"], "galactic")
    test.assertEqual(result["local"]["seat"], seat)
    test.assertEqual(result["local"]["faction"], faction)
    test.assertEqual(result["local"]["faction_seats"], faction_seats)
    test.assertEqual(result["local"]["camera_owner"], "seat")
    test.assertEqual([camera["seat"] for camera in result["cameras"]], [seat])
    test.assertEqual(len(result["planets"]), 9)
    test.assertEqual(len(result["routes"]), 12)
    test.assertEqual(len(result["fleets"]), 6)
    test.assertEqual(sum(route["typed"] for route in result["routes"]), 3)
    for planet in result["planets"]:
        expected = FACTIONS.get(planet["faction"], NEUTRAL)
        test.assertEqual(tuple(planet["colour"]), expected, planet)
        relation = "neutral" if planet["owner"] is None else ("own" if planet["faction"] == faction else "other")
        test.assertEqual(planet["relation"], relation, planet)
    # Ownership rings, routes and fleet markers draw unshaded in their faction's
    # colour, so each faction and the neutral grey appear on screen.
    pixels = [pixel for row in rows for pixel in row]
    for name, colour in [*FACTIONS.items(), ("Neutral", NEUTRAL)]:
        test.assertGreaterEqual(sum(near(pixel, colour) for pixel in pixels), 40, name)


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE")
class GalacticModeGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-galactic-mode-"))

    def test_live_capture_ticks_and_pause_are_owned_by_the_session(self):
        moving, _ = run_viewer(self, self.directory, "galactic-live-ticks", "--eawr-galactic-ticks", "12")
        self.assertEqual(moving["snapshot"]["source"], "LiveGameSession")
        self.assertEqual(moving["snapshot"]["tick"], 12)
        self.assertEqual(moving["live_session"]["galactic_frame"], 12)
        self.assertFalse(moving["paused"])
        paused, _ = run_viewer(self, self.directory, "galactic-live-pause", "--eawr-galactic-seat", "2",
            "--eawr-galactic-paused", "yes", "--eawr-galactic-ticks", "12")
        self.assertEqual(paused["snapshot"]["tick"], 1)
        self.assertEqual(paused["live_session"]["galactic_frame"], 0)
        self.assertEqual(paused["live_session"]["pause_depth"], 1)
        self.assertEqual(paused["live_session"]["submitted_orders"], 1)
        self.assertTrue(paused["paused"])
        self.assertTrue(any(event["kind"] == "galaxy_paused" and event["seat"] == 2
            for event in paused["live_session"]["snapshot_events"]))

    def test_invalid_capture_tick_target_is_rejected(self):
        for target in ("-1", "0", "3601", "x"):
            run_viewer(self, self.directory, "galactic-invalid-tick-" + target,
                "--eawr-galactic-ticks", target, expected_exit=2)

    def test_the_synthetic_galaxy_draws_placeholders_without_game_data(self):
        result, rows = run_viewer(self, self.directory, "galactic-synthetic-placeholders", "--eawr-galactic-seat", "5")
        assert_galaxy(self, result, rows, 5, "Empire", [3, 5])
        self.assertEqual({planet["drawn"] for planet in result["planets"]}, {"placeholder"})
        self.assertEqual(result["models"], [])
        self.assertEqual(result["instances"], 0)

    @unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT for the synthetic child battle")
    def test_space_battle_returns_to_the_resident_galaxy(self):
        before, _ = run_viewer(self, self.directory, "handoff-before", "--eawr-game-root", GAME_ROOT,
                               "--eawr-galactic-coop-preview", "yes")
        result, rows = run_viewer(self, self.directory, "handoff-return", "--eawr-game-root", GAME_ROOT,
                                  "--eawr-galactic-battle", "synthetic", "--eawr-galactic-battle-cycles", "4",
                                  "--eawr-galactic-coop-preview", "yes")
        handoff = result["handoff"]
        self.assertTrue(handoff["returned"], result)
        self.assertTrue(handoff["galaxy_scene_resident"])
        self.assertEqual(handoff["child_ready_tick"], 0)
        self.assertGreaterEqual(handoff["loading_frames"], 3)
        self.assertGreater(handoff["load_ms"], 0)
        self.assertTrue(result["campaign_owner_expired"])
        self.assertEqual(len(result["handoff_cycles"]), 4)
        for cycle in result["handoff_cycles"]:
            self.assertEqual(cycle["ready_tick"], 0)
            self.assertGreater(cycle["final_tick"], 120)
            for field in ("child_expired", "feed_expired", "scene_expired", "galaxy_same_instance"):
                self.assertTrue(cycle[field], cycle)
        self.assertEqual(result["models"], before["models"])
        self.assertEqual(result["instances"], before["instances"])
        self.assertEqual(result["camera"], before["camera"])
        assert_galaxy(self, result, rows, 1, "Rebel", [1, 2])

    @unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT to draw the stock planet models")
    def test_the_second_coop_seat_sees_its_factions_galaxy_with_stock_planet_models(self):
        result, rows = run_viewer(self, self.directory, "galactic-synthetic-models", "--eawr-galactic-seat", "2",
                                  "--eawr-game-root", GAME_ROOT, "--eawr-galactic-coop-preview", "yes")
        assert_galaxy(self, result, rows, 2, "Rebel", [1, 2])
        self.assertEqual(result["profile"], "foc")
        self.assertEqual({planet["drawn"] for planet in result["planets"]}, {"model"}, result["models"])
        drawn_shaders = set()
        for model in result["models"]:
            self.assertEqual(model["status"], "drawn", model)
            drawn_shaders |= {surface["shader"] for surface in model["surfaces"] if surface["status"] == "drawn"}
        # Eight of the nine models are Planet.fx planets; the asteroid field is not.
        self.assertIn("Planet.fx", drawn_shaders)
        fleet_models = [model for model in result["models"]
                        if model["logical_path"].lower().endswith("w_planet_fleet.alo")]
        self.assertEqual(len(fleet_models), 1)
        self.assertEqual(len(result["models"]) - len(fleet_models), 9)
        self.assertGreater(result["instances"], 9)
        # Textured planets add many colours beyond the flat overlay.
        self.assertGreater(len({pixel for row in rows for pixel in row}), 500)

    @unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT to draw the stock planet models")
    def test_planet_flight_uses_stock_constants_and_changes_the_rendered_view(self):
        results = []
        pictures = []
        for milliseconds, state, alpha in ((0, "inward", 0.0), (325, "inward", 0.5), (650, "focused", 1.0)):
            result, rows = run_viewer(self, self.directory, f"galactic-flight-{milliseconds}",
                "--eawr-game-root", GAME_ROOT, "--eawr-galactic-focus", "101",
                "--eawr-galactic-flight-ms", str(milliseconds))
            flight = result["flights"][0]
            self.assertEqual(flight["state"], state)
            self.assertAlmostEqual(flight["alpha"], alpha, places=5)
            self.assertEqual(flight["planet"], 101)
            self.assertAlmostEqual(result["camera_constants"]["zoom_seconds"], 0.65, places=5)
            self.assertEqual(result["camera_constants"]["source"], "data/xml/gameconstants.xml")
            self.assertEqual(len(result["camera_constants"]["sha256"]), 64)
            self.assertTrue(flight["alternate_model_requested"])
            results.append(result)
            pictures.append(rows)
        self.assertEqual(len({result["capture"]["sha256"] for result in results}), 3)
        # WGUI-63 / gc-ui-sensors: stock GMC_ZoomedPitchAngleDegrees (-35)
        # becomes +35 in the viewer's camera convention.
        self.assertAlmostEqual(results[2]["cameras"][0]["pitch"], 35.0, places=5)
        planet = next(planet for planet in results[2]["planets"] if planet["id"] == 101)
        self.assertAlmostEqual(results[2]["cameras"][0]["distance"], 3 * 4.92 * planet["scale"], places=3)
        for rows in pictures:
            self.assertGreater(len({pixel for row in rows for pixel in row}), 500)

    @unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT for VFS override tests")
    def test_camera_constants_use_the_effective_mod_layer_and_reject_invalid_values(self):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        mod = self.directory / "camera-constants-mod"
        xml = mod / "Data/XML/GameConstants.xml"
        xml.parent.mkdir(parents=True, exist_ok=True)
        (mod / "Data/MegaFiles.xml").write_text(
            "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
        source = Corpus(GAME_ROOT).read_effective("foc", "data/xml/gameconstants.xml")
        self.assertIsNotNone(source)

        def write_constants(fields):
            # A loose mod document replaces the entire VFS winner. Preserve
            # unrelated stock inputs needed by the HUD, routes and scene.
            constants = ET.fromstring(source.data)
            overrides = ET.fromstring(f"<GameConstants>{fields}</GameConstants>")
            names = {child.tag.lower() for child in overrides}
            for child in list(constants):
                if child.tag.lower() in names:
                    constants.remove(child)
            constants.extend(overrides)
            ET.ElementTree(constants).write(xml, encoding="utf-8", xml_declaration=True)

        # Synthetic inputs required by the scene lighting and planet-name HUD.
        required = ("<Galactic_Zoom_Out_Light_Angle>0,120,30</Galactic_Zoom_Out_Light_Angle>"
                    "<Num_Structures_For_Medium_Planet_Name>1</Num_Structures_For_Medium_Planet_Name>"
                    "<Num_Structures_For_Large_Planet_Name>2</Num_Structures_For_Large_Planet_Name>")
        write_constants(required + "<GMC_ZoomTime>1</GMC_ZoomTime>"
                       "<GMC_InitialPullbackDistance>900</GMC_InitialPullbackDistance>"
                       "<Galactic_Zoom_Light_Level>0.75</Galactic_Zoom_Light_Level>"
                       "<Unused_Test_Constant>ignored</Unused_Test_Constant>")
        result, _ = run_viewer(self, self.directory, "galactic-mod-flight", "--eawr-game-root", GAME_ROOT,
            "--eawr-mod-root", str(mod), "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "500")
        self.assertAlmostEqual(result["flights"][0]["alpha"], 0.5, places=5)
        self.assertAlmostEqual(result["flights"][0]["light_level"], 0.625, places=5)
        planet = next(planet for planet in result["planets"] if planet["id"] == 101)
        self.assertAlmostEqual(result["cameras"][0]["distance"], (900 + 3 * 4.92 * planet["scale"]) / 2, places=3)
        for index, fields in enumerate(("<GMC_ZoomTime>nan</GMC_ZoomTime>",
                "<GMC_ZoomTime>0</GMC_ZoomTime>",
                "<GMC_ZoomTime>1</GMC_ZoomTime><gmc_zoomtime>2</gmc_zoomtime>",
                "<Galactic_Zoom_In_Light_Angle>1,2</Galactic_Zoom_In_Light_Angle>")):
            write_constants(required + fields)
            rejected, _ = run_viewer(self, self.directory, f"galactic-invalid-constants-{index}",
                "--eawr-game-root", GAME_ROOT, "--eawr-mod-root", str(mod), expected_exit=2)
            self.assertIn("galactic camera constant", rejected["failure"])


if __name__ == "__main__":
    unittest.main()
