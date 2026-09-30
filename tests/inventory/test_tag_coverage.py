"""#628: the M2 scene against the tag registry, without game data (docs/tag-coverage.md)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))

import tag_coverage  # noqa: E402
import tag_registry  # noqa: E402

UNITS = (b"<Units>\n"
         b"<SpaceUnit Name=\"Base\">\n"
         b"  <Max_Speed>2.0</Max_Speed>\n"
         b"  <Unread_Tag>1</Unread_Tag>\n"
         b"  <Empty_Tag></Empty_Tag>\n"
         b"  <Abilities><Hunt Name=\"X\"><Range>5</Range></Hunt></Abilities>\n"
         b"</SpaceUnit>\n"
         b"<SpaceUnit Name=\"Untouched\"><Other_Tag>1</Other_Tag></SpaceUnit>\n"
         b"</Units>\n")
CONSTANTS = b"<GameConstants>\n  <Read_Constant>1</Read_Constant>\n  <unread_constant>2</unread_constant>\n</GameConstants>\n"
FILES = {"data/xml/units.xml": UNITS, "data/xml/gameconstants.xml": CONSTANTS}


def entry(kind: str, path: str, line: int, column: int, element: str, name: str = "") -> dict:
    return {"kind": kind, "logical_path": path, "source_id": "base", "line": line, "column": column,
            "element": element, "name": name}


# The loaders' columns count from the element name, one past the inventory tooling's `<`.
TRACE = [
    entry("object", "data/xml/units.xml", 2, 2, "SpaceUnit", "Base"),
    entry("used", "data/xml/units.xml", 3, 4, "Max_Speed"),
    entry("document", "data/xml/gameconstants.xml", 1, 2, "GameConstants"),
    entry("used", "data/xml/gameconstants.xml", 2, 4, "Read_Constant"),
]


def read_file(path: str):
    data = FILES.get(path)
    return None if data is None else (data, "base:loose")


class SceneSetTests(unittest.TestCase):
    def test_scene_minus_consumed(self) -> None:
        rows, problems = tag_coverage.build_sets(TRACE, read_file)
        self.assertEqual(problems, [])
        self.assertEqual({tag: row.consumed for tag, row in rows.items()}, {
            "SpaceUnit/@Name": True,
            "SpaceUnit/Max_Speed": True,
            "SpaceUnit/Unread_Tag": False,
            "SpaceUnit/Abilities/Hunt/@Name": False,
            "SpaceUnit/Abilities/Hunt/Range": False,
            "GameConstants/Read_Constant": True,
            "GameConstants/unread_constant": False,
        })
        self.assertEqual(rows["SpaceUnit/Unread_Tag"].owners, {"Base"})
        self.assertEqual(rows["GameConstants/unread_constant"].files, {"data/xml/gameconstants.xml"})

    def test_unjoined_entry_is_a_problem(self) -> None:
        _rows, problems = tag_coverage.build_sets(
            [entry("used", "data/xml/units.xml", 9, 1, "Nope"), entry("used", "data/xml/missing.xml", 1, 1, "X")],
            read_file)
        self.assertEqual(len(problems), 2)


def registry(*rows: dict) -> dict:
    return {"schema_version": 2, "evidence": {}, "rows": sorted(rows, key=tag_registry.row_order)}


APPLIED = [{"code": "src/units/unit_tables.cpp#max_speed", "rules": ["MV-11"]}]


class SceneReportTests(unittest.TestCase):
    def test_scene_against_the_registry(self) -> None:
        rows, _ = tag_coverage.build_sets(TRACE, read_file)
        good = registry(
            {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "applied", "area": "movement", "applied": APPLIED},
            {"tag": "Unread_Tag", "classes": ["SpaceUnit"], "status": "todo", "area": "combat", "ticket": 650},
            {"tag": "@Name", "classes": ["SpaceUnit"], "status": "applied", "area": "data",
             "applied": [{"code": "src/data/xml_registry.cpp#Name", "rules": ["R-08"]}]},
            {"tag": "Abilities/Hunt/@Name", "classes": ["SpaceUnit"], "status": "deferred", "area": "combat",
             "ticket": 76},
            {"tag": "Abilities/Hunt/Range", "classes": ["SpaceUnit"], "status": "deferred", "area": "combat",
             "ticket": 76},
            {"tag": "Read_Constant", "classes": ["GameConstants"], "status": "applied", "area": "combat",
             "applied": [{"code": "src/sim/tactical/combat.cpp#x", "rules": []}]},
            {"tag": "unread_constant", "classes": ["GameConstants"], "status": "land-or-galactic", "area": "movement",
             "evidence": "space-movement.md"},
        )
        value, failures = tag_coverage.scene_report(rows, good, [])
        self.assertEqual(failures, [])
        self.assertEqual(value["scene"], 7)
        self.assertEqual(value["consumed"], 3)

    def test_failures(self) -> None:
        rows, _ = tag_coverage.build_sets(TRACE, read_file)
        bad = registry(
            # Applied in traced code, but the scene never reads it.
            {"tag": "Unread_Tag", "classes": ["SpaceUnit"], "status": "applied", "area": "combat",
             "applied": APPLIED},
            # No row for Max_Speed, Abilities/Hunt/*, GameConstants/*: each is a failure.
        )
        _value, failures = tag_coverage.scene_report(rows, bad, [])
        text = chr(10).join(failures)
        self.assertIn("SpaceUnit/Unread_Tag is applied in the registry, but no loader of the M2 scene reads it", text)
        self.assertIn("scene tag SpaceUnit/Max_Speed has no registry row", text)
        self.assertIn("GameConstants/Read_Constant has no registry row", text)
        # An unrecorded trace (the footprint loader) or an applied row that cites presentation code is not judged.
        judged = registry(
            {"tag": "Unread_Tag", "classes": ["SpaceUnit"], "status": "applied", "area": "combat",
             "applied": APPLIED, "trace": "unrecorded"})
        _value, failures = tag_coverage.scene_report(rows, judged, [])
        self.assertFalse(any("Unread_Tag is applied" in f for f in failures))


if __name__ == "__main__":
    unittest.main()
