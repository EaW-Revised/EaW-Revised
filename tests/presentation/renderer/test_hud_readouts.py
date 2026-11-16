"""HCB-61/62: prepared-state probes of shared HUD readouts."""

import os
import pathlib
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_tactical_hud import extract_fonts, run  # noqa: E402

try:
    from PIL import Image
except ImportError:  # the pixel check needs Pillow; the rest of the file does not
    Image = None


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

    def test_deathstar_switch_states(self):
        # HCB-47: the presenter's three looks on the real shell (lit, approaching, fired) and no object.
        expected = {
            "lit": dict(shown=True, enabled=True, icon_frame=0, countdown=" 000.000"),
            "approaching": dict(shown=True, enabled=False, icon_frame=-1, countdown=" 097.766"),
            "fired": dict(shown=True, enabled=False, icon_frame=4, countdown=""),
            "hidden": dict(shown=False, enabled=False, icon_frame=-1, countdown=""),
        }
        for state, want in expected.items():
            with self.subTest(state=state):
                capture = self.directory / f"deathstar-{state}.png"
                result, log = run(self, ["--eawr-ui-gallery", "--eawr-font-cache", str(self.cache),
                                        "--eawr-ui-hud", "empire", "--eawr-ui-hud-switch", state,
                                        "--eawr-capture", str(capture)],
                                  self.directory / f"deathstar-{state}.json", resolution="1280x720")
                self.assertTrue(capture.is_file())
                switch = result["hud"]["deathstar_switch"]
                self.assertTrue(switch["present"])
                self.assertEqual(switch["shown"], want["shown"])
                self.assertEqual(switch["enabled"], want["enabled"])
                self.assertEqual(switch["icon_frame"], want["icon_frame"])
                self.assertEqual(switch["countdown_text"], want["countdown"])
                self.assertEqual(switch["countdown_shown"], bool(want["countdown"]))
                if want["shown"]:
                    x, y, width, height = switch["hit_rect"]
                    # The whole lever panel takes the pointer: the 50 x 106 art plus the 6-unit rim, at 720/768.
                    self.assertGreaterEqual(width, (50 + 12) * 720 / 768 - 0.5)
                    self.assertGreaterEqual(height, (106 + 12) * 720 / 768 - 0.5)
                if want["shown"]:
                    # Click -> presenter's answer -> visual: a probe clicks the centre and the four corners of the lever
                    # panel (all five reach the switch, the two points just outside it do not), and the answer follows the
                    # state: the press for a ready switch, the negative cue for a not-ready pull, silence once fired.
                    probes = {row["name"]: row["presses"] for row in switch["probe"]}
                    for name in ("panel_centre", "panel_top_left", "panel_bottom_right", "panel_top_right", "panel_bottom_left"):
                        self.assertEqual(probes[name], 1, (name, switch["probe"]))
                    for name in ("outside_left", "outside_above"):
                        self.assertEqual(probes[name], 0, (name, switch["probe"]))
                    clicks = switch["clicks"]
                    answer = {"lit": "press", "approaching": "negative", "fired": "none"}[state]
                    self.assertEqual(clicks[answer], 5, clicks)
                    self.assertEqual(sum(clicks.values()), 5, clicks)
                    # The look: ready is enabled with the lit lever, not-ready is drawn disabled; the state names differ.
                    self.assertEqual(switch["state"] == "disabled", not want["enabled"], switch["state"])
                if want["countdown"]:
                    # The readout is the component's dial art at its texel size (220 x 97) on the shell scale
                    # (720/768) around the shell's bone, with the text on the HUD's reference height: not the
                    # 9-pixel label of the first version, and inside the viewport.
                    self.assertTrue(switch["countdown_anchored"])
                    dx, dy, dw, dh = switch["countdown_dial_rect"]
                    self.assertAlmostEqual(dw, 220 * 720 / 768, places=1)
                    self.assertAlmostEqual(dh, 97 * 720 / 768, places=1)
                    self.assertGreaterEqual(dx, 0)
                    self.assertGreaterEqual(dy, 0)
                    self.assertLessEqual(dx + dw, 1280)
                    self.assertLessEqual(dy + dh, 720)
                    tx, ty, tw, th = switch["countdown_text_rect"]
                    self.assertEqual(switch["countdown_face"].casefold(), "empireatwar-light")
                    if any(self.cache.rglob("*.ttf")):
                        self.assertEqual(switch["countdown_face_source"], "cache")
                    else:
                        print(f"skipped face_source == 'cache' check: no staged user font cache on this host "
                              f"(face source was {switch['countdown_face_source']!r})")
                    if state == "lit":
                        self.assertEqual(switch["countdown_colour"], [255, 0, 0, 255])
                        # TSW-C2: the ready digits have opaque red interiors and a black
                        # border, even across the balance wedges. A thin antialiased
                        # face with a pale tint used to pass all layout-only checks.
                        if Image is None:
                            self.skipTest("Pillow is required for the digit pixel check")
                        with Image.open(capture) as image:
                            band = image.convert("RGB").crop((int(tx), int(ty),
                                                             int(tx + tw), int(ty + th)))
                            colours = list(band.getdata())
                        self.assertGreater(sum(r > 220 and g < 20 and b < 20 for r, g, b in colours), 30)
                        self.assertGreater(sum(max(rgb) < 8 for rgb in colours), 30)
                    self.assertGreater(tw, 0)
                    self.assertGreater(switch["countdown_pixels"], 9)
                    self.assertGreaterEqual(tx, dx)
                    self.assertLess(tx, dx + dw / 2)
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
