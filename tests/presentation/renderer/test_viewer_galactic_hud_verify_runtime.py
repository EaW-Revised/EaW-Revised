"""The retained galactic HUD state against a rebuild, after every publication of an all-AI campaign (#4331).

--eawr-galactic-hud-verify rebuilds the planet facts from nothing for each publication and compares them with the ones the
HUD keeps, and re-runs every command-bar and weapon-action update the viewer skipped because nothing they are written from
had changed. Owners, levels, income and production queues change over the run, so a fact input that changed must show in
the HUD's facts on the very next publication, and a skipped update must leave every control as the full update would.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support
from tools.capture.gc_ai_proof import make_overlay

STEPS = 1800


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticHudVerifyRuntime(unittest.TestCase):
    def verify(self, reveal):
        directory = Path(tempfile.mkdtemp(prefix="eawr-hud-verify-"))
        campaign = "Sandbox_Gateways_Rebel"
        overlay = make_overlay(support.GAME_ROOT, campaign, directory / "overlay")
        report = directory / "report.json"
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--",
            "--eawr-galactic", campaign, "--eawr-game-root", support.GAME_ROOT,
            "--eawr-mod-root", str(overlay), "--eawr-galactic-paused", "no", "--eawr-audio", "off",
            "--eawr-galactic-all-ai", "yes", "--eawr-galactic-seed", "37", "--eawr-galactic-ticks", str(STEPS),
            "--eawr-galactic-reveal", reveal, "--eawr-galactic-hud-verify", "yes",
            "--eawr-report", str(report), "--eawr-capture", str(directory / "capture.png")],
            cwd=support.ROOT, capture_output=True, text=True, timeout=1200, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertFalse(data["failure"])
        self.assertEqual(data["live_session"]["galactic_frame"], STEPS)
        work = data["hud_work"]
        print("HUD_VERIFY", json.dumps({"reveal": reveal, "directory": str(directory), **work}))
        # One facts check per publication, plus every command-bar and weapon-action update the viewer skipped.
        self.assertGreater(work["verify_checks"], STEPS)
        self.assertEqual(work["verify_mismatches"], 0, f"retained HUD state differs from a rebuild; see {directory}")
        # The run changed facts (production queues tick, planets change hands), so the check saw changes land.
        self.assertGreater(work["fact_changes"], 0)

    def test_fog_on(self):
        self.verify("off")

    def test_fog_off(self):
        self.verify("on")


if __name__ == "__main__":
    unittest.main()
