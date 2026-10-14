"""A live capture refreshes the build strip and ground roster together."""
import os
import pathlib
import shutil
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class CaptureRuntime(unittest.TestCase):
    def test_captured_planet_shows_build_menu_and_invading_troops(self):
        data, capture = support.PlanetHudRuntime.capture(
            self, "synthetic", "--eawr-galactic-hud-fixture", "capture-live",
            "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750")
        self.assertEqual(next(row for row in data["planets"] if row["id"] == 101)["owner"], 1)
        facts = next(row for row in data["hud"]["intelligence"] if row["planet"] == 101)
        self.assertEqual(facts["owner"], 1)
        self.assertEqual(facts["structures"], 0, "defender structures are removed on capture")
        offers = data["command_bar"]["production_choices"]
        self.assertTrue(any(row["type_id"] == 1004 and row["enabled"] for row in offers),
                        "pre-fix symptom: owned captured planet has no build menu")
        self.assertEqual([row["type_id"] for row in offers], [1004],
                         "capture replaces the previous faction's choices with the new owner's list")
        ground = data["ground_hud"]
        self.assertEqual(ground["count"], "1")
        self.assertTrue(ground["count_visible"])
        self.assertTrue(any(row["object"] == 7001 and row["enabled"]
                            for row in ground["widgets"]),
                        "pre-fix symptom: invading ground troops are hidden after capture")
        self.assertTrue(any(row["name"].startswith("Pad_") and row["visible"] and row["enabled"]
                            for row in ground["controls"]))
        self.assertEqual(ground["structures"]["capacity"], 5)
        self.assertTrue(capture.is_file())
        # A rig run can opt into a durable eye-check artifact beside its receipt.
        if directory := os.environ.get("EAWR_CAPTURE_EVIDENCE_DIR"):
            destination = pathlib.Path(directory)
            destination.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(capture, destination / "captured-planet.png")
            shutil.copyfile(capture.with_name("report.json"), destination / "captured-planet.json")


if __name__ == "__main__":
    unittest.main()
