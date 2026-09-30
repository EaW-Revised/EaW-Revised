from __future__ import annotations

import struct
import hashlib
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))
from corpus import Corpus, CorpusError  # noqa: E402
import lua_inventory  # noqa: E402
import xml_inventory  # noqa: E402


def write_meg(path: Path, entries: list[tuple[str, bytes]]) -> None:
    names = [name.encode("ascii") for name, _ in entries]
    table_size = 8 + sum(2 + len(name) for name in names) + 20 * len(entries)
    offset = table_size; rows = []
    for index, (_name, data) in enumerate(entries):
        rows.append(struct.pack("<IIIII", 0, 0, len(data), offset, index)); offset += len(data)
    value = struct.pack("<II", len(names), len(entries))
    value += b"".join(struct.pack("<H", len(name)) + name for name in names)
    value += b"".join(rows) + b"".join(data for _name, data in entries)
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(value)


class CorpusTests(unittest.TestCase):
    def test_raw_and_effective_match_vfs_precedence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); base = root / "GameData" / "Data"
            base.mkdir(parents=True); (root / "corruption" / "Data").mkdir(parents=True)
            (base / "MegaFiles.xml").write_text("<Mega_Files><File>A.meg</File><File>B.meg</File></Mega_Files>")
            write_meg(base / "A.meg", [("DATA/X/Test.XML", b"archive-a")])
            write_meg(base / "B.meg", [("DATA/X/Test.XML", b"archive-b")])
            write_meg(base / "Unused.meg", [("DATA/X/Unused.XML", b"inactive")])
            loose = base / "X" / "Test.XML"; loose.parent.mkdir(); loose.write_bytes(b"loose")
            corpus = Corpus(root)
            effective = list(corpus.iter_sources("eaw", ".xml", "effective"))
            winner = next(s for s in effective if s.logical_path == "data/x/test.xml")
            self.assertEqual(winner.data, b"loose")
            raw = list(corpus.iter_sources("eaw", ".xml", "raw"))
            self.assertEqual(len(raw), 5)  # manifest + loose winner + three archive members
            self.assertEqual(sum(not s.active for s in raw), 1)

    def test_missing_manifest_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); (root / "GameData" / "Data").mkdir(parents=True)
            with self.assertRaises(CorpusError): list(Corpus(root).iter_sources("eaw", ".xml", "raw"))

    def test_manifest_unicode_family_is_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); base = root / "GameData" / "Data"; base.mkdir(parents=True)
            valid = '<?xml version="1.0" encoding="UTF-16"?><Mega_Files><File>A.meg</File></Mega_Files>'
            (base / "MegaFiles.xml").write_bytes(valid.encode("utf-16")); write_meg(base / "A.meg", [])
            self.assertEqual(list(Corpus(root).iter_sources("eaw", ".lua", "effective")), [])
            invalid = '<?xml version="1.0" encoding="windows-1252"?><Mega_Files><File>A.meg</File></Mega_Files>'
            (base / "MegaFiles.xml").write_bytes(invalid.encode("ascii"))
            with self.assertRaises(CorpusError): list(Corpus(root).iter_sources("eaw", ".lua", "effective"))

    def test_repeated_mod_roots_are_leaf_first_with_honest_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "game"
            base = root / "GameData" / "Data"
            expansion = root / "corruption" / "Data"
            leaf = Path(temp) / "leaf" / "Data"
            dependency_a = Path(temp) / "dependency-a" / "Data"
            dependency_b = Path(temp) / "dependency-b" / "Data"
            for data_root, archive_name, members in (
                (base, "Base.meg", [("DATA/X/Shared.XML", b"base")]),
                (expansion, "Expansion.meg", [("DATA/X/Shared.XML", b"expansion")]),
                (leaf, "Leaf.meg", [("DATA/X/Shared.XML", b"leaf-archive"),
                                     ("DATA/X/LeafOnly.XML", b"leaf-only")]),
                (dependency_a, "DependencyA.meg", [("DATA/X/Ordering.XML", b"dependency-a-archive")]),
                (dependency_b, "DependencyB.meg", []),
            ):
                data_root.mkdir(parents=True)
                (data_root / "MegaFiles.xml").write_text(
                    f"<Mega_Files><File>{archive_name}</File></Mega_Files>", encoding="utf-8")
                write_meg(data_root / archive_name, members)
            shared_loose = dependency_a / "X" / "Shared.XML"
            shared_loose.parent.mkdir(); shared_loose.write_bytes(b"dependency-a-loose")
            ordering_loose = dependency_b / "X" / "Ordering.XML"
            ordering_loose.parent.mkdir(); ordering_loose.write_bytes(b"dependency-b-loose")
            unrelated = dependency_b / "X" / "Unrelated.XML"
            unrelated.write_bytes(b"dependency-b-unrelated")

            corpus = Corpus(root, [leaf, dependency_a.parent, dependency_b])
            raw = list(corpus.iter_sources("remake", ".xml", "raw"))
            self.assertEqual(len(raw), 13)
            self.assertEqual([layer["layer_id"] for layer in corpus.last_manifest["layers"]],
                             ["mod", "mod[1]", "mod[2]", "expansion", "base"])
            self.assertTrue(all(source.sha256 == hashlib.sha256(source.data).hexdigest()
                                for source in raw))

            effective = list(corpus.iter_sources("remake", ".xml", "effective"))
            winners = {source.logical_path: source for source in effective}
            self.assertEqual((winners["data/x/shared.xml"].data,
                              winners["data/x/shared.xml"].layer_id,
                              winners["data/x/shared.xml"].origin),
                             (b"leaf-archive", "mod", "archive"))
            self.assertEqual((winners["data/x/ordering.xml"].data,
                              winners["data/x/ordering.xml"].layer_id,
                              winners["data/x/ordering.xml"].origin),
                             (b"dependency-a-archive", "mod[1]", "archive"))
            self.assertEqual(winners["data/x/unrelated.xml"].layer_id, "mod[2]")
            self.assertEqual(len(effective), 5)

            # MODPATH layers never leak into base/expansion-only profiles.
            self.assertEqual({s.layer_id for s in Corpus(root, [leaf, dependency_a, dependency_b])
                              .iter_sources("eaw", ".xml", "effective")}, {"base"})
            self.assertNotIn("mod[1]", {s.layer_id for s in Corpus(root, leaf)
                                        .iter_sources("remake", ".xml", "effective")})
            with self.assertRaises(CorpusError):
                Corpus(root, [leaf, leaf])
            with self.assertRaises(CorpusError):
                Corpus(root, [dependency_a, dependency_a.parent])

    def test_both_clis_preserve_repeated_mod_root_order(self) -> None:
        roots = [Path("leaf"), Path("dependency-a"), Path("dependency-b")]
        with mock.patch.object(lua_inventory, "generate", return_value={}) as generate_lua:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(lua_inventory.main([
                    "--game-root", "game", "--mod-root", str(roots[0]),
                    "--mod-root", str(roots[1]), "--mod-root", str(roots[2]),
                    "--reference-index", "declarations.json", "--out", "output"]), 0)
            self.assertEqual(generate_lua.call_args.args[1], roots)
        with mock.patch.object(xml_inventory, "generate", return_value={}) as generate_xml:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(xml_inventory.main([
                    "--game-root", "game", "--mod-root", str(roots[0]),
                    "--mod-root", str(roots[1]), "--mod-root", str(roots[2]),
                    "--schema-root", "schema", "--out", "output"]), 0)
            self.assertEqual(generate_xml.call_args.args[1], roots)


if __name__ == "__main__": unittest.main()
