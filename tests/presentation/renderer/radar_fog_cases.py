"""WNO-41/43/44: separate live and remembered radar identities under real fog."""

import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from live_session_test_support import LiveSessionRunner
from space_hazard_cases import focused_camera, place_ship
from test_pad_capture import replay_units


def damage_replay(source, destination, entity, tick):
    """HD-30 accepts damage from a commandable issuer even when the victim is Neutral."""
    data = bytearray(source.read_bytes())
    assert struct.unpack_from("<Q", data, 64)[0] == 0
    body = (struct.pack("<QIQBBH", tick, 1, 99, 4, 0, 0)
            + struct.pack("<qII", 1000000 << 24, 0xffffffff, 0)
            + struct.pack("<IIQ", 1, 0, entity))
    struct.pack_into("<Q", data, 64, 1)
    destination.write_bytes(data + struct.pack("<I", len(body)) + body)
    return destination


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires installed-game GPU lane")
class RadarFogGraphical(LiveSessionRunner, unittest.TestCase):
    common = ("--eawr-live-ai", "off", "--eawr-audio", "off", "--eawr-live-step", "3",
              "--eawr-live-player", "1", "--eawr-hud", "tactical",
              "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on")

    def setup_map(self, directory, map_name, slots=(), players="1,2"):
        replay = directory / "setup.eawr-replay"
        code, report = self._run(directory, "setup", (*self.common,
            "--eawr-skirmish-players", players,
            "--eawr-skirmish-slot", "1:Rebel:0:human", "--eawr-skirmish-slot", "2:Empire:1:ai",
            *slots,
            "--eawr-skirmish-fleet", "1:Corellian_Corvette", "--eawr-skirmish-fleet", "2:none",
            "--eawr-live-replay-out", str(replay), "--eawr-live-ticks", "3"),
            session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
        self.assertEqual(code, 0, report.get("failure"))
        ship = next(row["entity"] for row in report["live_session"]["start_fleet"]
                    if row["player"] == 1 and row["type"] == "Corellian_Corvette")
        data = bytearray(replay.read_bytes())
        struct.pack_into("<Q", data, 40, 2100)
        replay.write_bytes(data)
        return replay, report, ship

    def run_subject(self, directory, replay, initial, ship, subject, name, map_name, ticks, extra=()):
        x, y, z = (v / (1 << 24) for v in subject[4:7])
        camera = focused_camera(directory, initial, x, y, z)
        code, report = self._run(directory, name, (*self.common, "--eawr-live-ticks", str(ticks), *extra),
            session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
            camera=camera, map_name=map_name)
        self.assertEqual(code, 0, report.get("failure"))
        self.assertTrue(report["live_session"]["headless_hashes_equal"])
        self.assertEqual(report["live_session"]["rejected"], [])
        return report

    def test_asteroid_discovery_capture_retreat_and_destroyed_memory(self):
        for map_label in ("bothawui", "hoth"):
            with self.subTest(map=map_label), tempfile.TemporaryDirectory(prefix="eawr-radar-fog-") as temporary:
                directory = Path(temporary)
                map_name = f"data/art/maps/_mp_space_{map_label}.ted"
                replay, initial, ship = self.setup_map(directory, map_name)
                subject = next(row for row in replay_units(replay)
                               if row[1] == zlib.crc32(b"SKIRMISH_HUTT_ASTEROID_BASE"))
                x, y, z = (v / (1 << 24) for v in subject[4:7])
                place_ship(replay, ship, x + 5000, y, z)
                hidden = self.run_subject(directory, replay, initial, ship, subject, "undiscovered", map_name, 30)
                self.assertFalse(any(row["id"] == subject[0] for row in hidden["hud"]["minimap"]["drawn"]))
                place_ship(replay, ship, x + 900, y, z)
                seen = self.run_subject(directory, replay, initial, ship, subject, "discovered", map_name, 30)
                live = [row for row in seen["hud"]["minimap"]["drawn"] if row["id"] == subject[0]]
                self.assertEqual(len(live), 1, live)
                self.assertFalse(live[0]["remembered"])
                self.assertEqual(live[0]["colour"], [100,100,100,255])
                retreat = ("--eawr-live-order", f"30:move:{ship}@{x + 6000},{y},{z}")
                remembered = self.run_subject(directory, replay, initial, ship, subject, "remembered", map_name, 1830,
                    (*retreat, "--eawr-live-input", f"1800:hover:@{x},{y},{z}",
                     "--eawr-live-input", f"1806:click:@{x},{y},{z}"))
                memory = [row for row in remembered["hud"]["minimap"]["drawn"] if row["id"] == subject[0]]
                self.assertEqual(len(memory), 1, memory)
                self.assertTrue(memory[0]["remembered"])
                self.assertEqual(memory[0]["colour"], [100,100,100,255])
                self.assertNotIn(subject[0], {row["entity"] for key in ("own_units", "hostile_units")
                                             for row in remembered["live_session"][key]})
                self.assertTrue(any(row["entity"] == subject[0] and row["ghost"] and row["pieces"] > 0
                                    for row in remembered["live_session"]["fog_ghost_log"]))
                self.assertNotIn(subject[0], remembered["battle_input"]["selected"])
                self.assertEqual(remembered["battle_input"]["cursor_samples"][-1]["hover"], "empty")
                # Hidden death keeps observer knowledge until the stored point is revealed again.
                death = damage_replay(replay, directory / "hidden-death.eawr-replay", subject[0], 1815)
                destroyed = self.run_subject(directory, death, initial, ship, subject, "hidden-death", map_name, 1860,
                    retreat)
                self.assertTrue(any(row["id"] == subject[0] and row["remembered"]
                                    for row in destroyed["hud"]["minimap"]["drawn"]))
                # Corvette's 1000 reveal range has an authored 0.2 dense multiplier.
                # Keep the centre inside that 200 range: capture radar colour uses raw
                # centre fog even when multisampling admits the model on dense Hoth.
                place_ship(replay, ship, x + 100, y, z)
                captured = self.run_subject(directory, replay, initial, ship, subject, "captured", map_name, 720,
                    ("--eawr-live-capture-ticks", "30,300,720"))
                pad = next(row for row in captured["live_session"]["pads"] if row["entity"] == subject[0])
                self.assertEqual(pad["owner"], 1)
                mark = next(row for row in captured["hud"]["minimap"]["drawn"] if row["id"] == subject[0])
                self.assertFalse(mark["remembered"])
                self.assertNotEqual(mark["colour"], [100,100,100,255])
                death = damage_replay(replay, directory / "visible-death.eawr-replay", subject[0], 15)
                visible_death = self.run_subject(directory, death, initial, ship, subject, "visible-death", map_name, 30)
                self.assertFalse(any(row["id"] == subject[0] for row in visible_death["hud"]["minimap"]["drawn"]))

    def test_community_local_allied_enemy_colours_and_neutralization(self):
        with tempfile.TemporaryDirectory(prefix="eawr-radar-team-") as temporary:
            directory = Path(temporary)
            map_name = "data/art/maps/_mp_space_coruscant.ted"
            replay, initial, ship = self.setup_map(directory, map_name, (
                "--eawr-skirmish-slot", "2:Rebel:0:ai", "--eawr-skirmish-slot", "3:Empire:1:ai",
                "--eawr-skirmish-slot", "4:Empire:1:ai", "--eawr-skirmish-fleet", "3:none",
                "--eawr-skirmish-fleet", "4:none"), players="1,2,3,4")
            subject = next(row for row in replay_units(replay) if row[1] == zlib.crc32(b"SKIRMISH_MERCHANT_DOCK"))
            x, y, z = (v / (1 << 24) for v in subject[4:7])
            place_ship(replay, ship, x + 900, y, z)
            source = replay.read_bytes()
            header = struct.unpack_from("<H", source, 10)[0]
            players = struct.unpack_from("<I", source, 48)[0]
            count = struct.unpack_from("<Q", source, 56)[0]
            offset = next(header + players * 24 + index * 80 for index in range(count)
                          if struct.unpack_from("<Q", source, header + players * 24 + index * 80)[0] == subject[0])
            colours = {row["player"]: row["rgb"] + [255] for row in initial["live_session"]["start_colours"]}
            for owner, representative in ((1,1), (2,1), (4,3)):
                with self.subTest(owner=owner):
                    data = bytearray(source)
                    struct.pack_into("<I", data, offset + 16, owner)
                    replay.write_bytes(data)
                    result = self.run_subject(directory, replay, initial, ship, subject, f"team-{owner}", map_name, 30)
                    mark = next(row for row in result["hud"]["minimap"]["drawn"] if row["id"] == subject[0])
                    self.assertEqual(mark["colour"], colours[representative])
            # Enemy merchant ownership moves towards neutral while the local corvette contests it.
            place_ship(replay, ship, x + 500, y, z)
            transitioning = self.run_subject(directory, replay, initial, ship, subject, "neutralizing", map_name, 120)
            pad = next(row for row in transitioning["live_session"]["pads"] if row["entity"] == subject[0])
            progress = pad["progress_raw"] / (1 << 24)
            self.assertGreater(progress, 0)
            self.assertLess(progress, 1)
            mark = next(row for row in transitioning["hud"]["minimap"]["drawn"] if row["id"] == subject[0])
            expected = [round(a + (100 - a) * progress) for a in colours[3][:3]] + [255]
            self.assertTrue(all(abs(a-b) <= 2 for a,b in zip(mark["colour"], expected)), (mark, pad, expected))

    def test_merchant_sensor_and_hutt_owner_controls(self):
        for map_label, type_name in (("coruscant", "SKIRMISH_MERCHANT_DOCK"), ("hoth", "N_REMOTE_SENSOR_POD"),
                                     ("coruscant", "ORBITAL_RESOURCE_CONTAINER")):
            with self.subTest(type=type_name), tempfile.TemporaryDirectory(prefix="eawr-radar-control-") as temporary:
                directory = Path(temporary)
                map_name = f"data/art/maps/_mp_space_{map_label}.ted"
                replay, initial, ship = self.setup_map(directory, map_name)
                subject = next(row for row in replay_units(replay) if row[1] == zlib.crc32(type_name.encode()))
                x, y, z = (v / (1 << 24) for v in subject[4:7])
                place_ship(replay, ship, x + 300, y, z)
                result = self.run_subject(directory, replay, initial, ship, subject, type_name.lower(), map_name,
                                          3 if type_name == "ORBITAL_RESOURCE_CONTAINER" else 30)
                marks = [row for row in result["hud"]["minimap"]["drawn"] if row["id"] == subject[0]]
                self.assertEqual(len(marks), 1, marks)
                self.assertFalse(marks[0]["remembered"])
                if type_name == "ORBITAL_RESOURCE_CONTAINER":
                    self.assertEqual(marks[0]["colour"], [255,128,0,255])
                else:
                    # These stock controls have no fogged-radar flag: only their retained model admits them.
                    hidden = self.run_subject(directory, replay, initial, ship, subject,
                        type_name.lower() + "-remembered", map_name, 1800,
                        ("--eawr-live-order", f"30:move:{ship}@{x + 6000},{y},{z}"))
                    memories = [row for row in hidden["hud"]["minimap"]["drawn"] if row["id"] == subject[0]]
                    self.assertEqual(len(memories), 1, memories)
                    self.assertTrue(memories[0]["remembered"])
                    self.assertEqual(memories[0]["colour"], [100,100,100,255])
