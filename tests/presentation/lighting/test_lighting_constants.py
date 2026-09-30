"""Contracts for plan/inventories/lighting-constants.json (P1-04).

The structural half runs everywhere. Given --scanner <lighting_constants_scan>
--game-root <install> [--mod-root <remake>], it regenerates the inventory
read-only and fails on any drift.
"""

import argparse
import json
import math
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
INVENTORY = ROOT / "plan/inventories/lighting-constants.json"
REFERENCE_MAPS = ROOT / "plan/inventories/map-reference-maps.json"
FORBIDDEN = re.compile(r"[A-Za-z]:[\\/]|SteamLibrary|steamapps|workshop", re.IGNORECASE)
ARGS = None


class Structure(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = INVENTORY.read_text(encoding="utf-8")
        cls.document = json.loads(cls.text)

    def test_no_host_paths(self):
        found = FORBIDDEN.search(self.text)
        self.assertIsNone(found, f"inventory leaks an installation path: {found}")

    def test_header_marks_the_mapping_unconfirmed(self):
        self.assertEqual(self.document["schema_version"], 1)
        mapping = self.document["candidate_mapping"]
        self.assertEqual(mapping["status"], "unconfirmed")
        self.assertIn("alo-viewer", mapping["basis"])
        self.assertIn("<GAME_ROOT>", self.document["generator"])

    def test_profiles(self):
        profiles = {entry["profile"]: entry for entry in self.document["profiles"]}
        self.assertIn("eaw", profiles)
        for name, entry in profiles.items():
            self.assertEqual(entry["view"], "effective")
            tags = {constant["tag"] for constant in entry["xml_constants"]}
            self.assertIn("Battle_Load_Planet_Ambient", tags, name)
            for constant in entry["xml_constants"]:
                self.assertTrue(constant["logical_path"].startswith("data/xml/"), constant)
                self.assertGreater(constant["line"], 0)
                self.assertTrue(constant["source_id"])
            environments = entry["map_environments"]
            self.assertGreater(environments["maps"], 0)
            self.assertLessEqual(environments["records_decoding_under_candidate"], environments["records"])
            ranges = environments["candidate_field_ranges"]
            # The ranges are what the candidate reading rests on.
            for key in ("0x08", "0x09", "0x0a"):
                self.assertGreaterEqual(ranges[key][0], 0.0)
                self.assertLess(ranges[key][1], 2 * math.pi + 1e-3)
            for key in ("0x0b", "0x0c", "0x0d"):
                self.assertGreaterEqual(ranges[key][0], -math.pi / 2 - 1e-3)
                self.assertLessEqual(ranges[key][1], math.pi / 2 + 1e-3)
            for key in ("0x05", "0x06", "0x07"):
                self.assertGreaterEqual(ranges[key][0], 0.0)
                self.assertLessEqual(ranges[key][1], 1.0)

    def test_pinned_maps_match_the_reference_pins(self):
        # constants_scan.cpp expands the Alderaan regression fixtures, not the
        # M1 reference maps, so compare against that role only.
        pins = {entry["logical_path"]: entry["sha256"]
                for entry in json.loads(REFERENCE_MAPS.read_text(encoding="utf-8"))["references"]
                if entry["role"] == "regression_fixture" and "alderaan" in entry["logical_path"]}
        eaw = next(entry for entry in self.document["profiles"] if entry["profile"] == "eaw")
        self.assertEqual({entry["logical_path"] for entry in eaw["pinned_maps"]}, set(pins))
        for entry in eaw["pinned_maps"]:
            self.assertEqual(entry["sha256"], pins[entry["logical_path"]])
            self.assertTrue(entry["environments"])
            for environment in entry["environments"]:
                candidate = environment["candidate"]
                self.assertIsNotNone(candidate)
                for light in candidate["lights"]:
                    self.assertAlmostEqual(math.hypot(*light["direction"]), 1.0, places=4)
                coefficients = candidate["sh_light_all_coefficients"]
                self.assertEqual([len(channel) for channel in coefficients], [9, 9, 9])


class Drift(unittest.TestCase):
    def test_regenerated_inventory_matches(self):
        if not ARGS or not ARGS.scanner or not ARGS.game_root:
            self.skipTest("pass --scanner and --game-root to re-measure against a read-only install")
        with tempfile.TemporaryDirectory(prefix="eawr-lighting-constants-") as temporary:
            report = pathlib.Path(temporary) / "lighting.json"
            command = [str(pathlib.Path(ARGS.scanner).resolve()), "--game-root", ARGS.game_root,
                       "--report", str(report)]
            if ARGS.mod_root:
                command[3:3] = ["--mod-root", ARGS.mod_root]
            completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, check=False)
            self.assertEqual(completed.returncode, 0, completed.stdout)
            self.assertEqual(report.read_bytes(), INVENTORY.read_bytes())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--scanner")
    parser.add_argument("--game-root")
    parser.add_argument("--mod-root")
    ARGS, rest = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *rest])
