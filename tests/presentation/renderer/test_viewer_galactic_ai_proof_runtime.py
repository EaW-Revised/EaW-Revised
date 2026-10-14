"""Same-host all-AI live campaign capture, with one snapshot per logical step."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support
from tools.capture.gc_ai_proof import collect, make_overlay


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticAiProofRuntime(unittest.TestCase):
    def test_three_minute_all_ai_repeatability(self):
        directory = Path(tempfile.mkdtemp(prefix="eawr-ai-proof-"))
        campaign = "Sandbox_Gateways_Rebel"
        overlay = make_overlay(support.GAME_ROOT, campaign, directory / "overlay")
        for name in ("a", "b"):
            report = directory / f"repeat-{name}.json"
            result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(support.ROOT / "apps/viewer/project"), "--",
                "--eawr-galactic", campaign, "--eawr-game-root", support.GAME_ROOT,
                "--eawr-mod-root", str(overlay), "--eawr-galactic-paused", "no",
                "--eawr-galactic-all-ai", "yes", "--eawr-galactic-seed", "37",
                "--eawr-galactic-ticks", "5400", "--eawr-report", str(report),
                "--eawr-capture", str(directory / f"repeat-{name}.png")],
                cwd=support.ROOT, capture_output=True, text=True, timeout=900, check=False)
            (directory / f"repeat-{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(report.is_file(), result.stdout + result.stderr)
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(data["live_session"]["submitted_orders"], 0)
            self.assertEqual(data["live_session"]["galactic_frame"], 5400)
        proof = collect(directory, directory / "collected", 5400)
        self.assertTrue(proof["deterministic_repeats"])
        self.assertEqual(len(proof["samples"]), 4)
        self.assertGreater(len(proof["samples"][-1]["players"]), 1)
        from tests.presentation.renderer.space_map_test_support import decode_png
        for frame in (1800, 3600, 5400):
            width, height, rows = decode_png((directory / f"repeat-a-frame-{frame}.png").read_bytes())
            self.assertEqual((width, height), (1280, 720))
            self.assertGreater(len({pixel[:3] for row in rows for pixel in row}), 32, "galaxy capture is blank")


if __name__ == "__main__":
    unittest.main()
