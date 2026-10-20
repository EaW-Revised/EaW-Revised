"""#683 (WST-40): the stock plot's heroes are placed in the running live galaxy and drawn as fleet cards."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_planet_hud_runtime as support

CAMPAIGN = "Sandbox_Equal_Footing_Underworld"


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticStoryHeroRuntime(unittest.TestCase):
    def run_galaxy(self, ticks):
        # The all-AI capture runs the authored campaign unpaused for a fixed number of frames; the reveal shows every
        # fleet card (presentation only, the simulation is the same).
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-story-hero-"))
        report, capture = directory / "report.json", directory / "capture.png"
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--", "--eawr-galactic", CAMPAIGN,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "no", "--eawr-galactic-all-ai", "yes",
            "--eawr-galactic-seed", "37", "--eawr-galactic-reveal", "on", "--eawr-galactic-ticks", str(ticks),
            "--eawr-report", str(report), "--eawr-capture", str(capture)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=600, check=False)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["failure"], "")
        return data, capture

    @staticmethod
    def groups(data):
        return [(card["owner"], card["planet"], group) for card in data["fleet_hud"]["cards"] for group in card["groups"]]

    def test_plot_heroes_appear_in_the_running_galaxy(self):
        # One frame in, the plots whose start event fires at once (Rebel and Empire, elapsed 0) have placed their heroes
        # and the Underworld plot (the local player's, one second of unpaused time) has not. At frame 45 it has placed
        # its heroes; the fleet cards show the ones that sit in an orbit fleet (the census holds all five).
        before, _ = self.run_galaxy(1)
        after, _ = self.run_galaxy(45)
        self.assertEqual(before["live_session"]["galactic_frame"], 1)
        self.assertEqual(after["live_session"]["galactic_frame"], 45)
        local = before["local"]["player"]
        early = [(owner, planet, group) for owner, planet, group in self.groups(before) if group["hero"]]
        print("heroes at frame 1:", [(owner, planet, group["type"], group["count"]) for owner, planet, group in early])
        self.assertTrue(any(owner != local for owner, _, _ in early), "the Rebel plot's start heroes are placed at the first service")
        self.assertFalse(any(owner == local for owner, _, _ in early), "the local plot has not started")
        late = [(owner, planet, group) for owner, planet, group in self.groups(after) if group["hero"] and owner == local]
        print("local heroes at frame 45:", [(planet, group["type"], group["count"]) for _, planet, group in late])
        self.assertTrue(late, "a hero of the local plot appears on a fleet card after a second")
        self.assertTrue(all(group["count"] >= 1 for _, _, group in late))


if __name__ == "__main__":
    unittest.main()
