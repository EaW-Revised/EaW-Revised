"""The retail benchmark must preserve native placement/orders and reject ambiguity."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("melee_staging", ROOT / "tools/performance/melee_staging.py")
staging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(staging)


def replay(*, attack=2, issuer=1, command_tick=0, squadron_members=(4,), version=3):
    # An asymmetric synthetic fixture makes ordering and coordinate mistakes visible.
    catalog = {"types": ["Ship_A", "Ship_B", "Team"], "factions": ["Side_A", "Side_B"]}
    pack = lambda fmt, *args: struct.pack("<" + fmt, *args)
    data = pack("8sHH5IQQIIQQ32s", b"EAWRPLY\0", version, 104, 1, 1, 24, 1, 30,
                601, 1, 2, 1, 4, 3, bytes(range(32)))
    for player, faction in ((1, "Side_A"), (2, "Side_B")):
        data += pack("IIQII", player, player, staging.type_id(faction), 1, 0)
    for entity, name, owner, x, y in ((1, "Ship_A", 1, -979, 1123),
                                       (2, "Ship_B", 2, 1007, -1160),
                                       (3, "Team", 1, -707, 87), (4, "Craft", 1, -715, 90)):
        data += pack("QQII7q", entity, staging.type_id(name), owner, 0,
                     x * staging.SCALE, y * staging.SCALE, 0, 0, 0, 0, staging.SCALE)
    data += pack("QII", 3, len(squadron_members), 0) + pack("Q" * len(squadron_members), *squadron_members)
    for owner, sequence, opcode, payload, ids in (
            (issuer, 1, 3, pack("Q", attack), (1,)),
            (1, 2, 6, pack("3qQ", 1000 * staging.SCALE, 0, 0, 0), (3,)),
            (2, 1, 3, pack("Q", 1), (2,))):
        body = pack("QIQBBH", command_tick, owner, sequence, opcode, 0, 0)
        body += payload + pack("II", len(ids), 0) + pack("Q" * len(ids), *ids)
        data += pack("I", len(body)) + body
    return data, catalog


class MeleeStagingTests(unittest.TestCase):
    def test_native_order_and_asymmetric_positions_are_preserved(self):
        blob, catalog = replay()
        fixture = staging.read_fixture(blob, catalog)
        self.assertEqual([u["id"] for u in fixture["units"]], [1, 2, 3])
        self.assertEqual(fixture["units"][0]["position"], [-979, 1123, 0])
        self.assertEqual(fixture["units"][1]["position"], [1007, -1160, 0])
        self.assertEqual(fixture["units"][2]["members"], [4])
        self.assertEqual(fixture["craft"][0]["position"], [-715, 90, 0])
        self.assertEqual(fixture["commands"][0]["target"], 2)
        self.assertEqual(fixture["commands"][1]["position"], [1000, 0, 0])
        self.assertEqual(fixture["players"][2]["faction"], "Side_B")

    def test_truncation_at_every_byte_is_rejected(self):
        blob, catalog = replay()
        for stop in range(len(blob)):
            with self.subTest(stop=stop), self.assertRaises(ValueError):
                staging.read_fixture(blob[:stop], catalog)

    def test_unsupported_or_ambiguous_staging_is_rejected(self):
        for changes in ({"attack": 1}, {"attack": 4}, {"issuer": 2},
                        {"command_tick": 1}, {"squadron_members": (4, 4)},
                        {"squadron_members": (2,)}, {"version": 5}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                staging.read_fixture(*replay(**changes))

    def test_missing_catalog_names_and_trailing_bytes_are_rejected(self):
        blob, catalog = replay()
        with self.assertRaises(ValueError):
            staging.read_fixture(blob + b"\0", catalog)
        with self.assertRaises(ValueError):
            staging.read_fixture(blob, {**catalog, "types": ["Ship_A", "Team"]})

    def test_native_fixture_catalog_resolves_roster_without_craft_names(self):
        catalog = staging.benchmark_catalog()
        self.assertGreater(len(catalog["types"]), 0)
        self.assertEqual(len(catalog["types"]), len(set(catalog["types"])))


if __name__ == "__main__":
    unittest.main()
