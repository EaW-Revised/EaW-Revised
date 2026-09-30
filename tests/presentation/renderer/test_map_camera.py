"""Graphical land MapMode camera loop against project-authored synthetic data."""

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
import camera_fixture  # noqa: E402
import scene_fixture  # noqa: E402

CONFIG = ROOT / "apps/viewer/project/config/map-camera.xml"
BINDINGS = ROOT / "apps/viewer/project/config/map-camera-bindings.json"
RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")


def fixture_root(path):
    scene_fixture.write_fixture_root(path)
    xml = path / "GameData/Data/XML"
    (xml / "tacticalcameras.xml").write_text(camera_fixture.tactical_cameras_xml(), encoding="utf-8")
    (xml / "gameconstants.xml").write_text(camera_fixture.game_constants_xml(), encoding="utf-8")
    return path


def run_viewer(root, name, extra=(), capture=False, unlocked_capture=False):
    executable = os.environ["EAWR_GODOT_EXECUTABLE"]
    report = root / f"{name}.json"
    png = root / f"{name}.png"
    command = [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-map", scene_fixture.MAP_LOGICAL_PATH,
               "--eawr-game-root", str(root), "--eawr-report", str(report)]
    if capture:
        command += ["--eawr-capture", str(png)]
    if unlocked_capture:
        command += ["--eawr-map-camera-unlocked-capture", str(png)]
    command += list(extra)
    completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=120, check=False)
    if not report.exists():
        raise AssertionError(completed.stdout)
    return completed, json.loads(report.read_text(encoding="utf-8")), png


class MapCameraStructure(unittest.TestCase):
    def test_authored_config_and_binding_table(self):
        self.assertIn('version="1" provenance="project-authored"', CONFIG.read_text(encoding="utf-8"))
        table = json.loads(BINDINGS.read_text(encoding="utf-8"))
        self.assertEqual(table["version"], 1)
        self.assertEqual(table["provenance"], "project-authored")
        self.assertEqual({entry["context"] for entry in table["bindings"]}, {"land"})
        self.assertNotIn("free_toggle", {entry["action"] for entry in table["bindings"]})
        # Owner feel check 2026-09-25: land pan at half speed. FoC middle-button
        # law (RO-7, #295): a plain drag translates at x4, Ctrl rotates at x1.
        self.assertEqual(table["pan_speed_scale"], 0.5)
        self.assertIs(table["click_reset"], True)
        self.assertIs(table["screen_mouse_units"], True)
        self.assertEqual({(entry["action"], entry["device"], entry["control"], entry["scale"])
                          for entry in table["bindings"] if entry.get("modifiers") == ["ctrl"]}, {
            ("rotate_grab", "mouse_button", "middle", 1.0), ("rotate", "mouse_motion", "x", -1.0),
            ("orbit_pitch", "mouse_motion", "y", -1.0)})
        self.assertEqual({(entry["action"], entry["control"], entry["scale"])
                          for entry in table["bindings"]
                          if entry["device"] == "mouse_motion" and "modifiers" not in entry}, {
            ("translate_x", "x", 4.0), ("translate_y", "y", -4.0)})
        presets = (ROOT / "apps/viewer/project/export_presets.cfg").read_text(encoding="utf-8")
        self.assertEqual(presets.count("config/*.xml"), 3)

    def test_committed_configs_carry_no_constant_overrides(self):
        # Default runs and fixed captures stay byte-identical to the pre-override path.
        for name in ("map-camera.xml", "map-camera-free.xml"):
            text = (ROOT / "apps/viewer/project/config" / name).read_text(encoding="utf-8")
            self.assertNotIn("constant_overrides", text, name)
        for name in ("map-camera-bindings.json", "map-camera-free-bindings.json",
                     "space-map-camera-bindings.json", "camera-bindings.json"):
            table = json.loads((ROOT / "apps/viewer/project/config" / name).read_text(encoding="utf-8"))
            self.assertEqual(table["provenance"], "project-authored", name)


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST for pinned graphical checks")
class MapCameraGraphical(unittest.TestCase):
    def test_ordinary_held_pan_captures_settled_reported_pose(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-camera-settle-") as directory:
            root = fixture_root(pathlib.Path(directory))
            held, report, held_png = run_viewer(root, "held", (
                "--eawr-map-camera-config", str(CONFIG),
                "--eawr-map-camera-terminal-hold-test"), unlocked_capture=True)
            self.assertEqual(held.returncode, 0, report.get("failure") or held.stdout)
            self.assertEqual(report["status"], "map_camera_render_passed")
            camera = report["map_camera"]
            self.assertFalse(camera["selftest"]["requested"])
            self.assertTrue(camera["terminal_hold_test"])
            self.assertFalse(camera["capture_locked"])
            self.assertEqual(camera["settle_frames"], 3)
            self.assertEqual(camera["steps_at_freeze"], camera["steps"])
            target = report["capture_identity"]["camera"]["target"]
            self.assertGreater(target[0], 320)
            self.assertLess(target[0], 640)
            self.assertGreater(target[2], -480)
            self.assertLess(target[2], 0)
            self.assertEqual(report["evidence"]["capture_sha256"],
                             hashlib.sha256(held_png.read_bytes()).hexdigest())

            reference_config = root / "settled-reference.xml"
            reference_config.write_text(CONFIG.read_text(encoding="utf-8").replace(
                'target_x="320" target_y="240"',
                f'target_x="{target[0]:.9g}" target_y="{-target[2]:.9g}"'),
                encoding="utf-8")
            (root / BINDINGS.name).write_bytes(BINDINGS.read_bytes())
            settled, reference, reference_png = run_viewer(root, "reference", (
                "--eawr-map-camera-config", str(reference_config)), unlocked_capture=True)
            self.assertEqual(settled.returncode, 0, reference.get("failure") or settled.stdout)
            self.assertEqual(reference["status"], "map_camera_render_passed")
            self.assertEqual(report["capture_identity"]["camera"],
                             reference["capture_identity"]["camera"])
            # Both unlocked probes pin their viewport at activation (#140), so a
            # window manager's resize cannot change either image's size.
            with Image.open(held_png) as held_image, Image.open(reference_png) as reference_image:
                self.assertEqual(held_image.size, (1280, 720))
                self.assertEqual(held_image.size,
                                 (report["capture_identity"]["viewport"]["width"],
                                  report["capture_identity"]["viewport"]["height"]))
                self.assertEqual(reference_image.size,
                                 (reference["capture_identity"]["viewport"]["width"],
                                  reference["capture_identity"]["viewport"]["height"]))
                self.assertEqual(held_image.size, reference_image.size)
                self.assertEqual(held_image.convert("RGBA").tobytes(),
                                 reference_image.convert("RGBA").tobytes())

    def test_pan_clamp_yaw_wheel_reset_focus_resize(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-camera-") as directory:
            root = fixture_root(pathlib.Path(directory))
            completed, report, _ = run_viewer(root, "movement", (
                "--eawr-map-camera-config", str(CONFIG), "--eawr-map-camera-selftest"))
            self.assertEqual(completed.returncode, 0, report.get("failure") or completed.stdout)
            self.assertEqual(report["status"], "map_camera_selftest_passed")
            camera = report["map_camera"]
            self.assertEqual(camera["bounds_render"], [0, 640, -480, 0])
            self.assertEqual(camera["provenance"], "project-authored")
            self.assertFalse(camera["capture_locked"])
            self.assertEqual(camera["resets"], 1)
            self.assertGreater(camera["input_callbacks"], 0)
            self.assertGreater(camera["focus_notifications"], 0)
            self.assertGreater(camera["resize_notifications"], 0)
            self.assertTrue(report["evidence"]["verified"])
            self.assertGreaterEqual(report["evidence"]["unlocked_changed_pixel_coverage"], 0.01)
            self.assertGreaterEqual(report["evidence"]["unlocked_terminal_changed_pixels"], 64)
            self.assertNotEqual(camera["final"]["target"][0], camera["initial"]["target_source"][0])
            self.assertTrue(camera["constant_sources"])
            self.assertEqual({entry["file"] for entry in camera["constant_sources"]}, {
                "data/xml/tacticalcameras.xml", "data/xml/gameconstants.xml"})
            self.assertTrue(all(entry["source_sha256"] == camera[
                "tactical_xml_sha256" if entry["file"].endswith("tacticalcameras.xml")
                else "gameconstants_xml_sha256"] for entry in camera["constant_sources"]))
            self.assertEqual({entry["name"] for entry in camera["selftest"]["checks"]}, {
                "focus established before input", "pan reaches authored X clamp",
                "grabbed motion translates without turning",
                "wheel changes zoom", "focus loss cancels held pan",
                "real resize cancels held pan", "pointer exit cancels edge pan",
                "reset restores authored pose",
                "input callbacks reached map adapter", "capture lock isolates input",
                "Ctrl grabbed motion rotates yaw and tilts", "Ctrl click keeps the view",
                "middle click resets the view around the target"})
            self.assertTrue(all(entry["passed"] for entry in camera["selftest"]["checks"]))
            # #348 owner deviation: FoC land tilts 0 per mouse unit, the project tilts land at
            # FoC space's -1.5 within 5..85; the middle click reset the view and its tilt.
            self.assertEqual(camera["final"]["orbit_pitch_range_degrees"], [5, 85])
            self.assertEqual(camera["final"]["orbit_pitch_per_mouse_unit"], -1.5)
            self.assertEqual(camera["final"]["orbit_pitch_offset_degrees"], 0)
            self.assertEqual(camera["view_resets"], 1)
            self.assertEqual(camera["input_defaults"]["pan_speed_scale"], 0.5)

    def test_fixed_capture_isolated_from_hostile_input(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-camera-lock-") as directory:
            root = fixture_root(pathlib.Path(directory))
            # Both captures use the fixed overview frame through the map camera
            # bridge, which pins the capture viewport on desktop-sized windows.
            baseline, first, first_png = run_viewer(
                root, "baseline", ("--eawr-map-camera-config", str(CONFIG)), capture=True)
            locked, second, second_png = run_viewer(root, "locked", (
                "--eawr-map-camera-config", str(CONFIG), "--eawr-map-camera-selftest"), capture=True)
            self.assertEqual(baseline.returncode, 0, first.get("failure") or baseline.stdout)
            self.assertEqual(locked.returncode, 0,
                             f"{second.get('failure') or locked.stdout}: "
                             f"{second.get('map_camera', {}).get('selftest', {}).get('checks')}")
            self.assertEqual(first["status"], "map_render_passed")
            self.assertEqual(second["status"], "map_camera_selftest_passed")
            self.assertEqual(first_png.read_bytes(), second_png.read_bytes())
            self.assertEqual(first["capture_identity"]["camera"],
                             second["capture_identity"]["camera"])
            self.assertEqual(first["capture_identity"]["viewport"],
                             second["capture_identity"]["viewport"])
            with Image.open(first_png) as baseline_image, Image.open(second_png) as locked_image:
                baseline_pixels = baseline_image.convert("RGBA")
                locked_pixels = locked_image.convert("RGBA")
                self.assertEqual(baseline_pixels.size, (1280, 720))
                self.assertEqual(locked_pixels.size, baseline_pixels.size)
                self.assertEqual(locked_pixels.tobytes(), baseline_pixels.tobytes())
                self.assertEqual(first["capture_identity"]["viewport"],
                                 {"width": locked_pixels.width, "height": locked_pixels.height})
            self.assertTrue(second["map_camera"]["capture_locked"])
            self.assertTrue(second["map_camera"]["resize_active_at_capture"])
            self.assertEqual(second["map_camera"]["steps"], 0)
            self.assertTrue(all(entry["passed"] for entry in second["map_camera"]["selftest"]["checks"]))
            self.assertEqual(second["evidence"]["capture_sha256"],
                             hashlib.sha256(first_png.read_bytes()).hexdigest())

    def test_unsupported_free_toggle_fails_activation(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-camera-free-") as directory:
            root = fixture_root(pathlib.Path(directory))
            config = root / "rejected.xml"
            config.write_text(CONFIG.read_text(encoding="utf-8").replace(
                "map-camera-bindings.json", "camera-bindings.json"), encoding="utf-8")
            (root / "camera-bindings.json").write_bytes(
                (ROOT / "apps/viewer/project/config/camera-bindings.json").read_bytes())
            completed, report, _ = run_viewer(root, "rejected", (
                "--eawr-map-camera-config", str(config)))
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(report["status"], "failed")
            self.assertIn("free-toggle", report["failure"])

    def override_config(self, root, name, overrides):
        config = root / f"{name}.xml"
        config.write_text(CONFIG.read_text(encoding="utf-8").replace("</map_camera>",
            '  <constant_overrides schema="eawr-map-camera-overrides" version="1"'
            ' source_id="synthetic-land-overrides-v1" authority="project-authored">\n'
            + "".join(f'    <override tag="{tag}" value="{value}"/>\n' for tag, value in overrides)
            + "  </constant_overrides>\n</map_camera>"), encoding="utf-8")
        (root / BINDINGS.name).write_bytes(BINDINGS.read_bytes())
        return config

    def test_map_constant_override_reaches_frame_with_provenance(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-camera-override-") as directory:
            root = fixture_root(pathlib.Path(directory))
            config = self.override_config(root, "override", [("Far_Clip", "12000")])
            completed, report, _ = run_viewer(root, "override", (
                "--eawr-map-camera-config", str(config), "--eawr-map-camera-selftest"))
            self.assertEqual(completed.returncode, 0, report.get("failure") or completed.stdout)
            self.assertEqual(report["status"], "map_camera_selftest_passed")
            camera = report["map_camera"]
            self.assertTrue(all(entry["passed"] for entry in camera["selftest"]["checks"]))
            self.assertEqual(camera["constant_precedence"],
                             ["effective-vfs-xml", "project-authored-map-override", "fixed-capture-frame"])
            self.assertEqual(camera["constant_overrides"], [{
                "tag": "Far_Clip", "value": 12000, "replaced_value": 8000,
                "file": "override.xml", "source_sha256": camera["config_sha256"],
                "source_id": "synthetic-land-overrides-v1", "authority": "project-authored",
                "effect": "consumed", "effect_reason": "read by the bounded tactical controller",
                "replaced": {"file": "data/xml/tacticalcameras.xml",
                             "source_sha256": camera["tactical_xml_sha256"],
                             "definition": "Land_Mode", "status": "supplied"}}])
            self.assertEqual(camera["config_sha256"], hashlib.sha256(config.read_bytes()).hexdigest())
            # The XML rows still describe the XML layer.
            far = [entry for entry in camera["constant_sources"] if entry["tag"] == "Far_Clip"]
            self.assertEqual(far[0]["file"], "data/xml/tacticalcameras.xml")
            ledger = camera["input_defaults"]
            self.assertEqual(ledger["control_provenance"], "project-authored")
            self.assertTrue(ledger["original_binding_source"].startswith("unresolved"))
            self.assertIn({"action": "zoom",
                           "rate_tags": ["Distance_Per_Mouse_Unit", "Distance_Min", "Distance_Max"]},
                          ledger["actions"])
            self.assertEqual(report["capture_identity"]["camera"]["far"], 12000)

    def test_map_constant_override_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-camera-override-bad-") as directory:
            root = fixture_root(pathlib.Path(directory))
            variants = {
                "unknown-tag": ([("Camera_Height", "1")], "not a scalar camera constant"),
                "spline": ([("Pitch_Spline", "0,1 1,2")], "not overridable"),
                "duplicate": ([("Far_Clip", "9000"), ("Far_Clip", "9500")], "more than once"),
                "malformed": ([("Far_Clip", "far")], "finite decimal"),
                "invalidating": ([("Distance_Max", "10")], "Distance_Max must exceed"),
                "negative-scroll": ([("Tactical_Min_Scroll_Speed", "-1")],
                                    "<Tactical_Min_Scroll_Speed> must not be negative"),
                "inverted-scroll": ([("Tactical_Max_Scroll_Speed", "1")],
                                    "overridden Tactical_Max_Scroll_Speed must not be below"),
                "controller-fov": ([("Fov_Max", "200"), ("Fov_Default", "190")],
                                   "the tactical controller refuses the overridden constants"),
                "controller-yaw": ([("Yaw_Min", "10")],
                                   "the tactical controller refuses the overridden constants"),
            }
            for name, (overrides, needle) in variants.items():
                with self.subTest(name=name):
                    config = self.override_config(root, name, overrides)
                    completed, report, _ = run_viewer(root, name, (
                        "--eawr-map-camera-config", str(config)))
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertEqual(report["status"], "failed")
                    self.assertIn(needle, report["failure"])
                    self.assertNotIn("map_camera", report)

    def test_fixed_capture_outranks_map_constant_override(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-camera-override-lock-") as directory:
            root = fixture_root(pathlib.Path(directory))
            config = self.override_config(root, "override-lock",
                                          [("Far_Clip", "12000"), ("Distance_Per_Mouse_Unit", "160")])
            baseline, first, first_png = run_viewer(
                root, "baseline", ("--eawr-map-camera-config", str(CONFIG)), capture=True)
            locked, second, second_png = run_viewer(root, "override-lock", (
                "--eawr-map-camera-config", str(config), "--eawr-map-camera-selftest"), capture=True)
            self.assertEqual(baseline.returncode, 0, first.get("failure") or baseline.stdout)
            self.assertEqual(locked.returncode, 0, second.get("failure") or locked.stdout)
            self.assertEqual(second["status"], "map_camera_selftest_passed")
            self.assertEqual(len(second["map_camera"]["constant_overrides"]), 2)
            self.assertTrue(second["map_camera"]["capture_locked"])
            self.assertEqual(first_png.read_bytes(), second_png.read_bytes())
            self.assertEqual(first["capture_identity"]["camera"],
                             second["capture_identity"]["camera"])
            self.assertEqual(first["capture_identity"]["viewport"],
                             second["capture_identity"]["viewport"])

    def test_config_version_identity_and_bounds_fail_closed(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-camera-config-") as directory:
            root = fixture_root(pathlib.Path(directory))
            source = CONFIG.read_text(encoding="utf-8")
            variants = {
                "version": source.replace('version="1"', 'version="2"'),
                "identity": source.replace('map_sha256="3ae3', 'map_sha256="4ae3'),
                "bounds": source.replace('min_x="0" max_x="640"', 'min_x="640" max_x="0"'),
                "first-child": source.replace("  <bounds ", "  <unexpected/>\n  <bounds "),
            }
            (root / "map-camera-bindings.json").write_bytes(BINDINGS.read_bytes())
            for name, text in variants.items():
                with self.subTest(name=name):
                    config = root / f"{name}.xml"
                    config.write_text(text, encoding="utf-8")
                    completed, report, _ = run_viewer(root, name, (
                        "--eawr-map-camera-config", str(config)))
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertEqual(report["status"], "failed")
                    self.assertTrue(report["failure"])
                    self.assertNotIn("map_camera", report)


if __name__ == "__main__":
    unittest.main()
