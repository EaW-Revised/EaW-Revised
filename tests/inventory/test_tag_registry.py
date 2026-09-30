"""The XML tag registry and its gate without game data (docs/tag-coverage.md)."""
from __future__ import annotations

import contextlib
import copy
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))

import tag_registry as tr  # noqa: E402


def inventory_row(path: str, kind: str = "element", usage: int = 1, profile: str = "foc") -> dict:
    return {"profile": profile, "object_type": "GameObjectType", "tag_path": path, "node_kind": kind,
            "tag_name": path.rsplit("/", 1)[-1].lstrip("@"), "usage_count": usage, "file_count": 1}


INVENTORY = {"rows": [
    inventory_row("SpaceUnit"),
    inventory_row("SpaceUnit/Max_Speed", usage=3),
    inventory_row("SpaceUnit/Layer_Z_Adjust"),
    inventory_row("SpaceUnit/Junk_Tag"),
    inventory_row("SpaceUnit/Abilities"),
    inventory_row("SpaceUnit/Abilities/Stun_Ability/Stun_Range"),
    inventory_row("SpaceUnit/@Name", "attribute"),
    inventory_row("Goals/Build_Fleet/Reachability"),
    inventory_row("Goals/Hunt/Reachability"),
    inventory_row("GUIDialogs/Textures/IDD_A/Frame_Left"),
    inventory_row("GUIDialogs/Textures/IDD_B/Frame_Left"),
    inventory_row("SpaceUnit/EaW_Only", profile="eaw"),
]}
CODE = "src/sim/motion.cpp"
CODE_TEXT = "void apply() { unit.max_speed = 1; unit.layer_z = 2; }\n"


def applied(identifier: str = "max_speed", **extra) -> dict:
    return {"code": f"{CODE}#{identifier}", "rules": ["MV-11"], **extra}


def registry(*rows: dict, evidence: dict | None = None) -> dict:
    ordered = sorted(rows, key=tr.row_order)
    return {"schema_version": 2, "evidence": evidence or {"DB-NOTAG": "debug build: no tag table row carries the name"},
            "rows": ordered}


def full_rows() -> list[dict]:
    return [
        {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "applied", "area": "movement",
         "applied": [applied()]},
        {"tag": "Layer_Z_Adjust", "classes": ["SpaceUnit"], "status": "partial", "area": "movement", "ticket": 666,
         "applied": [applied("layer_z", types=["craft"])], "missing_types": ["ship"]},
        {"tag": "Junk_Tag", "classes": ["SpaceUnit"], "status": "foc-ignores", "area": "combat",
         "evidence": "DB-NOTAG"},
        {"tag": "Abilities/Stun_Ability/Stun_Range", "classes": ["SpaceUnit"], "status": "todo", "area": "combat",
         "ticket": 650},
        {"tag": "@Name", "classes": ["SpaceUnit"], "status": "applied", "area": "data", "applied": [applied()]},
        {"tag": "*/Reachability", "classes": ["Goals"], "status": "deferred", "area": "ai", "ticket": 652},
        {"tag": "Textures/*/Frame_Left", "classes": ["GUIDialogs"], "status": "presentation-later",
         "area": "presentation", "evidence": "the skirmish HUD comes first"},
    ]


class Fixture(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "src" / "sim").mkdir(parents=True)
        (self.root / CODE).write_text(CODE_TEXT, encoding="utf-8")
        (self.root / "docs" / "behaviour").mkdir(parents=True)
        (self.root / "docs" / "behaviour" / "movement.md").write_text("MV-11 a rule\n", encoding="utf-8")
        self.universe = tr.load_universe(INVENTORY)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def problems(self, value: dict, universe=True, root=True) -> str:
        return "\n".join(tr.registry_problems(value, self.universe if universe else None, self.root if root else None))


class UniverseTests(unittest.TestCase):
    def test_paths_collapse_data_names(self) -> None:
        self.assertEqual(tr.normalise_path("Goals/Build_Fleet/Reachability"), "Goals/*/Reachability")
        self.assertEqual(tr.normalise_path("GUIDialogs/Textures/IDD_A/Frame_Left"), "GUIDialogs/Textures/*/Frame_Left")
        self.assertEqual(tr.normalise_path("GUIDialogs/Fonts/Default/Emboss"), "GUIDialogs/Fonts/*/Emboss")
        self.assertEqual(tr.normalise_path("Equations/Any_Name"), "Equations/*")
        self.assertEqual(tr.normalise_path("MovementClassType/Hover"), "MovementClassType/*")
        self.assertEqual(tr.normalise_path("Terrain_Effectiveness/Desert/Walker"), "Terrain_Effectiveness/*/*")
        self.assertEqual(tr.normalise_path("SpaceUnit/Max_Speed"), "SpaceUnit/Max_Speed")

    def test_leaves_only_and_foc_profile(self) -> None:
        universe = tr.load_universe(INVENTORY)
        tags = {(row.object_class, row.tag) for row in universe.values()}
        self.assertEqual(tags, {
            ("SpaceUnit", "Max_Speed"), ("SpaceUnit", "Layer_Z_Adjust"), ("SpaceUnit", "Junk_Tag"),
            ("SpaceUnit", "Abilities/Stun_Ability/Stun_Range"), ("SpaceUnit", "@Name"),
            ("Goals", "*/Reachability"), ("GUIDialogs", "Textures/*/Frame_Left")})
        self.assertEqual(universe[("spaceunit", "max_speed")].usage, 3)
        self.assertEqual(universe[("goals", "*/reachability")].usage, 2)


class RegistryRuleTests(Fixture):
    def test_a_complete_registry_passes(self) -> None:
        self.assertEqual(self.problems(registry(*full_rows())), "")

    def test_every_loaded_tag_needs_a_row_and_no_row_may_be_stale(self) -> None:
        rows = [row for row in full_rows() if row["tag"] != "Max_Speed"]
        rows.append({"tag": "Gone_Tag", "classes": ["SpaceUnit"], "status": "deferred", "area": "combat", "ticket": 1})
        text = self.problems(registry(*rows))
        self.assertIn("loaded tag SpaceUnit/Max_Speed has no registry row", text)
        self.assertIn("registry row spaceunit/gone_tag is not in the loaded XML", text)

    def test_a_pair_has_one_row(self) -> None:
        rows = full_rows() + [{"tag": "max_speed", "classes": ["SpaceUnit"], "status": "deferred", "area": "movement",
                               "ticket": 5}]
        self.assertIn("is also in the row for", self.problems(registry(*rows)))

    def test_status_rules(self) -> None:
        cases = {
            "an applied row lists the code": {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "applied",
                                              "area": "movement"},
            "a todo row links its ticket": {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "todo",
                                            "area": "movement"},
            "foc-ignores evidence cites the debug build": {"tag": "Max_Speed", "classes": ["SpaceUnit"],
                                                           "status": "foc-ignores", "area": "movement",
                                                           "evidence": "a recording"},
            "a deferred row names its ticket": {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "deferred",
                                                "area": "movement"},
            "names a ticket or gives its reason": {"tag": "Max_Speed", "classes": ["SpaceUnit"],
                                                   "status": "land-or-galactic", "area": "movement"},
            "a partial row lists the missing types": {"tag": "Max_Speed", "classes": ["SpaceUnit"],
                                                      "status": "partial", "area": "movement", "ticket": 1,
                                                      "applied": [applied(types=["craft"])]},
            "a partial row lists the types it applies to": {"tag": "Max_Speed", "classes": ["SpaceUnit"],
                                                            "status": "partial", "area": "movement", "ticket": 1,
                                                            "applied": [applied()], "missing_types": ["ship"]},
            "only a partial row has missing_types": {"tag": "Max_Speed", "classes": ["SpaceUnit"],
                                                     "status": "applied", "area": "movement",
                                                     "applied": [applied()], "missing_types": ["ship"]},
            "status 'maybe'": {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "maybe", "area": "movement"},
            "area 'sound'": {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "todo", "area": "sound",
                             "ticket": 1},
            "ticket must be an issue number": {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "todo",
                                               "area": "movement", "ticket": "x"},
            "unknown fields": {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "todo", "area": "movement",
                               "ticket": 1, "color": "red"},
        }
        for needle, row in cases.items():
            with self.subTest(needle):
                rows = [r for r in full_rows() if r["tag"] != "Max_Speed"] + [row]
                self.assertIn(needle, self.problems(registry(*rows)))

    def test_code_locations_must_exist(self) -> None:
        rows = [r for r in full_rows() if r["tag"] != "Max_Speed"]
        gone = {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "applied", "area": "movement",
                "applied": [{"code": "src/sim/moved.cpp#max_speed", "rules": ["MV-11"]}]}
        renamed = {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "applied", "area": "movement",
                   "applied": [{"code": f"{CODE}#renamed_field", "rules": ["MV-11"]}]}
        malformed = {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "applied", "area": "movement",
                     "applied": [{"code": CODE, "rules": ["MV-11"]}]}
        self.assertIn("src/sim/moved.cpp does not exist", self.problems(registry(*rows, gone)))
        self.assertIn("'renamed_field' is not in", self.problems(registry(*rows, renamed)))
        self.assertIn("is not path#identifier", self.problems(registry(*rows, malformed)))
        # Without a repository root the locations are not looked at.
        self.assertEqual(self.problems(registry(*rows, gone), root=False), "")

    def test_rules_must_exist_and_reviewed_rows_need_them(self) -> None:
        rows = [r for r in full_rows() if r["tag"] != "Max_Speed"]
        unknown = {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "applied", "area": "movement",
                   "applied": [{"code": f"{CODE}#max_speed", "rules": ["XX-99"]}]}
        none = {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "applied", "area": "movement",
                "applied": [{"code": f"{CODE}#max_speed", "rules": []}]}
        auto = {**none, "basis": "auto"}
        noted = {**none, "note": "no behaviour-note rule mentions this tag yet"}
        self.assertIn("rule XX-99 is in no docs/behaviour note", self.problems(registry(*rows, unknown)))
        self.assertIn("names its rule IDs", self.problems(registry(*rows, none)))
        self.assertEqual(self.problems(registry(*rows, auto)), "")
        self.assertEqual(self.problems(registry(*rows, noted)), "")

    def test_rows_are_sorted_and_classes_too(self) -> None:
        rows = full_rows()
        value = registry(*rows)
        value["rows"].reverse()
        self.assertIn("not sorted", self.problems(value))
        rows[0]["classes"] = ["SpaceUnit", "Goals"]
        self.assertIn("classes must be a non-empty sorted list", self.problems(registry(*rows)))

    def test_an_evidence_id_must_be_defined(self) -> None:
        rows = [dict(r) for r in full_rows()]
        rows[2]["evidence"] = "DB-OTHER"
        self.assertIn("is not defined in the evidence table", self.problems(registry(*rows)))


class LayoutTests(Fixture):
    def test_canonical_layout_is_checked_and_the_report_is_on_demand(self) -> None:
        directory = self.root / "docs" / "tag-coverage"
        directory.mkdir(parents=True)
        statuses = directory / "statuses.json"
        inventory = self.root / "tags.json"
        inventory.write_text(json.dumps(INVENTORY), encoding="utf-8")
        value = registry(*full_rows())
        statuses.write_bytes(tr.dumps_registry(value).encode("utf-8"))
        args = ["check", "--statuses", str(statuses), "--inventory", str(inventory), "--repo", str(self.root)]
        self.assertEqual(tr.main(args), 0)  # no rendered file to keep in step: parallel row edits merge green
        report = self.root / "out" / "statuses.md"
        self.assertEqual(tr.main(["render", "--statuses", str(statuses), "--inventory", str(inventory),
                                  "--out", str(report)]), 0)
        self.assertEqual(tr.main(args), 0)
        self.assertFalse((directory / "statuses.md").exists())
        text = report.read_text(encoding="utf-8")
        self.assertIn("| partial | 1 |", text)
        self.assertIn("| `Layer_Z_Adjust` | SpaceUnit | craft | ship | #666 |", text)
        # An edit that leaves the JSON parseable but not canonical fails, and format repairs it.
        statuses.write_text(json.dumps(value, indent=1), encoding="utf-8")
        self.assertEqual(tr.main(args), 1)
        self.assertEqual(tr.main(["format", "--statuses", str(statuses)]), 0)
        self.assertEqual(tr.main(args), 0)

    def test_a_defective_row_gets_a_message_naming_it_not_a_bare_key(self) -> None:
        """A todo row without a ticket, a partial row without missing_types and a row without a status used to end
        in a KeyError from the renderer that hid the problem list."""
        directory = self.root / "docs" / "tag-coverage"
        directory.mkdir(parents=True)
        statuses = directory / "statuses.json"
        inventory = self.root / "tags.json"
        inventory.write_text(json.dumps(INVENTORY), encoding="utf-8")
        args = ["check", "--statuses", str(statuses), "--inventory", str(inventory), "--repo", str(self.root)]
        defects = {
            "a todo row links its ticket": lambda row: row.pop("ticket") if row["status"] == "todo" else None,
            "a partial row lists the missing types": lambda row: row.pop("missing_types", None),
            "is not one of": lambda row: row.pop("status") if row["tag"] == "Max_Speed" else None,
        }
        for expected, damage in defects.items():
            rows = [dict(r) for r in full_rows()]
            for row in rows:
                damage(row)
            statuses.write_bytes(tr.dumps_registry(registry(*rows)).encode("utf-8"))
            problems, _registry, _universe = tr.gate_problems(statuses, inventory, self.root)
            self.assertTrue(any(expected in problem for problem in problems), (expected, problems))
            self.assertFalse(any("KeyError" in problem or "cannot be rendered" in problem for problem in problems))
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(tr.main(args), 1)
            self.assertIn(expected, stderr.getvalue())
        self.assertEqual(tr.main(["render", "--statuses", str(statuses), "--inventory", str(inventory),
                                  "--out", str(self.root / "out" / "x.md")]), 1)

    def test_dump_round_trips(self) -> None:
        value = registry(*full_rows())
        self.assertEqual(json.loads(tr.dumps_registry(value)), value)
        self.assertEqual(tr.dumps_registry(copy.deepcopy(value)), tr.dumps_registry(value))


class CheckedInRegistryTests(unittest.TestCase):
    def test_gate(self) -> None:
        """The gate itself: the committed registry against the committed FoC tag inventory and the sources."""
        self.assertEqual(tr.main(["check"]), 0)

    def test_every_status_is_in_use_or_deliberately_empty(self) -> None:
        value = tr.load_registry(tr.DEFAULT_STATUSES)
        by_status, _ = tr.counts(value)
        for status in ("applied", "partial", "todo", "foc-ignores", "land-or-galactic"):
            self.assertGreater(by_status.get(status, 0), 0, status)

    def test_the_layer_z_adjust_class_of_bug_is_recorded(self) -> None:
        """#666: the tag reached the craft and not the ships, and the registry said `partial` instead of covered.
        Since #718 the start applies it to every object; a tag the sim still applies for one unit kind only is
        partial the same way (#843)."""
        value = tr.load_registry(tr.DEFAULT_STATUSES)
        rows = [row for row in value["rows"] if row["tag"] == "Layer_Z_Adjust" and "SpaceUnit" in row["classes"]]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["status"], "applied")
        partial = [row for row in value["rows"] if row["status"] == "partial"]
        self.assertTrue(partial)
        for row in partial:
            self.assertTrue(row["missing_types"])
            self.assertTrue(any(target.get("types") for target in row["applied"]))


if __name__ == "__main__":
    unittest.main()
