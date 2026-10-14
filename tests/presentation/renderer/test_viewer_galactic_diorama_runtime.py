"""GHUD-S52 the focused organize shell shows the panel of the planet's Zoomed_Terrain_Index."""
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
    from .space_map_test_support import decode_png
except ImportError:
    import test_viewer_planet_hud_runtime as support
    from space_map_test_support import decode_png

CAMPAIGN = "Sandbox_Equal_Footing_Underworld"


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticDioramaRuntime(unittest.TestCase):
    def focused(self, planet):
        return support.PlanetHudRuntime.capture(
            self, CAMPAIGN, "--eawr-galactic-focus", str(planet), "--eawr-galactic-flight-ms", "750")

    def test_two_planets_show_different_panels(self):
        overview, _ = support.PlanetHudRuntime.capture(self, CAMPAIGN)
        by_index = {}
        for row in overview["planets"]:
            index = row.get("terrain_index")
            if index is not None and row["drawn"] == "model" and index not in by_index:
                by_index[index] = row
        self.assertGreaterEqual(len(by_index), 2, "the campaign must author two different terrain indexes")
        (first, second) = [by_index[index] for index in sorted(by_index)[:2]]
        shown = []
        for planet in (first, second):
            data, capture = self.focused(planet["id"])
            hud = data["fleet_hud"]
            self.assertEqual(hud["diorama_panel"], planet["terrain_index"], planet["name"])
            pads = [row["rect"] for row in data["ground_hud"]["controls"] if row["name"].startswith("Pad_")]
            self.assertTrue(pads)
            left = int(min(r[0] for r in pads)); top = int(min(r[1] for r in pads))
            right = int(max(r[0] + r[2] for r in pads)); bottom = int(max(r[1] + r[3] for r in pads))
            width, height, rows = decode_png(capture.read_bytes())
            shown.append([row[left:right] for row in rows[top:bottom]])
        differing = sum(a != b for pa, pb in zip(*shown) for a, b in zip(pa, pb))
        self.assertGreater(differing, 200, "the two planets draw the same diorama panel")


if __name__ == "__main__":
    unittest.main()
