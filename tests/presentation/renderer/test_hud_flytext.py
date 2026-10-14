"""HCB-64: prepared local bounty feedback through the shared HUD readout."""

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
class HudFlytextGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-hud-flytext-"))
        cls.cache = cls.directory / "fonts"
        extract_fonts(cls.cache)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.cache, ignore_errors=True)

    def test_authored_feedback_moves_fades_and_clears(self):
        from PIL import Image, ImageChops

        captures = {}
        for state, age, alpha in (("start", 0, 255), ("hold", 30, 255), ("fade", 45, 127),
                                  ("expired", 61, 0), ("behind", 0, 0), ("clear", 0, 0)):
            with self.subTest(state=state):
                capture = self.directory / f"{state}.png"
                result, log = run(self, ["--eawr-ui-gallery", "--eawr-font-cache", str(self.cache),
                                        "--eawr-ui-hud", "underworld", "--eawr-ui-hud-flytext", state,
                                        "--eawr-capture", str(capture)],
                                  self.directory / f"{state}.json", resolution="1280x720")
                shown = result["flytext"]
                self.assertEqual(shown["face"], "EmpireAtWar-Medium")
                self.assertEqual(shown["point_size"], 7)
                self.assertTrue(shown["emboss"])
                self.assertFalse(shown["outline"])
                self.assertEqual(shown["alpha"], alpha)
                self.assertEqual(shown["text"], "+250" if alpha else "")
                if alpha:
                    self.assertAlmostEqual(shown["position"][0], 640)
                    self.assertAlmostEqual(shown["position"][1], 288 - age * 70 / 60)
                    x, y, w, h = shown["rect"]
                    self.assertAlmostEqual(x + w / 2, 640, places=2)
                    self.assertAlmostEqual(y + h / 2, shown["position"][1], places=2)
                    self.assertGreater(w, 0)
                self.assertNotIn("ERROR:", log)
                with Image.open(capture) as image:
                    captures[state] = image.convert("RGB").crop((550, 190, 730, 320))
        blank = captures["expired"]
        for state in ("behind", "clear"):
            self.assertIsNone(ImageChops.difference(captures[state], blank).getbbox())
        for state in ("start", "hold", "fade"):
            delta = ImageChops.difference(captures[state], blank)
            self.assertIsNotNone(delta.getbbox(), f"{state}: visible glyph pixels")
        self.assertTrue(any(r > 150 and g > 150 and b < 100
                            for r, g, b in captures["start"].getdata()), "XML-authored yellow glyph ink")
        # Movement relocates the glyph, and the same glyph at the faded position emits less light.
        self.assertIsNotNone(ImageChops.difference(captures["start"], captures["hold"]).getbbox())
        strength = lambda state: sum(sum(pixel) for pixel in ImageChops.difference(captures[state], blank).getdata())
        self.assertLess(strength("fade"), strength("hold"))


if __name__ == "__main__":
    unittest.main()
