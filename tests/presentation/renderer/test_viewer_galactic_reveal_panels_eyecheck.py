"""Eye-check captures for the #3954 reveal panels: a live Empire campaign with the home planet focused (friendly
layout, captions) and the synthetic fleet-slot fixture (another owner's fleet present: enemy layout)."""
import shutil
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class RevealPanelsEyeCheck(unittest.TestCase):
    def test_live_empire_home_planet_focused(self):
        overview, _ = support.PlanetHudRuntime.capture(self, "Sandbox_Equal_Footing_Empire")
        local = [card for card in overview["fleet_hud"]["cards"] if card["local"] and card["fleet"] is not None]
        self.assertTrue(local, "the Empire seat starts with a local fleet card")
        planet = local[0]["planet"]
        data, capture = support.PlanetHudRuntime.capture(
            self, "Sandbox_Equal_Footing_Empire", "--eawr-galactic-focus", str(planet), "--eawr-galactic-flight-ms", "750")
        print("eyecheck live focus planet", planet, "capture", capture)
        shutil.copyfile(capture, capture.with_name("eyecheck-live-empire-focused.png"))
        print("eyecheck captions", data["fleet_hud"]["captions"])
        self.assertTrue(data["fleet_hud"]["cards"])


if __name__ == "__main__":
    unittest.main()
