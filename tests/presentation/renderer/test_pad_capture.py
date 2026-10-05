"""Mining-pad capture and construction through the selected-map setup and mouse input."""

import os
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))

from live_session_test_support import LiveSessionRunner
from space_hazard_cases import focused_camera, place_ship


def replay_units(path):
    data = path.read_bytes()
    header = struct.unpack_from("<H", data, 10)[0]
    players = struct.unpack_from("<I", data, 48)[0]
    count = struct.unpack_from("<Q", data, 56)[0]
    return [struct.unpack_from("<QQII3q4q", data, header + players * 24 + index * 80)
            for index in range(count)]


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed game and GPU rig")
class MiningPadCaptureGraphical(LiveSessionRunner, unittest.TestCase):
    def test_enemy_capture_structures_are_visible_without_empty_pad_flags(self):
        # #1490: WBP-35's empty-pad gate belongs to TACTICAL_BUILD_OBJECTS,
        # not ordinary CAPTURE_POINT secondary structures (WBP-01).
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-enemy-capture-") as temporary:
            directory = Path(temporary)
            replay = directory / "setup.eawr-replay"
            mod = directory / "menu"
            xml = mod / "Data/XML"
            xml.mkdir(parents=True)
            (mod / "Data/MegaFiles.xml").write_text(
                "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            source = corpus.read_effective("foc", "data/xml/gameconstants.xml")
            self.assertIsNotNone(source)
            tree = ET.fromstring(source.data)
            # A recorded match policy rebuilds the selected map's capture
            # profiles even after this private probe changes setup ownership.
            next(node for node in tree.iter() if node.tag == "MP_Default_Allow_Heroes").text = "no"
            (xml / "gameconstants.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
            map_name = "data/art/maps/_mp_space_coruscant.ted"
            common = ("--eawr-mod-root", str(mod), "--eawr-live-ai", "off",
                      "--eawr-live-player", "1", "--eawr-live-reveal", "on",
                      "--eawr-live-step", "1", "--eawr-audio", "off",
                      "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on")
            code, initial = self._run(directory, "setup", (*common,
                "--eawr-skirmish-slot", "1:Rebel:0:human", "--eawr-skirmish-slot", "2:Empire:1:ai",
                "--eawr-skirmish-fleet", "1:Corellian_Corvette", "--eawr-skirmish-fleet", "2:none",
                "--eawr-live-replay-out", str(replay), "--eawr-live-ticks", "1"),
                session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
            self.assertEqual(code, 0, initial.get("failure"))
            ship = next(row for row in initial["live_session"]["start_fleet"]
                        if row["player"] == 1 and row["type"] == "Corellian_Corvette")
            rows = replay_units(replay)
            source = replay.read_bytes()
            for type_name in ("N_GRAVITY_WELL_STATION", "SKIRMISH_MERCHANT_DOCK",
                              "MINERAL_EXTRACTOR_PAD"):
                subject = next(row for row in rows if row[1] == zlib.crc32(type_name.encode()))
                x, y, z = (value / (1 << 24) for value in subject[4:7])
                camera = focused_camera(directory, initial, x, y, z)
                for owner in (1, 2):
                    with self.subTest(type=type_name, owner=owner):
                        data = bytearray(source)
                        header = struct.unpack_from("<H", data, 10)[0]
                        players = struct.unpack_from("<I", data, 48)[0]
                        for index in range(len(rows)):
                            offset = header + 24 * players + 80 * index
                            if struct.unpack_from("<Q", data, offset)[0] == subject[0]:
                                struct.pack_into("<I", data, offset + 16, owner)
                        struct.pack_into("<Q", data, 40, 120)
                        replay.write_bytes(data)
                        # Beyond the capture radius: neither sample changes ownership.
                        place_ship(replay, ship["entity"], x + 2000, y, z)
                        name = f"{type_name.lower()}-{owner}"
                        code, result = self._run(directory, name, (*common, "--eawr-live-ticks", "120",
                            "--eawr-live-capture-ticks", ",".join(str(tick) for tick in range(10, 121, 10)),
                            "--eawr-hud", "off"),
                            session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
                            camera=camera, map_name=map_name)
                        self.assertEqual(code, 0, result.get("failure"))
                        live = result["live_session"]
                        self.assertTrue(live["headless_hashes_equal"])
                        self.assertEqual(live["rejected"], [])
                        pad = next(row for row in live["pads"] if row["entity"] == subject[0])
                        self.assertEqual(pad["owner"], owner)
                        self.assertEqual(pad["constructed"], 0)
                        visible = live["own_units"] if owner == 1 else live["hostile_units"]
                        self.assertIn(subject[0], {row["entity"] for row in visible})
                        self.assertTrue((directory / f"{name}.png").is_file())

    def test_squadron_craft_capture_with_a_noncollidable_container(self):
        with tempfile.TemporaryDirectory(prefix="eawr-unit-collidable-") as temporary:
            directory = Path(temporary)
            replay = directory / "squadron.eawr-replay"
            map_name = "data/art/maps/_mp_space_polus.ted"
            look = ("--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on")
            code, initial = self._run(directory, "setup", (
                "--eawr-skirmish-players", "1,2", "--eawr-skirmish-slot", "1:Rebel:0:human",
                "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-fleet", "1:A_Wing_Squadron",
                "--eawr-skirmish-fleet", "2:none", "--eawr-live-ai", "off", "--eawr-audio", "off",
                "--eawr-live-replay-out", str(replay), "--eawr-live-ticks", "1", "--eawr-live-step", "1",
                "--eawr-live-reveal", "on", *look),
                session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
            self.assertEqual(code, 0, initial.get("failure"))
            squadron = next(row for row in initial["live_session"]["start_fleet"]
                            if row["player"] == 1 and row["type"] == "A_Wing_Squadron")
            rows = replay_units(replay)
            scale = 1 << 24
            pad = min((row for row in rows if row[1] == zlib.crc32(b"MINERAL_EXTRACTOR_PAD")),
                      key=lambda row: math.dist(tuple(value / scale for value in row[4:6]),
                                                squadron["position"][:2]))
            x, y, z = (value / scale for value in pad[4:7])
            members = [row for row in rows if row[2] == 1
                       and row[1] in (zlib.crc32(b"A_WING_SQUADRON"), zlib.crc32(b"A-WING"))]
            self.assertGreater(len(members), 1, "the purchased squadron includes independently simulated craft")
            origin = squadron["position"]
            for row in members:
                px, py, pz = (value / scale for value in row[4:7])
                place_ship(replay, row[0], x + 100 + px - origin[0], y + py - origin[1], pz)
            # WBP-51: a live upgrade modifier has no capture presence. This private
            # staged probe makes the previous false admission visible: the enemy's
            # modifier used to contest the pad even with our craft beside it.
            data = bytearray(replay.read_bytes())
            header = struct.unpack_from("<H", data, 10)[0]
            players = struct.unpack_from("<I", data, 48)[0]
            count = struct.unpack_from("<Q", data, 56)[0]
            end = header + players * 24 + count * 80
            upgrade = struct.pack("<QQII3q4q", max(row[0] for row in rows) + 1,
                                  zlib.crc32(b"ES_INCREASED_SUPPLIES_L1_UPGRADE"), 2, 0,
                                  round(x * scale), round(y * scale), round(z * scale), 0, 0, 0, scale)
            data[end:end] = upgrade
            struct.pack_into("<Q", data, 56, count + 1)
            replay.write_bytes(data)
            camera = focused_camera(directory, initial, x, y, z)
            code, report = self._run(directory, "craft-capture", (
                "--eawr-live-ai", "off", "--eawr-live-reveal", "on", "--eawr-live-step", "1",
                "--eawr-live-input", f"350:click:@{x},{y},{z}",
                "--eawr-live-ticks", "390", "--eawr-live-capture-ticks", "0,300,390",
                "--eawr-live-player", "1", "--eawr-hud", "tactical", "--eawr-audio", "off", *look),
                session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
                camera=camera, map_name=map_name)
            self.assertEqual(code, 0, report.get("failure"))
            live = report["live_session"]
            self.assertTrue(live["headless_hashes_equal"])
            state = next(row for row in live["pads"] if row["entity"] == pad[0])
            self.assertEqual(state["owner"], 1, state)
            self.assertTrue(any(f"pad palette {pad[0]}" in row
                                for row in report["battle_input"]["log"]),
                            report["battle_input"]["log"])
            craft = [row for row in live["own_units"] if row["entity"] in {member[0] for member in members}
                     and row["entity"] != squadron["entity"]]
            self.assertTrue(craft, "craft remain live after capture")
            self.assertTrue(any(math.dist(row["position"][:2], (x, y)) < 550 for row in craft), craft)

    def approach_inputs(self, x, y, z):
        return ("--eawr-live-input", f"3:click:@{x + 600},{y},{z}",
                "--eawr-live-input", "5:key:M",
                "--eawr-live-input", f"6:rclick:@{x + 100},{y},{z}")

    def test_map_hazard_does_not_contest_capture_or_block_the_build_palette(self):
        with tempfile.TemporaryDirectory(prefix="eawr-pad-capture-") as temporary:
            directory = Path(temporary)
            replay = directory / "map.eawr-replay"
            map_name = "data/art/maps/_mp_space_polus.ted"
            code, initial = self._run(directory, "setup", (
                "--eawr-skirmish-players", "1,2", "--eawr-skirmish-slot", "1:Rebel:0:human",
                "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-fleet", "1:Corellian_Corvette",
                "--eawr-skirmish-fleet", "2:none", "--eawr-live-ai", "off", "--eawr-audio", "off",
                "--eawr-live-replay-out", str(replay), "--eawr-live-ticks", "1", "--eawr-live-step", "1",
                "--eawr-live-reveal", "on", "--eawr-lighting", "sh", "--eawr-environment", "map",
                "--eawr-shadows", "on"), session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
            self.assertEqual(code, 0, initial.get("failure"))
            ship = next(row for row in initial["live_session"]["start_fleet"]
                        if row["player"] == 1 and row["type"] == "Corellian_Corvette")
            scale = 1 << 24
            pad_type = zlib.crc32(b"MINERAL_EXTRACTOR_PAD")
            pads = [row for row in replay_units(replay) if row[1] == pad_type]
            fields = [row for row in initial["hud"]["minimap"]["hazards"] if row[4] & 1]
            # Keep every object from the real setup, including the overlapping neutral field.
            pads = [row for row in pads if any((row[4] / scale - field[0]) ** 2
                    + (row[5] / scale - field[1]) ** 2 < 550 ** 2 for field in fields)]
            self.assertTrue(pads, "the stock map must exercise a mining pad beside an asteroid field")
            pad = min(pads, key=lambda row: (row[4] / scale - ship["position"][0]) ** 2
                      + (row[5] / scale - ship["position"][1]) ** 2)
            x, y, z = (value / scale for value in pad[4:7])
            # Stage only the approach pose; movement, capture and the purchase use ordinary input.
            place_ship(replay, ship["entity"], x + 600, y, z)
            camera = focused_camera(directory, initial, x, y, z)
            gestures = (
                *self.approach_inputs(x, y, z),
                "--eawr-live-input", f"600:click:@{x},{y},{z}",
                "--eawr-live-input", "610:click:card=0")
            balances = []
            for tick in (1800, 1890):
                code, report = self._run(directory, f"capture-{tick}", (
                    *gestures, "--eawr-live-player", "1", "--eawr-live-ai", "off",
                    "--eawr-live-reveal", "on", "--eawr-live-step", "1", "--eawr-live-ticks", str(tick),
                    "--eawr-live-capture-ticks", "0,450,620,1800", "--eawr-hud", "tactical",
                    "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
                    "--eawr-audio", "off"), session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
                    camera=camera, map_name=map_name)
                self.assertEqual(code, 0, report.get("failure"))
                live = report["live_session"]
                self.assertTrue(live["headless_hashes_equal"])
                self.assertEqual(report["battle_input"]["orders"], 1, report["battle_input"]["log"])
                approach = next(row["position"] for row in live["own_units"] if row["entity"] == ship["entity"])
                self.assertLess(math.dist(approach[:2], (x, y)), 550, approach)
                state = next(row for row in live["pads"] if row["entity"] == pad[0])
                self.assertEqual(state["owner"], 1, state)
                self.assertGreater(state["constructed"], 0, state)
                self.assertEqual(state["under_construction"], 0, state)
                log = report["battle_input"]["log"]
                self.assertTrue(any(f"pad palette {pad[0]}" in row for row in log), log)
                self.assertTrue(any("pad build 0: dispatched" in row for row in log), log)
                balances.append(float(live["economy"]["credits"]))
            # A completed stock mine contributes 60 credits over these 90 frames,
            # in addition to the untouched starbase's ordinary income.
            self.assertGreaterEqual(balances[1] - balances[0], 59, balances)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed game and GPU lane")
class CaptureColorizationGraphical(LiveSessionRunner, unittest.TestCase):
    def test_capture_preserves_unmasked_material_regions(self):
        from PIL import Image, ImageChops

        with tempfile.TemporaryDirectory(prefix="eawr-capture-colour-") as temporary:
            directory = Path(temporary)
            replay = directory / "setup.eawr-replay"
            map_name = "data/art/maps/_mp_space_bespin.ted"
            look = ("--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on")
            code, initial = self._run(directory, "setup", (
                "--eawr-skirmish-players", "1,2", "--eawr-skirmish-slot", "1:Rebel:0:human",
                "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-fleet", "1:Corellian_Corvette",
                "--eawr-skirmish-fleet", "2:none", "--eawr-live-ai", "off", "--eawr-audio", "off",
                "--eawr-live-replay-out", str(replay), "--eawr-live-ticks", "1", "--eawr-live-step", "1",
                "--eawr-live-reveal", "on", *look),
                session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
            self.assertEqual(code, 0, initial.get("failure"))
            ship = next(row for row in initial["live_session"]["start_fleet"]
                        if row["player"] == 1 and row["type"] == "Corellian_Corvette")
            rows = replay_units(replay)
            source = replay.read_bytes()
            for name, type_name, radius, ticks, unchanged_min in (
                    ("asteroid", "MINERAL_EXTRACTOR_PAD", 550, 340, 0.8),
                    ("merchant", "SKIRMISH_MERCHANT_DOCK", 800, 490, 0.4)):
                with self.subTest(structure=name):
                    subject = min((row for row in rows if row[1] == zlib.crc32(type_name.encode())),
                                  key=lambda row: (row[4] / (1 << 24) - ship["position"][0]) ** 2
                                  + (row[5] / (1 << 24) - ship["position"][1]) ** 2)
                    x, y, z = (value / (1 << 24) for value in subject[4:7])
                    camera = focused_camera(directory, initial, x, y, z)
                    images = []
                    self.assertNotEqual(subject[2], 1, "the map subject starts neutral")
                    for state, offset, owner in (("neutral", 1600, subject[2]), ("captured", radius - 50, 1)):
                        replay.write_bytes(source)
                        place_ship(replay, ship["entity"], x + offset, y, z)
                        label = f"{name}-{state}"
                        code, report = self._run(directory, label, (
                            "--eawr-live-ai", "off", "--eawr-live-player", "1", "--eawr-live-reveal", "on",
                            "--eawr-live-step", "10", "--eawr-live-ticks", str(ticks), "--eawr-audio", "off",
                            "--eawr-live-capture-ticks", f"0,{ticks // 20 * 10},{ticks - 20},{ticks}",
                            "--eawr-hud", "off", *look),
                            session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
                            camera=camera, map_name=map_name)
                        self.assertEqual(code, 0, report.get("failure"))
                        self.assertTrue(report["live_session"]["headless_hashes_equal"])
                        colour = next(row["rgb"] for row in report["live_session"]["start_colours"]
                                      if row["player"] == 1)
                        self.assertNotEqual(colour, [255, 255, 255], "the probe needs a nonwhite capture owner")
                        pad = next(row for row in report["live_session"]["pads"] if row["entity"] == subject[0])
                        self.assertEqual(pad["owner"], owner, pad)
                        self.assertEqual(pad["constructed"], 0, pad)
                        images.append(Image.open(directory / f"{label}.png").convert("RGB"))
                    # WBP-52: the central body retains its material colour while masked trim can change.
                    # Keep the subject centred and the influence ship outside this sample.
                    crop = (590, 310, 690, 410)
                    delta = ImageChops.difference(images[0].crop(crop), images[1].crop(crop))
                    unchanged = sum(max(pixel) <= 3 for pixel in delta.getdata()) / 10000
                    (directory / f"{name}-comparison.json").write_text(json.dumps({
                        "crop": crop, "unchanged_fraction": unchanged, "minimum": unchanged_min}))
                    self.assertGreater(unchanged, unchanged_min, "ownership recoloured the unmasked central body")
                    if name == "asteroid":
                        code, built = self._run(directory, "asteroid-built", (
                            "--eawr-live-ai", "off", "--eawr-live-player", "1", "--eawr-live-reveal", "on",
                            "--eawr-live-step", "10", "--eawr-live-ticks", "1800", "--eawr-audio", "off",
                            "--eawr-live-input", f"350:click:@{x},{y},{z}",
                            "--eawr-live-input", "360:click:card=0", "--eawr-live-capture-ticks", "350,370,1800",
                            "--eawr-hud", "tactical", *look),
                            session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
                            camera=camera, map_name=map_name)
                        self.assertEqual(code, 0, built.get("failure"))
                        self.assertTrue(built["live_session"]["headless_hashes_equal"])
                        pad = next(row for row in built["live_session"]["pads"] if row["entity"] == subject[0])
                        self.assertEqual(pad["owner"], 1, pad)
                        self.assertGreater(pad["constructed"], 0, pad)
                        self.assertEqual(pad["under_construction"], 0, pad)


if __name__ == "__main__":
    unittest.main()
