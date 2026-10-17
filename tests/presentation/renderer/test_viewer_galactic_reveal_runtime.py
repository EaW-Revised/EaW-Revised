"""#3931: the galactic fog reveal changes only what is drawn, never the simulation."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support
from tools.capture.gc_ai_proof import make_overlay


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticRevealRuntime(unittest.TestCase):
    def test_reveal_keeps_the_seeded_campaign_state_identical(self):
        directory = Path(tempfile.mkdtemp(prefix="eawr-galactic-reveal-"))
        campaign = "Sandbox_Gateways_Rebel"
        overlay = make_overlay(support.GAME_ROOT, campaign, directory / "overlay")
        reports = {}
        for flag in ("off", "on"):
            report = directory / f"reveal-{flag}.json"
            result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(support.ROOT / "apps/viewer/project"), "--",
                "--eawr-galactic", campaign, "--eawr-game-root", support.GAME_ROOT,
                "--eawr-mod-root", str(overlay), "--eawr-galactic-paused", "no",
                "--eawr-galactic-all-ai", "yes", "--eawr-galactic-seed", "37",
                "--eawr-galactic-ticks", "900", "--eawr-galactic-reveal", flag,
                "--eawr-report", str(report), "--eawr-capture", str(directory / f"reveal-{flag}.png")],
                cwd=support.ROOT, capture_output=True, text=True, timeout=900, check=False)
            (directory / f"reveal-{flag}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            reports[flag] = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(reports[flag]["failure"], "")
            self.assertIs(reports[flag]["live_session"]["galactic_reveal"], flag == "on")
        off, on = reports["off"]["live_session"], reports["on"]["live_session"]
        self.assertEqual(off["galactic_frame"], 900)
        self.assertEqual(on["galactic_frame"], 900)
        # The digest is of the truth publication, so it is the same whatever the player is shown.
        self.assertEqual(off["snapshot_sha256"], on["snapshot_sha256"])
        self.assertEqual(off["snapshot_events"], on["snapshot_events"])


if __name__ == "__main__":
    unittest.main()
