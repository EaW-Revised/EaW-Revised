"""Opt-in synthetic space fog join (P1-07 #28): production space MapMode with
caller-declared unit admission. No installed assets are required.

The structural checks always run. The graphical checks need
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and Pillow; set
EAWR_SPACE_FOG_OUTPUT to retain reports, grids and PNGs.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

try:
    from PIL import Image, ImageChops
except ImportError:  # graphical runs require the optional decoder
    Image = ImageChops = None

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
import space_fog_fixture as fixture  # noqa: E402
from viewer_mode_sources import mode_source  # noqa: E402

Q24 = 1 << 24
GODOT = os.environ.get("EAWR_GODOT_EXECUTABLE")

# Proposed synthetic attenuation bytes, row-major (x = column, y = row).
TEAM2 = (255, 60, 128, 200, 90, 0)
TEAM7 = (60, 255, 128, 0, 200, 90)

# Front-face samples (source x, y, z) and the cell (column, row) each lies in.
FRONT_Z = -30.0
SAMPLES = {
    "u0_left": ((-70.0, 140.0, FRONT_Z), (0, 0)),
    "u0_right": ((-30.0, 140.0, FRONT_Z), (1, 0)),
    "u1_left": ((30.0, 240.0, FRONT_Z), (1, 1)),
    "u1_right": ((70.0, 240.0, FRONT_Z), (2, 1)),
    "u2": ((200.0, 280.0, FRONT_Z), None),
}


def write_grid(path: Path, team: int, cells, origin=None, revision: int = 1) -> str:
    """Independent canonical fog-stub-v1 encoder; never calls production code."""
    grid = fixture.GRID
    ox, oy = origin or grid["origin"]
    payload = struct.pack("<8sIIIIqqqqIQQ", b"EAWRFOG\0", 1, team, grid["width"], grid["height"],
                          ox * Q24, oy * Q24, grid["cell"][0] * Q24, grid["cell"][1] * Q24,
                          1, revision, len(cells)) + bytes(cells)
    path.write_bytes(payload)
    return hashlib.sha256(payload).hexdigest()


def fog_arguments(grids, team: int, revision: int = 1, admit=("SpaceUnit",)):
    arguments = []
    for path, digest in grids:
        arguments += ["--eawr-fog-grid", str(path), "--eawr-fog-sha256", digest]
    arguments += ["--eawr-fog-team", str(team), "--eawr-fog-revision", str(revision), "--eawr-fog-tick", "47"]
    for name in admit:
        arguments += ["--eawr-space-fog-admit", name]
    return arguments


def run(root: Path, report: Path, capture: Path | None, extra=()):
    command = [GODOT, "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-map", fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
               "--eawr-report", str(report), "--eawr-space-camera", fixture.CAMERA]
    if capture:
        command += ["--eawr-capture", str(capture)]
    command += list(extra)
    completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False, timeout=240)
    report.with_suffix(".log").write_text(completed.stdout, encoding="utf-8")
    return completed, json.loads(report.read_text(encoding="utf-8"))


def retain(path: Path, label: str) -> None:
    configured = os.environ.get("EAWR_SPACE_FOG_OUTPUT")
    if not configured:
        return
    destination = Path(configured) / label
    destination.mkdir(parents=True, exist_ok=True)
    for file in path.rglob("*"):
        if file.is_file() and file.suffix in {".json", ".log", ".png", ".pgm", ".eawr-fog"}:
            shutil.copy2(file, destination / file.name)


def project(report: dict, point) -> tuple[int, int]:
    """Pixel of a source point under the fixture camera (origin, looking +Y)."""
    x, y, z = point
    viewport = report["capture_identity"]["viewport"]
    tangent = math.tan(math.radians(report["capture_identity"]["camera"]["fov_degrees"] / 2))
    aspect = viewport["width"] / viewport["height"]
    px = (1 + (x / y) / (tangent * aspect)) * viewport["width"] / 2
    py = (1 - (z / y) / tangent) * viewport["height"] / 2
    return round(px), round(py)


class SpaceFogStructure(unittest.TestCase):
    def test_fixture_places_units_across_cell_edges(self):
        grid = fixture.GRID
        edges_x = [grid["origin"][0] + grid["cell"][0] * i for i in range(grid["width"] + 1)]
        half = fixture.UNIT_HALF[0]
        for index in (0, 1):
            x = fixture.PLACEMENTS[index][1][0]
            self.assertTrue(any(x - half < edge < x + half for edge in edges_x), index)
        x = fixture.PLACEMENTS[2][1][0]
        self.assertGreaterEqual(x - half, edges_x[-1], "unit 2 must lie wholly outside the grid")
        self.assertEqual(len(TEAM2), grid["width"] * grid["height"])
        self.assertIn('<SpaceUnit Name="EAWR_SPACE_FOG_UNIT">', fixture.objects_xml())
        self.assertIn('<SpaceProp Name="EAWR_SPACE_FOG_PROP">', fixture.objects_xml())

    def test_space_composition_keeps_the_legacy_route_out_of_the_sky_module(self):
        environment = mode_source("space_environment")
        self.assertNotIn("MaterialRoute::legacy_effect", environment)
        units = (ROOT / "apps/viewer/src/space_fog_units.cpp").read_text(encoding="utf-8")
        self.assertIn("MaterialRoute::legacy_effect", units)
        self.assertIn("declare_fog_consumer(renderer_asset)", units)
        self.assertNotIn("placeholder", units.lower())
        pure = (ROOT / "apps/viewer/src/space_fog.cpp").read_text(encoding="utf-8")
        for token in ("godot_cpp", "#include <godot", "RenderingServer", "GodotRenderer"):
            self.assertNotIn(token, pure)
        mode = mode_source("map_mode")
        self.assertNotIn("fog map mode currently supports land maps only", mode)
        self.assertIn("--eawr-space-fog-admit applies only to a kind-2 (space) map", mode)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and GODOT and Image,
                     "set the graphical Godot variables and install Pillow")
class SpaceFogGraphical(unittest.TestCase):
    maxDiff = None

    def _team_run(self, path: Path, root: Path, team: int):
        grids = [(path / "team2.eawr-fog", write_grid(path / "team2.eawr-fog", 2, TEAM2)),
                 (path / "team7.eawr-fog", write_grid(path / "team7.eawr-fog", 7, TEAM7))]
        capture = path / f"team{team}.png"
        done, report = run(root, path / f"team{team}.json", capture, fog_arguments(grids, team))
        self.assertEqual(done.returncode, 0, report.get("failure") or done.stdout)
        units = Image.open(capture.with_name(f"team{team}.fog_units.png")).convert("RGB")
        sky = Image.open(capture).convert("RGB")
        return report, units, sky

    def _check_report(self, report: dict, team: int, cells):
        self.assertEqual(report["status"], "space_fog_passed")
        fog = report["fog"]
        self.assertEqual(fog["team"], team)
        self.assertEqual((fog["mapping"], fog["filter"], fog["outside_grid"]),
                         ("source_xy_from_world_x_negative_z", "nearest", "dark"))
        self.assertTrue(fog["fixed_capture"])
        self.assertFalse(fog["override"])
        self.assertEqual((fog["sky"]["fog"], fog["sky"]["consumer"]), ("excluded", False))
        self.assertEqual(fog["evidence"]["status"], "verified")
        self.assertEqual(fog["evidence"]["changed_outside"], 0)
        self.assertGreater(fog["evidence"]["outside_pixels"], 500000)
        source = fog["evidence"]["phases"]["source"]
        self.assertEqual((source["uploads"], source["updates"], source["bound_revision"]), (1, 0, 1))
        self.assertEqual(fog["renderer"]["readiness"], "ready")
        self.assertEqual(fog["unsupported"], [])
        decisions = {(item["object_id"], item["decision"], item["type"]) for item in fog["admission"]["placements"]}
        self.assertEqual(decisions, {
            ("EAWR_SPACE_FOG_UNIT", "admitted", "SpaceUnit"),
            ("EAWR_SPACE_FOG_PROP", "not_admitted", "SpaceProp"),
            ("", "uncatalogued", ""),  # the marker CRC matches no catalog winner
        })
        self.assertEqual(len(fog["admission"]["placements"]), 5)
        self.assertEqual(report["populate"]["composed"], 3)
        units = fog["units"]
        self.assertEqual([unit["scene_ordinal"] for unit in units], [0, 1, 2])
        for unit in units:
            self.assertTrue(unit["submitted"] and unit["fog_material_attached"], unit)
            self.assertGreater(unit["interior_pixels"], 50, unit)
        visible = [any(cells[c] for c in cover) for cover in ((0, 1), (4, 5), ())]
        self.assertEqual([unit["potentially_visible"] for unit in units], visible)
        self.assertEqual(units[2]["lit_interior"], 0, "outside-grid unit must sample dark")
        # The renderer's fog consumers are exactly the unit surfaces, so no sky
        # asset is one (the run also checks the consumer asset ids in-engine).
        renderer = fog["renderer"]
        surfaces = sum(len(unit["assets"]) for unit in units)
        self.assertEqual((renderer["declared_consumers"], renderer["attached_consumers"]), (surfaces, surfaces))
        self.assertTrue(all(asset >= 2001 for unit in units for asset in unit["assets"]), units)
        self.assertEqual(report["space"]["pixel_evidence_status"], "verified")

    def _samples(self, report: dict, image) -> dict:
        return {name: image.getpixel(project(report, point)) for name, (point, _) in SAMPLES.items()}

    def _check_regions(self, samples: dict, cells) -> None:
        width = fixture.GRID["width"]
        level = {name: sum(rgb) for name, rgb in samples.items()}
        attenuation = {name: (cells[cell[1] * width + cell[0]] if cell else 0)
                       for name, (_, cell) in SAMPLES.items()}
        for name, value in attenuation.items():
            if value == 0:
                self.assertLessEqual(level[name], 10, (name, samples[name]))
            else:
                self.assertGreater(level[name], 10, (name, samples[name]))
                red, green, blue = samples[name]
                self.assertGreater(blue, red, (name, samples[name]))
        names = sorted(attenuation)
        for a in names:
            for b in names:
                if attenuation[a] >= attenuation[b] + 30:
                    self.assertGreater(level[a], level[b], (a, b, samples[a], samples[b]))

    def test_team_grids_darken_expected_unit_regions_and_leave_the_sky(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-fog-") as temporary:
            path = Path(temporary)
            root = fixture.write_fixture_root(path / "root")
            try:
                report2, units2, sky2 = self._team_run(path, root, 2)
                report7, units7, sky7 = self._team_run(path, root, 7)
                nofog, nofog_report = run(root, path / "nofog.json", path / "nofog.png",
                                          ["--eawr-space-control", "none"])
            finally:
                retain(path, "teams")
            self.assertEqual(nofog.returncode, 0, nofog_report.get("failure") or nofog.stdout)
            self.assertNotIn("fog", nofog_report)
            # The sky is unfogged: with the grid bound, each team's sky-only
            # frame is byte-identical to the no-fog run's frame. (The fog phase's
            # outside-mask check compares two frames that both carry the grid,
            # so on its own it could not see a fogged sky.)
            baseline = hashlib.sha256((path / "nofog.png").read_bytes()).hexdigest()
            for team in (2, 7):
                self.assertEqual(hashlib.sha256((path / f"team{team}.png").read_bytes()).hexdigest(), baseline, team)
            self._check_report(report2, 2, TEAM2)
            self._check_report(report7, 7, TEAM7)
            samples2 = self._samples(report2, units2)
            samples7 = self._samples(report7, units7)
            print("team2", samples2, "\nteam7", samples7)
            self._check_regions(samples2, TEAM2)
            self._check_regions(samples7, TEAM7)
            # Unit 0's halves swap brightness between the teams at the x = -50 edge.
            self.assertGreater(sum(samples2["u0_left"]), sum(samples2["u0_right"]))
            self.assertLess(sum(samples7["u0_left"]), sum(samples7["u0_right"]))
            # Nearest filtering: 3 source units either side of the edge match the
            # interior of their own cell, not a blend.
            for report, image, samples in ((report2, units2, samples2), (report7, units7, samples7)):
                for offset, name in ((-3.0, "u0_left"), (3.0, "u0_right")):
                    edge = image.getpixel(project(report, (-50.0 + offset, 140.0, FRONT_Z)))
                    self.assertLessEqual(sum(abs(a - b) for a, b in zip(edge, samples[name])), 12, (edge, name))
            # The prop (not admitted) is never drawn.
            self.assertEqual(sky2.tobytes(), sky7.tobytes())
            self.assertEqual(report2["fog"]["evidence"]["phases"]["source"]["capture_sha256"] !=
                             report7["fog"]["evidence"]["phases"]["source"]["capture_sha256"], True)
            prop = project(report2, (-20.0, 170.0, 30.0))
            self.assertEqual(units2.getpixel(prop), sky2.getpixel(prop))
            self.assertEqual(units7.getpixel(prop), sky7.getpixel(prop))
            # Two fresh fixed captures of the same inputs are byte identical.
            again = path / "again"
            again.mkdir()
            report_again, units_again, _ = self._team_run(again, root, 2)
            self.assertEqual(units_again.tobytes(), units2.tobytes())
            self.assertEqual(report_again["fog"]["evidence"]["phases"], report2["fog"]["evidence"]["phases"])

    def test_zero_grid_hides_every_unit_with_presence_witnesses(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-fog-zero-") as temporary:
            path = Path(temporary)
            root = fixture.write_fixture_root(path / "root")
            digest = write_grid(path / "zero.eawr-fog", 2, bytes(6))
            done, report = run(root, path / "zero.json", path / "zero.png",
                               fog_arguments([(path / "zero.eawr-fog", digest)], 2))
            retain(path, "zero")
            self.assertEqual(done.returncode, 0, report.get("failure") or done.stdout)
            units = report["fog"]["units"]
            self.assertEqual([unit["potentially_visible"] for unit in units], [False] * 3)
            for unit in units:
                self.assertEqual(unit["lit_interior"], 0, unit)
                self.assertTrue(unit["submitted"] and unit["fog_material_attached"], unit)
            # Drawn dark over the sky rather than skipped: some hidden unit pixels differ from the sky.
            self.assertGreater(sum(unit["changed_from_sky"] for unit in units), 0)

    def test_footprint_ending_on_a_row_edge_does_not_reveal_the_next_row(self):
        # Rows [60, 160) and [160, 260): unit 0 (y 140..160) ends exactly on the
        # row edge, so only its all-zero row 0 can reveal it; unit 1 lies in the
        # nonzero row 1. A closed max edge would wrongly expect unit 0 lit.
        with tempfile.TemporaryDirectory(prefix="eawr-space-fog-edge-") as temporary:
            path = Path(temporary)
            root = fixture.write_fixture_root(path / "root")
            digest = write_grid(path / "edge.eawr-fog", 2, (0, 0, 0, 200, 200, 200), origin=(-150, 60))
            done, report = run(root, path / "edge.json", path / "edge.png",
                               fog_arguments([(path / "edge.eawr-fog", digest)], 2))
            retain(path, "edge")
            self.assertEqual(done.returncode, 0, report.get("failure") or done.stdout)
            units = report["fog"]["units"]
            self.assertEqual(units[0]["source_bounds"]["y"][1], 160)
            self.assertEqual([unit["potentially_visible"] for unit in units], [False, True, False])
            self.assertEqual(units[0]["lit_interior"], 0, units[0])
            self.assertGreater(units[1]["lit_interior"], 0, units[1])

    def test_paint_override_uploads_on_revision_change_and_restores_source(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-fog-paint-") as temporary:
            path = Path(temporary)
            root = fixture.write_fixture_root(path / "root")
            digest = write_grid(path / "team2.eawr-fog", 2, TEAM2)
            done, report = run(root, path / "paint.json", None,
                               fog_arguments([(path / "team2.eawr-fog", digest)], 2)
                               + ["--eawr-fog-paint-evidence", str(path / "paint.png")])
            retain(path, "paint")
            self.assertEqual(done.returncode, 0, report.get("failure") or done.stdout)
            phases = report["fog"]["evidence"]["phases"]
            self.assertEqual({label: phases[label]["uploads"] for label in phases},
                             {"source": 1, "painted": 2, "restored": 3})
            self.assertEqual({label: phases[label]["updates"] for label in phases},
                             {"source": 0, "painted": 1, "restored": 1})
            self.assertEqual(phases["painted"]["bound_revision"], 2)
            self.assertEqual(phases["restored"]["bound_revision"], 1)
            self.assertNotEqual(phases["source"]["capture_sha256"], phases["painted"]["capture_sha256"])
            self.assertEqual(phases["source"]["capture_sha256"], phases["restored"]["capture_sha256"])
            self.assertFalse(report["fog"]["fixed_capture"])
            self.assertFalse(report["fog"]["override"])
            self.assertEqual(report["fog"]["source"][0]["sha256"], digest)
            source = Image.open(path / "paint.source.png").convert("RGB")
            painted = Image.open(path / "paint.painted.png").convert("RGB")
            # The painted centre cell (1, 1), 90 in the source, is painted 255.
            # Of the units only unit 1's left half (x 10..50) lies in it, so
            # every changed pixel of the whole frame must fall inside that
            # sub-box's projected rectangle; the sky cannot change.
            left = project(report, SAMPLES["u1_left"][0])
            right = project(report, SAMPLES["u1_right"][0])
            other = project(report, SAMPLES["u0_left"][0])
            self.assertGreater(sum(painted.getpixel(left)), sum(source.getpixel(left)))
            self.assertEqual(painted.getpixel(right), source.getpixel(right))
            self.assertEqual(painted.getpixel(other), source.getpixel(other))
            changed = ImageChops.difference(source, painted).getbbox()
            self.assertIsNotNone(changed)
            corners = [project(report, (x, y, z)) for x in (10.0, 50.0) for y in (240.0, 260.0) for z in (-40.0, -20.0)]
            margin = 2
            region = (min(c[0] for c in corners) - margin, min(c[1] for c in corners) - margin,
                      max(c[0] for c in corners) + margin + 1, max(c[1] for c in corners) + margin + 1)
            self.assertTrue(region[0] <= changed[0] and region[1] <= changed[1]
                            and changed[2] <= region[2] and changed[3] <= region[3], (changed, region))

    def test_admission_is_refused_on_land_maps(self):
        import scene_fixture
        with tempfile.TemporaryDirectory(prefix="eawr-space-fog-land-") as temporary:
            path = Path(temporary)
            land = scene_fixture.write_fixture_root(path / "land")
            digest = write_grid(path / "team2.eawr-fog", 2, TEAM2)
            report = path / "land.json"
            completed = subprocess.run(
                [GODOT, "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-map", scene_fixture.MAP_LOGICAL_PATH,
                 "--eawr-game-root", str(land), "--eawr-report", str(report), "--eawr-capture", str(path / "land.png"),
                 *fog_arguments([(path / "team2.eawr-fog", digest)], 2)],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=240)
            result = json.loads(report.read_text(encoding="utf-8"))
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("--eawr-space-fog-admit applies only to a kind-2 (space) map", result["failure"])

    def test_invalid_inputs_fail_closed(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-fog-neg-") as temporary:
            path = Path(temporary)
            root = fixture.write_fixture_root(path / "root")
            blocked = fixture.write_fixture_root(path / "blocked", "unsupported")
            unfogged = fixture.write_fixture_root(path / "unfogged", "no_fog_material")
            partial = fixture.write_fixture_root(path / "partial", "prop_no_fog_material")
            digest = write_grid(path / "team2.eawr-fog", 2, TEAM2)
            grids = [(path / "team2.eawr-fog", digest)]
            cases = {
                "no_admission": (root, fog_arguments(grids, 2, admit=()), "at least one --eawr-space-fog-admit"),
                "bad_admission": (root, fog_arguments(grids, 2, admit=("Space-Unit",)), "XML element name"),
                "duplicate_admission": (root, fog_arguments(grids, 2, admit=("SpaceUnit", "SpaceUnit")),
                                        "declared twice"),
                "admits_nothing": (root, fog_arguments(grids, 2, admit=("Squadron",)), "admitted no drawable"),
                "missing_team": (root, fog_arguments(grids, 9), "selected fog team has no grid"),
                "hash_mismatch": (root, fog_arguments([(grids[0][0], "0" * 64)], 2), "SHA-256 mismatch"),
                "revision_mismatch": (root, fog_arguments(grids, 2, revision=2), "revision does not match"),
                "admission_without_fog": (root, ["--eawr-space-fog-admit", "SpaceUnit"], "requires the fog grid"),
                "paint_on_space": (root, fog_arguments(grids, 2) + ["--eawr-fog-paint"], "not wired for space maps"),
                "unsupported_surface": (blocked, fog_arguments(grids, 2), "cannot compose"),
                "no_fog_material": (unfogged, fog_arguments(grids, 2), "not a fog consumer"),
                # Units 0..2 compose (assets 2001..2003) before the prop fails.
                "partial_composition": (partial, fog_arguments(grids, 2, admit=("SpaceUnit", "SpaceProp")),
                                        "not a fog consumer"),
            }
            try:
                for name, (game_root, extra, needle) in cases.items():
                    with self.subTest(name):
                        # Interactive painting is refused before any capture question arises.
                        capture = None if name == "paint_on_space" else path / f"{name}.png"
                        done, report = run(game_root, path / f"{name}.json", capture, extra)
                        self.assertNotEqual(done.returncode, 0, name)
                        self.assertEqual(report["status"], "failed", name)
                        self.assertIn(needle, report["failure"], name)
                        if name == "unsupported_surface":
                            fog = report["fog"]
                            self.assertTrue(any("MeshShadowVolume.fx" in item for item in fog["unsupported"]))
                            self.assertEqual({item["decision"] for item in fog["admission"]["placements"]
                                              if item["type"] == "SpaceUnit"}, {"admitted_blocked"})
                            self.assertEqual(fog["units"], [])
                            self.assertEqual(report["space"]["lifecycle"]["resources_after_teardown"], 0)
                        if name == "no_fog_material":
                            fog = report["fog"]
                            self.assertTrue(any("EAWR-FOG-0006" in item and "MeshGloss.fx" in item
                                                for item in fog["unsupported"]), fog["unsupported"])
                            self.assertEqual(report["space"]["lifecycle"]["resources_after_teardown"], 0)
                        if name == "partial_composition":
                            fog = report["fog"]
                            self.assertEqual(len(fog["unsupported"]), 1, fog["unsupported"])
                            self.assertIn("EAWR_SPACE_FOG_PROP (SpaceProp)", fog["unsupported"][0])
                            self.assertIn("asset 2004", fog["unsupported"][0])
                            self.assertIn("EAWR-FOG-0006", fog["unsupported"][0])
                            self.assertEqual(report["space"]["lifecycle"]["resources_after_teardown"], 0)
                        if name in {"unsupported_surface", "no_fog_material", "partial_composition"}:
                            # A failed composition reports no unit as composed.
                            self.assertEqual(report["populate"]["composed"], 0, name)
                            self.assertEqual(report["fog"]["units"], [], name)
            finally:
                retain(path, "negative")


if __name__ == "__main__":
    unittest.main()
