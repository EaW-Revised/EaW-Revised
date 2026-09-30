from __future__ import annotations

import csv
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from unpack_megs import assemble  # noqa: E402


def meg(path: Path, members: list[tuple[str, bytes]]) -> None:
    names = [name.encode("ascii") for name, _ in members]
    offset = 8 + sum(2 + len(name) for name in names) + 20 * len(members)
    rows = []
    for index, (_name, data) in enumerate(members):
        rows.append(struct.pack("<IIIII", 0, 0, len(data), offset, index))
        offset += len(data)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<II", len(names), len(members))
                     + b"".join(struct.pack("<H", len(name)) + name for name in names)
                     + b"".join(rows) + b"".join(data for _name, data in members))


class UnpackTests(unittest.TestCase):
    def test_all_sources_and_effective_precedence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            game = Path(temporary) / "game"
            output = Path(temporary) / "reference"
            base = game / "GameData" / "Data"
            foc = game / "corruption" / "Data"
            for root in (base, foc):
                root.mkdir(parents=True)
                (root / "MegaFiles.xml").write_text(
                    "<Mega_Files><File>Data/A.meg</File><File>Data/B.meg</File></Mega_Files>")
            for root, layer in ((base, "base"), (foc, "foc")):
                meg(root / "A.meg", [("DATA\\XML\\SHARED.XML", layer.encode() + b"-a")])
                meg(root / "B.meg", [("DATA\\XML\\SHARED.XML", layer.encode() + b"-b")])
            meg(foc / "Patch2.meg", [("DATA\\XML\\SHARED.XML", b"foc-patch2")])
            (base / "XML").mkdir()
            (base / "XML" / "SHARED.XML").write_bytes(b"base-loose")
            result = assemble(game, output)
            self.assertEqual(result["archives"], 5)
            self.assertEqual((output / "effective/data/xml/shared.xml").read_bytes(), b"foc-patch2")
            self.assertEqual((output / "eaw/loose/data/xml/shared.xml").read_bytes(), b"base-loose")
            with (output / "MANIFEST.tsv").open(encoding="utf-8", newline="") as stream:
                rows = {row["path"]: row for row in csv.DictReader(stream, delimiter="\t")}
            self.assertEqual(rows["eaw/loose/data/xml/shared.xml"]["overridden-by"],
                             "foc/Patch2.meg/data/xml/shared.xml")
            self.assertEqual(rows["effective/data/xml/shared.xml"]["source"],
                             "foc/Patch2.meg/data/xml/shared.xml")


if __name__ == "__main__":
    unittest.main()
