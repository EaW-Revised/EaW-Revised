"""Synthetic graphical integration checks for land MapMode free flight."""

import json
import hashlib
import os
import pathlib
import sys
import tempfile
import unittest

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from test_map_camera import fixture_root, run_viewer


ROOT = pathlib.Path(__file__).resolve().parents[3]
CONFIG = ROOT / "apps/viewer/project/config/map-camera-free.xml"
BINDINGS = ROOT / "apps/viewer/project/config/map-camera-free-bindings.json"
RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")


class MapFreeCameraStructure(unittest.TestCase):
    def test_explicit_v2_land_free_fixture(self):
        self.assertIn('version="2" provenance="project-authored"',
                      CONFIG.read_text(encoding="utf-8"))
        table = json.loads(BINDINGS.read_text(encoding="utf-8"))
        self.assertEqual(table["version"], 2)
        self.assertEqual(table["provenance"], "project-authored")
        self.assertEqual({entry["context"] for entry in table["bindings"]}, {"land", "free"})
        self.assertEqual({entry["context"] for entry in table["bindings"]
                          if entry["action"] == "free_toggle"}, {"land", "free"})
        self.assertEqual(set(table["free_camera"]), {
            "move_speed", "vertical_speed", "look_degrees_per_unit",
            "pitch_min_degrees", "pitch_max_degrees"})


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST for pinned graphical checks")
class MapFreeCameraGraphical(unittest.TestCase):
    def test_real_callback_entry_move_look_exit_and_cancellations(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-free-") as directory:
            root = fixture_root(pathlib.Path(directory))
            completed, report, _ = run_viewer(root, "flight", (
                "--eawr-map-camera-config", str(CONFIG), "--eawr-map-free-selftest"))
            self.assertEqual(completed.returncode, 0,
                             f"{report.get('failure') or completed.stdout}: "
                             f"{report.get('map_camera', {}).get('free_selftest')}")
            self.assertEqual(report["status"], "map_camera_selftest_passed")
            camera = report["map_camera"]
            free = camera["free_camera"]
            self.assertEqual(camera["config_version"], 2)
            self.assertEqual(camera["active_mode"], "land")
            self.assertEqual(free["settings_provenance"], "project-authored-bindings-v2")
            self.assertEqual(free["transitions"], ["enter", "exit"])
            self.assertEqual((free["entries"], free["exits"], free["rejections"]), (1, 1, 0))
            self.assertNotEqual(free["pose"]["eye"], report["capture_identity"]["camera"]["position"])
            checks = camera["free_selftest"]["checks"]
            self.assertGreaterEqual(len(checks), 7)
            self.assertTrue(all(item["passed"] for item in checks), checks)
            self.assertGreater(camera["resize_notifications"], 0)
            self.assertGreater(camera["focus_notifications"], 0)
            self.assertGreater(camera["cancellations"], 2)

    def test_incompatible_entry_pitch_is_rejected_without_change(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-free-reject-") as directory:
            root = fixture_root(pathlib.Path(directory))
            table = json.loads(BINDINGS.read_text(encoding="utf-8"))
            table["free_camera"]["pitch_min_degrees"] = -1.0
            table["free_camera"]["pitch_max_degrees"] = 1.0
            (root / BINDINGS.name).write_text(json.dumps(table), encoding="utf-8")
            config = root / CONFIG.name
            config.write_bytes(CONFIG.read_bytes())
            baseline, initial, _ = run_viewer(root, "initial", (
                "--eawr-map-camera-config", str(config)))
            completed, report, _ = run_viewer(root, "pitch", (
                "--eawr-map-camera-config", str(config), "--eawr-map-free-selftest"))
            self.assertEqual(baseline.returncode, 0, initial.get("failure") or baseline.stdout)
            self.assertEqual(completed.returncode, 0, report.get("failure") or completed.stdout)
            camera = report["map_camera"]
            self.assertEqual(camera["free_camera"]["transitions"], ["rejected"])
            self.assertEqual(camera["free_camera"]["entries"], 0)
            self.assertEqual(camera["free_camera"]["rejections"], 1)
            self.assertIn("pitch", camera["free_camera"]["rejection"])
            self.assertTrue(all(item["passed"] for item in camera["free_selftest"]["checks"]))
            self.assertEqual(camera["active_mode"], "land")
            self.assertEqual(report["capture_identity"], initial["capture_identity"])

    def test_fixed_capture_is_baseline_after_persistent_resize(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-free-fixed-") as directory:
            root = fixture_root(pathlib.Path(directory))
            baseline, first, first_png = run_viewer(root, "baseline", capture=True)
            locked, second, second_png = run_viewer(root, "locked", (
                "--eawr-map-camera-config", str(CONFIG), "--eawr-map-free-selftest"), capture=True)
            self.assertEqual(baseline.returncode, 0, first.get("failure") or baseline.stdout)
            self.assertEqual(locked.returncode, 0,
                             f"{second.get('failure') or locked.stdout}: "
                             f"{second.get('map_camera', {}).get('free_selftest')}")
            self.assertEqual(first_png.read_bytes(), second_png.read_bytes())
            self.assertEqual(first["capture_identity"], second["capture_identity"])
            camera = second["map_camera"]
            self.assertTrue(camera["capture_locked"])
            self.assertEqual(camera["free_camera"]["entries"], 0)
            self.assertEqual(camera["free_camera"]["steps"], 0)
            self.assertTrue(camera["resize_active_at_capture"])
            self.assertTrue(all(item["passed"] for item in camera["free_selftest"]["checks"]))

    def test_held_free_motion_settles_to_reported_pose(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-free-settle-") as directory:
            root = fixture_root(pathlib.Path(directory))
            baseline, initial, baseline_png = run_viewer(root, "baseline", (
                "--eawr-map-camera-config", str(CONFIG)), unlocked_capture=True)
            held, first, held_png = run_viewer(root, "held", (
                "--eawr-map-camera-config", str(CONFIG),
                "--eawr-map-free-terminal-hold-test"), unlocked_capture=True)
            released, second, reference_png = run_viewer(root, "released", (
                "--eawr-map-camera-config", str(CONFIG),
                "--eawr-map-free-terminal-release-test"), unlocked_capture=True)
            self.assertEqual(baseline.returncode, 0, initial.get("failure") or baseline.stdout)
            self.assertEqual(held.returncode, 0, first.get("failure") or held.stdout)
            self.assertEqual(released.returncode, 0, second.get("failure") or released.stdout)
            held_camera = first["map_camera"]
            released_camera = second["map_camera"]
            self.assertEqual(held_camera["active_mode"], "free")
            self.assertEqual(released_camera["active_mode"], "free")
            for camera in (held_camera, released_camera):
                self.assertTrue(camera["free_terminal_forward_held_at_step"])
                self.assertGreaterEqual(camera["events_routed"], 2)
                self.assertEqual(camera["settle_frames"], 3)
                self.assertEqual(camera["steps_at_freeze"], camera["steps"])
            self.assertTrue(held_camera["free_terminal_forward_held_at_capture"])
            self.assertFalse(released_camera["free_terminal_forward_held_at_capture"])
            self.assertGreater(released_camera["events_routed"], held_camera["events_routed"])
            self.assertNotEqual(first["capture_identity"]["camera"]["position"],
                                initial["capture_identity"]["camera"]["position"])
            self.assertEqual(first["capture_identity"], second["capture_identity"])
            with (Image.open(baseline_png) as baseline_image,
                  Image.open(held_png) as held_image,
                  Image.open(reference_png) as reference_image):
                baseline_pixels = baseline_image.convert("RGBA").tobytes()
                held_pixels = held_image.convert("RGBA").tobytes()
                reference_pixels = reference_image.convert("RGBA").tobytes()
            self.assertNotEqual(held_pixels, baseline_pixels)
            self.assertEqual(held_pixels, reference_pixels)
            self.assertEqual(first["evidence"]["capture_sha256"],
                             hashlib.sha256(held_png.read_bytes()).hexdigest())

    def test_space_binding_rejected(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-free-space-") as directory:
            root = fixture_root(pathlib.Path(directory))
            table = json.loads(BINDINGS.read_text(encoding="utf-8"))
            entry = dict(next(item for item in table["bindings"]
                              if item["id"] == "land.pan-left.a"))
            entry["id"] = "space.pan-left.a"
            entry["context"] = "space"
            table["bindings"].append(entry)
            (root / BINDINGS.name).write_text(json.dumps(table), encoding="utf-8")
            config = root / CONFIG.name
            config.write_bytes(CONFIG.read_bytes())
            completed, report, _ = run_viewer(root, "space", (
                "--eawr-map-camera-config", str(config)))
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(report["status"], "failed")
            self.assertIn("space", report["failure"])


if __name__ == "__main__":
    unittest.main()
