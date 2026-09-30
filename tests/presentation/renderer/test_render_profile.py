"""Render profiles (#184): captures keep the retail look, players get the enhanced one.

A capture or test run renders without MSAA, screen-space AA or anisotropic
filtering; an interactive run that captures and tests nothing gets 4x MSAA,
SMAA and 16x anisotropic filtering (apps/viewer/src/render_profile.hpp).
`--eawr-render-profile retail|enhanced` forces either for evidence runs.

The graphical half is opt-in: set EAWR_GODOT_VIEWER_RUNTIME_TEST,
EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT. It captures the populated FoC
Coruscant map in both profiles. AA may only change edges: wherever the retail
frame is flat over a 3x3 neighbourhood the enhanced frame keeps its value, so
the stored-value decode still runs once with MSAA and after SMAA
(docs/rendering.md, render profiles). Captures stay in temporary directories.
"""

import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from PIL import Image


ROOT = pathlib.Path(__file__).resolve().parents[3]
CORUSCANT = "data/art/maps/_mp_space_coruscant.ted"
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
RETAIL = {"name": "retail", "msaa_samples": 0, "screen_space_aa": "disabled", "anisotropic_filtering": 0}
ENHANCED = {"name": "enhanced", "msaa_samples": 4, "screen_space_aa": "smaa", "anisotropic_filtering": 16}


def run_viewer(directory: pathlib.Path, name: str, *arguments: str):
    report = directory / f"{name}.json"
    capture = directory / f"{name}.png"
    completed = subprocess.run([
        os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
        "--path", str(ROOT / "apps/viewer/project"), "--", *arguments,
        "--eawr-report", str(report), "--eawr-capture", str(capture)],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300, check=False)
    result = json.loads(report.read_text(encoding="utf-8")) if report.is_file() else {}
    return completed, result, capture


def flat_pixels(image: Image.Image):
    """Interior pixels whose 3x3 neighbourhood has one colour."""
    width, height = image.size
    pixels = image.load()
    for y in range(1, height - 1):
        for x in range(1, width - 1):
            centre = pixels[x, y]
            if all(pixels[x + dx, y + dy] == centre for dy in (-1, 0, 1) for dx in (-1, 0, 1)):
                yield x, y


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST for the graphical render-profile runs")
class RenderProfileRuntime(unittest.TestCase):
    def test_unknown_profile_fails_before_any_mode(self):
        with tempfile.TemporaryDirectory(prefix="eawr-render-profile-") as temporary:
            for value in ("bogus", "Retail"):
                with self.subTest(value=value):
                    completed, result, capture = run_viewer(
                        pathlib.Path(temporary), f"bad-{value}", "--eawr-render-profile", value)
                    self.assertEqual(completed.returncode, 2, completed.stdout)
                    self.assertEqual(result.get("status"), "failed")
                    self.assertEqual(result.get("failure"), "--eawr-render-profile expects retail or enhanced")
                    self.assertFalse(capture.exists())

    @unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT for the FoC Coruscant map")
    def test_captures_are_retail_and_enhanced_changes_only_edges(self):
        with tempfile.TemporaryDirectory(prefix="eawr-render-profile-") as temporary:
            directory = pathlib.Path(temporary)
            scene = ("--eawr-game-root", GAME_ROOT, "--eawr-map", CORUSCANT, "--eawr-populate")
            runs = {
                "default": run_viewer(directory, "default", *scene),
                "retail": run_viewer(directory, "retail", *scene, "--eawr-render-profile", "retail"),
                "enhanced": run_viewer(directory, "enhanced", *scene, "--eawr-render-profile", "enhanced"),
            }
            for name, (completed, result, capture) in runs.items():
                with self.subTest(run=name):
                    self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                    self.assertEqual(result["status"], "space_environment_rendered")
                    self.assertNotIn("stored-value output", completed.stdout)
                    self.assertTrue(capture.is_file())
            self.assertEqual(runs["default"][1]["render_profile"], RETAIL)
            self.assertEqual(runs["retail"][1]["render_profile"], RETAIL)
            self.assertEqual(runs["enhanced"][1]["render_profile"], ENHANCED)
            self.assertEqual(runs["default"][2].read_bytes(), runs["retail"][2].read_bytes())

            retail = Image.open(runs["retail"][2]).convert("RGB")
            enhanced = Image.open(runs["enhanced"][2]).convert("RGB")
            self.assertEqual(retail.size, enhanced.size)
            before, after = retail.load(), enhanced.load()
            flat = list(flat_pixels(retail))
            self.assertGreater(len(flat), 10_000, "the Coruscant frame has flat sky regions")
            # One level of slack for SMAA's 8-bit round trip.
            moved = [(x, y) for x, y in flat
                     if max(abs(a - b) for a, b in zip(before[x, y], after[x, y])) > 1]
            self.assertLessEqual(len(moved), len(flat) // 1000, moved[:10])
            width, height = retail.size
            edges = sum(before[x, y] != after[x, y] for y in range(height) for x in range(width))
            self.assertGreater(edges, 0, "the enhanced profile antialiases some edge")


if __name__ == "__main__":
    unittest.main()
