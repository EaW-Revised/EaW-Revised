"""Synthetic production land MapMode fog exercise; no installed assets required."""

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
    from PIL import Image
except ImportError:  # graphical runs require the optional decoder
    Image = None

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
import scene_fixture  # noqa: E402

Q24 = 1 << 24
GODOT = os.environ.get("EAWR_GODOT_EXECUTABLE")


def write_grid(path: Path, team: int, cells: bytes,
               origin: tuple[int, int] = (400, 70),
               cell: tuple[int, int] = (80, 100)) -> str:
    # Independent canonical encoder: deliberately no production parser call.
    payload = struct.pack("<8sIIIIqqqqIQQ", b"EAWRFOG\0", 1, team, 3, 2,
                          origin[0] * Q24, origin[1] * Q24, cell[0] * Q24, cell[1] * Q24,
                          1, 1, len(cells)) + cells
    path.write_bytes(payload)
    return hashlib.sha256(payload).hexdigest()


def run(root: Path, report: Path, capture: Path | None, extra=()):
    command = [GODOT, "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-map", scene_fixture.MAP_LOGICAL_PATH,
               "--eawr-game-root", str(root), "--eawr-report", str(report)]
    if capture:
        command.extend(("--eawr-capture", str(capture)))
    command.extend(extra)
    completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False, timeout=180)
    report.with_suffix(".log").write_text(completed.stdout, encoding="utf-8")
    return completed, json.loads(report.read_text(encoding="utf-8"))


def retain(path: Path, label: str) -> None:
    configured = os.environ.get("EAWR_MAP_FOG_OUTPUT")
    if not configured:
        return
    destination = Path(configured) / label
    destination.mkdir(parents=True, exist_ok=True)
    for file in path.iterdir():
        if file.is_file():
            shutil.copy2(file, destination / file.name)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and GODOT and Image,
                     "set the graphical Godot variables and install Pillow")
class MapFogGraphical(unittest.TestCase):
    @staticmethod
    def pixel(image: Image.Image, report: dict, x: float, y: float,
              height: float = 0) -> tuple[int, int, int]:
        camera = report["capture_identity"]["camera"]
        viewport = report["capture_identity"]["viewport"]
        eye = camera["position"]
        depth = eye[1] - height
        half_h = depth * math.tan(math.radians(camera["fov_degrees"] / 2))
        aspect = viewport["width"] / viewport["height"]
        px = round((1 + (x - eye[0]) / (half_h * aspect)) * viewport["width"] / 2)
        py = round((1 + (-y - eye[2]) / half_h) * viewport["height"] / 2)
        return image.getpixel((px, py))

    def test_paint_changes_pixels_then_restores_source(self):
        with tempfile.TemporaryDirectory(prefix="eawr-land-fog-paint-") as temporary:
            path = Path(temporary)
            old = scene_fixture.PLACEMENTS
            try:
                scene_fixture.PLACEMENTS = [old[2]]
                root = scene_fixture.write_fixture_root(path)
            finally:
                scene_fixture.PLACEMENTS = old
            grid = path / "team2.eawr-fog"
            source_hash = write_grid(grid, 2, bytes((40, 220, 70, 180, 80, 255)))
            done, report = run(root, path / "paint.json", None,
                               ("--eawr-populate", "--eawr-fog-grid", str(grid),
                                "--eawr-fog-sha256", source_hash,
                                "--eawr-fog-team", "2", "--eawr-fog-revision", "1",
                                "--eawr-fog-tick", "47", "--eawr-fog-paint-evidence",
                                str(path / "paint.png")))
            self.assertEqual(done.returncode, 0, report.get("failure") or done.stdout)
            fog = report["fog"]
            self.assertEqual(fog["source"][0]["sha256"], source_hash)
            self.assertFalse(fog["override"])
            self.assertEqual(fog["scene_sha256"], report["populate"]["scene_sha256"])
            self.assertEqual(fog["paint_uploads"],
                             {"source": 1, "painted": 2, "restored": 3})
            self.assertEqual(fog["paint_updates"],
                             {"source": 0, "painted": 1, "restored": 1})
            source = Image.open(path / "paint.source.png").convert("RGB")
            painted = Image.open(path / "paint.painted.png").convert("RGB")
            restored = Image.open(path / "paint.restored.png").convert("RGB")
            self.assertNotEqual(source.tobytes(), painted.tobytes())
            self.assertEqual(source.tobytes(), restored.tobytes())
            self.assertEqual(fog["paint_evidence"]["source"],
                             fog["paint_evidence"]["restored"])
            retain(path, "paint")

    def test_hidden_and_outside_grid_units(self):
        with tempfile.TemporaryDirectory(prefix="eawr-land-fog-hidden-") as temporary:
            path = Path(temporary)
            old = scene_fixture.PLACEMENTS
            try:
                scene_fixture.PLACEMENTS = [old[2]]
                root = scene_fixture.write_fixture_root(path / "supported")
            finally:
                scene_fixture.PLACEMENTS = old
            for label, origin in (("zero", (400, 70)), ("outside", (700, 700))):
                grid = path / f"{label}.eawr-fog"
                digest = write_grid(grid, 2, bytes(6) if label == "zero" else bytes([255] * 6), origin)
                capture = path / f"{label}.png"
                done, report = run(root, path / f"{label}.json", capture,
                                   ("--eawr-populate", "--eawr-fog-grid", str(grid),
                                    "--eawr-fog-sha256", digest, "--eawr-fog-team", "2",
                                    "--eawr-fog-revision", "1", "--eawr-fog-tick", "47"))
                retain(path, "hidden")
                self.assertEqual(done.returncode, 0, report.get("failure") or done.stdout)
                self.assertTrue(report["populate"]["evidence"]["verified"])
                self.assertEqual(report["populate"]["evidence"]["units_expected_hidden"], 1)
                self.assertTrue(report["populate"]["evidence"]["unit_submissions_and_fog_materials_verified"])
                self.assertEqual(report["populate"]["evidence"]["changed_pixels_inside_unit_bounds"], 0)
                self.assertEqual(report["evidence"]["capture_sha256"],
                                 report["populate"]["evidence"]["terrain_only_capture_sha256"])
                self.assertEqual(report["fog"]["source"][0]["sha256"], digest)
                self.assertEqual(report["fog"]["source_revision"], 1)
                self.assertEqual(report["fog"]["bound_revision"], 1)
                self.assertFalse(report["fog"]["override"])
                self.assertTrue(report["fog"]["fixed_capture"])
                image = Image.open(capture).convert("RGB")
                self.assertLessEqual(max(self.pixel(image, report, 480, 120, 20)), 2)
            retain(path, "hidden")

    def test_hidden_missing_and_visible_without_coverage_fail(self):
        with tempfile.TemporaryDirectory(prefix="eawr-land-fog-unit-negative-") as temporary:
            path = Path(temporary)
            hidden_grid = path / "hidden.eawr-fog"
            visible_grid = path / "visible.eawr-fog"
            hidden_hash = write_grid(hidden_grid, 2, bytes(6))
            visible_hash = write_grid(visible_grid, 2, bytes([255] * 6))

            def arguments(grid: Path, digest: str) -> tuple[str, ...]:
                return ("--eawr-populate", "--eawr-fog-grid", str(grid),
                        "--eawr-fog-sha256", digest, "--eawr-fog-team", "2",
                        "--eawr-fog-revision", "1", "--eawr-fog-tick", "47")
            old_placements = scene_fixture.PLACEMENTS
            old_model = scene_fixture.MODELS["eawr_scene_depot.alo"]
            try:
                scene_fixture.PLACEMENTS = []
                missing = scene_fixture.write_fixture_root(path / "missing")
                scene_fixture.PLACEMENTS = [old_placements[2]]
                scene_fixture.MODELS["eawr_scene_depot.alo"] = [
                    ("BatchMeshGloss.fx", "eawr_scene_blue.tga", 0.0, 0.0, 0.0)]
                degenerate = scene_fixture.write_fixture_root(path / "degenerate")
            finally:
                scene_fixture.PLACEMENTS = old_placements
                scene_fixture.MODELS["eawr_scene_depot.alo"] = old_model
            done, report = run(missing, path / "missing.json", path / "missing.png",
                               arguments(hidden_grid, hidden_hash))
            self.assertNotEqual(done.returncode, 0)
            self.assertFalse(report["populate"]["evidence"]["verified"])
            self.assertFalse(report["populate"]["evidence"]["unit_submissions_and_fog_materials_verified"])
            self.assertIn("no drawn placement projects", report["failure"])
            done, report = run(degenerate, path / "degenerate.json", path / "degenerate.png",
                               arguments(visible_grid, visible_hash))
            self.assertNotEqual(done.returncode, 0)
            self.assertFalse(report["populate"]["evidence"]["verified"])
            self.assertIn("unit instances did not add coverage", report["failure"])
            retain(path, "unit-negative")

    def test_two_teams_fixed_capture_and_unsupported_unit(self):
        with tempfile.TemporaryDirectory(prefix="eawr-land-fog-") as temporary:
            path = Path(temporary)
            old = scene_fixture.PLACEMENTS
            old_textures = scene_fixture.TEXTURES
            try:
                # One translated, yaw-rotated BatchMeshGloss unit straddles the
                # x=480 boundary; the authored fixture generates its ALO/TED/XML.
                scene_fixture.PLACEMENTS = [old[2]]
                # The texel alpha is the DX8 pass's gloss mask (#200): zero
                # keeps the placeholder sun's highlight off the depot top, so
                # the blue witness below measures fog, not specular.
                scene_fixture.TEXTURES = {**old_textures, "eawr_scene_blue.dds": (30, 60, 230, 0)}
                root = scene_fixture.write_fixture_root(path / "supported")
            finally:
                scene_fixture.PLACEMENTS = old
                scene_fixture.TEXTURES = old_textures
            grid2 = path / "team2.eawr-fog"
            grid7 = path / "team7.eawr-fog"
            hash2 = write_grid(grid2, 2, bytes((40, 220, 70, 180, 80, 255)))
            hash7 = write_grid(grid7, 7, bytes((220, 40, 180, 70, 255, 80)))
            common = ("--eawr-populate", "--eawr-fog-grid", str(grid2),
                      "--eawr-fog-sha256", hash2, "--eawr-fog-grid", str(grid7),
                      "--eawr-fog-sha256", hash7, "--eawr-fog-revision", "1",
                      "--eawr-fog-tick", "47")
            captures = []
            reports = []
            for index, team in enumerate((2, 2, 7)):
                capture = path / f"capture-{index}.png"
                done, report = run(root, path / f"report-{index}.json", capture,
                                   common + ("--eawr-fog-team", str(team))
                                   + (("--eawr-fog-inject-input",) if index == 1 else ()))
                self.assertEqual(done.returncode, 0, report.get("failure") or done.stdout)
                self.assertEqual(report["status"], "map_render_passed")
                self.assertEqual(report["fog"]["source"][0]["sha256"], hash2)
                self.assertEqual(report["fog"]["source"][1]["sha256"], hash7)
                self.assertEqual(report["fog"]["snapshot_tick"], 47)
                self.assertEqual(report["fog"]["grid"], {
                    "width": 3, "height": 2,
                    "origin_raw": [400 * Q24, 70 * Q24],
                    "cell_raw": [80 * Q24, 100 * Q24],
                })
                self.assertEqual(report["fog"]["renderer"]["readiness"], "ready")
                self.assertEqual(report["fog"]["renderer"]["uploads"], 1)
                self.assertFalse(report["fog"]["override"])
                self.assertTrue(report["fog"]["fixed_capture"])
                captures.append(Image.open(capture).convert("RGB"))
                reports.append(report)
            self.assertEqual(captures[0].tobytes(), captures[1].tobytes())
            self.assertEqual(reports[1]["fog"]["ignored_input_events"], 3)
            self.assertEqual(reports[0]["evidence"]["capture_sha256"],
                             reports[1]["evidence"]["capture_sha256"])
            self.assertNotEqual(captures[0].tobytes(), captures[2].tobytes())
            self.assertEqual(reports[0]["populate"]["scene_sha256"],
                             reports[2]["populate"]["scene_sha256"])
            self.assertEqual(reports[0]["map"]["sha256"], reports[2]["map"]["sha256"])
            # Same authored terrain texel under the two selected teams: six
            # independently specified cells have reversed attenuation order.
            a = captures[0]
            b = captures[2]
            for row, y in enumerate((120, 220)):
                for col, x in enumerate((440, 520, 600)):
                    first = sum(self.pixel(a, reports[0], x, y))
                    second = sum(self.pixel(b, reports[2], x, y))
                    expected = ((40, 220, 70), (180, 80, 255))[row][col]
                    opposite = ((220, 40, 180), (70, 255, 80))[row][col]
                    self.assertEqual(first > second, expected > opposite,
                                     (row, col, first, second))
            # The translated, yaw-rotated depot top crosses x=480, so its
            # blue pixels must reverse their brightness ordering either side.
            left_a = self.pixel(a, reports[0], 472, 120, 20)
            left_b = self.pixel(b, reports[2], 472, 120, 20)
            right_a = self.pixel(a, reports[0], 488, 120, 20)
            right_b = self.pixel(b, reports[2], 488, 120, 20)
            for pixel in (left_a, left_b, right_a, right_b):
                self.assertGreater(pixel[2], pixel[0], pixel)
            self.assertLess(sum(left_a), sum(left_b))
            self.assertGreater(sum(right_a), sum(right_b))

            done, lit = run(root, path / "lit.json", path / "lit.png",
                            common + ("--eawr-fog-team", "2", "--eawr-lighting", "sh"))
            self.assertEqual(done.returncode, 0, lit.get("failure") or done.stdout)
            self.assertEqual(lit["fog"]["renderer"]["readiness"], "ready")
            self.assertEqual(lit["fog"]["renderer"]["uploads"], 1)

            # Land fog derives a stage for every accepted legacy family (#28):
            # the MeshGloss tower and MeshAlpha beacon are fogged, and the
            # undrawn shadow-volume surface is listed rather than failing.
            derived = scene_fixture.write_fixture_root(path / "derived")
            done, report = run(derived, path / "derived.json", path / "derived.png",
                               common + ("--eawr-fog-team", "2"))
            self.assertEqual(done.returncode, 0, report.get("failure") or done.stdout)
            self.assertEqual(report["fog"]["unsupported"], [])
            self.assertTrue(any("MeshShadowVolume.fx" in item and "not drawn" in item
                                for item in report["fog"]["uncomposed_placements"]))
            retain(path, "fixed")

    def test_default_report_and_paint_capture_conflict(self):
        with tempfile.TemporaryDirectory(prefix="eawr-land-fog-control-") as temporary:
            path = Path(temporary)
            root = scene_fixture.write_fixture_root(path)
            grid = path / "team2.eawr-fog"
            digest = write_grid(grid, 2, bytes((40, 220, 70, 180, 80, 255)))
            done, report = run(root, path / "default.json", None)
            self.assertEqual(done.returncode, 0, report.get("failure") or done.stdout)
            self.assertNotIn("fog", report)
            done, report = run(root, path / "conflict.json", path / "conflict.png",
                               ("--eawr-fog-grid", str(grid), "--eawr-fog-sha256", digest,
                                "--eawr-fog-team", "2", "--eawr-fog-revision", "1",
                                "--eawr-fog-tick", "47", "--eawr-fog-paint"))
            self.assertNotEqual(done.returncode, 0)
            self.assertIn("contradicts fixed", report["failure"])
            done, report = run(root, path / "hash-mismatch.json", path / "hash-mismatch.png",
                               ("--eawr-fog-grid", str(grid), "--eawr-fog-sha256", "0" * 64,
                                "--eawr-fog-team", "2", "--eawr-fog-revision", "1",
                                "--eawr-fog-tick", "47"))
            self.assertNotEqual(done.returncode, 0)
            self.assertIn("SHA-256 mismatch", report["failure"])
            done, report = run(root, path / "missing-team.json", path / "missing-team.png",
                               ("--eawr-fog-grid", str(grid), "--eawr-fog-sha256", digest,
                                "--eawr-fog-team", "7", "--eawr-fog-revision", "1",
                                "--eawr-fog-tick", "47"))
            self.assertNotEqual(done.returncode, 0)
            self.assertIn("no grid", report["failure"])
            retain(path, "negative")


if __name__ == "__main__":
    unittest.main()
