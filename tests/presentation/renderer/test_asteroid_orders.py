"""Ordinary right clicks through stock asteroid fields reach the move point."""
import math
import os
import struct
import zlib
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))

from live_session_test_support import LiveSessionRunner
from space_hazard_cases import focused_camera, place_ship
import test_pad_capture as pad_capture


class AsteroidPadOrdinaryClick(pad_capture.MiningPadCaptureGraphical):
    def approach_inputs(self, x, y, z):
        # Click field space beside the pad's pick mesh, within capture range after arrival.
        # The pad itself keeps its authored tactical-build mouse contact.
        return ("--eawr-live-input", f"3:click:@{x + 600},{y},{z}",
                "--eawr-live-input", f"6:rclick:@{x + 200},{y},{z}")


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed game and GPU rig")
class AsteroidFieldFighters(LiveSessionRunner, unittest.TestCase):
    def test_fighter_squadron_receives_an_ordinary_move_into_a_field(self):
        with tempfile.TemporaryDirectory(prefix="eawr-field-orders-") as temporary:
            directory = Path(temporary)
            replay = directory / "field.eawr-replay"
            map_name = "data/art/maps/_mp_space_polus.ted"
            code, initial = self._run(directory, "setup", (
                "--eawr-skirmish-players", "1,2", "--eawr-skirmish-slot", "1:Rebel:0:human",
                "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-fleet", "1:Rebel_X-Wing_Squadron",
                "--eawr-skirmish-fleet", "2:none", "--eawr-live-ai", "off", "--eawr-audio", "off",
                "--eawr-live-replay-out", str(replay), "--eawr-live-ticks", "1", "--eawr-live-step", "1",
                "--eawr-live-reveal", "on", "--eawr-lighting", "sh", "--eawr-environment", "map",
                "--eawr-shadows", "on"), session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
            self.assertEqual(code, 0, initial.get("failure"))
            squadron = next(row for row in initial["live_session"]["squadrons"] if row["owner"] == 1)
            field = next(row for row in initial["hud"]["minimap"]["hazards"] if row[4] & 1)
            x, y = field[:2]
            place_ship(replay, squadron["container"], x + 600, y, 0)
            for index, member in enumerate(squadron["members"]):
                place_ship(replay, member, x + 600, y + (index - 2) * 20, 0)
            camera = focused_camera(directory, initial, x + 100, y)
            code, report = self._run(directory, "move", (
                "--eawr-live-input", f"3:click:icon={squadron['container']}",
                "--eawr-live-input", f"6:rclick:@{x},{y},0",
                "--eawr-live-player", "1", "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                "--eawr-live-step", "1", "--eawr-live-ticks", "300", "--eawr-live-capture-ticks", "0,120,300",
                "--eawr-hud", "tactical", "--eawr-lighting", "sh", "--eawr-environment", "map",
                "--eawr-shadows", "on", "--eawr-audio", "off"),
                session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
                camera=camera, map_name=map_name)
            self.assertEqual(code, 0, report.get("failure"))
            battle, live = report["battle_input"], report["live_session"]
            self.assertEqual(battle["selected"], [squadron["container"]], battle["log"])
            self.assertEqual(battle["orders"], 1, battle["log"])
            self.assertTrue(any(row.startswith("move @") for row in battle["log"]), battle["log"])
            self.assertTrue(live["headless_hashes_equal"])
            for member in squadron["members"]:
                arrival = next(row["position"] for row in live["own_units"] if row["entity"] == member)
                self.assertLess(math.dist(arrival[:2], (x, y)), 500, (member, arrival))


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed game and GPU rig")
class AsteroidTraversal(LiveSessionRunner, unittest.TestCase):
    def test_class_paths_and_double_click_override(self):
        # WHZ-08a/10: private staging uses stock types and ordinary UI input.
        # Its field and ship positions match the original-game Lua observation.
        map_name = "data/art/maps/_mp_space_coruscant.ted"
        look = ("--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on")
        x, y = -2000, 2000
        for kind in ("Corellian_Corvette", "Nebulon_B_Frigate", "Rebel_X-Wing_Squadron"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory(prefix="eawr-field-traversal-") as temporary:
                directory = Path(temporary)
                replay = directory / "fresh.eawr-replay"
                code, initial = self._run(directory, "setup", (
                    "--eawr-skirmish-players", "1,2", "--eawr-skirmish-slot", "1:Rebel:0:human",
                    "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-fleet", f"1:{kind}",
                    "--eawr-skirmish-fleet", "2:none", "--eawr-live-ai", "off", "--eawr-audio", "off",
                    "--eawr-live-replay-out", str(replay), "--eawr-live-ticks", "1", "--eawr-live-step", "1",
                    "--eawr-live-reveal", "on", *look),
                    session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
                self.assertEqual(code, 0, initial.get("failure"))
                ship = next(row for row in initial["live_session"]["start_fleet"]
                            if row["player"] == 1 and row["type"] == kind)
                squadron = next((row for row in initial["live_session"]["squadrons"]
                                 if row["container"] == ship["entity"]), None)
                members = squadron["members"] if squadron else [ship["entity"]]
                rows = pad_capture.replay_units(replay)
                neutral = next(row[2] for row in rows if row[1] == zlib.crc32(b"MINERAL_EXTRACTOR_PAD"))
                data = bytearray(replay.read_bytes())
                header = struct.unpack_from("<H", data, 10)[0]
                players = struct.unpack_from("<I", data, 48)[0]
                count = struct.unpack_from("<Q", data, 56)[0]
                scale = 1 << 24
                end = header + players * 24 + count * 80
                field_id = max(row[0] for row in rows) + 1
                data[end:end] = struct.pack("<QQII3q4q", field_id, zlib.crc32(b"ASTEROID FIELD SMALL"), neutral, 0,
                                            x * scale, y * scale, 0, 0, 0, 0, scale)
                struct.pack_into("<Q", data, 56, count + 1)
                replay.write_bytes(data)
                fixture = replay.read_bytes()
                camera = focused_camera(directory, initial, x, y)
                # Keep both +/-900 endpoints on screen for real selection and world clicks.
                camera_config = ET.parse(camera)
                camera_config.getroot().find("initial").set("zoom", "0.95")
                camera_config.write(camera, encoding="utf-8", xml_declaration=True)
                for case in ("ordinary", "double", "inside"):
                    with self.subTest(case=case):
                        replay.write_bytes(fixture)
                        origin = -150 if case == "inside" else -900
                        target = 150 if case == "inside" else 900
                        for index, member in enumerate(members):
                            height = next(row[6] / scale for row in rows if row[0] == member)
                            place_ship(replay, member, x + origin, y + (index - (len(members)-1)/2) * 20, height)
                        if squadron:
                            place_ship(replay, ship["entity"], x + origin, y)
                        gesture = "rdclick" if case == "double" else "rclick"
                        selection = "icon" if squadron else "unit"
                        code, report = self._run(directory, case, (
                            "--eawr-live-input", f"3:click:{selection}={ship['entity']}",
                            "--eawr-live-input", f"6:{gesture}:@{x+target},{y},0",
                            "--eawr-live-player", "1", "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                            "--eawr-live-step", "1", "--eawr-live-ticks", "1050",
                            "--eawr-live-capture-ticks", ",".join(map(str, range(0, 1051, 10)))
                            if case != "inside" and kind != "Rebel_X-Wing_Squadron" else "0,120,240,360,480,600,1050",
                            "--eawr-hud", "tactical", "--eawr-audio", "off", *look),
                            session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
                            camera=camera, map_name=map_name)
                        self.assertEqual(code, 0, report.get("failure"))
                        battle = report["battle_input"]
                        self.assertEqual(battle["selected"], [ship["entity"]], battle["log"])
                        self.assertEqual(battle["orders"], 2 if case == "double" else 1, battle["log"])
                        live = report["live_session"]
                        self.assertTrue(live["headless_hashes_equal"])
                        for member in members:
                            arrival = next(row["position"] for row in live["own_units"] if row["entity"] == member)
                            self.assertLess(math.dist(arrival[:2], (x+target, y)), 200, (case, member, arrival))
                        impacts = [row for row in report["battle_effects"]["spawn_log"] if row[1].startswith("asteroid:")]
                        self.assertEqual(bool(impacts), kind == "Nebulon_B_Frigate" and case != "ordinary",
                                         (kind, case, impacts[:3]))


if __name__ == "__main__":
    unittest.main()
