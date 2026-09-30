"""Contracts for the P1-11 all-map unresolved-placement inventories.

plan/inventories/unresolved-placements.json is the EaW effective view and
plan/inventories/unresolved-placements-foc.json the FoC one (expansion over
base). The structural half runs everywhere and needs no corpus: it checks that
each committed inventory is internally consistent, names no host path, and that
the scene builder's legacy selector table is the one the coverage manifest
lists.

Given --scanner <scene_scan executable>, an asset-free layered fixture checks
how the FoC mount chooses maps, objects, models and XML identity. Given
--game-root <install> as well, it regenerates both inventories read-only and
fails on any drift, which is how the committed input hashes are shown to match
a private corpus.
"""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
INVENTORIES = {
    "eaw": ROOT / "plan/inventories/unresolved-placements.json",
    "foc": ROOT / "plan/inventories/unresolved-placements-foc.json",
}
VIEW_NAMES = {"eaw": "EaW", "foc": "FoC"}
COVERAGE = ROOT / "plan/inventories/godot-material-coverage.json"
SCENE_SOURCE = ROOT / "src/scene/scene.cpp"
SCENE_SOURCES = [ROOT / "src/scene" / name for name in (
    "scene.cpp", "scene_assets.cpp", "scene_build.cpp", "scene_evidence.cpp", "scene_internal.hpp")]
SCENE_HEADER = ROOT / "include/eawr/scene/scene.hpp"

FORBIDDEN = re.compile(r"[A-Za-z]:[\\/]|SteamLibrary|steamapps|workshop", re.IGNORECASE)
HEX64 = re.compile(r"^[0-9a-f]{64}$")
RANGES = re.compile(r"^\d+(-\d+)?(,\d+(-\d+)?)*$")

SCANNER = None
GAME_ROOT = None


def ordinal_count(text: str) -> int:
    total = 0
    for part in text.split(","):
        if "-" in part:
            low, high = (int(value) for value in part.split("-"))
            assert high > low, part
            total += high - low + 1
        else:
            total += 1
    return total


sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
import scene_fixture  # noqa: E402


def run_scanner(*arguments):
    return subprocess.run([str(pathlib.Path(SCANNER).resolve()), *arguments],
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)


class InventoryStructure:
    """Mixed into one TestCase per profile; PROFILE selects the inventory."""

    PROFILE = ""

    @classmethod
    def setUpClass(cls):
        cls.text = INVENTORIES[cls.PROFILE].read_text(encoding="utf-8")
        cls.document = json.loads(cls.text)

    def test_no_host_paths(self):
        found = FORBIDDEN.search(self.text)
        self.assertIsNone(found, f"inventory leaks an installation path: {found}")

    def test_header(self):
        document = self.document
        self.assertEqual(document["schema_version"], 2)
        self.assertEqual(document["scene_contract_version"], 1)
        self.assertEqual(document["profile"], self.PROFILE)
        self.assertEqual(document["view"], "effective")
        self.assertEqual(document["generator"],
                         f"scene_scan --profile {self.PROFILE} --game-root <GAME_ROOT> --all")
        self.assertIn(f"every TED map in the {VIEW_NAMES[self.PROFILE]} effective VFS view",
                      document["provenance"])
        self.assertIn(f"effective {VIEW_NAMES[self.PROFILE]} VFS", document["inputs"]["catalog_input_scope"])
        self.assertRegex(document["inputs"]["catalog_sha256"], HEX64)
        self.assertRegex(document["inputs"]["catalog_input_sha256"], HEX64)
        self.assertIn("active registry roots", document["inputs"]["catalog_input_scope"])
        self.assertGreater(document["inputs"]["catalog_game_object_winners"], 0)
        roots = document["inputs"]["unloaded_registry_roots"]
        missing = document["inputs"]["unloaded_registry_includes"]
        self.assertEqual(document["inputs"]["catalog_provisional"], bool(roots or missing))
        self.assertEqual(len(roots), len({item["logical_path"] for item in roots}))
        self.assertTrue(all(item["logical_path"].startswith("data/xml/") and item["outcome"] != "loaded"
                            for item in roots), roots)
        self.assertEqual(len(missing), len(set(missing)))
        self.assertTrue(all(path.startswith(("data/xml/", "invalid-logical-path:")) for path in missing), missing)

    def test_every_map_is_attempted_and_totals_add_up(self):
        document = self.document
        maps = document["maps"]
        totals = document["totals"]
        self.assertEqual(totals["maps_attempted"], len(maps))
        paths = [entry["logical_path"] for entry in maps]
        self.assertEqual(paths, sorted(paths), "maps are listed in logical-path order")
        self.assertEqual(len(set(paths)), len(paths))
        built = [entry for entry in maps if entry["status"] == "built"]
        self.assertEqual(totals["maps_built"], len(built))
        for entry in maps:
            self.assertTrue(entry["logical_path"].startswith("data/art/maps/"), entry["logical_path"])
            self.assertTrue(entry["logical_path"].endswith(".ted"))
            self.assertRegex(entry["sha256"], HEX64)
            if entry["status"] != "built":
                self.assertTrue(entry["failure"], entry["logical_path"])
        for field in ("placements", "resolved", "drawable", "unresolved"):
            self.assertEqual(totals[field], sum(entry[field] for entry in built), field)
        self.assertEqual(totals["placements"], totals["resolved"] + totals["unresolved"])
        causes = set(totals["placements_by_cause"])
        for cause in causes:
            self.assertEqual(totals["placements_by_cause"][cause],
                             sum(entry["placements_by_cause"][cause] for entry in built), cause)
            self.assertEqual(totals["maps_with_cause"][cause],
                             sum(1 for entry in built if entry["placements_by_cause"][cause]), cause)

    def test_every_unresolved_placement_is_named(self):
        blocking = set(self.document["blocking_causes"])
        for entry in self.document["maps"]:
            if entry["status"] != "built":
                continue
            self.assertLessEqual(entry["drawable"], entry["placements"])
            self.assertRegex(entry["scene_sha256"], HEX64)
            ordinals_by_cause = {}
            unresolved = set()
            for group in entry["unresolved_groups"]:
                self.assertIn(group["cause"], entry["placements_by_cause"])
                self.assertTrue(group["object_id"] or group["cause"] == "crc_absent", group)
                self.assertRegex(group["record_ordinals"], RANGES)
                self.assertEqual(ordinal_count(group["record_ordinals"]), group["count"], group)
                expanded = set()
                for part in group["record_ordinals"].split(","):
                    low, _, high = part.partition("-")
                    expanded.update(range(int(low), int(high or low) + 1))
                for ordinal in expanded:
                    self.assertLess(ordinal, entry["placements"])
                ordinals_by_cause.setdefault(group["cause"], set()).update(expanded)
                unresolved.update(expanded)
                if group["cause"] in {"shader_unsupported", "texture_unresolved", "effect_unresolved",
                                      "model_not_in_vfs", "crc_missing", "crc_collision"}:
                    self.assertTrue(group["detail"], group)
            for cause, count in entry["placements_by_cause"].items():
                self.assertEqual(len(ordinals_by_cause.get(cause, set())), count,
                                 f"{entry['logical_path']} {cause}")
            self.assertEqual(len(unresolved), entry["unresolved"], entry["logical_path"])
            blocked = set()
            for cause in blocking:
                blocked |= ordinals_by_cause.get(cause, set())
            self.assertLessEqual(entry["drawable"], entry["placements"] - len(blocked))


class EawInventoryStructure(InventoryStructure, unittest.TestCase):
    PROFILE = "eaw"


class FocInventoryStructure(InventoryStructure, unittest.TestCase):
    PROFILE = "foc"


class SelectorTable(unittest.TestCase):
    def test_scene_selectors_match_the_coverage_manifest(self):
        source = SCENE_SOURCE.read_text(encoding="utf-8")
        table = source[source.index("selector_table{{"):source.index("}};", source.index("selector_table{{"))]
        rows = re.findall(r'\{"([^"]+)", "([^"]+)", "([^"]+)", (true|false)(?:, (?:true|false))?\}', table)
        entries = [row[:3] for row in rows]
        coverage = json.loads(COVERAGE.read_text(encoding="utf-8"))
        manifest = [(entry["program"], entry["technique"], entry["pass_name"])
                    for entry in coverage["routes"]["legacy_effect"]["selectors"]]
        self.assertEqual(entries, manifest,
                         "a selector added to the scene table must be added to the renderer and manifest")
        passes = [entry.get("render_pass", "opaque")
                  for entry in coverage["routes"]["legacy_effect"]["selectors"]]
        self.assertEqual([("transparent" if row[3] == "true" else "opaque") for row in rows], passes)
        renderer = "".join((ROOT / "src/presentation/godot" / name).read_text(encoding="utf-8") for name in (
            "renderer.cpp", "renderer_fog.cpp", "renderer_instances.cpp", "renderer_upload.cpp"))
        contract = (ROOT / "src/presentation/godot/renderer_contract.cpp").read_text(encoding="utf-8")
        # A family with its own adapter file is reached through legacy/registry.hpp.
        registry = (ROOT / "src/presentation/godot/legacy/registry.hpp").read_text(encoding="utf-8")
        families = {}
        for path in (ROOT / "src/presentation/godot/legacy").glob("*.hpp"):
            for found in re.finditer(r'inline constexpr Family (\w+)\{\s*\.program = "([^"]+)",\s*'
                                     r'\.technique = "([^"]+)",\s*\.pass_name = "([^"]+)"',
                                     path.read_text(encoding="utf-8")):
                if f"{path.stem}::{found.group(1)}" in registry:
                    families[found.group(2)] = found.groups()[1:]
        for program, technique, pass_name in manifest:
            if program in families:
                self.assertEqual(families[program], (program, technique, pass_name), program)
                continue
            self.assertIn(f'"{program}"', renderer, program)
            self.assertIn(f'"{program}"', contract, program)
        count = re.search(r"std::array<LegacySelector, (\d+)> selector_table", source)
        self.assertEqual(int(count.group(1)), len(manifest))

    def test_scene_module_is_engine_free_and_sim_facing_only_through_ids(self):
        for path in (*SCENE_SOURCES, SCENE_HEADER):
            text = path.read_text(encoding="utf-8")
            for token in ("godot_cpp", "#include <godot", "RenderingServer", "#include <fstream>",
                          "std::filesystem", "unordered_map", "unordered_set"):
                self.assertFalse(token in text, f"{token} in {path.name}")
        for path in (ROOT / "include/eawr/sim").rglob("*.hpp"):
            self.assertNotIn("eawr/scene", path.read_text(encoding="utf-8"), path.name)
        for path in (ROOT / "src/sim").rglob("*.cpp"):
            self.assertNotIn("eawr/scene", path.read_text(encoding="utf-8"), path.name)


class InventoryDrift(unittest.TestCase):
    def regenerate(self, profile):
        if not SCANNER or not GAME_ROOT:
            self.skipTest("pass --scanner and --game-root to re-measure against a read-only install")
        with tempfile.TemporaryDirectory(prefix="eawr-scene-scan-") as temporary:
            report = pathlib.Path(temporary) / "unresolved.json"
            completed = run_scanner("--profile", profile, "--game-root", GAME_ROOT, "--all", "--report", str(report))
            self.assertEqual(completed.returncode, 0, completed.stdout)
            self.assertEqual(report.read_bytes(), INVENTORIES[profile].read_bytes(),
                             f"the committed {profile} inventory drifted from the scanner's output")

    def test_regenerated_eaw_inventory_matches(self):
        self.regenerate("eaw")

    def test_regenerated_foc_inventory_matches(self):
        self.regenerate("foc")


class CatalogInputIdentity(unittest.TestCase):
    def test_changed_xml_value_changes_input_identity_without_moving_winner(self):
        if not SCANNER:
            self.skipTest("pass --scanner to exercise the native scanner")
        with tempfile.TemporaryDirectory(prefix="eawr-scene-input-") as temporary:
            root = pathlib.Path(temporary)
            xml = root / "GameData" / "Data" / "XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
            registries = {
                "GameObjectFiles.xml": "<Game_Object_Files><File>objects.xml</File></Game_Object_Files>",
                "HardpointDataFiles.xml": "<Hard_Point_Files></Hard_Point_Files>",
                "FactionFiles.xml": "<Faction_Files></Faction_Files>",
                "CampaignFiles.xml": "<Campaign_Files></Campaign_Files>",
                "SFXEventFiles.xml": "<SFXEvent_Files></SFXEvent_Files>",
            }
            for name, content in registries.items():
                (xml / name).write_text(content, encoding="utf-8")
            object_file = xml / "objects.xml"

            def scan(model: str, label: str):
                object_file.write_text(
                    "<Objects><GroundBuildable Name=\"TEST\"><Land_Model_Name>"
                    + model + "</Land_Model_Name></GroundBuildable></Objects>", encoding="utf-8")
                report = root / (label + ".json")
                completed = subprocess.run(
                    [str(pathlib.Path(SCANNER).resolve()), "--profile", "eaw",
                     "--game-root", str(root), "--all", "--report", str(report)],
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                self.assertEqual(completed.returncode, 0, completed.stdout)
                return json.loads(report.read_text(encoding="utf-8"))["inputs"]

            before = scan("a.alo", "before")
            after = scan("b.alo", "after")
            self.assertEqual(before["catalog_sha256"], after["catalog_sha256"])
            self.assertNotEqual(before["catalog_input_sha256"], after["catalog_input_sha256"])
            (xml / "SFXEventFiles.xml").unlink()
            missing_root = scan("b.alo", "missing-root")
            self.assertTrue(missing_root["catalog_provisional"])
            self.assertIn({"logical_path": "data/xml/sfxeventfiles.xml", "outcome": "missing"},
                          missing_root["unloaded_registry_roots"])
            (xml / "GameObjectFiles.xml").write_text(
                "<Game_Object_Files><File>objects.xml</File>"
                "<File>C:/Users/PrivateUser/private.xml</File></Game_Object_Files>", encoding="utf-8")
            unsafe = scan("b.alo", "unsafe")
            self.assertTrue(any(path.startswith("invalid-logical-path:")
                                for path in unsafe["unloaded_registry_includes"]))
            self.assertNotIn("PrivateUser", json.dumps(unsafe))


# An asset-free shared install: GameData/Data is the base layer and
# corruption/Data the expansion layer. Object and file names carry an
# EAWR_LAYER prefix so no value can be mistaken for game content.
SHADED = [("MeshGloss.fx", "eawr_layer_red.tga", 20.0, 20.0, 40.0)]
REGISTRIES = {
    "HardpointDataFiles.xml": "<Hard_Point_Files></Hard_Point_Files>",
    "FactionFiles.xml": "<Faction_Files></Faction_Files>",
    "CampaignFiles.xml": "<Campaign_Files></Campaign_Files>",
    "SFXEventFiles.xml": "<SFXEvent_Files></SFXEvent_Files>",
}


def placements(*names):
    return [(name, (100.0 + 60.0 * index, 100.0, 0.0), (0.0, 0.0, 0.0)) for index, name in enumerate(names)]


def objects_xml(entries):
    body = "".join(f'<GroundBuildable Name="{name}"><Land_Model_Name>{model}</Land_Model_Name>'
                   f"<Scale_Factor>{scale}</Scale_Factor></GroundBuildable>"
                   for name, model, scale in entries)
    return f"<GroundStructures>{body}</GroundStructures>"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class LayeredFixture:
    BASE_TED = scene_fixture.ted_bytes(placements("EAWR_LAYER_BASE_OBJECT"))
    BASE_SHARED_TED = scene_fixture.ted_bytes(placements("EAWR_LAYER_SHARED"))
    EXPANSION_SHARED_TED = scene_fixture.ted_bytes(placements("EAWR_LAYER_SHARED", "EAWR_LAYER_BASE_OBJECT"))
    EXPANSION_TED = scene_fixture.ted_bytes(placements("EAWR_LAYER_SHARED", "EAWR_LAYER_SHARED"))

    def __init__(self, root: pathlib.Path, layers=("GameData", "corruption")):
        self.root = root
        self.base = root / "GameData" / "Data"
        self.expansion = root / "corruption" / "Data"
        for data in (root / layer / "Data" for layer in layers):
            for folder in ("Art/Maps", "Art/Models", "Art/Textures", "XML"):
                (data / folder).mkdir(parents=True, exist_ok=True)
            (data / "MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>",
                                                encoding="utf-8")
        if "GameData" in layers:
            xml = self.base / "XML"
            (xml / "GameObjectFiles.xml").write_text(
                "<Game_Object_Files><File>eawr_layer_shared.xml</File>"
                "<File>eawr_layer_base.xml</File></Game_Object_Files>", encoding="utf-8")
            for name, content in REGISTRIES.items():
                (xml / name).write_text(content, encoding="utf-8")
            # A base-only include, read from base in both profiles.
            (xml / "eawr_layer_base.xml").write_text(
                objects_xml([("EAWR_LAYER_BASE_OBJECT", "eawr_layer_base.alo", "1.0")]), encoding="utf-8")
            # The base copy of the shared include names a model no layer holds.
            self.write_base_shared("1.0")
            for name in ("eawr_layer_base.alo", "eawr_layer_fallback.alo"):
                (self.base / "Art" / "Models" / name).write_bytes(scene_fixture.alo_bytes(SHADED))
            (self.base / "Art" / "Textures" / "eawr_layer_red.dds").write_bytes(
                scene_fixture.dds_bytes((200, 40, 40, 255)))
            (self.base / "Art" / "Maps" / "EAWR_LAYER_BASE_ONLY.TED").write_bytes(self.BASE_TED)
            (self.base / "Art" / "Maps" / "EAWR_LAYER_SHARED.TED").write_bytes(self.BASE_SHARED_TED)
        if "corruption" in layers:
            # The expansion copy shadows it and names a model only base holds.
            self.write_expansion_shared("1.0")
            (self.expansion / "Art" / "Maps" / "EAWR_LAYER_SHARED.TED").write_bytes(self.EXPANSION_SHARED_TED)
            (self.expansion / "Art" / "Maps" / "EAWR_LAYER_EXPANSION_ONLY.TED").write_bytes(self.EXPANSION_TED)

    def write_base_shared(self, scale):
        (self.base / "XML" / "eawr_layer_shared.xml").write_text(
            objects_xml([("EAWR_LAYER_SHARED", "eawr_layer_absent.alo", scale)]), encoding="utf-8")

    def write_expansion_shared(self, scale):
        (self.expansion / "XML" / "eawr_layer_shared.xml").write_text(
            objects_xml([("EAWR_LAYER_SHARED", "eawr_layer_fallback.alo", scale)]), encoding="utf-8")


class LayeredProfiles(unittest.TestCase):
    def setUp(self):
        if not SCANNER:
            self.skipTest("pass --scanner to exercise the native scanner")
        temporary = tempfile.TemporaryDirectory(prefix="eawr-scene-layers-")
        self.addCleanup(temporary.cleanup)
        self.directory = pathlib.Path(temporary.name)
        self.fixture = LayeredFixture(self.directory / "install")

    def scan(self, *profile):
        report = self.directory / "report.json"
        completed = run_scanner(*profile, "--game-root", str(self.fixture.root), "--all", "--report", str(report))
        self.assertEqual(completed.returncode, 0, completed.stdout)
        text = report.read_text(encoding="utf-8")
        self.assertNotIn(self.directory.name, text)
        self.assertIsNone(FORBIDDEN.search(text))
        return json.loads(text)

    def rejected(self, *arguments):
        report = self.directory / "unwritten.json"
        completed = run_scanner(*arguments, "--all", "--report", str(report))
        self.assertEqual(completed.returncode, 2, completed.stdout)
        self.assertFalse(report.exists())
        return completed.stdout

    @staticmethod
    def maps(document):
        return {entry["logical_path"]: entry for entry in document["maps"]}

    def test_foc_mounts_expansion_over_base(self):
        document = self.scan("--profile", "foc")
        self.assertEqual(document["profile"], "foc")
        self.assertEqual(document["generator"], "scene_scan --profile foc --game-root <GAME_ROOT> --all")
        self.assertIn("FoC effective VFS view", document["provenance"])
        self.assertIn("effective FoC VFS", document["inputs"]["catalog_input_scope"])
        self.assertFalse(document["inputs"]["catalog_provisional"])
        maps = self.maps(document)
        self.assertEqual(list(maps), ["data/art/maps/eawr_layer_base_only.ted",
                                      "data/art/maps/eawr_layer_expansion_only.ted",
                                      "data/art/maps/eawr_layer_shared.ted"])
        base_only = maps["data/art/maps/eawr_layer_base_only.ted"]
        self.assertEqual(base_only["sha256"], sha256(LayeredFixture.BASE_TED))
        self.assertEqual((base_only["placements"], base_only["resolved"]), (1, 1))
        # The expansion XML wins EAWR_LAYER_SHARED; the model it names exists
        # only in base, and the effective view falls through to it.
        expansion_only = maps["data/art/maps/eawr_layer_expansion_only.ted"]
        self.assertEqual(expansion_only["sha256"], sha256(LayeredFixture.EXPANSION_TED))
        self.assertEqual((expansion_only["placements"], expansion_only["resolved"]), (2, 2))
        shared = maps["data/art/maps/eawr_layer_shared.ted"]
        self.assertEqual(shared["sha256"], sha256(LayeredFixture.EXPANSION_SHARED_TED))
        self.assertEqual((shared["placements"], shared["resolved"], shared["unresolved"]), (2, 2, 0))

    def test_eaw_reads_base_only(self):
        document = self.scan("--profile", "eaw")
        self.assertEqual(document, self.scan())
        self.assertEqual(document["profile"], "eaw")
        self.assertEqual(document["generator"], "scene_scan --profile eaw --game-root <GAME_ROOT> --all")
        maps = self.maps(document)
        self.assertEqual(list(maps), ["data/art/maps/eawr_layer_base_only.ted",
                                      "data/art/maps/eawr_layer_shared.ted"])
        shared = maps["data/art/maps/eawr_layer_shared.ted"]
        self.assertEqual(shared["sha256"], sha256(LayeredFixture.BASE_SHARED_TED))
        self.assertEqual((shared["placements"], shared["resolved"]), (1, 0))
        self.assertEqual(shared["placements_by_cause"]["model_not_in_vfs"], 1)

    def test_expansion_xml_value_changes_foc_identity_only(self):
        foc, eaw = self.scan("--profile", "foc")["inputs"], self.scan("--profile", "eaw")["inputs"]
        self.fixture.write_expansion_shared("2.0")
        foc_after, eaw_after = self.scan("--profile", "foc")["inputs"], self.scan("--profile", "eaw")["inputs"]
        self.assertEqual(foc["catalog_sha256"], foc_after["catalog_sha256"])
        self.assertNotEqual(foc["catalog_input_sha256"], foc_after["catalog_input_sha256"])
        self.assertEqual(eaw, eaw_after)

    def test_shadowed_base_xml_value_leaves_foc_identity(self):
        foc, eaw = self.scan("--profile", "foc")["inputs"], self.scan("--profile", "eaw")["inputs"]
        self.fixture.write_base_shared("2.0")
        foc_after, eaw_after = self.scan("--profile", "foc")["inputs"], self.scan("--profile", "eaw")["inputs"]
        self.assertEqual(foc, foc_after)
        self.assertEqual(eaw["catalog_sha256"], eaw_after["catalog_sha256"])
        self.assertNotEqual(eaw["catalog_input_sha256"], eaw_after["catalog_input_sha256"])

    def test_foc_requires_both_data_directories(self):
        for present in ("GameData", "corruption"):
            root = self.directory / ("only-" + present)
            LayeredFixture(root, layers=(present,))
            output = self.rejected("--profile", "foc", "--game-root", str(root))
            self.assertIn("GameData/Data and corruption/Data", output)
            self.assertNotIn(self.directory.name, output)

    def test_unsupported_profile_is_a_usage_error(self):
        for profile in ("remake", "FOC", ""):
            output = self.rejected("--profile", profile, "--game-root", str(self.fixture.root))
            self.assertIn("Usage: scene_scan [--profile <eaw|foc>]", output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--scanner")
    parser.add_argument("--game-root")
    known, rest = parser.parse_known_args()
    SCANNER = known.scanner
    GAME_ROOT = known.game_root
    unittest.main(argv=[sys.argv[0], *rest])
