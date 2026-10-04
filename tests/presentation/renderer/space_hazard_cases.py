"""Opt-in hazard cases for the registered live-session driver (WHZ-14/70/71/72)."""

import os
from pathlib import Path
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

from live_session_test_support import LiveSessionRunner, ROOT


def focused_camera(directory, report, x, y, z=0):
    """Author a capture region around the subject; these are camera limits, not TED bounds."""
    config = ET.parse(ROOT / "apps/viewer/project/config/coruscant-duel-camera.xml")
    root = config.getroot()
    root.set("map_path", report["map"]["logical_path"])
    root.set("map_sha256", report["map"]["sha256"])
    root.find("bounds").attrib.update(min_x=str(x - 2000), max_x=str(x + 2000),
                                    min_y=str(y - 2000), max_y=str(y + 2000),
                                    source_id="hazard-feedback-camera-region")
    root.find("initial").attrib.update(target_x=str(x), target_y=str(y), target_height=str(z), zoom="0.45", yaw_degrees="0")
    root.find("bindings").set("path", str(ROOT / "apps/viewer/project/config/space-live-camera-bindings.json"))
    camera = directory / "feedback-camera.xml"
    config.write(camera, encoding="utf-8", xml_declaration=True)
    return camera


def place_ship(replay, entity, x, y, z=0):
    """Change only one setup pose; keep the newly recorded content binding and command stream."""
    data = bytearray(replay.read_bytes())
    assert data[:8] == b"EAWRPLY\0"
    version, header_size = struct.unpack_from("<HH", data, 8)
    assert version in (2, 3, 4, 5) and header_size >= 104
    players = struct.unpack_from("<I", data, 48)[0]
    units = struct.unpack_from("<Q", data, 56)[0]
    scale = 1 << struct.unpack_from("<I", data, 20)[0]
    for index in range(units):
        offset = header_size + players * 24 + index * 80
        if struct.unpack_from("<Q", data, offset)[0] == entity:
            struct.pack_into("<qqq", data, offset + 24, round(x * scale), round(y * scale), round(z * scale))
            # The movement case begins facing its eastward destination, so it exercises translation.
            struct.pack_into("<qqqq", data, offset + 48, 0, 0, 0, scale)
            replay.write_bytes(data)
            return
    raise AssertionError("the selected ship is absent from the freshly recorded setup")


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game data")
class SpaceHazardsGpu(LiveSessionRunner, unittest.TestCase):
    def _start(self, directory, map_name):
        replay = directory / "fresh.eawr-replay"
        code, report = self._run(directory, "map", (
            "--eawr-skirmish-players", "3,4", "--eawr-skirmish-slot", "3:Rebel:1:human",
            "--eawr-skirmish-slot", "4:Empire:0:ai", "--eawr-skirmish-fleet", "3:Nebulon_B_Frigate",
            "--eawr-skirmish-fleet", "4:none", "--eawr-skirmish-seed", "908",
            "--eawr-live-replay-out", str(replay), "--eawr-live-ai", "off", "--eawr-audio", "off",
            "--eawr-live-ticks", "8", "--eawr-live-step", "1", "--eawr-map-timed-frames", "10",
            "--eawr-live-reveal", "on", "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"),
            session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
        self.assertEqual(code, 0, report.get("failure"))
        self.assertTrue(report["live_session"]["headless_hashes_equal"])
        minimap = report["hud"]["minimap"]
        self.assertGreater(minimap["hazard_pixels"], 0, minimap)
        self.assertTrue(all(row[2] > 0 and row[3] > 0 for row in minimap["hazards"]), minimap)
        ship = next(row["entity"] for row in report["live_session"]["start_fleet"]
                    if row["player"] == 3 and row["type"].lower() == "nebulon_b_frigate")
        return replay, report, ship

    def test_nebula_blend_on_selected_map_keeps_headless_hashes(self):
        with tempfile.TemporaryDirectory(prefix="eawr-nebula-feedback-") as temporary:
            directory = Path(temporary)
            map_name = "data/art/maps/_mp_space_endor.ted"
            replay, report, ship = self._start(directory, map_name)
            nebula = next(row for row in report["hud"]["minimap"]["hazards"] if row[4] & 4)
            place_ship(replay, ship, *nebula[:2])
            camera = focused_camera(directory, report, *nebula[:2])
            code, result = self._run(directory, "nebula", (
                "--eawr-live-player", "3", "--eawr-live-reveal", "on", "--eawr-live-step", "1",
                "--eawr-live-ticks", "8", "--eawr-live-capture-ticks", "0,3,8", "--eawr-audio", "off",
                "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"),
                session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)), camera=camera, map_name=map_name)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertTrue(live["headless_hashes_equal"])
            blend = next(row["blend"] for row in live["nebula_blends"] if row["entity"] == ship)
            self.assertAlmostEqual(blend, 1.0, places=5)

    def test_asteroid_particles_on_real_field_keeps_headless_hashes(self):
        with tempfile.TemporaryDirectory(prefix="eawr-asteroid-feedback-") as temporary:
            directory = Path(temporary)
            map_name = "data/art/maps/_mp_space_bespin.ted"
            replay, report, ship = self._start(directory, map_name)
            field = next(row for row in report["hud"]["minimap"]["hazards"] if row[4] & 1)
            # This stock small field has a 300-unit damage radius. Stage near its south
            # edge, in front of the opaque rock for the south-facing capture camera.
            x, y = field[0], field[1] - 275
            place_ship(replay, ship, x, y)
            camera = focused_camera(directory, report, x, y)
            code, result = self._run(directory, "asteroid", (
                "--eawr-live-player", "3", "--eawr-live-reveal", "on", "--eawr-live-step", "1",
                "--eawr-live-ticks", "90", "--eawr-live-capture-ticks", "15,30,60,90",
                "--eawr-live-order", f"1:move:{ship}@{x+400},{y},0",
                "--eawr-audio", "on", "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"),
                session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)), camera=camera, map_name=map_name)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertTrue(result["live_session"]["headless_hashes_equal"])
            asteroid = [row for row in result["battle_effects"]["spawn_log"] if row[1].startswith("asteroid:")]
            self.assertTrue(asteroid, result["battle_effects"])
            sound = result["battle_audio"]
            self.assertTrue(any(key.startswith("asteroid:") and count > 0 for key, count in sound["requested"].items()), sound)
            self.assertGreater(sound["results"].get("asteroid", {}).get("playing", 0), 0, sound)


if __name__ == "__main__":
    unittest.main()
