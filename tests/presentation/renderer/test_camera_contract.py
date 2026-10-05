"""P1-09 tactical camera contract.

Three jobs:

1. Structural assertions that always run: the viewer's option surface, the fact
   that the camera mode never touches the frozen P1-01 fixed-capture framing,
   and the report fields the completion evidence depends on.
2. A drift guard. The C++ contract test transcribes real XML constants into
   tests/presentation/camera/main.cpp. This re-reads every one of them out of
   the committed plan/inventories/camera-constants.json and fails if a single
   transcription disagrees, so the C++ expectations cannot silently stop being
   XML-sourced.
3. An opt-in graphical run, gated on the same environment switches as the
   accepted P1-01 runtime exercise, that drives the real viewer against a
   wholly original synthetic camera fixture.
"""

import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "assets" / "fixtures"))

import camera_fixture as fixture  # noqa: E402
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import mode_source  # noqa: E402

INVENTORY = ROOT / "plan/inventories/camera-constants.json"
CAMERA_TEST = ROOT / "tests/presentation/camera/main.cpp"

# Every scalar Constants field the C++ test sets, and the XML tag it transcribes.
FIELD_TAGS = {
    "distance_min": "Distance_Min",
    "distance_max": "Distance_Max",
    "distance_default": "Distance_Default",
    "distance_per_mouse_unit": "Distance_Per_Mouse_Unit",
    "distance_smooth_time": "Distance_Smooth_Time",
    "pitch_min": "Pitch_Min",
    "pitch_max": "Pitch_Max",
    "pitch_default": "Pitch_Default",
    "pitch_per_mouse_unit": "Pitch_Per_Mouse_Unit",
    "pitch_per_zoom_unit": "Pitch_Per_Zoom_Unit",
    "pitch_when_zoomed_in": "Pitch_When_Zoomed_In",
    "pitch_zoom_begin_fraction": "Pitch_Zoom_Begin_Fraction",
    "yaw_min": "Yaw_Min",
    "yaw_max": "Yaw_Max",
    "yaw_default": "Yaw_Default",
    "yaw_per_mouse_unit": "Yaw_Per_Mouse_Unit",
    "fov_min": "Fov_Min",
    "fov_max": "Fov_Max",
    "fov_default": "Fov_Default",
    "fov_per_mouse_unit": "Fov_Per_Mouse_Unit",
    "near_clip": "Near_Clip",
    "far_clip": "Far_Clip",
}

SCROLL_TAGS = {
    "tactical_min_scroll_speed": "Tactical_Min_Scroll_Speed",
    "tactical_max_scroll_speed": "Tactical_Max_Scroll_Speed",
    "tactical_edge_scroll_region": "Tactical_Edge_Scroll_Region",
    "tactical_offscreen_scroll_region": "Tactical_Offscreen_Scroll_Region",
    "push_scroll_speed_modifier": "Push_Scroll_Speed_Modifier",
    "scroll_acceleration_factor": "Scroll_Acceleration_Factor",
    "scroll_deceleration_factor": "Scroll_Deceleration_Factor",
}

# C++ helper -> (profile, XML definition, base helper it copies from).
TRANSCRIPTIONS = {
    "eaw_land": ("eaw", "Land_Mode", None),
    "foc_land": ("foc", "Land_Mode", "eaw_land"),
    "eaw_space": ("eaw", "Space_Mode", None),
    "remake_land": ("remake", "Land_Mode", None),
    "remake_space": ("remake", "Space_Mode", "remake_land"),
}

_SCALAR = re.compile(r"constants\.(\w+) = (-?[\d.]+)F;")
_SPLINE = re.compile(r"constants\.(\w+_spline) = \{(.+?)\};", re.S)
_POINT = re.compile(r"\{(-?[\d.]+)F, (-?[\d.]+)F\}")


def load_inventory():
    return json.loads(INVENTORY.read_text(encoding="utf-8"))


def inventory_fields(inventory, profile, definition):
    for row in inventory["profiles"]:
        if row["profile"] != profile:
            continue
        for mode in row["modes"]:
            if mode["name"] == definition:
                return {field["tag"]: field for field in mode["fields"]}
    raise AssertionError(f"{profile}/{definition} is absent from the inventory")


def inventory_scroll(inventory, profile):
    for row in inventory["profiles"]:
        if row["profile"] == profile:
            return {field["tag"]: field for field in row["scroll_constants"]}
    raise AssertionError(f"{profile} is absent from the inventory")


def cpp_helper_body(name):
    source = CAMERA_TEST.read_text(encoding="utf-8")
    start = source.index(f"camera::Constants {name}() {{")
    end = source.index("\n}\n", start)
    return source[start:end]


def cpp_transcription(name):
    """Scalars and splines a C++ helper sets, merged over the helper it copies."""
    profile, definition, base = TRANSCRIPTIONS[name]
    values = dict(cpp_transcription(base)[0]) if base else {}
    splines = dict(cpp_transcription(base)[1]) if base else {}
    body = cpp_helper_body(name)
    for field, literal in _SCALAR.findall(body):
        values[field] = float(literal)
    for field, points in _SPLINE.findall(body):
        splines[field] = [[float(a), float(b)] for a, b in _POINT.findall(points)]
    del profile, definition
    return values, splines


class CameraInventoryDriftGuard(unittest.TestCase):
    def test_inventory_shape_is_complete(self):
        inventory = load_inventory()
        self.assertEqual(inventory["schema_version"], 1)
        self.assertEqual(inventory["sources"]["camera"], "data/xml/tacticalcameras.xml")
        self.assertEqual(inventory["sources"]["constants"], "data/xml/gameconstants.xml")
        self.assertEqual(inventory["summary"]["profiles"], 3)
        self.assertEqual(inventory["summary"]["modes"], 9)
        self.assertEqual(inventory["summary"]["missing_scroll_tags"], 0)
        self.assertEqual(inventory["summary"]["unexpected_camera_definitions"], 0)
        self.assertTrue(inventory["manifest_id"])
        for row in inventory["profiles"]:
            for mode in row["modes"]:
                for field in mode["fields"]:
                    # Only tag names, values and logical source paths, never a
                    # native path or a game root.
                    self.assertEqual(set(field) - {"number", "points"},
                                     {"layer_id", "logical_path", "tag", "value"})
                    self.assertFalse(field["logical_path"].startswith("/"))
                    self.assertNotIn(":", field["logical_path"])

    def test_cpp_expectations_match_the_committed_inventory(self):
        inventory = load_inventory()
        for name, (profile, definition, _base) in TRANSCRIPTIONS.items():
            values, splines = cpp_transcription(name)
            fields = inventory_fields(inventory, profile, definition)
            scroll = inventory_scroll(inventory, profile)
            checked = 0
            for field, literal in values.items():
                if field in SCROLL_TAGS:
                    row = scroll.get(SCROLL_TAGS[field])
                    self.assertIsNotNone(row, f"{name}.{field} has no inventory row")
                    self.assertAlmostEqual(literal, row["number"], places=4,
                                           msg=f"{name}.{field} drifted from the inventory")
                    checked += 1
                    continue
                tag = FIELD_TAGS.get(field)
                if tag is None:
                    continue
                row = fields.get(tag)
                self.assertIsNotNone(row, f"{name}: {tag} is absent from the inventory")
                self.assertIn("number", row, f"{name}: {tag} is not numeric in the inventory")
                self.assertAlmostEqual(literal, row["number"], places=4,
                                       msg=f"{name}.{field} ({tag}) drifted from the inventory")
                checked += 1
            self.assertGreaterEqual(checked, 20, f"{name} transcribes too few constants")

            for field, points in splines.items():
                tag = "Distance_Spline" if field == "distance_spline" else "Pitch_Spline"
                row = fields.get(tag)
                self.assertIsNotNone(row, f"{name}: {tag} is absent from the inventory")
                self.assertIn("points", row, f"{name}: {tag} did not parse as a curve")
                self.assertEqual(len(points), len(row["points"]),
                                 f"{name}.{field} control-point count drifted")
                for transcribed, recorded in zip(points, row["points"]):
                    self.assertAlmostEqual(transcribed[0], recorded[0], places=4,
                                           msg=f"{name}.{field} fraction drifted")
                    self.assertAlmostEqual(transcribed[1], recorded[1], places=4,
                                           msg=f"{name}.{field} value drifted")

    def test_use_splines_transcription_matches_the_inventory(self):
        inventory = load_inventory()
        for name, (profile, definition, _base) in TRANSCRIPTIONS.items():
            body = cpp_helper_body(name)
            fields = inventory_fields(inventory, profile, definition)
            recorded = fields.get("Use_Splines", {}).get("value", "no").strip().lower()
            expected = recorded in ("yes", "true", "1")
            if "constants.use_splines = " not in body:
                continue  # inherited from the base helper, checked there
            transcribed = "constants.use_splines = true" in body
            self.assertEqual(transcribed, expected,
                             f"{name}: Use_Splines transcription drifted from the inventory")


class CameraViewerContract(unittest.TestCase):
    def test_viewer_accepts_the_camera_options(self):
        source = mode_source("viewer_host")
        self.assertIn('argument == "--eawr-camera-mode"', source)
        self.assertIn('argument == "--eawr-camera-zoom"', source)
        self.assertIn("parse_camera_mode(options_->camera_mode)", source)
        self.assertIn("tactical_camera::solve(run->constants, zoom)", source)

    def test_camera_mode_reads_both_xml_sources_through_the_vfs(self):
        source = mode_source("viewer_host")
        self.assertIn('"data/xml/tacticalcameras.xml"', source)
        self.assertIn('"data/xml/gameconstants.xml"', source)
        self.assertIn("mount_content_layers(", source)
        # The shared strict loader now owns these checks for both viewer paths.
        loader = (ROOT / "src/presentation/camera/constants_source.cpp").read_text(encoding="utf-8")
        self.assertIn("is missing required tag", loader)
        self.assertIn("if (required && !found)", loader)
        # DOCTYPE and external entities are refused before a tree is built.
        self.assertIn("node_doctype", loader)
        self.assertIn("tactical_camera::load_constants(", source)

    def test_camera_mode_does_not_disturb_the_frozen_fixed_capture(self):
        source = mode_source("viewer_host")
        physical = (ROOT / "apps/viewer/src/viewer_host_exercises_effects.cpp").read_text(encoding="utf-8")
        start = physical.index("bool ViewerHost::start_tactical_camera()")
        end = physical.index("bool ViewerHost::start_atlas_overlay()")
        body = physical[start:end]
        # The #22 fixed-capture override pins its own camera and must stay the
        # sole owner of the regression framing.
        self.assertNotIn("fixed_capture_snapshot_", body)
        self.assertNotIn("expected_scene_hash", body)
        self.assertIn("capture_camera_.eye = run->eye;", body)
        # The frozen path is still reached only from load_replay/_process.
        self.assertIn("fixed_capture_snapshot_ = std::make_shared", source)

    def test_report_records_mode_zoom_distance_pitch_and_pan_speed(self):
        source = mode_source("viewer_host")
        for token in ('\\"tactical_camera\\"', '\\"mode\\"', '\\"zoom\\"',
                      '\\"distance\\"', '\\"pitch_degrees\\"', '\\"pan_speed\\"',
                      '\\"tactical_camera_verified\\"', '\\"input_mapping\\"',
                      "tactical_camera_exercise_passed"):
            self.assertIn(token, source)

    def test_module_is_engine_independent_and_simulation_free(self):
        header = (ROOT / "include/eawr/presentation/camera/camera.hpp").read_text(
            encoding="utf-8")
        source = (ROOT / "src/presentation/camera/camera.cpp").read_text(encoding="utf-8")
        for text in (header, source):
            self.assertNotIn("godot", text)
            self.assertNotIn("eawr/sim", text)
            self.assertNotIn("eawr/vfs", text)
            self.assertNotIn("fstream", text)
        # Presentation numeric policy: float, stated in the header.
        self.assertIn("docs/fixed-point.md", header)
        self.assertNotIn("Fixed", source)

    def test_fixture_is_wholly_original_and_unlike_the_shipped_constants(self):
        inventory = load_inventory()
        shipped = set()
        for row in inventory["profiles"]:
            for mode in row["modes"]:
                for field in mode["fields"]:
                    if "number" in field:
                        shipped.add((field["tag"], field["number"]))
        # The fixture's distance limits must not coincide with any shipped
        # distance limit, so a fixture number can never be mistaken for
        # original-game evidence.
        for _name, fields in fixture.MODES:
            for tag in ("Distance_Min", "Distance_Max", "Distance_Default"):
                self.assertNotIn((tag, float(fields[tag])), shipped,
                                 f"fixture {tag} coincides with a shipped constant")

    def test_fixture_expectations_are_self_consistent(self):
        # Hand-checked anchors, independent of the implementation.
        land = fixture.expected_state("land", 0.5)
        self.assertAlmostEqual(land["distance"], 300.0, places=4)
        self.assertAlmostEqual(land["pitch_degrees"], 40.0, places=4)
        self.assertAlmostEqual(land["pan_speed"], 1500.0, places=4)
        space = fixture.expected_state("space", 1.0)
        self.assertAlmostEqual(space["distance"], 1200.0, places=4)
        self.assertAlmostEqual(space["pitch_degrees"], 45.0, places=4)
        self.assertAlmostEqual(space["pan_speed"], 2400.0, places=4)
        self.assertAlmostEqual(fixture.default_zoom("land"), 0.5, places=4)


class CameraGraphicalRun(unittest.TestCase):
    def _run(self, workspace, mode, zoom=None, name="camera.json"):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable,
                        "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        mod_root = fixture.write_fixture_root(workspace / "fixture")
        report = workspace / name
        command = [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                   "--eawr-mod-root", str(mod_root),
                   "--eawr-camera-mode", mode,
                   "--eawr-report", str(report)]
        if zoom is not None:
            command += ["--eawr-camera-zoom", str(zoom)]
        completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, check=False)
        self.assertEqual(completed.returncode, 0, completed.stdout)
        return json.loads(report.read_text(encoding="utf-8"))

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_three_zoom_levels_per_mode_reach_the_engine(self):
        with tempfile.TemporaryDirectory(prefix="eawr-camera-") as temporary:
            workspace = pathlib.Path(temporary)
            for mode in ("land", "space"):
                for index, zoom in enumerate((0.0, 0.5, 1.0)):
                    result = self._run(workspace, mode, zoom, f"{mode}-{index}.json")
                    expected = fixture.expected_state(mode, zoom)
                    solved = result["tactical_camera"]["solved"]
                    where = f"{mode} @ zoom {zoom}"
                    self.assertEqual(result["status"], "tactical_camera_exercise_passed", where)
                    self.assertTrue(result["tactical_camera_verified"], where)
                    self.assertAlmostEqual(solved["zoom"], expected["zoom"], places=4, msg=where)
                    self.assertAlmostEqual(solved["distance"], expected["distance"],
                                           places=3, msg=where)
                    self.assertAlmostEqual(solved["pitch_degrees"], expected["pitch_degrees"],
                                           places=3, msg=where)
                    self.assertAlmostEqual(solved["pan_speed"], expected["pan_speed"],
                                           places=2, msg=where)
                    self.assertAlmostEqual(solved["fov_degrees"], expected["fov_degrees"],
                                           places=3, msg=where)
                    self.assertTrue(result["capture_sha256"], where)

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_zoom_limits_and_pan_speed_ordering(self):
        with tempfile.TemporaryDirectory(prefix="eawr-camera-limits-") as temporary:
            workspace = pathlib.Path(temporary)
            near = self._run(workspace, "land", 0.0, "near.json")["tactical_camera"]
            far = self._run(workspace, "land", 1.0, "far.json")["tactical_camera"]
            self.assertAlmostEqual(near["solved"]["distance"],
                                   near["limits"]["distance_min"], places=3)
            self.assertAlmostEqual(far["solved"]["distance"],
                                   far["limits"]["distance_max"], places=3)
            # Pan speed rises with zoom and the camera pulls back and looks down.
            self.assertGreater(far["solved"]["pan_speed"], near["solved"]["pan_speed"])
            self.assertGreater(far["solved"]["pitch_degrees"], near["solved"]["pitch_degrees"])
            self.assertGreater(far["eye"][1], near["eye"][1])
            # A request outside the range clamps instead of failing.
            clamped = self._run(workspace, "land", 5.0, "clamped.json")["tactical_camera"]
            self.assertAlmostEqual(clamped["solved"]["zoom"], 1.0, places=4)

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_input_mapping_and_source_provenance_are_recorded(self):
        with tempfile.TemporaryDirectory(prefix="eawr-camera-input-") as temporary:
            workspace = pathlib.Path(temporary)
            # No --eawr-camera-zoom uses the mode's own Distance_Default.
            result = self._run(workspace, "land", None, "default.json")
            camera = result["tactical_camera"]
            self.assertAlmostEqual(camera["solved"]["zoom"], fixture.default_zoom("land"),
                                   places=4)
            paths = [row["logical_path"] for row in camera["sources"]]
            self.assertEqual(paths, [fixture.CAMERA_LOGICAL_PATH,
                                     fixture.CONSTANTS_LOGICAL_PATH])
            for row in camera["sources"]:
                self.assertEqual(len(row["sha256"]), 64)

            mapping = camera["input_mapping"]
            span = camera["limits"]["distance_max"] - camera["limits"]["distance_min"]
            step = float(fixture.LAND["Distance_Per_Mouse_Unit"]) / span
            self.assertAlmostEqual(mapping["zoom_step"],
                                   min(camera["solved"]["zoom"] + step, 1.0), places=4)
            self.assertAlmostEqual(mapping["pan_x_per_second"],
                                   camera["solved"]["pan_speed"], places=2)
            self.assertAlmostEqual(
                mapping["push_pan_x_per_second"],
                camera["solved"]["pan_speed"] * float(fixture.SCROLL[
                    "Push_Scroll_Speed_Modifier"]), places=1)
            self.assertEqual(mapping["edge_scroll_left"], -1)
            self.assertEqual(mapping["edge_scroll_right"], 1)
            self.assertAlmostEqual(mapping["yaw_after_drag"],
                                   float(fixture.LAND["Yaw_Per_Mouse_Unit"]) * 10.0, places=3)

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_an_unknown_mode_fails_closed(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable)
        with tempfile.TemporaryDirectory(prefix="eawr-camera-bad-") as temporary:
            workspace = pathlib.Path(temporary)
            mod_root = fixture.write_fixture_root(workspace / "fixture")
            report = workspace / "bad.json"
            completed = subprocess.run(
                [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-mod-root", str(mod_root),
                 "--eawr-camera-mode", "orbital",
                 "--eawr-report", str(report)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False)
            self.assertEqual(completed.returncode, 2, completed.stdout)
            result = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "failed")
            self.assertIn("land, space or unlocked", result["failure"])
            self.assertFalse(result["tactical_camera_verified"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
