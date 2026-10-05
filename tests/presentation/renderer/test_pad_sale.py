"""Focused real-data Coruscant contracts for WBP-22 and WBP-30..32."""
import os
from pathlib import Path
import runpy
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

from live_session_test_support import CAMERA, CORUSCANT, ROOT, LiveSessionRunner


def pad_replay(directory: Path, pad_type: str, uc_type: str, final_tick: int, upgrade_type=None):
    decoder = runpy.run_path(str(ROOT / "tests/skirmish/test_skirmish_cli.py"))
    source = decoder["decode_setup"]((ROOT / "tests/skirmish/fixtures/m2-start.eawr-replay").read_bytes())
    crc = decoder["crc32_upper"]
    pad = list(next(row for row in source["units"] if row[1] == crc(pad_type)))
    ship = list(next(row for row in source["units"] if row[1] == crc("Corellian_Corvette")))
    pad[0], pad[2] = 2, 1
    ship[0], ship[2] = 1, 1
    ship[4:7] = pad[4:7]
    ship[4] += 250 * (1 << 24)
    body = struct.pack("<QIQBBH", 0, 1, 0, 13, 0, 0) + struct.pack("<QIIQ", crc(uc_type), 1, 0, 2)
    commands = [body]
    if upgrade_type:
        commands.append(struct.pack("<QIQBBH", 1100, 1, 1, 9, 0, 0)
                        + struct.pack("<QIIQ", crc(upgrade_type), 1, 0, 4))
    header = b"EAWRPLY\0" + struct.pack("<HHIIIIIQQIIQQ", 2, 104, 1, 1, 24, 1, 30,
        source["seed"], final_tick, len(source["players"]), 0, 2, len(commands)) + source["identity"]
    replay = header + b"".join(struct.pack("<IIQII", *row) for row in source["players"])
    replay += b"".join(struct.pack("<QQII3q4q", *row) for row in (ship, pad))
    replay += b"".join(struct.pack("<I", len(command)) + command for command in commands)
    path = directory / "pad.eawr-replay"
    path.write_bytes(replay)
    camera = ET.parse(CAMERA)
    initial = camera.getroot().find("initial")
    initial.set("target_x", str(pad[4] / (1 << 24)))
    initial.set("target_y", str(pad[5] / (1 << 24)))
    # Preserve the config-relative binding file when this camera moves to a temporary folder.
    camera.getroot().find("bindings").set("path", str(CAMERA.parent / "space-live-camera-bindings.json"))
    camera_path = directory / "camera.xml"
    camera.write(camera_path, encoding="utf-8", xml_declaration=True)
    # The completed child is created after replay start, so initial-unit selectors
    # cannot address it. Drive the ordinary mouse picker at the fixed pad position.
    point = "@" + ",".join(str(value / (1 << 24)) for value in pad[4:7])
    return path, camera_path, point


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed game and GPU rig")
class PadSaleGraphical(LiveSessionRunner, unittest.TestCase):
    def test_mine_upgrade_ticks_modified_income_on_coruscant(self):
        with tempfile.TemporaryDirectory(prefix="eawr-pad-upgrade-") as temporary:
            directory = Path(temporary)
            replay, camera, point = pad_replay(directory, "Mineral_Extractor_Pad", "UC_Rebel_Mineral_Extractor",
                                        2490, "RS_Increased_Supplies_L1_Upgrade")
            balances = []
            for tick in (2400, 2490):
                code, report = self._run(directory, f"upgrade-{tick}", ("--eawr-live-step", "3", "--eawr-live-ticks", str(tick),
                    "--eawr-live-reveal", "on", "--eawr-hud", "tactical", "--eawr-live-input", f"2350:click:{point}",
                    "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on", "--eawr-audio", "off"),
                    session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)), camera=camera, map_name=CORUSCANT)
                self.assertEqual(code, 0, report.get("failure"))
                live = report["live_session"]
                self.assertTrue(live["headless_hashes_equal"])
                self.assertEqual(live["pads"][0]["constructed"], 4)
                balances.append(float(live["economy"]["credits"]))
            self.assertLessEqual(abs(balances[1] - balances[0] - 72), 2, balances)

    def test_completed_mine_ticks_income_on_coruscant(self):
        with tempfile.TemporaryDirectory(prefix="eawr-pad-income-") as temporary:
            directory = Path(temporary)
            replay, camera, _ = pad_replay(directory, "Mineral_Extractor_Pad", "UC_Rebel_Mineral_Extractor", 1200)
            balances = []
            for tick in (1110, 1200):
                code, report = self._run(directory, f"income-{tick}", ("--eawr-live-step", "3", "--eawr-live-ticks", str(tick),
                    "--eawr-live-reveal", "on", "--eawr-hud", "tactical",
                    "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on", "--eawr-audio", "off"),
                    session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)), camera=camera, map_name=CORUSCANT)
                self.assertEqual(code, 0, report.get("failure"))
                live = report["live_session"]
                self.assertTrue(live["headless_hashes_equal"])
                self.assertEqual(live["pads"][0]["constructed"], 4)
                balances.append(float(live["economy"]["credits"]))
            # Ninety logical frames = three seconds at the authored 20 credits/sec.
            self.assertLessEqual(abs(balances[1] - balances[0] - 60), 2, balances)

    def test_guard_override_sells_child_and_reveals_live_pad(self):
        with tempfile.TemporaryDirectory(prefix="eawr-pad-sale-") as temporary:
            directory = Path(temporary)
            replay, camera, point = pad_replay(directory, "Defense_Satellite_Laser_Pad", "UC_Rebel_Defense_Satellite_Laser", 810)
            code, report = self._run(directory, "sale", ("--eawr-live-step", "1", "--eawr-live-ticks", "810",
                "--eawr-live-reveal", "on", "--eawr-live-capture-ticks", "754,780", "--eawr-hud", "tactical",
                "--eawr-live-input", f"755:click:{point}", "--eawr-live-input", f"760:rclick:{point}+ctrl+alt",
                "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on", "--eawr-audio", "off"),
                session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)), camera=camera, map_name=CORUSCANT)
            self.assertEqual(code, 0, report.get("failure"))
            live = report["live_session"]
            self.assertTrue(live["headless_hashes_equal"])
            self.assertEqual(len(live["pads"]), 1)
            self.assertEqual(live["pads"][0]["entity"], 2)
            self.assertEqual(live["pads"][0]["constructed"], 0)
            self.assertEqual(live["pads"][0]["owner"], 1)
            self.assertEqual(float(live["economy"]["credits"]), 6000 - 875 + 438)
            self.assertTrue(any("sell 4" in row for row in report["battle_input"]["log"]))


if __name__ == "__main__":
    unittest.main()
