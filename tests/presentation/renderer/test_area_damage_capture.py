"""G4 synthetic rocket witness; installed XML remains in temporary output only."""

import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from live_session_test_support import LiveSessionRunner, ROOT, DUEL_CAMERA
from capture_replay_cases import canonical_replay_header
sys.path.insert(0, str(ROOT / "tools"))
from megx import read_index


def installed_xml(game, filename):
    """Read one winning XML member, without exporting the retail corpus."""
    for layer in (game / "corruption" / "Data", game / "GameData" / "Data"):
        loose = layer / "XML" / filename
        if loose.is_file():
            return loose.read_bytes()
        manifest = layer / "MegaFiles.xml"
        if not manifest.is_file():
            continue
        for child in reversed(list(ET.fromstring(manifest.read_bytes()))):
            if child.tag != "File" or not child.text:
                continue
            relative = child.text.strip().replace("\\", "/")
            if relative.lower().startswith("data/"):
                relative = relative[5:]
            archive = layer / relative
            if not archive.is_file():
                continue
            for entry in read_index(archive):
                if entry.name.replace("\\", "/").lower() == "data/xml/" + filename.lower():
                    with archive.open("rb") as stream:
                        stream.seek(entry.offset)
                        return stream.read(entry.size)
    raise AssertionError("installed XML unavailable: " + filename)


def set_tag(node, tag, text):
    children = [child for child in node if child.tag.lower() == tag.lower()]
    if not children:
        children = [ET.SubElement(node, tag)]
    # Duplicate scalar tags use the last authored value. Update every occurrence.
    for child in children:
        child.text = str(text)


def rocket_overlay(game, root):
    data = root / "Data"
    xml = data / "XML"
    xml.mkdir(parents=True)
    # A loose-only overlay still requires a nonempty, valid archive manifest.
    # Keep its single project-authored member unrelated to the retail XML catalog.
    name = b"DATA/EAWR-WITNESS.TXT"
    payload = b"synthetic rocket witness\n"
    offset = 8 + 2 + len(name) + 20
    archive = (struct.pack("<IIH", 1, 1, len(name)) + name
               + struct.pack("<IIIII", 0x12345678, 0, len(payload), offset, 0) + payload)
    (data / "Witness.meg").write_bytes(archive)
    (data / "MegaFiles.xml").write_text(
        "<Mega_Files><File>Witness.meg</File></Mega_Files>", encoding="utf-8")
    ships = ET.fromstring(installed_xml(game, "SpaceUnitsCorvettes.xml"))
    tartan = next(node for node in ships if node.get("Name", "").lower() == "tartan_patrol_cruiser")
    for tag, value in {
        "HardPoints": "", "Projectile_Types": "Proj_Ship_Diamond_Boron_Missile",
        "Projectile_Fire_Recharge_Seconds": 0.2, "Projectile_Fire_Pulse_Count": 5,
        "Projectile_Fire_Pulse_Delay_Seconds": 1, "Targeting_Max_Attack_Distance": 2000,
        "Turret_Rotate_Extent_Degrees": 360, "Turret_Elevate_Extent_Degrees": 180,
    }.items():
        set_tag(tartan, tag, value)
    ET.ElementTree(ships).write(xml / "SpaceUnitsCorvettes.xml", encoding="utf-8", xml_declaration=True)
    projectiles = ET.fromstring(installed_xml(game, "Projectiles.xml"))
    rocket = next(node for node in projectiles if node.get("Name") == "Proj_Ship_Diamond_Boron_Missile")
    # Project-authored speed accelerates the witness; inherited damage/blast inputs remain retail.
    for tag, value in {"Max_Speed": 30, "Projectile_Rocket_Curve_Offset": 0}.items():
        set_tag(rocket, tag, value)
    ET.ElementTree(projectiles).write(xml / "Projectiles.xml", encoding="utf-8", xml_declaration=True)


def point_height_replay(recorded):
    # The witness retains two ships, with no squadrons or tagged setup metadata.
    # Read tables after the source header, then emit the canonical v2 header.
    header, size = canonical_replay_header(recorded, 2)
    struct.pack_into("<I", header, 52, 0)
    count = struct.unpack_from("<I", header, 48)[0]
    players = recorded[size:size + 24 * count]
    start = size + 24 * count
    unit_count = struct.unpack_from("<Q", header, 56)[0]
    units = {struct.unpack_from("<Q", recorded, start + i * 80)[0]:
             bytearray(recorded[start + i * 80:start + (i + 1) * 80]) for i in range(unit_count)}
    shooter, target = units[11], units[5]
    scale = 1 << 24
    struct.pack_into("<qqqqqqq", shooter, 24, -400 * scale, -1500 * scale, 0, 0, 0, 0, scale)
    struct.pack_into("<qqqqqqq", target, 24, 400 * scale, -1500 * scale, 160 * scale, 0, 0, 0, scale)
    owner = struct.unpack_from("<I", shooter, 16)[0]
    common = struct.pack("<QIQBBH", 0, owner, 1, 3, 0, 0)
    body = common + struct.pack("<QIIQ", 5, 1, 0, 11)
    struct.pack_into("<Q", header, 40, 120)
    struct.pack_into("<QQ", header, 56, 2, 1)
    return bytes(header) + players + target + shooter + struct.pack("<I", len(body)) + body


def container_death_replay(recorded):
    """Retain a stock container and remote ships, then request its ordinary death."""
    header, size = canonical_replay_header(recorded, 2)
    player_count = struct.unpack_from("<I", header, 48)[0]
    players = recorded[size:size + 24 * player_count]
    start = size + 24 * player_count
    count = struct.unpack_from("<Q", header, 56)[0]
    rows = [bytearray(recorded[start + index * 80:start + (index + 1) * 80]) for index in range(count)]
    container_type = zlib.crc32(b"ORBITAL_RESOURCE_CONTAINER")
    container = next(row for row in rows if struct.unpack_from("<Q", row, 8)[0] == container_type)
    ships = [next(row for row in rows if struct.unpack_from("<Q", row)[0] == entity) for entity in (5, 11)]
    scale = 1 << 24
    struct.pack_into("<qqq", container, 24, 0, -1500 * scale, 0)
    for ship, x in zip(ships, (-3000, 3000)):
        struct.pack_into("<qqq", ship, 24, x * scale, -1500 * scale, 0)
    retained = sorted([container, *ships], key=lambda row: struct.unpack_from("<Q", row)[0])
    entity = struct.unpack_from("<Q", container)[0]
    owner = struct.unpack_from("<I", ships[0], 16)[0]
    body = struct.pack("<QIQBBHqIIIIQ", 15, owner, 1, 4, 0, 0,
                       1000000 * scale, 0xffffffff, 0, 1, 0, entity)
    struct.pack_into("<I", header, 52, 0)
    struct.pack_into("<Q", header, 40, 90)
    struct.pack_into("<QQ", header, 56, 3, 1)
    return bytes(header) + players + b"".join(retained) + struct.pack("<I", len(body)) + body


class PointHeightReplayFormatTests(unittest.TestCase):
    def test_legacy_and_tagged_headers_produce_the_same_two_ship_witness(self):
        original = (ROOT / "tests/skirmish/fixtures/m2-start.eawr-replay").read_bytes()
        source_header, setup_start = canonical_replay_header(original, 3)
        self.assertEqual(struct.unpack_from("<HH", source_header, 8), (3, 104))
        player_count = struct.unpack_from("<I", original, 48)[0]
        players = original[setup_start:setup_start + 24 * player_count]
        expected = point_height_replay(original)
        for version in (2, 3, 4, 5):
            with self.subTest(version=version):
                header = bytearray(source_header)
                # A valid SPOL record makes the tagged header larger than 104.
                extension = struct.pack("<IHHI", 1, 1, 4, 1) if version >= 4 else b""
                struct.pack_into("<HH", header, 8, version, 104 + len(extension))
                tables = original[setup_start:]
                if version in (2, 4):
                    struct.pack_into("<I", header, 52, 0)
                    struct.pack_into("<Q", header, 64, 0)
                    unit_count = struct.unpack_from("<Q", header, 56)[0]
                    tables = tables[:24 * player_count + 80 * unit_count]
                witness = point_height_replay(bytes(header) + extension + tables)
                self.assertEqual(witness, expected)
                self.assertEqual(struct.unpack_from("<HH", witness, 8), (2, 104))
                self.assertEqual(witness[104:104 + len(players)], players)
                self.assertEqual(struct.unpack_from("<QQ", witness, 56), (2, 1))
                start = 104 + len(players)
                self.assertEqual(struct.unpack_from("<Q", witness, start)[0], 5)
                self.assertEqual(struct.unpack_from("<Q", witness, start + 80)[0], 11)
                self.assertEqual(struct.unpack_from("<qqq", witness, start + 24),
                                 (400 << 24, -1500 << 24, 160 << 24))
                self.assertEqual(struct.unpack_from("<qqq", witness, start + 104),
                                 (-400 << 24, -1500 << 24, 0))
                self.assertEqual(len(witness), start + 160 + 4 + 48)

    def test_container_witness_keeps_its_authored_owner_and_one_death_command(self):
        original = (ROOT / "tests/skirmish/fixtures/m2-start.eawr-replay").read_bytes()
        replay = container_death_replay(original)
        count = struct.unpack_from("<I", replay, 48)[0]
        start = 104 + 24 * count
        self.assertEqual(struct.unpack_from("<QQ", replay, 56), (3, 1))
        rows = [replay[start + index * 80:start + (index + 1) * 80] for index in range(3)]
        container = next(row for row in rows if struct.unpack_from("<Q", row, 8)[0] == zlib.crc32(b"ORBITAL_RESOURCE_CONTAINER"))
        self.assertEqual(struct.unpack_from("<qqq", container, 24), (0, -1500 << 24, 0))
        _, original_size = canonical_replay_header(original, 2)
        original_start = original_size + 24 * struct.unpack_from("<I", original, 48)[0]
        original_count = struct.unpack_from("<Q", original, 56)[0]
        authored = next(original[original_start + index * 80:original_start + (index + 1) * 80]
                        for index in range(original_count)
                        if struct.unpack_from("<Q", original, original_start + index * 80)[0]
                        == struct.unpack_from("<Q", container)[0])
        self.assertEqual(container[16:24], authored[16:24])
        self.assertEqual(struct.unpack_from("<B", replay, start + 240 + 4 + 20)[0], 4)
        command = start + 240
        self.assertEqual(struct.unpack_from("<I", replay, command)[0], 56)
        self.assertEqual(struct.unpack_from("<qIIIIQ", replay, command + 4 + 24),
                         (1000000 << 24, 0xffffffff, 0, 1, 0,
                          struct.unpack_from("<Q", container)[0]))


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed game and GPU viewer")
class AreaDamageCapture(LiveSessionRunner, unittest.TestCase):
    def test_container_death_keeps_one_payload_and_independent_visual_explosion(self):
        with tempfile.TemporaryDirectory(prefix="eawr-container-death-") as temporary:
            directory = Path(temporary)
            record = directory / "identity.eawr-replay"
            code, initial = self._run(directory, "container-identity", (
                "--eawr-live-ai", "off", "--eawr-live-ticks", "1",
                "--eawr-live-replay-out", str(record)))
            self.assertEqual(code, 0, initial.get("failure"))
            replay = directory / "container-death.eawr-replay"
            replay.write_bytes(container_death_replay(record.read_bytes()))
            ticks = (0, 15, 16, 18, 24, 30, 45, 60, 90)
            code, result = self._run(directory, "container-lit", (
                "--eawr-live-replay", str(replay), "--eawr-live-reveal", "on",
                "--eawr-live-step", "1", "--eawr-live-ticks", "90", "--eawr-live-player", "2",
                "--eawr-live-capture-ticks", ",".join(map(str, ticks)), "--eawr-hud", "off",
                "--eawr-audio", "on", "--eawr-environment", "map", "--eawr-lighting", "sh",
                "--eawr-shadows", "on"), session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertTrue(result["live_session"]["headless_hashes_equal"])
            effects = result["battle_effects"]
            self.assertIsNone(effects["failure"])
            main = [row for row in effects["spawn_log"] if row[1].startswith("lifetime_detonation:")]
            visual = [row for row in effects["spawn_log"] if row[1].startswith("death:")]
            self.assertEqual(len(main), 1, effects["spawn_log"])
            self.assertEqual(len(visual), 1, effects["spawn_log"])
            audio = result["battle_audio"]
            self.assertEqual(sum(count for key, count in audio["requested"].items()
                                 if key.startswith("death_projectile_detonation:")), 1, audio)
            self.assertEqual(audio["results"].get("death_projectile_detonation"), {"playing": 1}, audio)
            for tick in ticks:
                self.assertTrue((directory / f"container-lit_t{tick:04d}.png").is_file())

    def test_synthetic_rocket_height_and_terminal_blast(self):
        with tempfile.TemporaryDirectory(prefix="eawr-area-rocket-") as temporary:
            directory = Path(temporary)
            overlay = directory / "synthetic-mod"
            rocket_overlay(Path(os.environ["EAWR_EAW_GAME_ROOT"]), overlay)
            record = directory / "identity.eawr-replay"
            mod_args = ("--eawr-mod-root", str(overlay), "--eawr-audio", "off")
            code, result = self._run(directory, "identity", (*mod_args, "--eawr-live-ai", "off",
                "--eawr-live-ticks", "1", "--eawr-live-replay-out", str(record)))
            self.assertEqual(code, 0, result.get("failure"))
            replay = directory / "synthetic-rocket.eawr-replay"
            replay.write_bytes(point_height_replay(record.read_bytes()))
            ticks = (4, 8, 12, 16, 20, 24, 28, 40, 48, 56)
            code, result = self._run(directory, "rocket-lit", (*mod_args,
                "--eawr-live-replay", str(replay), "--eawr-live-reveal", "on", "--eawr-live-player", "2",
                "--eawr-live-step", "1", "--eawr-live-ticks", "120", "--eawr-environment", "map",
                "--eawr-lighting", "sh", "--eawr-live-capture-ticks", ",".join(map(str, ticks))),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertTrue(result["live_session"]["headless_hashes_equal"])
            effects = result["battle_effects"]
            self.assertIsNone(effects["failure"])
            pool = effects["projectile_models"]["Proj_Ship_Diamond_Boron_Missile"]
            self.assertGreater(pool["bindings"], 0, pool)
            self.assertEqual(pool["refused"], 0, pool)
            self.assertGreater(effects["projectile_models_drawn"], 0)
            self.assertTrue(any("DB_Missile_Explosion" in key and value > 0
                                for key, value in effects["spawned"].items()), effects["spawned"])
            for tick in ticks:
                self.assertTrue((directory / f"rocket-lit_t{tick:04d}.png").is_file())

    @unittest.skip("Broadside roster gate remains closed; full retail witness is U-07")
    def test_stock_broadside_rocket(self):
        pass

    @unittest.skip("Marauder roster gate remains closed; full retail witness is U-07")
    def test_stock_marauder_rocket(self):
        pass


if __name__ == "__main__":
    unittest.main()
