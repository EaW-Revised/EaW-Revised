"""BatchMeshAlpha fog selector and synthetic MapMode composition checks."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

try:
    from PIL import Image
except ImportError:
    Image = None

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_batchmesh_alpha import centre, write_scene  # noqa: E402
from test_map_fog import write_grid  # noqa: E402
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
import scene_fixture  # noqa: E402

GODOT = os.environ.get("EAWR_GODOT_EXECUTABLE")


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and GODOT and Image,
                     "set graphical viewer variables and install Pillow")
class BatchMeshAlphaFogMapMode(unittest.TestCase):
    def test_synthetic_map_composes_alpha_and_fog(self):
        with tempfile.TemporaryDirectory(prefix="eawr-batch-alpha-map-fog-") as temporary:
            path = Path(temporary)
            root = write_scene(path / "scene", [
                ("EAWR_ALPHA_FOG", "BatchMeshAlpha.fx", (220, 30, 30, 128), 0.6, 0.0)])

            def capture(label: str, grid: Path | None = None, digest: str = ""):
                report = path / f"{label}.json"
                image = path / f"{label}.png"
                command = [GODOT, "--path", str(ROOT / "apps/viewer/project"), "--",
                           "--eawr-map", scene_fixture.MAP_LOGICAL_PATH,
                           "--eawr-game-root", str(root), "--eawr-report", str(report),
                           "--eawr-capture", str(image), "--eawr-populate"]
                if grid:
                    command += ["--eawr-fog-grid", str(grid), "--eawr-fog-sha256", digest,
                                "--eawr-fog-team", "2", "--eawr-fog-revision", "1",
                                "--eawr-fog-tick", "47"]
                completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, timeout=180, check=False)
                self.assertTrue(report.is_file(), completed.stdout)
                data = json.loads(report.read_text(encoding="utf-8"))
                self.assertEqual(completed.returncode, 0, data.get("failure") or completed.stdout)
                return data, Image.open(image).convert("RGB")

            baseline_report, baseline_image = capture("baseline")
            full_grid = path / "full.eawr-fog"
            half_grid = path / "half.eawr-fog"
            full_hash = write_grid(full_grid, 2, bytes([255] * 6), origin=(280, 200), cell=(30, 40))
            half_hash = write_grid(half_grid, 2, bytes([110] * 6), origin=(280, 200), cell=(30, 40))
            full_report, full_image = capture("full", full_grid, full_hash)
            half_report, half_image = capture("half", half_grid, half_hash)
            baseline = centre(baseline_image, baseline_report)
            full = centre(full_image, full_report)
            half = centre(half_image, half_report)
            self.assertLessEqual(max(abs(a - b) for a, b in zip(baseline, full)), 2)
            self.assertLess(sum(half), sum(full))
            self.assertEqual(full_report["fog"]["renderer"]["uploads"], 1)
            self.assertEqual(half_report["fog"]["renderer"]["uploads"], 1)
            self.assertTrue(full_report["populate"]["evidence"]["unit_submissions_and_fog_materials_verified"])
            self.assertTrue(half_report["populate"]["evidence"]["unit_submissions_and_fog_materials_verified"])


if __name__ == "__main__":
    unittest.main()
