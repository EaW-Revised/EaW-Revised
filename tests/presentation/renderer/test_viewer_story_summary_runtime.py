"""WST-58: the stock campaign's intro holocron goal opens the story summary once, at the authored chapter."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
# The summary panel's border colour (0.35, 0.6, 0.9) as 8-bit channels.
BORDER = (89, 153, 230)


def near_border(pixel):
    return all(abs(int(a) - b) <= 6 for a, b in zip(pixel[:3], BORDER))


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the GPU route and installed FoC data")
class StorySummaryRuntime(unittest.TestCase):
    def test_equal_footing_opens_the_intro_summary_after_the_first_story_service(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-story-summary-"))
        report = directory / "summary.json"
        capture = directory / "summary.png"
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
            str(ROOT / "apps/viewer/project"), "--",
            "--eawr-galactic", "Sandbox_Equal_Footing_Underworld", "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "no", "--eawr-galactic-ticks", "75",
            "--eawr-report", str(report), "--eawr-capture", str(capture)], cwd=ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue(capture.is_file(), log)
        from space_map_test_support import decode_png
        width, height, rows = decode_png(capture.read_bytes())
        self.assertEqual((width, height), (1280, 720))
        border = sum(1 for row in rows for pixel in row if near_border(pixel))
        self.assertGreater(border, 400, "the summary panel's border is on screen")
        self.assertNotIn("ERROR:", log)


if __name__ == "__main__":
    unittest.main()
