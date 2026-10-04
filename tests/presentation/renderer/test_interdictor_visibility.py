"""BP-69: an idle Interdictor must not draw its authored-hidden blue stripe mesh."""

import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from battle_input_test_support import BattleInputRunner, decode_png


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
                     and os.environ.get("EAWR_EAW_GAME_ROOT"), "requires the GPU viewer and installed game")
class InterdictorVisibility(BattleInputRunner, unittest.TestCase):
    def test_idle_hull_has_no_blue_stripes_lit(self):
        with tempfile.TemporaryDirectory(prefix="eawr-interdictor-visibility-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "idle-interdictor", (
                "--eawr-skirmish-fleet", "2:Interdictor_Cruiser",
                "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                "--eawr-live-player", "2", "--eawr-live-follow", "11",
                "--eawr-live-step", "2", "--eawr-hud", "off",
                "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on"),
                camera=None, end_tick=120, session="skirmish")
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertTrue(live["headless_hashes_equal"])
            self.assertEqual(live["ion_shots"]["stunned"], [])
            width, height, rows = decode_png((directory / "idle-interdictor.png").read_bytes())
            self.assertEqual((width, height), (1280, 720))
            # The pinned follow view frames the dome area here. In the faulty
            # lit GTX baseline the scrolling ss mesh contributes hundreds of
            # bright cyan pixels; the normal hull and its lamps stay below 20.
            cyan = sum(g > 150 and b > 150 and r < g * 0.8
                       for row in rows[340:505] for r, g, b in row[530:755])
            self.assertLess(cyan, 20, "the idle gravity-well domes still carry bright blue stripes")


if __name__ == "__main__":
    unittest.main()
