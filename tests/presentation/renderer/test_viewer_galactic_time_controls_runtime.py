"""Capture the GC clock controls with paired synthetic lockstep seats."""
import json
import contextlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires GPU viewer and installed FoC")
class GalacticTimeControlsRuntime(unittest.TestCase):
    def test_clock_controls_by_role(self):
        destination = os.environ.get("EAWR_LAN_EYECHECK")
        with contextlib.nullcontext(tempfile.mkdtemp(prefix="gc-clock-")) as temporary:
            output = Path(destination or temporary).resolve() / "gc-clock"
            output.mkdir(parents=True, exist_ok=True)
            for role, fixture in (("solo", "accessible"), ("host", "clock-mp-host"),
                                  ("client", "clock-mp-client"), ("coop", "clock-mp-coop")):
                with self.subTest(role=role):
                    report, capture = output / (role + ".json"), output / (role + ".png")
                    command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--render-thread", "separate",
                               "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-galactic", "synthetic",
                               "--eawr-galactic-hud-fixture", fixture, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                               "--eawr-report", str(report), "--eawr-capture", str(capture)]
                    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
                    self.assertTrue(report.is_file(), result.stdout + result.stderr)
                    data = json.loads(report.read_text(encoding="utf-8"))
                    self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
                    self.assertEqual(data["status"], "captured")
                    controls = {item["name"]: item for item in data["command_bar"]["controls"]}
                    # #4552: the authored pause button drives the shared clock in a LAN match; fast forward stays
                    # solo-only (TM-04), and no developer placeholder buttons are drawn.
                    self.assertTrue(controls["b_play_pause"]["enabled"], role)
                    self.assertEqual(controls["b_fast_forward"]["enabled"], role == "solo", role)
                    self.assertEqual([name for name in controls if name.startswith("SharedClock")], [])
                    self.assertTrue(capture.is_file())
