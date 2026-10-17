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
import tag_registry_seed as seed  # noqa: E402


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
        (self.root / "docs" / "behaviour" / "movement.md").write_text("## MV-11 a rule\n", encoding="utf-8")
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
    def test_campaign_scene_requires_reviewed_sourced_consumers(self) -> None:
        rows = full_rows()
        row = next(row for row in rows if row["tag"] == "Max_Speed")
        row.update(scene="campaign", basis="reviewed", note="Campaign scene application only.")
        self.assertEqual(self.problems(registry(*rows)), "")
        for change in ({"scene": "skirmish"}, {"basis": "auto"}, {"basis": None},
                       {"status": "partial"}, {"status": "todo"}, {"note": ""},
                       {"applied": []}, {"applied": [applied(rules=[])]}):
            with self.subTest(change=change):
                changed = [{**item, **change} if item is row else item for item in rows]
                self.assertIn("scene must be campaign", self.problems(registry(*changed)))
        # Scope cannot hide a dead source or an undeclared rule.
        row["applied"] = [applied("absent_identifier")]
        self.assertIn("'absent_identifier' is not in", self.problems(registry(*rows)))
        row["applied"] = [applied(rules=["WGCH-999999"])]
        self.assertIn("WGCH-999999", self.problems(registry(*rows)))

    def test_identifier_matches_whole_words_outside_comments(self) -> None:
        value = registry(*full_rows())
        for body in ("void longer_max_speed() {}", "void max_speed_extra() {}",
                     "// max_speed\nvoid other_code() {}", "/* max_speed */ void other_code() {}",
                     "int count = 1'000; // max_speed ' ignored",
                     "// continued \\\nmax_speed\nvoid other_code() {}"):
            with self.subTest(body=body):
                (self.root / CODE).write_text(body + "\nint layer_z;", encoding="utf-8")
                self.assertIn("'max_speed' is not in", self.problems(value))
        for body in ('void max_speed() {}', 'const char* url = "https://host"; int max_speed;',
                     'const char* raw = R"tag(/* //)tag"; int max_speed;',
                     'int max_speed/* comment */;'):
            with self.subTest(body=body):
                (self.root / CODE).write_text(body + "\nint layer_z;", encoding="utf-8")
                self.assertEqual(self.problems(value), "")
        self.assertEqual(tr.uncomment_cpp("max_/* comment */speed"), "max_ speed")

    def test_generic_and_short_identifiers_require_specific_targets(self) -> None:
        for identifier in ("own", "id", "name", "list", "node", "box", "rules", "curve", "short", "RULES", "result", "config", "buffer", "number", "params"):
            rows = full_rows()
            rows[0]["applied"] = [applied(identifier)]
            self.assertIn("TODO code location", self.problems(registry(*rows), root=False))
        rows = full_rows()
        rows[0]["applied"] = [applied("apply_motion")]
        (self.root / CODE).write_text("void apply_motion() {} int max_speed; int layer_z;", encoding="utf-8")
        self.assertEqual(self.problems(registry(*rows)), "")

    def test_rule_ids_require_declarations(self) -> None:
        (self.root / "docs/behaviour/movement.md").write_text(
            "Prose references XX-99 and CRC-32.\n"
            "## MV-11 movement\n"
            "### C-08: UTF-16 units\n"
            "- **AV-05** (research example) footprint\n"
            "- AV-12: step costs\n"
            "* __UA-07 Death clone.__ example\n"
            "1. `SP-02` death completion\n"
            "**BOLD-01** paragraph declaration\n"
            "| Rule | Meaning |\n| --- | --- |\n"
            "| **TAB-02** | references REF-03 |\n"
            "| `TAB-04` | hash SHA-256 |\n"
            "| Logical path | SHA-256 |\n"
            "```\n## FAKE-01\n| FAKE-02 | example |\n```\n", encoding="utf-8")
        self.assertEqual(tr.known_rules(self.root), {"MV-11", "TAB-02", "TAB-04", "C-08", "AV-05", "AV-12", "UA-07", "SP-02", "BOLD-01"})
        rows = full_rows()
        rows[0]["applied"][0]["rules"] = ["XX-99", "CRC-32", "SHA-256", "REF-03", "UTF-16"]
        problems = self.problems(registry(*rows))
        for identifier in rows[0]["applied"][0]["rules"]:
            self.assertIn(f"rule {identifier} is in no docs/behaviour note", problems)

    def test_missing_identifier_reports_other_source_files(self) -> None:
        (self.root / CODE).write_text("int layer_z; // max_speed", encoding="utf-8")
        moved = self.root / "src/sim/moved.cpp"
        moved.write_text("int max_speed;", encoding="utf-8")
        (self.root / "src/sim/comment.cpp").write_text("// max_speed", encoding="utf-8")
        (self.root / "src/sim/substring.cpp").write_text("int max_speed_extra;", encoding="utf-8")
        problems = self.problems(registry(*full_rows()))
        self.assertIn("also found in: src/sim/moved.cpp", problems)
        self.assertNotIn("comment.cpp", problems)
        self.assertNotIn("substring.cpp", problems)
        (self.root / CODE).unlink()
        self.assertIn("also found in: src/sim/moved.cpp", self.problems(registry(*full_rows())))

    def test_unresolved_notes_are_reported_by_the_gate(self) -> None:
        directory = self.root / "docs/tag-coverage"
        directory.mkdir(parents=True)
        statuses = directory / "statuses.json"
        inventory = self.root / "tags.json"
        inventory.write_text(json.dumps(INVENTORY), encoding="utf-8")
        rows = full_rows()
        rows[0]["note"] = "TODO #855: identify the declared movement rule."
        statuses.write_text(tr.dumps_registry(registry(*rows)), encoding="utf-8")
        output = io.StringIO()
        from unittest.mock import patch
        # Exercise diagnostic reporting independently of the production zero-TODO policy.
        with patch.object(tr, "TODO_NOTE_CEILING", 1), contextlib.redirect_stdout(output):
            result = tr.main(["check", "--statuses", str(statuses), "--inventory", str(inventory),
                              "--repo", str(self.root)])
        self.assertEqual(result, 0)
        self.assertIn("TODO row 'Max_Speed' ['SpaceUnit']: identify the declared movement rule", output.getvalue())

    def test_todo_note_ceiling_and_tracking_ticket(self) -> None:
        rows = full_rows()
        rows[0]["note"] = "TODO: identify the declared movement rule."
        self.assertIn("TODO note needs a tracking ticket", self.problems(registry(*rows)))
        rows[0]["note"] = "TODO #2566: identify the declared movement rule."
        from unittest.mock import patch
        with patch.object(tr, "TODO_NOTE_CEILING", 1):
            self.assertEqual(self.problems(registry(*rows)), "")
            rows[1]["note"] = "TODO #2566: identify the declared layer rule."
            self.assertIn("TODO note count 2 exceeds ceiling 1", self.problems(registry(*rows)))
            rows[0].pop("note")
            self.assertEqual(self.problems(registry(*rows)), "")
        with patch.object(tr, "TODO_NOTE_CEILING", 0):
            self.assertIn("TODO note count 1 exceeds ceiling 0", self.problems(registry(*rows)))

    def test_unauthored_engine_default_requires_catalog_owner_tag_and_evidence(self) -> None:
        catalog = self.root / "docs/tag-coverage/engine-tags.json"
        catalog.parent.mkdir(parents=True)
        catalog.write_text(json.dumps({"schema_version": 1, "evidence": {"ET-T053": "debug build"},
            "rows": [{"tag": "Turret_XY_Only", "table_kind": "object-definition",
                      "value_type": "boolean", "evidence": "ET-T053"}]}), encoding="utf-8")
        row = {"tag": "Turret_XY_Only", "classes": ["SpaceUnit"], "status": "applied", "area": "combat",
               "applied": [applied()], "engine_default": "GameObjectType", "evidence": "ET-T053"}
        value = registry(*full_rows(), row, evidence={"DB-NOTAG": "debug build", "ET-T053": "debug build catalog"})
        self.assertEqual(self.problems(value), "")
        for field, replacement in (("tag", "Unknown_Turret_Flag"), ("engine_default", "GameConstants"),
                                   ("evidence", "DB-NOTAG"), ("status", "todo")):
            changed = copy.deepcopy(value)
            target = next(r for r in changed["rows"] if r["tag"] == "Turret_XY_Only")
            target[field] = replacement
            changed["rows"].sort(key=tr.row_order)
            self.assertIn("engine default needs", self.problems(changed))

    def test_explicit_trace_scopes_are_validated(self) -> None:
        for scope in ({"ability_types": ["CORRUPT_SYSTEMS"]},
                      {"object_types": ["Cloaking_Upgrade"]},
                      {"exclude_behaviors": ["MULTIPLAYER_BEACON"]},
                      {"exclude_space_behaviors": ["SPECIAL_WEAPON"]}):
            rows = full_rows()
            rows[0]["applied"][0]["trace_scope"] = scope
            self.assertEqual(self.problems(registry(*rows)), "")
        for scope in ({}, "craft", {"unknown": ["X"]}, {"ability_types": []},
                      {"ability_types": "X"}, {"ability_types": [1]},
                      {"ability_types": ["X", "X"]}, {"ability_types": [" "]},
                      {"exclude_behaviors": []}, {"exclude_behaviors": ["X", "X"]}):
            with self.subTest(scope=scope):
                rows = full_rows()
                rows[0]["applied"][0]["trace_scope"] = scope
                self.assertIn("trace_scope", self.problems(registry(*rows)))

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

    def test_a_by_design_row_cites_a_rule_that_exists(self) -> None:
        rows = [r for r in full_rows() if r["tag"] != "Max_Speed"]
        evidence = {"DB-NOTAG": "debug build: no tag table row carries the name",
                    "REPLACED": "MV-11 says our code replaces it", "NO-RULE": "our code replaces it",
                    "GONE": "XX-99 says our code replaces it"}

        def by_design(source: str) -> dict:
            return {"tag": "Max_Speed", "classes": ["SpaceUnit"], "status": "by-design", "area": "movement",
                    "evidence": source}
        self.assertEqual(self.problems(registry(*rows, by_design("REPLACED"), evidence=evidence)), "")
        self.assertIn("cites the rule ID", self.problems(registry(*rows, by_design("NO-RULE"), evidence=evidence)))
        self.assertIn("rule XX-99 is in no docs/behaviour note",
                      self.problems(registry(*rows, by_design("GONE"), evidence=evidence)))
    def test_a_tactical_applied_row_must_cite_code_outside_auto_resolve(self) -> None:
        autos = ["src/campaign/auto_resolve_strength.cpp", "include/eawr/campaign/auto_resolve_strength.hpp"]
        for auto in autos:
            (self.root / auto).parent.mkdir(parents=True, exist_ok=True)
            (self.root / auto).write_text(CODE_TEXT, encoding="utf-8")
        rows = [r for r in full_rows() if r["tag"] != "Max_Speed"]

        def row(classes: list[str], codes: list[dict], status: str = "applied", **extra) -> dict:
            return {"tag": "Max_Speed", "classes": classes, "status": status, "area": "movement",
                    "applied": codes, **extra}
        auto_only = [{"code": f"{autos[0]}#max_speed", "rules": ["MV-11"]}]
        header_only = [{"code": f"{autos[1]}#max_speed", "rules": ["MV-11"]}]
        for codes in (auto_only, header_only):
            self.assertIn("tactical class SpaceUnit cites only auto-resolve code",
                          self.problems(registry(*rows, row(["SpaceUnit"], codes))))
        self.assertIn("tactical class HeroUnit cites only auto-resolve code",
                      self.problems(registry(*rows, row(["HeroUnit"], auto_only))))
        partial = row(["SpaceUnit"], [dict(auto_only[0], types=["craft"])], "partial", ticket=666, missing_types=["ship"])
        self.assertIn("cites only auto-resolve code", self.problems(registry(*rows, partial)))
        self.assertEqual(self.problems(registry(*rows, row(["SpaceUnit"], auto_only + [applied()]))), "")
        self.assertEqual(self.problems(registry(*rows, row(["SpaceUnit"], header_only + [applied()]))), "")
        self.assertEqual(self.problems(registry(*rows, row(["GameConstants"], auto_only)), universe=False), "")
        self.assertEqual(self.problems(registry(*rows, row(["GroundInfantry"], auto_only)), universe=False), "")

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


class ScopeOfNameTests(unittest.TestCase):
    """The seed's name-based scope rule: a tactical class keeps the tag todo, a pure ground class does not."""

    def test_tactical_classes_drop_the_name_scope_and_keep_menu_scope(self) -> None:
        self.assertIsNone(seed.scope_of_name("Hyperspace", "Hyperspace", "SpaceUnit"))
        self.assertIsNone(seed.scope_of_name("Land_FOW_Reveal_Range", "Land_FOW_Reveal_Range", "HeroUnit"))
        self.assertEqual(seed.scope_of_name("Garrison_Radius", "Garrison_Radius", "UniqueUnit")[2], "SCOPE-LAND")
        self.assertEqual(seed.scope_of_name("Hyperspace", "Hyperspace", "GroundCompany")[2], "SCOPE-GALACTIC")
        self.assertEqual(seed.scope_of_name("Piracy_Bonus", "Piracy_Bonus", "GroundCompany")[2], "SCOPE-GALACTIC")
        self.assertEqual(seed.scope_of_name("Galactic_Movie_Credits", "Galactic_Movie_Credits", "SpaceUnit")[2],
                         "SCOPE-MENU")

    def test_galactic_map_fields_stay_galactic_on_every_class(self) -> None:
        for cls in ("SpaceUnit", "Squadron", "HeroUnit"):
            self.assertEqual(seed.scope_of_name("Piracy_Value_Credits", "Piracy_Value_Credits", cls)[2], "SCOPE-GALACTIC")

    def test_the_gate_and_the_seed_share_one_class_list(self) -> None:
        self.assertIs(seed.TACTICAL_CLASSES, tr.TACTICAL_CLASSES)


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


class ShardTests(Fixture):
    def setUp(self) -> None:
        super().setUp()
        self.statuses = self.root / "statuses.json"
        self.galactic = self.root / "statuses-galactic.json"
        self.inventory = self.root / "tags.json"
        self.inventory.write_text(json.dumps(INVENTORY), encoding="utf-8")
        rows = full_rows()
        self.main = registry(*rows[:3])
        self.main["shards"] = [self.galactic.name]
        self.child = registry(*rows[3:])
        self.save()

    def save(self) -> None:
        tr.write_text(self.statuses, tr.dumps_registry(self.main))
        tr.write_text(self.galactic, tr.dumps_registry(self.child))

    def gate(self) -> list[str]:
        return tr.gate_problems(self.statuses, self.inventory, self.root)[0]

    def test_split_preserves_coverage_counts_and_report(self) -> None:
        whole = registry(*full_rows())
        combined = tr.load_registry(self.statuses)
        self.assertEqual(combined, whole)
        self.assertEqual(self.gate(), [])
        self.assertEqual(tr.render(combined, self.universe), tr.render(whole, self.universe))
        # A list of independent roots is equivalent to the referenced split.
        self.main.pop("shards")
        self.save()
        self.assertEqual(tr.load_registry([self.statuses, self.galactic]), whole)
        self.assertEqual(tr.main(["check", "--statuses", str(self.statuses), "--statuses", str(self.galactic),
                                  "--inventory", str(self.inventory), "--repo", str(self.root)]), 0)

    def test_unreferenced_evidence_survives_loading_writing_and_formatting(self) -> None:
        self.main["evidence"]["UNUSED-ROOT"] = "debug build: retained root evidence"
        self.child["evidence"]["UNUSED-CHILD"] = "debug build: retained shard evidence"
        self.save()
        expected = {**self.main["evidence"], **self.child["evidence"]}
        combined = tr.load_registry(self.statuses)
        self.assertEqual(combined["evidence"], expected)
        tr.write_registry(self.statuses, combined)
        self.assertEqual(tr.load_registry(self.statuses)["evidence"], expected)
        self.assertEqual(tr.main(["format", "--statuses", str(self.statuses)]), 0)
        self.assertEqual(tr.load_registry(self.statuses)["evidence"], expected)
        self.assertEqual(tr.load_shard(self.statuses)["evidence"], self.main["evidence"])
        self.assertEqual(tr.load_shard(self.galactic)["evidence"], self.child["evidence"])

    def test_duplicate_and_missing_pairs_cross_shards(self) -> None:
        self.child["rows"].append(copy.deepcopy(self.main["rows"][0]))
        self.child["rows"].sort(key=tr.row_order)
        self.save()
        self.assertTrue(any("is also in the row" in p for p in self.gate()))
        self.child["rows"] = []
        self.save()
        self.assertTrue(any("has no registry row" in p for p in self.gate()))

    def test_each_shard_is_canonical_and_format_preserves_ownership(self) -> None:
        self.child["rows"].reverse()
        self.galactic.write_text(json.dumps(self.child), encoding="utf-8")
        self.assertTrue(any("statuses-galactic.json" in p and "canonical" in p for p in self.gate()))
        self.assertTrue(any("rows are not sorted" in p for p in self.gate()))
        before = self.statuses.read_bytes()
        self.assertEqual(tr.main(["format", "--statuses", str(self.statuses)]), 0)
        self.assertEqual(self.statuses.read_bytes(), before)
        self.assertEqual(self.gate(), [])

    def test_missing_repeated_and_cyclic_shards_fail(self) -> None:
        self.galactic.unlink()
        with self.assertRaises(OSError):
            tr.load_registry(self.statuses)
        self.save()
        self.main["shards"] *= 2
        self.save()
        with self.assertRaisesRegex(tr.RegistryError, "repeated"):
            tr.load_registry(self.statuses)
        self.main["shards"] = [self.galactic.name]
        self.child["shards"] = [self.statuses.name]
        self.save()
        with self.assertRaisesRegex(tr.RegistryError, "cycle"):
            tr.load_registry(self.statuses)

    def test_conflicting_evidence_and_unknown_shard_fields_fail(self) -> None:
        self.child["evidence"]["DB-NOTAG"] = "different evidence"
        self.save()
        with self.assertRaisesRegex(tr.RegistryError, "conflicting evidence"):
            tr.load_registry(self.statuses)
        self.child = registry(*full_rows()[3:])
        self.child["unexpected"] = True
        # dumps_registry intentionally only emits recognized fields.
        self.galactic.write_text(json.dumps(self.child), encoding="utf-8")
        with self.assertRaisesRegex(tr.RegistryError, "unknown top-level"):
            tr.load_registry(self.statuses)

    def test_update_preserves_shard_ownership_and_new_rows_go_to_root(self) -> None:
        old_main = {tr.pair_key(c, t) for c, t, _r in tr.expand(self.main)}
        old_child = {tr.pair_key(c, t) for c, t, _r in tr.expand(self.child)}
        combined = tr.load_registry(self.statuses)
        for row in combined["rows"]:
            if row["tag"] == "Abilities/Stun_Ability/Stun_Range":
                row["ticket"] = 777
        combined["rows"].append(dict(tag="New_Tag", classes=["Planet"], status="todo", area="data", ticket=888))
        tr.write_registry(self.statuses, combined)
        main, child = tr.load_shard(self.statuses), tr.load_shard(self.galactic)
        self.assertEqual({tr.pair_key(c, t) for c, t, _r in tr.expand(child)}, old_child)
        self.assertEqual({tr.pair_key(c, t) for c, t, _r in tr.expand(main)}, old_main | {("planet", "new_tag")})
        self.assertEqual(main["shards"], [self.galactic.name])
        self.assertEqual(next(r for r in child["rows"] if r["tag"].startswith("Abilities/"))["ticket"], 777)


class CheckedInRegistryTests(unittest.TestCase):
    def test_galactic_shard_owns_campaign_planet_and_trade_route(self) -> None:
        root = tr.load_shard(tr.DEFAULT_STATUSES)
        child = tr.load_shard(tr.DEFAULT_STATUSES.with_name("statuses-galactic.json"))
        self.assertEqual(root["shards"], ["statuses-galactic.json"])
        classes = {"Campaign", "Planet", "TradeRoute"}
        self.assertFalse(any(classes.intersection(row["classes"]) for row in root["rows"]))
        self.assertTrue(classes.issubset({c for c, _t, _r in tr.expand(child)}))
        registry = tr.load_registry(tr.DEFAULT_STATUSES)
        universe = tr.load_universe(tr.load_inventory(tr.DEFAULT_INVENTORY))
        registered = {tr.pair_key(c, tag) for c, tag, _row in tr.expand(registry)}
        # The tactical registry also reviews the two mod-authorable turret tags
        # absent from the pinned authored stock inventory. Retain exact coverage
        # and reject any other unexplained extra class/tag pair.
        extra = {tr.pair_key(c, tag) for c in ("SpaceBuildable", "SpaceUnit", "SpecialStructure")
                 for tag in ("Turret_Rotate_To_Rest_Delay_Sec", "Turret_XY_Only")}
        self.assertEqual(registered - set(universe), extra)
        self.assertEqual(sum(tr.counts(registry)[0].values()), len(universe) + len(extra))

    def test_gate(self) -> None:
        """The gate itself: the committed registry against the committed FoC tag inventory and the sources."""
        self.assertEqual(tr.main(["check"]), 0)

    def test_every_status_is_in_use_or_deliberately_empty(self) -> None:
        value = tr.load_registry(tr.DEFAULT_STATUSES)
        by_status, _ = tr.counts(value)
        for status in ("applied", "partial", "todo", "foc-ignores", "land-or-galactic", "by-design"):
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
