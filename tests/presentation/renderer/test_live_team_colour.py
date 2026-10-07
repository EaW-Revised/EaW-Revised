"""Same-faction teammates bind arrivals to models composed in their lobby colour."""

import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from live_session_test_support import LiveSessionRunner
from test_pad_capture import replay_units

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.inventory.corpus import Corpus


def command(tick, player, sequence, opcode, payload, units=()):
    body = struct.pack("<QIQBBH", tick, player, sequence, opcode, 0, 0) + payload
    body += struct.pack("<II", len(units), 0)
    body += b"".join(struct.pack("<Q", unit) for unit in units)
    return struct.pack("<I", len(body)) + body


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed game and GPU rig")
class TeamColourGraphical(LiveSessionRunner, unittest.TestCase):
    def test_teammates_purchase_same_hull_in_reverse_slot_order(self):
        with tempfile.TemporaryDirectory(prefix="eawr-team-colour-") as temporary:
            directory = Path(temporary)
            mod = directory / "menu"
            xml = mod / "Data/XML"
            xml.mkdir(parents=True)
            (mod / "Data/MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            # Keep the actual purchase/reinforcement path, with a short isolated build queue.
            for filename, name in (("starbases.xml", "Skirmish_Rebel_Star_Base_1"),
                                   ("spaceunitsfrigates.xml", "Nebulon_B_Frigate")):
                source = corpus.read_effective("foc", "data/xml/" + filename)
                self.assertIsNotNone(source)
                tree = ET.fromstring(source.data)
                obj = next(node for node in tree if node.attrib.get("Name") == name)
                changes = ({"Tactical_Buildable_Objects_Multiplayer": "Rebel, Nebulon_B_Frigate"}
                           if filename == "starbases.xml" else
                           {"Tactical_Build_Time_Seconds": "1", "Tactical_Build_Cost_Multiplayer": "1",
                            "Required_Star_Base_Level": "1"})
                for tag, value in changes.items():
                    for child in list(obj):
                        if child.tag == tag:
                            obj.remove(child)
                    ET.SubElement(obj, tag).text = value
                (xml / filename).write_bytes(ET.tostring(tree, encoding="utf-8"))
            common = ("--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-audio", "off",
                      "--eawr-live-reveal", "on", "--eawr-live-step", "1", "--eawr-live-player", "1",
                      "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
                      "--eawr-live-purchase-slots", "1")
            setup = ["--eawr-skirmish-players", "1,2,3,4"]
            for slot in ("1:Rebel:0:human", "2:Empire:1:human", "3:Rebel:0:human", "4:Empire:1:human"):
                setup += ["--eawr-skirmish-slot", slot]
            for player in (1, 2, 3, 4):
                setup += ["--eawr-skirmish-fleet", f"{player}:none"]
            replay = directory / "team.eawr-replay"
            code, initial = self._run(directory, "setup", (*common, *setup, "--eawr-live-ticks", "1",
                "--eawr-live-replay-out", str(replay)), session=("--eawr-live-session", "skirmish"), camera=None)
            self.assertEqual(code, 0, initial.get("failure"))
            station = next(unit for unit in replay_units(replay)
                           if unit[1] == zlib.crc32(b"SKIRMISH_REBEL_STAR_BASE_1"))
            x, y, z = station[4:7]
            hull = zlib.crc32(b"NEBULON_B_FRIGATE")
            commands = []
            # Player 3 arrives first, while player 1's unused model is first in the pool.
            for player, buy_tick, arrival_tick, offset in ((3, 10, 100, -600), (1, 50, 300, 600)):
                commands.append((buy_tick, command(buy_tick, player, 1, 9, struct.pack("<Q", hull), (station[0],))))
                point = struct.pack("<Q3q", hull, x + (600 << 24), y + (offset << 24), z)
                commands.append((arrival_tick, command(arrival_tick, player, 2, 11, point)))
            data = bytearray(replay.read_bytes())
            self.assertEqual(struct.unpack_from("<Q", data, 64)[0], 0)
            struct.pack_into("<Q", data, 40, 650)
            struct.pack_into("<Q", data, 64, len(commands))
            data += b"".join(row for _, row in sorted(commands))
            replay.write_bytes(data)
            code, report = self._run(directory, "purchases", (*common, "--eawr-live-ticks", "650"),
                session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)), camera=None)
            self.assertEqual(code, 0, report.get("failure"))
            live = report["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertTrue(live["headless_hashes_equal"])
            arrivals = [row for row in live["arrivals"] if row["type"] == hull]
            self.assertEqual({row["owner"] for row in arrivals}, {1, 3}, arrivals)
            colours = {row["player"]: row["rgb"] for row in live["start_colours"]}
            self.assertNotEqual(colours[1], colours[3])
            drawn = {row["entity"]: row for row in live["drawn_launch_colours"]}
            for arrival in arrivals:
                row = drawn[arrival["unit"]]
                self.assertEqual(row["player"], arrival["owner"])
                self.assertEqual(row["rgb"], colours[row["player"]], row)
                if "death_rgb" in row:
                    self.assertEqual(row["death_rgb"], colours[row["player"]], row)
            # Initial station hangar launches use the same binding path.
            for row in drawn.values():
                self.assertEqual(row["rgb"], colours[row["player"]], row)


if __name__ == "__main__":
    unittest.main()
