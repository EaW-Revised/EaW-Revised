"""The fighter icon waits at the landing point while the craft jump in (WU-49)."""

import math
import os
import pathlib
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from battle_input_test_support import BattleInputRunner, CAMERA


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game")
class HyperspaceIcons(BattleInputRunner, unittest.TestCase):
    def test_bought_fighter_icon_waits_at_destination_then_hands_off(self):
        with tempfile.TemporaryDirectory(prefix="eawr-arrival-icon-") as temporary:
            directory = pathlib.Path(temporary)
            common = ["--eawr-live-ai", "off", "--eawr-live-player", "1", "--eawr-live-reveal", "on",
                      "--eawr-live-step", "1", "--eawr-audio", "off",
                      "--eawr-skirmish-slot", "1:Rebel:0:human", "--eawr-skirmish-slot", "2:Empire:1:ai",
                      "--eawr-skirmish-fleet", "1:Rebel_X-Wing_Squadron", "--eawr-skirmish-fleet", "2:none",
                      "--eawr-live-input", "10:click:unit=1"]
            code, menu = self._run(directory, "menu", common, camera=None, end_tick=30, session="skirmish")
            self.assertEqual(code, 0, menu.get("failure"))
            card = next(row for row in menu["hud"]["unit_cards"]["drawn"]
                        if row["type"] == "Rebel_X-Wing_Squadron")
            ready = 20 + card["build_frames"] + 60
            placement = ready + 20
            station = next(row["position"] for row in menu["live_session"]["own_units"] if row["entity"] == 1)
            point = f"@{station[0] + 600},{station[1] - 500},0"
            camera = directory / "arrival-camera.xml"
            shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
            camera.write_text(re.sub(r"<initial [^>]*/>",
                f'<initial target_x="{station[0] + 600}" target_y="{station[1] - 500}" '
                'target_height="0" zoom="0.8" yaw_degrees="0"/>', CAMERA.read_text()))
            code, result = self._run(directory, "arrival", common + [
                "--eawr-live-input", f"20:click:card={card['slot']}",
                "--eawr-live-input", f"{ready}:click:hud=b_reinforcement",
                "--eawr-live-input", f"{ready + 10}:press:hud=r_0000",
                "--eawr-live-input", f"{ready + 15}:hover:{point}",
                "--eawr-live-input", f"{placement}:release:{point}"],
                camera=camera, end_tick=placement + 215, session="skirmish")
            self.assertEqual(code, 0, result.get("failure"))
            self.assertTrue(result["live_session"]["headless_hashes_equal"])
            self.assertEqual(result["live_session"]["rejected"], [])
            arrivals = result["live_session"]["arrivals"]
            self.assertEqual(len(arrivals), 6, "five craft and their squadron must arrive")
            group = max(row["unit"] for row in arrivals)
            trace = next(row for row in result["world_ui"]["gripper_points"] if row["squadron"] == group)
            self.assertEqual(trace["arrival_samples_dropped"], 0)
            samples = trace["arrival_samples"]
            during = [row for row in samples if row["arriving"]]
            after = [row for row in samples if not row["arriving"]]
            self.assertGreaterEqual(len(during), 110, "cover the visible jump, rather than just its final frame")
            self.assertGreaterEqual(len(after), 55, "cover the handoff to ordinary icon movement")
            self.assertGreater(math.dist(during[0]["desired"], during[-1]["desired"]), 3000)
            self.assertTrue(all(row["drawn"] for row in during), "the icon must actually be drawn during arrival")
            destination = during[0]["anchor"]
            self.assertAlmostEqual(destination[0], station[0] + 600, delta=.01)
            self.assertAlmostEqual(destination[1], station[1] - 500, delta=.01)
            self.assertLessEqual(max(math.dist(row["anchor"], destination) for row in during), .01)
            self.assertGreater(math.dist(during[0]["craft_centre"], destination), 3000)
            self.assertLessEqual(math.dist(during[-1]["craft_centre"], destination), 10)
            self.assertLessEqual(math.dist(after[0]["anchor"], after[0]["desired"]), .01)
            # Landing uses the presented container bounds, slightly offset from the craft centroid.
            # Later ordinary formation movement retains the velocity-capped slide (WSU-34).
            self.assertLessEqual(math.dist(after[0]["anchor"], after[0]["craft_centre"]), 10)


if __name__ == "__main__":
    unittest.main()
