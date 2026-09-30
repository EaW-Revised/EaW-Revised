"""End-to-end repeated-MODPATH checks for both inventory CLIs."""
from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def write_meg(path: Path, entries: list[tuple[str, bytes]]) -> None:
    names = [name.encode("ascii") for name, _ in entries]
    table_size = 8 + sum(2 + len(name) for name in names) + 20 * len(entries)
    offset = table_size
    rows: list[bytes] = []
    for index, (_name, data) in enumerate(entries):
        rows.append(struct.pack("<IIIII", 0, 0, len(data), offset, index))
        offset += len(data)
    value = struct.pack("<II", len(names), len(entries))
    value += b"".join(struct.pack("<H", len(name)) + name for name in names)
    value += b"".join(rows) + b"".join(data for _name, data in entries)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value)


def manifest(*names: str) -> str:
    return "<Mega_Files>" + "".join(f"<File>{name}</File>" for name in names) + "</Mega_Files>"


def data_root(root: Path) -> Path:
    value = root / "Data"
    value.mkdir(parents=True, exist_ok=True)
    return value


def synthetic_schema(root: Path) -> Path:
    """Create a valid pinned-schema-shaped checkout without ignored dependencies."""
    root.mkdir(parents=True, exist_ok=True)
    (root / "LICENSE").write_text("MIT License\n\nSynthetic acceptance schema.\n", encoding="utf-8")
    for game in ("eaw", "foc"):
        game_root = root / game
        game_root.mkdir()
        (game_root / "_index.json").write_text(
            json.dumps({"meta": [], "tags": [], "types": []}, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    init = subprocess.run(
        ["git", "-c", "init.defaultBranch=main", "init"],
        cwd=root, capture_output=True, text=True, check=False,
    )
    if init.returncode != 0:
        raise AssertionError(f"could not initialize synthetic schema checkout: {init.stderr}")
    for args in (
        ["git", "config", "user.name", "inventory-acceptance"],
        ["git", "config", "user.email", "inventory-acceptance@example.invalid"],
        ["git", "add", "LICENSE", "eaw/_index.json", "foc/_index.json"],
        ["git", "-c", "commit.gpgsign=false", "commit", "--no-verify", "-m", "synthetic schema"],
    ):
        result = subprocess.run(args, cwd=root, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise AssertionError(f"could not prepare synthetic schema checkout: {result.stderr}")
    return root


def outcome(manifest_data: dict, profile: str, mode: str, logical: str) -> list[dict]:
    profile_data = next(p for p in manifest_data["profiles"] if p["profile"] == profile)
    return [row for row in profile_data[f"{'raw_' if mode == 'raw' else ''}file_outcomes"]
            if row["logical_path"] == logical]


class ModPathCliTests(unittest.TestCase):
    def test_both_clis_repeated_modpath_precedence_and_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "game"
            schema_root = synthetic_schema(Path(temporary) / "schema")
            leaf = Path(temporary) / "leaf"
            dependency_one = Path(temporary) / "dependency-one"
            dependency_two = Path(temporary) / "dependency-two"
            base = data_root(root / "GameData")
            expansion = data_root(root / "corruption")
            leaf_data = data_root(leaf)
            dep1_data = data_root(dependency_one)
            dep2_data = data_root(dependency_two)

            base.joinpath("MegaFiles.xml").write_text(manifest("Base.meg"), encoding="utf-8")
            expansion.joinpath("MegaFiles.xml").write_text(manifest("Expansion.meg"), encoding="utf-8")
            leaf_data.joinpath("MegaFiles.xml").write_text(
                manifest("Leaf.meg", "Patch.meg", "Patch2.meg", "64Patch.meg"), encoding="utf-8")
            dep1_data.joinpath("MegaFiles.xml").write_text(manifest("Dependency1.meg"), encoding="utf-8")
            dep2_data.joinpath("MegaFiles.xml").write_text(manifest("Dependency2.meg"), encoding="utf-8")

            base.joinpath("Base.meg").parent.mkdir(parents=True, exist_ok=True)
            write_meg(base / "Base.meg", [("DATA/Scripts/base.lua", b"Base_Call()")])
            write_meg(expansion / "Expansion.meg", [("DATA/Scripts/expansion.lua", b"Expansion_Call()")])
            write_meg(leaf_data / "Leaf.meg", [
                ("DATA/Scripts/leaf_archive.lua", b"Leaf_Archive_Call()"),
                ("DATA/Scripts/leaf_vs_dependency.lua", b"Leaf_Archive_Wins()"),
                ("DATA/Scripts/same_layer.lua", b"Archive_Loses_To_Loose()"),
                ("DATA/Scripts/patch.lua", b"Patch_Leaf()"),
                ("DATA/XML/leaf.xml", b"<Leaf><Value>archive</Value></Leaf>"),
                ("DATA/XML/leaf_vs_dependency.xml", b"<LeafArchive/>"),
                ("DATA/XML/same_layer.xml", b"<ArchiveLoses/>"),
                ("DATA/XML/patch.xml", b"<PatchLeaf/>"),
            ])
            write_meg(leaf_data / "Patch.meg", [("DATA/Scripts/patch.lua", b"Patch_One()"),
                                                  ("DATA/XML/patch.xml", b"<PatchOne/>")])
            write_meg(leaf_data / "Patch2.meg", [("DATA/Scripts/patch.lua", b"Patch_Two()"),
                                                   ("DATA/XML/patch.xml", b"<PatchTwo/>")])
            write_meg(leaf_data / "64Patch.meg", [("DATA/Scripts/patch.lua", b"Patch_64()"),
                                                   ("DATA/XML/patch.xml", b"<Patch64/>")])
            write_meg(leaf_data / "Inactive.meg", [("DATA/Scripts/inactive.lua", b"Inactive()"),
                                                    ("DATA/XML/inactive.xml", b"<Inactive/>")])
            write_meg(dep1_data / "Dependency1.meg", [
                ("DATA/Scripts/dep_order.lua", b"Dependency_One_Archive()"),
                ("DATA/Scripts/leaf_vs_dependency.lua", b"Dependency_Archive_Loses()"),
                ("DATA/XML/dep_order.xml", b"<DependencyOneArchive/>"),
                ("DATA/XML/leaf_vs_dependency.xml", b"<DependencyArchiveLoses/>"),
            ])
            write_meg(dep2_data / "Dependency2.meg", [("DATA/Scripts/dep_two.lua", b"Dependency_Two_Archive()"),
                                                        ("DATA/XML/dep_two.xml", b"<DependencyTwoArchive/>")])

            (leaf_data / "Scripts").mkdir(parents=True, exist_ok=True)
            (leaf_data / "Scripts" / "same_layer.lua").write_bytes(b"Loose_Wins()")
            (dep2_data / "Scripts").mkdir(parents=True, exist_ok=True)
            (dep2_data / "Scripts" / "dep_order.lua").write_bytes(b"Dependency_Two_Loose()")
            (dep2_data / "Scripts" / "leaf_vs_dependency.lua").write_bytes(b"Dependency_Two_Loose()")
            (dep2_data / "Scripts" / "unrelated.lua").write_bytes(b"Unrelated()")
            (dep2_data / "XML").mkdir(parents=True, exist_ok=True)
            (dep2_data / "XML" / "dep_order.xml").write_bytes(b"<DependencyTwoLoose/>")
            (dep2_data / "XML" / "leaf_vs_dependency.xml").write_bytes(b"<DependencyTwoLoose/>")
            (leaf_data / "XML").mkdir(parents=True, exist_ok=True)
            (leaf_data / "XML" / "leaf.xml").write_bytes(b"<Leaf><Value>loose</Value></Leaf>")
            (leaf_data / "XML" / "same_layer.xml").write_bytes(b"<LooseWins/>")

            out = Path(temporary) / "out"
            cli_args = ["--game-root", str(root), "--mod-root", str(leaf),
                        "--mod-root", str(dependency_one), "--mod-root", str(dependency_two)]
            lua = subprocess.run([sys.executable, str(ROOT / "tools/inventory/lua_inventory.py"),
                                  *cli_args, "--reference-index", str(ROOT / "plan/inventories/lua-declarations.json"),
                                  "--out", str(out / "lua")], cwd=ROOT, capture_output=True, text=True)
            xml = subprocess.run([sys.executable, str(ROOT / "tools/inventory/xml_inventory.py"),
                                  *cli_args, "--schema-root", str(schema_root),
                                  "--out", str(out / "xml")], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(lua.returncode, 0, lua.stderr)
            self.assertEqual(xml.returncode, 0, xml.stderr)

            for directory in (out / "lua", out / "xml"):
                loaded = json.loads((directory / f"{'lua' if directory.name == 'lua' else 'xml'}-manifest.json").read_text())
                raw = loaded["profiles"][-1]["raw_file_outcomes"]
                effective = loaded["profiles"][-1]["file_outcomes"]
                self.assertEqual({r["layer_id"] for r in raw if r["layer_id"].startswith("mod")},
                                 {"mod", "mod[1]", "mod[2]"})
                suffix = "scripts" if directory.name == "lua" else "xml"
                extension = "lua" if directory.name == "lua" else "xml"
                for logical, expected_origin, expected_source in (
                    (f"data/{suffix}/leaf_vs_dependency.{extension}", "archive", "mod:Data/Leaf.meg"),
                    (f"data/{suffix}/dep_order.{extension}", "archive", "mod[1]:Data/Dependency1.meg"),
                    (f"data/{suffix}/same_layer.{extension}", "loose", f"mod:loose:Data/{'Scripts' if suffix == 'scripts' else 'XML'}/same_layer.{extension}"),
                    (f"data/{suffix}/patch.{extension}", "archive", "mod:Data/64Patch.meg"),
                ):
                    winner = next(row for row in effective if row["logical_path"] == logical)
                    self.assertEqual((winner["origin"], winner["source_id"]),
                                     (expected_origin, expected_source), logical)
                    shadowed = [row for row in raw if row["logical_path"] == logical and not row["winner"]]
                    self.assertTrue(shadowed, logical)
                    self.assertTrue(all(len(row["sha256"]) == 64 for row in raw if row["logical_path"] == logical))
                inactive = f"data/{suffix}/inactive.{extension}"
                self.assertFalse(any(row["logical_path"] == inactive for row in effective))
                self.assertTrue(any(row["logical_path"] == inactive and not row["active"]
                                    for row in raw))

            one_root = subprocess.run([sys.executable, str(ROOT / "tools/inventory/lua_inventory.py"),
                                       "--game-root", str(root), "--mod-root", str(leaf),
                                       "--reference-index", str(ROOT / "plan/inventories/lua-declarations.json"),
                                       "--out", str(out / "one-root")], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(one_root.returncode, 0, one_root.stderr)
            one_manifest = json.loads((out / "one-root/lua-manifest.json").read_text())
            remake = next(p for p in one_manifest["profiles"] if p["profile"] == "remake")
            self.assertTrue(all(row["layer_id"] == "mod" for row in remake["raw_file_outcomes"]
                                if row["layer_id"].startswith("mod")))

            missing = subprocess.run([sys.executable, str(ROOT / "tools/inventory/xml_inventory.py"),
                                      "--game-root", str(root), "--mod-root", str(Path(temporary) / "missing"),
                                      "--schema-root", str(schema_root),
                                      "--out", str(out / "missing")], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(missing.returncode, 2)
            self.assertIn("missing data root", missing.stderr)

            empty = Path(temporary) / "empty" / "Data"
            empty.mkdir(parents=True)
            empty_result = subprocess.run([sys.executable, str(ROOT / "tools/inventory/lua_inventory.py"),
                                           "--game-root", str(root), "--mod-root", str(empty),
                                           "--reference-index", str(ROOT / "plan/inventories/lua-declarations.json"),
                                           "--out", str(out / "empty")], cwd=ROOT,
                                          capture_output=True, text=True)
            self.assertEqual(empty_result.returncode, 2)
            self.assertIn("missing Data/MegaFiles.xml", empty_result.stderr)

            missing_game = subprocess.run([sys.executable, str(ROOT / "tools/inventory/xml_inventory.py"),
                                           "--game-root", str(Path(temporary) / "missing-game"),
                                           "--mod-root", str(leaf),
                                           "--schema-root", str(schema_root),
                                           "--out", str(out / "missing-game")], cwd=ROOT,
                                          capture_output=True, text=True)
            self.assertEqual(missing_game.returncode, 2)
            self.assertIn("missing data root", missing_game.stderr)


if __name__ == "__main__":
    unittest.main()
