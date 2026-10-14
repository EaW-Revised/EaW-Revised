"""HCB-61/62: prepared-state probes of shared HUD readouts."""

import os
import pathlib
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_tactical_hud import extract_fonts, run  # noqa: E402


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
                     and os.environ.get("EAWR_EAW_GAME_ROOT"), "requires installed viewer runtime")
class HudReadoutsGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-hud-readouts-"))
        cls.cache = cls.directory / "fonts"
        extract_fonts(cls.cache)

    @classmethod
    def tearDownClass(cls):
        # Captures and reports stay in the rig's retained temporary evidence.
        shutil.rmtree(cls.cache, ignore_errors=True)

    def test_tech_activation_and_authored_placement(self):
        for faction, mode, expected in (("rebel", "campaign", "Tech: 4"),
                                         ("rebel", "skirmish", ""),
                                         ("underworld", "campaign", ""),
                                         ("empire", "expired", "")):
            with self.subTest(faction=faction, mode=mode):
                stem = f"{faction}-{mode}"
                capture = self.directory / f"{stem}.png"
                result, log = run(self, ["--eawr-ui-gallery", "--eawr-font-cache", str(self.cache),
                                        "--eawr-ui-hud", faction, "--eawr-ui-hud-readouts", mode,
                                        "--eawr-capture", str(capture)],
                                  self.directory / f"{stem}.json", resolution="1280x720")
                self.assertTrue(capture.is_file())
                panel = result["hud"]["production"]
                self.assertEqual(panel["tactical_tech"], expected)
                self.assertEqual(panel["tactical_population"], "7/25")
                if mode == "expired":
                    self.assertIsNone(panel["garrison_timer_progress"])
                    self.assertIsNone(panel["reinforcement_timer_progress"])
                    self.assertEqual(panel["garrison_timer_rect"], [0, 0, 0, 0])
                    self.assertEqual(panel["reinforcement_timer_rect"], [0, 0, 0, 0])
                else:
                    self.assert_timer_row(panel)
                x, y, width, height = panel["tactical_tech_rect"]
                self.assertGreater(width, 0)
                self.assertGreater(height, 0)
                self.assertLess(x, 300)
                self.assertGreater(y, 450)
                self.assertLess(y, 540)
                self.assertNotIn("ERROR:", log)

    def assert_timer_row(self, panel):
        self.assertEqual(panel["garrison_timer_progress"], 0.5)
        self.assertEqual(panel["reinforcement_timer_progress"], 0.25)
        gx, gy, gw, gh = panel["garrison_timer_rect"]
        rx, ry, rw, rh = panel["reinforcement_timer_rect"]
        self.assertAlmostEqual(gx + gw / 2, 30 * 720 / 768, places=2)
        self.assertAlmostEqual(gy + gh / 2, 60 * 720 / 768, places=2)
        self.assertGreater(gw, 0)
        self.assertGreater(gh, 0)
        self.assertAlmostEqual(rx + rw / 2 - (gx + gw / 2), gw + 5 * 720 / 768, places=2)
        self.assertEqual((gw, gh), (rw, rh))


if __name__ == "__main__":
    unittest.main()
