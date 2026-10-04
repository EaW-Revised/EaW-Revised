"""Captured merchant production through the actual command bar and reinforcement pane."""

import math
import os
from pathlib import Path
import sys
import struct
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from live_session_test_support import LiveSessionRunner
from space_hazard_cases import focused_camera, place_ship
from test_pad_capture import replay_units


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed game and GPU rig")
class MerchantPurchaseGraphical(LiveSessionRunner, unittest.TestCase):
    def test_captured_dock_purchase_and_reinforcement(self):
        # WBP-33/34, WPR-22/30/33: an ordinary queue, then the player's placement gesture.
        with tempfile.TemporaryDirectory(prefix="eawr-merchant-") as temporary:
            directory = Path(temporary)
            replay = directory / "merchant.eawr-replay"
            map_name = "data/art/maps/_mp_space_coruscant.ted"
            look = ("--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on")
            common = ("--eawr-live-ai", "off", "--eawr-audio", "off", "--eawr-live-reveal", "on",
                      "--eawr-live-step", "1", "--eawr-live-player", "1", "--eawr-hud", "tactical", *look)
            code, initial = self._run(directory, "setup", (
                "--eawr-skirmish-players", "1,2", "--eawr-skirmish-slot", "1:Rebel:0:human",
                "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-fleet", "1:Corellian_Corvette",
                "--eawr-skirmish-fleet", "2:none", "--eawr-live-replay-out", str(replay),
                "--eawr-live-ticks", "1", *common),
                session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
            self.assertEqual(code, 0, initial.get("failure"))
            ship = next(row for row in initial["live_session"]["start_fleet"]
                        if row["player"] == 1 and row["type"] == "Corellian_Corvette")
            dock = next(row for row in replay_units(replay)
                        if row[1] == zlib.crc32(b"SKIRMISH_MERCHANT_DOCK"))
            x, y, z = (value / (1 << 24) for value in dock[4:7])
            place_ship(replay, ship["entity"], x + 100, y, z)
            # A nondefault policy makes this staged Coruscant recording bind its
            # recorded custom roster, rather than the legacy exact M2 fixture.
            data = bytearray(replay.read_bytes())
            version, header = struct.unpack_from("<HH", data, 8)
            self.assertIn(version, (2, 3))
            self.assertEqual(header, 104)
            data[header:header] = struct.pack("<IHHI", 1, 1, 4, 1)  # heroes off
            struct.pack_into("<HH", data, 8, version + 2, 116)
            replay.write_bytes(data)
            camera = focused_camera(directory, initial, x + 500, y, z)
            code, report = self._run(directory, "purchase", (
                *common, "--eawr-live-ticks", "1200", "--eawr-live-capture-ticks", "480,560,880,960,1120",
                "--eawr-live-input", f"510:click:unit={dock[0]}",
                "--eawr-live-input", "540:click:card=0",
                "--eawr-live-input", "900:click:hud=b_reinforcement",
                "--eawr-live-input", "920:press:hud=r_0000",
                "--eawr-live-input", f"930:hover:@{x + 1000},{y},0",
                "--eawr-live-input", f"940:release:@{x + 1000},{y},0"),
                session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
                camera=camera, map_name=map_name)
            self.assertEqual(code, 0, report.get("failure"))
            live, battle = report["live_session"], report["battle_input"]
            state = next(row for row in live["pads"] if row["entity"] == dock[0])
            self.assertEqual(state["owner"], 1, state)
            self.assertEqual((state["under_construction"], state["constructed"]), (0, 0), state)
            self.assertEqual(live["rejected"], [])
            self.assertTrue(live["headless_hashes_equal"])
            self.assertEqual(battle["production"]["buys"], 1, battle["log"])
            self.assertEqual(battle["production"]["placements"], 1, battle["log"])
            self.assertEqual(live["economy"]["pool"], [])
            self.assertEqual(live["economy"]["population"], 2)
            self.assertLessEqual(abs(float(live["economy"]["credits"]) - (6000 - 1200 + 1200 * 5 / 30)), 2)
            arrivals = [row for row in live["arrivals"] if row["owner"] == 1]
            self.assertTrue(arrivals, live["arrivals"])
            self.assertTrue(all(row["landed_tick"] - row["first_tick"] == 150 for row in arrivals), arrivals)
            bought = [row for row in live["own_units"] if row["entity"] in {item["unit"] for item in arrivals}]
            self.assertTrue(bought, live["own_units"])
            self.assertTrue(any(math.dist(row["position"][:2], (x + 1000, y)) < 100 for row in bought), bought)


if __name__ == "__main__":
    unittest.main()
