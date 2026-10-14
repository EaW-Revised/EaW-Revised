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
    def test_campaign_scene_exception_requires_reviewed_application(self) -> None:
        rows, _ = tag_coverage.build_sets(TRACE, read_file)
        row = {"tag": "Unread_Tag", "classes": ["SpaceUnit"], "status": "applied",
               "area": "economy", "basis": "reviewed", "scene": "campaign",
               "note": "Campaign battle bounty only, WGCH-65/WGP-189.",
               "applied": [{"code": "src/sim/galactic/control.cpp#execute_landing",
                            "rules": ["WGCH-65", "WGP-189"]}, *APPLIED]}
        _value, failures = tag_coverage.scene_report(rows, registry(row), [])
        self.assertFalse(any("Unread_Tag is applied" in failure for failure in failures))
        # Campaign scope must not suppress the galactic consumer's read requirement.
        _value, failures = tag_coverage.scene_report(rows, registry(row), [], "galactic")
        self.assertTrue(any("no loader of the GC scene reads it" in failure for failure in failures))
        for change in ({"scene": "skirmish"}, {"basis": "auto"}, {"basis": None},
                       {"status": "partial"}, {"note": ""},
                       {"applied": [{**APPLIED[0], "rules": []}]},
                       {"applied": [{**APPLIED[0], "rules": ["not-a-rule"]}]}):
            with self.subTest(change=change):
                _value, failures = tag_coverage.scene_report(rows, registry({**row, **change}), [])
                self.assertTrue(any("no loader of the M2 scene reads it" in failure for failure in failures))

    def test_galactic_sim_targets_are_required_only_in_the_galactic_scene(self) -> None:
        rows, _ = tag_coverage.build_sets(TRACE, read_file)
        for code in ("src/sim/galactic/control.cpp#execute_landing",
                     "src/sim/galactic/session.cpp#refresh_credit_caps"):
            with self.subTest(code=code):
                row = {"tag": "Unread_Tag", "classes": ["SpaceUnit"], "status": "partial",
                       "area": "movement", "applied": [{"code": code, "rules": []}]}
                _value, failures = tag_coverage.scene_report(rows, registry(row), [], "tactical")
                self.assertFalse(any("no loader of the M2 scene reads it" in f for f in failures))
                _value, failures = tag_coverage.scene_report(rows, registry(row), [], "galactic")
                self.assertTrue(any("no loader of the GC scene reads it" in f for f in failures))
                # An independent tactical consumer of that same tag must still be checked.
                row["applied"].append(APPLIED[0])
                _value, failures = tag_coverage.scene_report(rows, registry(row), [], "tactical")
                self.assertTrue(any("no loader of the M2 scene reads it" in f for f in failures))

    def test_galactic_scope_does_not_require_tactical_consumers(self) -> None:
        rows, _ = tag_coverage.build_sets(TRACE, read_file)
        tactical = registry({"tag": "Unread_Tag", "classes": ["SpaceUnit"], "status": "applied",
                             "area": "combat", "applied": APPLIED})
        _value, failures = tag_coverage.scene_report(rows, tactical, [], "galactic")
        self.assertFalse(any("Unread_Tag is applied" in failure for failure in failures))
        galactic = registry({"tag": "Unread_Tag", "classes": ["SpaceUnit"], "status": "applied",
                            "area": "movement", "applied": [{"code": "src/sim/galactic/movement.cpp", "rules": []}]})
        _value, failures = tag_coverage.scene_report(rows, galactic, [], "galactic")
        self.assertTrue(any("no loader of the GC scene reads it" in failure for failure in failures))

    def scoped_scene(self, xml: bytes, reads: set[int], scope: dict) -> list[str]:
        nodes = tag_coverage.parse_nodes(xml, "fixture")
        trace = [entry("object", "fixture", node.line, node.column + 1, node.name, f"object-{index}")
                 for index, node in enumerate(nodes) if node.depth == 1]
        trace += [entry("used", "fixture", nodes[index].line, nodes[index].column + 1, nodes[index].name)
                  for index in reads]
        rows, problems = tag_coverage.build_sets(trace, lambda _: (xml, "fixture"))
        self.assertEqual(problems, [])
        applied_rows = []
        for tag in rows:
            object_class, tail = tag_registry.split_key(tag)
            applied_rows.append({"tag": tail, "classes": [object_class], "status": "applied", "area": "combat",
                                 "applied": [{**APPLIED[0], "trace_scope": scope}]}
                                if tail.endswith("Value") else
                                {"tag": tail, "classes": [object_class], "status": "todo", "area": "combat",
                                 "ticket": 650})
        return tag_coverage.scene_report(rows, registry(*applied_rows), [])[1]

    def test_unrepresented_ability_consumer_does_not_fail(self) -> None:
        xml = (b'<Units><UniqueUnit Name="Other"><Unit_Abilities_Data><Unit_Ability>'
               b'<Type>WEAKEN_ENEMY</Type><Value>1</Value></Unit_Ability></Unit_Abilities_Data>'
               b'</UniqueUnit></Units>')
        self.assertEqual(self.scoped_scene(xml, set(), {"ability_types": ["CORRUPT_SYSTEMS"]}), [])

    def test_represented_ability_consumer_must_be_read(self) -> None:
        xml = (b'<Units><UniqueUnit Name="Present"><Unit_Abilities_Data><Unit_Ability>'
               b'<Type>corrupt_systems</Type><Value>1</Value></Unit_Ability></Unit_Abilities_Data>'
               b'</UniqueUnit></Units>')
        self.assertEqual(len(self.scoped_scene(xml, set(), {"ability_types": ["CORRUPT_SYSTEMS"]})), 1)
        self.assertEqual(self.scoped_scene(xml, {5}, {"ability_types": ["CORRUPT_SYSTEMS"]}), [])

    def test_read_on_another_handler_does_not_cover_the_scoped_handler(self) -> None:
        xml = (b'<Units><UniqueUnit Name="Present"><Unit_Abilities_Data>'
               b'<Unit_Ability><Type>WEAKEN_ENEMY</Type><Value>1</Value></Unit_Ability>'
               b'<Unit_Ability><Type>CORRUPT_SYSTEMS</Type><Value>2</Value></Unit_Ability>'
               b'</Unit_Abilities_Data></UniqueUnit></Units>')
        self.assertEqual(len(self.scoped_scene(xml, {5}, {"ability_types": ["CORRUPT_SYSTEMS"]})), 1)
        self.assertEqual(self.scoped_scene(xml, {8}, {"ability_types": ["CORRUPT_SYSTEMS"]}), [])

    def test_special_weapon_does_not_require_ordinary_body_tags(self) -> None:
        xml = (b'<Units><SpecialStructure Name="Source"><SpaceBehavior> REVEAL, special_weapon </SpaceBehavior>'
               b'<Value>1</Value></SpecialStructure></Units>')
        self.assertEqual(self.scoped_scene(xml, set(), {"exclude_space_behaviors": ["SPECIAL_WEAPON"]}), [])

    def test_beacon_behavior_exclusion_keeps_other_object_reads_required(self) -> None:
        xml = (b'<Units><MiscObject Name="Marker"><Behavior> IDLE, multiplayer_beacon </Behavior>'
               b'<Value>1</Value></MiscObject><MiscObject Name="ModBody">'
               b'<SpaceBehavior>MULTIPLAYER_BEACON</SpaceBehavior><Behavior>OTHER</Behavior>'
               b'<Value>2</Value></MiscObject></Units>')
        scope = {"exclude_behaviors": ["MULTIPLAYER_BEACON"]}
        self.assertEqual(len(self.scoped_scene(xml, set(), scope)), 1)
        self.assertEqual(len(self.scoped_scene(xml, {3}, scope)), 1)
        self.assertEqual(self.scoped_scene(xml, {7}, scope), [])
        # Behavior and SpaceBehavior selectors operate on their own XML fields.
        self.assertEqual(len(self.scoped_scene(xml, {7}, {"exclude_space_behaviors": ["MULTIPLAYER_BEACON"]})), 1)

    def test_ordinary_body_still_requires_a_read_with_special_weapon_present(self) -> None:
        xml = (b'<Units><SpecialStructure Name="Source"><SpaceBehavior>SPECIAL_WEAPON</SpaceBehavior>'
               b'<Value>1</Value></SpecialStructure><SpecialStructure Name="Body">'
               b'<SpaceBehavior>SPACE_OBSTACLE, SHIELDED</SpaceBehavior><Value>2</Value>'
               b'</SpecialStructure></Units>')
        scope = {"exclude_space_behaviors": ["SPECIAL_WEAPON"]}
        self.assertEqual(len(self.scoped_scene(xml, {3}, scope)), 1)
        self.assertEqual(self.scoped_scene(xml, {6}, scope), [])

    def test_one_object_read_does_not_cover_another_scoped_object(self) -> None:
        xml = (b'<Units><SpecialStructure Name="A"><Value>1</Value></SpecialStructure>'
               b'<SpecialStructure Name="B"><Value>2</Value></SpecialStructure></Units>')
        self.assertEqual(len(self.scoped_scene(xml, {2}, {"exclude_space_behaviors": ["SPECIAL_WEAPON"]})), 1)

    def test_unrepresented_object_type_does_not_fail(self) -> None:
        xml = b'<Units><UpgradeObject Name="Other"><Value>1</Value></UpgradeObject></Units>'
        self.assertEqual(self.scoped_scene(xml, set(), {"object_types": ["Unloaded"]}), [])

    def test_represented_object_type_must_be_read(self) -> None:
        xml = (b'<Units><UpgradeObject Name="Other"><Value>1</Value></UpgradeObject>'
               b'<UpgradeObject Name="Cloaking"><Value>2</Value></UpgradeObject></Units>')
        # Fixture trace owners are named by their root node index, independently of XML spelling.
        scope = {"object_types": ["OBJECT-3"]}
        self.assertEqual(len(self.scoped_scene(xml, {2}, scope)), 1)
        self.assertEqual(self.scoped_scene(xml, {4}, scope), [])

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
