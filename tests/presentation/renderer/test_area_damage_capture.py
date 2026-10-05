"""G4 synthetic rocket witness; installed XML remains in temporary output only."""

import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from live_session_test_support import LiveSessionRunner, ROOT, DUEL_CAMERA
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
    header = bytearray(recorded[:104])
    version, size = struct.unpack_from("<HH", header, 8)
    assert version in (2, 3) and size == 104
    # The witness retains two ships and no squadron table. M2 records v3 when
    # squadrons are present; this reduced setup therefore uses canonical v2.
    struct.pack_into("<H", header, 8, 2)
    struct.pack_into("<I", header, 52, 0)
    count = struct.unpack_from("<I", header, 48)[0]
    players = recorded[104:104 + 24 * count]
    start = 104 + 24 * count
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


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the installed game and GPU viewer")
class AreaDamageCapture(LiveSessionRunner, unittest.TestCase):
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
