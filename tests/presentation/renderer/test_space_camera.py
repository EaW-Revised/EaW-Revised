"""P1 #30 opt-in space tactical camera on the synthetic space map.

The structural half runs everywhere from committed sources and the wholly
synthetic fixtures (tests/assets/fixtures/space_environment_fixture.py and
camera_fixture.py).

The graphical half is opt-in: set EAWR_GODOT_VIEWER_RUNTIME_TEST and
EAWR_GODOT_EXECUTABLE to run real Godot key, button, wheel, motion, focus and
resize events through the pinned binary. Captures stay in temporary
directories.

Movement is project policy computed from invented XML constants. Nothing here
claims original-game defaults, speed, zoom, bindings or map bounds, and no TED
extent, volume or #26 source-bounds ledger is read.
"""

import hashlib
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
import camera_fixture  # noqa: E402
import space_environment_fixture as fixture  # noqa: E402
from viewer_mode_sources import mode_source  # noqa: E402

CONFIG_DIR = ROOT / "apps/viewer/project/config"
CONFIG = CONFIG_DIR / "space-map-camera.xml"
BINDINGS = CONFIG_DIR / "space-map-camera-bindings.json"
RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")

CHECKS = {
    "focus established before input", "pan reaches authored X clamp", "grabbed drag translates without turning",
    "wheel changes only zoom", "wheel distance eases toward target",
    "wheel distance continues on idle frames", "Alt mouse motion pans target",
    "focus loss cancels held pan", "fresh press after focus regain pans",
    "real resize cancels held pan", "edge pan changes only target", "pointer exit cancels edge pan",
    "reset restores authored pose", "input callbacks reached space adapter", "capture lock isolates input",
    "Ctrl grabbed drag orbits under the plane", "Ctrl click keeps the view",
    "middle click resets the view around the target",
}

def orbit_rows(table, context):
    """The committed Ctrl + middle-drag orbit chords of one context."""
    return {(entry["action"], entry["device"], entry["control"], entry["scale"])
            for entry in table["bindings"] if entry.get("modifiers") == ["ctrl"]
            and entry["context"] == context}


# FoC middle-button law (RO-7, #295): Ctrl rotates and tilts at x1, x reversed
# to the owner's direction check, y so that screen-up is positive as in FoC.
ORBIT = {("rotate_grab", "mouse_button", "middle", 1.0), ("rotate", "mouse_motion", "x", -1.0),
         ("orbit_pitch", "mouse_motion", "y", -1.0)}


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _reject_constant(token: str):
    raise ValueError(f"non-JSON numeric token {token!r} in the report")


def strict_json(text: str):
    return json.loads(text, parse_constant=_reject_constant)


def fixture_root(path: pathlib.Path) -> pathlib.Path:
    fixture.write_fixture_root(path)
    xml = path / "GameData/Data/XML"
    (xml / "tacticalcameras.xml").write_text(camera_fixture.tactical_cameras_xml(), encoding="utf-8")
    (xml / "gameconstants.xml").write_text(camera_fixture.game_constants_xml(), encoding="utf-8")
    return path


def expected_pan_speed(zoom: float) -> float:
    return 823.529412 + (2400.0 - 823.529412) * zoom


def run_viewer(root: pathlib.Path, name: str, extra=(), capture=False, unlocked_capture=False, harness=False):
    executable = os.environ["EAWR_GODOT_EXECUTABLE"]
    report = root.parent / f"{name}.json"
    png = root.parent / f"{name}.png"
    command = [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-map", fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
               "--eawr-report", str(report)]
    if capture:
        command += ["--eawr-capture", str(png)]
    if unlocked_capture:
        command += ["--eawr-map-camera-unlocked-capture", str(png)]
    command += list(extra)
    if harness:
        command += ["--eawr-space-control", "none"]
    completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=180, check=False)
    if not report.exists():
        raise AssertionError(completed.stdout)
    return completed, strict_json(report.read_text(encoding="utf-8")), png


def rgba(path: pathlib.Path) -> bytes:
    with Image.open(path) as image:
        return image.convert("RGBA").tobytes()


def changed_pixels(first: pathlib.Path, second: pathlib.Path) -> int:
    with Image.open(first) as a, Image.open(second) as b:
        left = a.convert("RGB")
        right = b.convert("RGB")
        if left.size != right.size:
            return left.size[0] * left.size[1]
        first_bytes, second_bytes = left.tobytes(), right.tobytes()
        return sum(1 for index in range(0, len(first_bytes), 3)
                   if first_bytes[index:index + 3] != second_bytes[index:index + 3])


class SpaceCameraStructure(unittest.TestCase):
    def test_every_space_config_carries_owner_deviations(self):
        reference = ET.parse(CONFIG_DIR / "coruscant-live-session-camera.xml").getroot()
        expected = reference.find("constant_overrides")
        self.assertIsNotNone(expected)
        space_configs = []
        for path in sorted(CONFIG_DIR.glob("*.xml")):
            root = ET.parse(path).getroot()
            if root.get("schema") != "eawr-space-map-camera":
                continue
            space_configs.append(path.name)
            with self.subTest(config=path.name):
                block = root.find("constant_overrides")
                self.assertIsNotNone(block, f"{path.name} lacks the owner deviations")
                self.assertEqual(block.attrib, expected.attrib)
                self.assertEqual([(child.tag, child.attrib) for child in block],
                                 [(child.tag, child.attrib) for child in expected])
        self.assertTrue(space_configs, "no space camera configs were checked")

    def test_authored_space_config_and_bindings(self):
        text = CONFIG.read_text(encoding="utf-8")
        self.assertIn('schema="eawr-space-map-camera" version="1" provenance="project-authored"', text)
        self.assertIn(f'map_path="{fixture.MAP_LOGICAL_PATH}"', text)
        self.assertIn(f'map_sha256="{hashlib.sha256(fixture.ted_bytes()).hexdigest()}"', text)
        self.assertIn('authority="project-authored"', text)
        self.assertIn('<bindings path="space-map-camera-bindings.json"/>', text)
        table = json.loads(BINDINGS.read_text(encoding="utf-8"))
        self.assertEqual(table["version"], 1)
        self.assertEqual(table["provenance"], "project-authored")
        self.assertNotIn("free_camera", table)
        self.assertEqual({entry["context"] for entry in table["bindings"]}, {"space"})
        self.assertFalse({entry["action"] for entry in table["bindings"]} & {"free_toggle"})
        self.assertTrue(all(entry["id"].startswith("space.") for entry in table["bindings"]))
        # Owner feel check 2026-09-25: space pan unchanged. FoC law: a plain
        # middle drag translates at x4, only Ctrl rotates, a click resets.
        self.assertNotIn("pan_speed_scale", table)
        self.assertIs(table["click_reset"], True)
        self.assertIs(table["screen_mouse_units"], True)
        self.assertEqual(orbit_rows(table, "space"), ORBIT)
        self.assertEqual({(entry["action"], entry["control"], entry["scale"])
                          for entry in table["bindings"]
                          if entry["device"] == "mouse_motion" and "modifiers" not in entry}, {
            ("translate_x", "x", 4.0), ("translate_y", "y", -4.0)})
        stripped = dict(table)
        stripped.pop("notice")
        for token in ("bounds", "retail", "original", "ted"):
            self.assertNotIn(token, json.dumps(stripped).lower())

    def test_constants_are_read_for_space_mode_through_the_shared_loader(self):
        source = mode_source("map_mode")
        self.assertIn("state.load_map_camera(camera::Mode::space, camera_failure)", source)
        self.assertIn("state.load_map_camera(camera::Mode::land, camera_failure)", source)
        loader = source[source.index("MapMode::State::load_map_camera("):]
        loader = loader[:loader.index("\n}\n")]
        self.assertIn("camera::load_constants(", loader)
        self.assertIn("parse_map_camera_config(*config_text, options.map_path, map_hash, mode)", loader)
        # No TED extent/volume or pending source-bounds ledger feeds the bounds.
        for token in ("volume", "extent", "footprint", "source_bounds", "ledger"):
            self.assertNotIn(token, loader.lower())
        self.assertNotIn("bounded map camera applies only to a land map", source)

    def test_space_bridge_has_no_free_flight_and_shared_host_is_untouched(self):
        environment = mode_source("space_environment")
        for token in ("free_camera", "FreeCamera", "free_toggle"):
            self.assertNotIn(token, environment)
        self.assertIn("camera_input::Context::space", environment)
        bridge = read("apps/viewer/src/map_camera.cpp")
        self.assertIn("space map camera rejects unsupported free-camera bindings", bridge)
        self.assertIn("space map camera rejects incompatible land bindings", bridge)
        host = mode_source("viewer_host")
        for token in ("eawr-space", "SpaceEnvironment", "space::"):
            self.assertNotIn(token, host)

    def test_interactive_run_never_evaluates_fixed_sky_fidelity(self):
        source = mode_source("space_environment")
        body = source[source.index("std::optional<int> SpaceEnvironment::State::interactive_process("):]
        body = body[:body.index("std::optional<int> SpaceEnvironment::State::finish()")]
        for token in ("evaluate_pixels", "rasterize(", "phases.", "occluder_mask", ".pgm"):
            self.assertNotIn(token, body)
        self.assertIn('pixel_status = "not_evaluated_interactive";', body)
        # The fixed capture camera is written only from the parsed fixed camera
        # or, when unlocked, from the bridge.
        ready = source[source.index("bool SpaceEnvironment::ready("):]
        self.assertIn("if (state.options.map_camera && !state.activate_camera(host)) return false;", ready)

    def test_bridge_contract_is_registered_with_ctest(self):
        cmake = read("tests/presentation/camera/CMakeLists.txt")
        self.assertIn("add_test(NAME map_camera_bridge_contracts COMMAND map_camera_bridge_tests)", cmake)
        self.assertIn("map_camera_bridge_main.cpp", cmake)

    def test_export_presets_carry_the_space_fixtures(self):
        presets = read("apps/viewer/project/export_presets.cfg")
        self.assertEqual(presets.count("config/*.json,config/*.xml"), 3)


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST for pinned graphical checks")
class SpaceCameraGraphical(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant duel")
    def test_duel_view_reaches_owner_pitch_floor(self):
        camera_config = CONFIG_DIR / "coruscant-duel-camera.xml"
        replay = ROOT / "tests/fidelity/fixtures/S-15-duel-damage.eawr-replay"
        with tempfile.TemporaryDirectory(prefix="eawr-duel-camera-") as directory:
            report_path = pathlib.Path(directory) / "duel.json"
            command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                       "--path", str(ROOT / "apps/viewer/project"), "--",
                       "--eawr-map", "data/art/maps/_mp_space_coruscant.ted",
                       "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                       "--eawr-report", str(report_path),
                       "--eawr-capture", str(pathlib.Path(directory) / "duel.png"),
                       "--eawr-populate",
                       "--eawr-map-camera-config", str(camera_config),
                       "--eawr-live-session", "replay", "--eawr-live-replay", str(replay),
                       "--eawr-live-reveal", "on", "--eawr-live-player", "2",
                       "--eawr-live-ticks", "60", "--eawr-live-step", "3",
                       "--eawr-hud", "off", "--eawr-environment", "map",
                       "--eawr-live-input", "20:mdrag:0,-300+ctrl",
                       "--eawr-live-input", "40:mdrag:0,-300+ctrl",
                       "--eawr-live-capture-ticks", "42"]
            completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, timeout=180, check=False)
            self.assertTrue(report_path.is_file(), completed.stdout[-4000:])
            report = strict_json(report_path.read_text(encoding="utf-8"))
            self.assertEqual(completed.returncode, 0, report.get("failure") or completed.stdout)
            self.assertEqual(report["map_camera"]["config_sha256"],
                             hashlib.sha256(camera_config.read_bytes()).hexdigest())
            self.assertEqual(report["battle_input"]["scripted_fired"], 2)
            samples = {sample["label"]: sample for sample in report["battle_input"]["camera_samples"]}
            tilted = next(sample for label, sample in samples.items()
                          if label.startswith("scripted mdrag at tick 40") and label.endswith("after"))
            eye, target = tilted["eye"], tilted["target"]
            pitch = math.degrees(math.atan2(eye[1] - target[1],
                                          math.hypot(eye[0] - target[0], eye[2] - target[2])))
            self.assertAlmostEqual(pitch, -60.0, delta=0.05)
            self.assertIn("duel_t0042.png", report["captures"])

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant environment camera")
    def test_default_environment_accepts_space_tactical_camera(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-environment-camera-") as directory:
            folder = pathlib.Path(directory)
            report_path = folder / "environment.json"
            png = folder / "environment.png"
            config = CONFIG_DIR / "coruscant-space-map-camera.xml"
            completed = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--path", str(ROOT / "apps/viewer/project"), "--",
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                "--eawr-map-camera-config", str(config), "--eawr-report", str(report_path),
                "--eawr-capture", str(png)], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=180, check=False)
            report = strict_json(report_path.read_text(encoding="utf-8"))
            self.assertEqual(completed.returncode, 0, report.get("failure") or completed.stdout)
            self.assertEqual(report["status"], "space_environment_rendered")
            self.assertEqual(report["space"]["slice"], "E-space-environment-v1")
            self.assertEqual(report["map_camera"]["context"], "space")
            # The authored config starts at local player 1's TED spawn marker, at distance 1200
            # since #348 (owner: a bit further out than FoC's Distance_Default 1000), and zooms in
            # to 100 since #390 (owner: closer ship detail than FoC's Distance_Min 200).
            self.assertEqual(report["map_camera"]["config_sha256"], hashlib.sha256(config.read_bytes()).hexdigest())
            self.assertEqual(report["space"]["camera"]["default"]["target_record"], 55)
            self.assertEqual(report["capture_identity"]["camera"]["target"], [4118.37, 0, 4976])
            self.assertGreater(report["map_camera"]["steps"], 0)
            self.assertGreater(report["capture_identity"]["camera"]["far"], 48000)
            self.assertTrue(png.is_file())

    def _check_identity_fields(self, report, root):
        camera = report["space_camera"]
        self.assertEqual((camera["mode"], camera["context"], camera["definition"]), ("space", "space", "Space_Mode"))
        self.assertEqual(camera["config_version"], 1)
        self.assertEqual(camera["config_sha256"], hashlib.sha256(CONFIG.read_bytes()).hexdigest())
        self.assertEqual(camera["bindings_sha256"], hashlib.sha256(BINDINGS.read_bytes()).hexdigest())
        xml = root / "GameData/Data/XML"
        self.assertEqual(camera["tactical_xml_sha256"],
                         hashlib.sha256((xml / "tacticalcameras.xml").read_bytes()).hexdigest())
        self.assertEqual(camera["gameconstants_xml_sha256"],
                         hashlib.sha256((xml / "gameconstants.xml").read_bytes()).hexdigest())
        bounds = camera["bounds"]
        self.assertEqual(bounds["authority"], "project-authored")
        self.assertEqual(bounds["source_id"], "synthetic-space-target-rectangle-v1")
        self.assertEqual(bounds["map_path"], fixture.MAP_LOGICAL_PATH)
        self.assertEqual(bounds["map_sha256"], report["map"]["sha256"])
        self.assertEqual(bounds["source"], [-400, 400, 0, 800])
        self.assertEqual(bounds["render"], [-400, 400, -800, 0])
        self.assertIn("not an original-game measurement", camera["movement_policy"])
        sources = camera["constant_sources"]
        self.assertEqual({entry["file"] for entry in sources},
                         {"data/xml/tacticalcameras.xml", "data/xml/gameconstants.xml"})
        self.assertEqual({entry["definition"] for entry in sources
                          if entry["file"].endswith("tacticalcameras.xml")}, {"Space_Mode"})
        self.assertTrue(all(entry["source_sha256"] == camera[
            "tactical_xml_sha256" if entry["file"].endswith("tacticalcameras.xml")
            else "gameconstants_xml_sha256"] for entry in sources))
        self.assertEqual(camera["constant_precedence"],
                         ["effective-vfs-xml", "project-authored-map-override", "fixed-capture-frame"])
        self.assertEqual({entry["tag"] for entry in camera["constant_overrides"]},
                         {"Distance_Min", "Tactical_Min_Scroll_Speed", "Pitch_Min"})
        ledger = camera["input_defaults"]
        self.assertEqual(ledger["control_provenance"], "project-authored")
        self.assertTrue(ledger["original_binding_source"].startswith("unresolved"))
        self.assertIn({"action": "rotate", "rate_tags": ["Yaw_Per_Mouse_Unit"]}, ledger["actions"])
        self.assertIn({"action": "orbit_pitch", "rate_tags": ["Pitch_Per_Mouse_Unit"]}, ledger["actions"])
        self.assertIn({"action": "translate_x", "rate_tags": ["Distance_Min", "Distance_Max"]},
                      ledger["actions"])
        self.assertEqual(ledger["pan_speed_scale"], 1)
        # The owner floor replaces the synthetic XML Pitch_Min; Pitch_Max stays 80.
        self.assertEqual(camera["final"]["orbit_pitch_range_degrees"], [-60, 80])
        self.assertEqual(report["map"]["kind"], "space")

    def test_real_input_pan_clamp_drag_wheel_edge_focus_resize_reset(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-camera-") as directory:
            root = fixture_root(pathlib.Path(directory) / "root")
            completed, report, png = run_viewer(root, "selftest", (
                "--eawr-map-camera-config", str(CONFIG), "--eawr-map-camera-selftest"),
                unlocked_capture=True, harness=True)
            camera = report.get("space_camera", {})
            self.assertEqual(completed.returncode, 0,
                             f"{report.get('failure') or completed.stdout}: {camera.get('selftest')}")
            self.assertEqual(report["status"], "space_camera_selftest_passed")
            self._check_identity_fields(report, root)
            self.assertFalse(camera["capture_locked"])
            checks = camera["selftest"]["checks"]
            self.assertEqual({entry["name"] for entry in checks}, CHECKS)
            self.assertTrue(all(entry["passed"] for entry in checks), checks)
            self.assertEqual(camera["resets"], 1)
            self.assertGreaterEqual(camera["input_callbacks"], 8)
            self.assertGreater(camera["focus_notifications"], 0)
            self.assertGreater(camera["resize_notifications"], 0)
            self.assertGreater(camera["cancellations"], 0)
            # Stale fixed-camera sky evidence is never inherited.
            self.assertTrue(camera["sky_fidelity"].startswith("not_evaluated"))
            self.assertEqual(report["space"]["pixel_evidence_status"], "not_evaluated_interactive")
            self.assertEqual(camera["drawn_evidence"]["status"], "verified")
            self.assertGreaterEqual(camera["drawn_evidence"]["changed_pixels"], 64)
            self.assertEqual(report["space"]["camera"]["source"],
                             "interactive space map camera (project-authored config, Space_Mode constants)")
            # The trace records the wheel, drag and pan quantities separately.
            trace = camera["trace"]["entries"]
            self.assertTrue(trace)
            self.assertLessEqual(len(trace), camera["trace"]["limit"])
            wheel = [entry for entry in trace if entry["consumed"]["zoom_detents"] != 0]
            drag = [entry for entry in trace if entry["consumed"]["rotate_units"] != 0]
            self.assertTrue(wheel and drag)
            for entry in wheel:
                self.assertEqual(entry["target_before"], entry["target_after"])
                self.assertEqual(entry["yaw_before"], entry["yaw_after"])
                self.assertGreater(entry["zoom_after"], entry["zoom_before"])
            for entry in drag:
                self.assertEqual(entry["target_before"], entry["target_after"])
                self.assertEqual(entry["zoom_before"], entry["zoom_after"])
                self.assertLess(entry["yaw_after"], entry["yaw_before"])
            # A plain middle drag translates and never turns (FoC).
            translate = [entry for entry in trace if entry["consumed"]["translate_units"] != [0, 0]]
            self.assertTrue(translate)
            for entry in translate:
                self.assertNotEqual(entry["target_before"], entry["target_after"])
                self.assertEqual(entry["yaw_before"], entry["yaw_after"])
                self.assertEqual(entry["consumed"]["rotate_units"], 0)
            # The Ctrl drag went under the battle plane, within Pitch_Min.
            orbit = [entry for entry in trace if entry["consumed"]["orbit_pitch_units"] != 0]
            self.assertTrue(orbit)
            self.assertTrue(all(entry["pitch_after"] < entry["pitch_before"] for entry in orbit))
            self.assertLess(orbit[-1]["pitch_after"], 0)
            self.assertGreaterEqual(orbit[-1]["pitch_after"], -60)
            # The later middle click reset the view: default pitch, no orbit.
            self.assertEqual(camera["view_resets"], 1)
            self.assertEqual([entry["consumed"]["view_resets"] for entry in trace].count(1), 1)
            self.assertEqual(camera["final"]["pitch_degrees"], 45)
            self.assertEqual(camera["final"]["orbit_pitch_offset_degrees"], 0)
            self.assertEqual(camera["final"]["orbit_pitch_range_degrees"], [-60, 80])
            self.assertEqual(report["captures"]["configured"], hashlib.sha256(png.read_bytes()).hexdigest())
            identity = report["capture_identity"]
            self.assertEqual(identity["camera"]["position"], camera["final"]["eye"])
            self.assertEqual(identity["camera"]["target"], camera["final"]["target"])
            with Image.open(png) as image:
                self.assertEqual({"width": image.width, "height": image.height}, identity["viewport"])

    def test_timed_pan_moves_and_terminal_hold_release_freeze_identically(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-camera-terminal-") as directory:
            root = fixture_root(pathlib.Path(directory) / "root")
            config = ("--eawr-map-camera-config", str(CONFIG))
            runs = {}
            for name, extra in (("baseline", ("--eawr-map-camera-terminal-baseline-test",)),
                                ("held", ("--eawr-map-camera-terminal-hold-test",)),
                                ("released", ("--eawr-map-camera-terminal-release-test",))):
                completed, report, png = run_viewer(root, name, config + extra,
                                                    unlocked_capture=True, harness=True)
                self.assertEqual(completed.returncode, 0, f"{name}: {report.get('failure') or completed.stdout}")
                self.assertEqual(report["status"], "space_camera_render_passed", name)
                self._check_identity_fields(report, root)
                camera = report["space_camera"]
                self.assertEqual(camera["terminal_baseline_test"], name == "baseline")
                self.assertEqual(camera["settle_frames"], 3, name)
                self.assertEqual(camera["steps"],
                                 report["frame_time"]["warmup_frames"]
                                 + report["frame_time"]["timed_frames"], name)
                self.assertEqual(camera["steps"], camera["steps_at_freeze"], name)
                self.assertEqual(report["captures"]["configured"], hashlib.sha256(png.read_bytes()).hexdigest())
                self.assertEqual(camera["drawn_evidence"]["status"], "verified", name)
                runs[name] = (report, png)
            baseline, baseline_png = runs["baseline"]
            held, held_png = runs["held"]
            released, released_png = runs["released"]
            self.assertEqual(baseline["space_camera"]["trace"]["entries"], [])
            self.assertFalse(baseline["space_camera"]["terminal_pan_held_at_step"])
            # Nonvacuous: the pan reached the adapter before the declared step,
            # and only the hold run still holds it at capture.
            self.assertTrue(held["space_camera"]["terminal_pan_held_at_step"])
            self.assertTrue(released["space_camera"]["terminal_pan_held_at_step"])
            self.assertTrue(held["space_camera"]["terminal_pan_held_at_capture"])
            self.assertFalse(released["space_camera"]["terminal_pan_held_at_capture"])
            # Timed trace: one declared step, nonzero target motion by policy.
            for report in (held, released):
                camera = report["space_camera"]
                trace = camera["trace"]["entries"]
                self.assertEqual(len(trace), 1, trace)
                entry = trace[0]
                self.assertEqual(entry["step"], camera["steps"])
                self.assertAlmostEqual(entry["seconds"], camera["terminal_step_seconds"], places=6)
                self.assertEqual(entry["consumed"]["pan"], [1, 0])
                self.assertEqual(entry["target_before"], baseline["space_camera"]["final"]["target"])
                seconds = camera["terminal_step_seconds"]
                acceleration = 0.1  # Synthetic fixture Scroll_Acceleration_Factor.
                eased_travel = seconds - acceleration * (1 - math.exp(-seconds / acceleration))
                self.assertAlmostEqual(entry["target_after"][0] - entry["target_before"][0],
                                       expected_pan_speed(0.5) * eased_travel, places=2)
                self.assertEqual(entry["target_after"][1:], entry["target_before"][1:])
                self.assertEqual((entry["zoom_before"], entry["yaw_before"]), (entry["zoom_after"], entry["yaw_after"]))
                self.assertEqual(camera["final"]["target"], entry["target_after"])
            # Unlocked motion changes identity and decoded pixels.
            self.assertNotEqual(held["capture_identity"], baseline["capture_identity"])
            self.assertGreater(changed_pixels(baseline_png, held_png), 64)
            # Held and released terminal input freeze to the same identity and pixels.
            self.assertEqual(held["capture_identity"], released["capture_identity"])
            self.assertEqual(rgba(held_png), rgba(released_png))

    def test_fixed_capture_hostile_input_matches_untouched_fixed_baseline(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-camera-fixed-") as directory:
            root = fixture_root(pathlib.Path(directory) / "root")
            fixed = ("--eawr-space-camera", fixture.CAMERA)
            baseline, first, first_png = run_viewer(root, "baseline", fixed, capture=True, harness=True)
            locked, second, second_png = run_viewer(root, "locked", fixed + (
                "--eawr-map-camera-config", str(CONFIG), "--eawr-map-camera-selftest"),
                capture=True, harness=True)
            self.assertEqual(baseline.returncode, 0, first.get("failure") or baseline.stdout)
            camera = second.get("space_camera", {})
            self.assertEqual(locked.returncode, 0,
                             f"{second.get('failure') or locked.stdout}: {camera.get('selftest')}")
            # The fixed sky evidence (masks, controls, comparison phases) stays valid.
            for report in (first, second):
                self.assertEqual(report["status"], "space_primary_sky_passed")
                self.assertEqual(report["space"]["pixel_evidence_status"], "verified")
            self.assertNotIn("space_camera", first)
            self._check_identity_fields(second, root)
            self.assertTrue(camera["capture_locked"])
            self.assertTrue(camera["sky_fidelity"].startswith("fixed_camera_evidence"))
            self.assertEqual(camera["steps"], 0)
            self.assertEqual(camera["trace"]["entries"], [])
            self.assertTrue(camera["resize_active_at_capture"])
            self.assertGreater(camera["ignored_ineligible"], 0)
            self.assertEqual({entry["name"] for entry in camera["selftest"]["checks"]}, CHECKS)
            self.assertTrue(all(entry["passed"] for entry in camera["selftest"]["checks"]))
            self.assertEqual(first_png.read_bytes(), second_png.read_bytes())
            self.assertEqual(rgba(first_png), rgba(second_png))
            self.assertEqual(first["capture_identity"], second["capture_identity"])
            self.assertEqual(first["captures"], second["captures"])
            self.assertEqual(first["space"]["surfaces"], second["space"]["surfaces"])

    def test_incompatible_configs_bindings_and_options_reject(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-camera-reject-") as directory:
            base = pathlib.Path(directory)
            root = fixture_root(base / "root")
            source = CONFIG.read_text(encoding="utf-8")
            for name in ("map-camera-bindings.json", "camera-bindings.json", "space-map-camera-bindings.json"):
                (base / name).write_bytes((CONFIG_DIR / name).read_bytes())
            variants = {
                "land-bindings": (source.replace("space-map-camera-bindings.json", "map-camera-bindings.json"),
                                  (), "incompatible land bindings"),
                "free-bindings": (source.replace("space-map-camera-bindings.json", "camera-bindings.json"),
                                  (), "unsupported free-camera bindings"),
                "land-config": ((CONFIG_DIR / "map-camera.xml").read_text(encoding="utf-8"), (), "EAWR-CAMERA-0501"),
                "identity": (source.replace('map_sha256="3537', 'map_sha256="4537'), (), "EAWR-CAMERA-0501"),
                "inverted-bounds": (source.replace('min_x="-400" max_x="400"', 'min_x="400" max_x="-400"'),
                                    (), "EAWR-CAMERA-0401"),
                "free-selftest": (source, ("--eawr-map-free-selftest",), "no free flight"),
                "unlocked-fixed-camera": (source, ("--eawr-space-camera", fixture.CAMERA), "--eawr-space-camera"),
                "unlocked-control": (source, ("--eawr-space-control", "occluder"), "--eawr-space-control"),
                # The labelled MeshAdditive control resolves to a base scene
                # control of none or occluder; the guard still refuses both.
                "unlocked-meshadditive": (source, ("--eawr-space-control", fixture.MESHADDITIVE_CONTROL),
                                          "--eawr-space-control applies only to the fixed capture camera"),
                "unlocked-meshadditive-occluder": (
                    source, ("--eawr-space-control", fixture.MESHADDITIVE_CONTROL + " occluder"),
                    "--eawr-space-control applies only to the fixed capture camera"),
            }
            for name, (text, extra, needle) in variants.items():
                with self.subTest(name=name):
                    config = base / f"{name}.xml"
                    config.write_text(text, encoding="utf-8")
                    completed, report, _ = run_viewer(root, name,
                        ("--eawr-map-camera-config", str(config)) + extra,
                        harness=name in ("free-bindings", "unlocked-fixed-camera"))
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertEqual(report["status"], "failed")
                    self.assertIn(needle, report["failure"])
                    self.assertNotIn("space_camera", report)


if __name__ == "__main__":
    unittest.main()
