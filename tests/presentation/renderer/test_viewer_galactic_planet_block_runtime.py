"""GHUD-S63/S64: the planet block's row structure (emblem, tint, unit-count icon, anchoring)."""
import os
import pathlib
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support
ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT

LEFT, CENTER, RIGHT = 0, 1, 2  # Godot HorizontalAlignment


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticPlanetBlockRuntime(unittest.TestCase):
    capture = support.GalacticCommandBarRuntime.capture

    def block(self):
        data, _ = self.capture(3, "production")
        return {row["name"]: row for row in data["command_bar"]["planet_block"]}

    def test_rows_follow_the_authored_anchoring(self):
        rows = self.block()
        # Unjustified text is centred on its anchor; the day counter is left- and the figures right-justified.
        self.assertEqual(rows["Text_Planet"]["align"], CENTER)
        self.assertEqual(rows["Text_Day_Counter"]["align"], LEFT)
        for name in ("Text_Credits", "Text_Day_Credits", "Text_Planetary_Income", "Text_Pop_Cap"):
            self.assertEqual(rows[name]["align"], RIGHT, name)

    def test_title_row_has_the_emblem_and_the_value_shares_the_tint(self):
        rows = self.block()
        emblem, title, value = rows["Text_Allegiance_A"], rows["Text_Planet"], rows["Text_Planetary_Income"]
        self.assertTrue(emblem["visible"])
        self.assertGreater(emblem["rect"][2], 0)
        centre_y = emblem["rect"][1] + emblem["rect"][3] / 2
        self.assertAlmostEqual(centre_y, title["rect"][1] + title["rect"][3] / 2, delta=title["rect"][3])
        self.assertLess(emblem["rect"][0] + emblem["rect"][2], title["rect"][0] + title["rect"][2] / 2)
        self.assertEqual(title["colour"], value["colour"])

    def test_reserve_is_a_dollar_prefixed_grouped_whole_number(self):
        reserve = self.block()["Text_Credits"]["text"]
        self.assertRegex(reserve, r"^\$ -?\d{1,3}(,\d{3})*$")

    def test_unit_count_row_carries_its_icon_beside_the_text(self):
        rows = self.block()
        count, credits = rows["Text_Pop_Cap"], rows["Text_Credits"]
        icon = count["icon"]
        self.assertTrue(icon["visible"])
        self.assertGreater(icon["rect"][2], 0)
        # The text ends at the anchor and the icon is centred right of it (Icon_Offset).
        self.assertGreaterEqual(icon["rect"][0] + icon["rect"][2] / 2, count["rect"][0] + count["rect"][2])
        # The count shares the bottom row with the reserve figure.
        self.assertAlmostEqual(count["rect"][1], credits["rect"][1], delta=4)
        self.assertRegex(count["text"], r"^\d+ / \d+$")

    def test_cached_coruscant_title_fits_its_authored_width(self):
        from tools.fonts.extract_eaw_fonts import extract, locate_executable
        cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-title-fonts-"))
        extract(locate_executable(pathlib.Path(GAME_ROOT)), cache, False, False)
        previous = os.environ.get("EAWR_FONT_CACHE")
        os.environ["EAWR_FONT_CACHE"] = str(cache)
        try:
            # Authored campaigns number human seats independently of synthetic faction IDs.
            data, _ = self.capture(1, campaign="Sandbox_Equal_Footing_Empire")
        finally:
            if previous is None:
                os.environ.pop("EAWR_FONT_CACHE", None)
            else:
                os.environ["EAWR_FONT_CACHE"] = previous
        self.assertEqual(data["hud"]["name_font_source"], "cache", "a fallback font cannot qualify the authored raster")
        title = next(row for row in data["command_bar"]["planet_block"] if row["name"] == "Text_Planet")
        self.assertEqual(title["text"], "CORUSCANT")
        # GHUD-S66: 9 pt exceeds Max_Text_Width in shell units at 720p; 8 pt fits.
        self.assertEqual(title["glyph_pixels"], 12)
        self.assertLessEqual(title["text_width"], 110 * 720 / 768)


if __name__ == "__main__":
    unittest.main()
