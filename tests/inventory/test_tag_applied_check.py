"""The tag perturbation check's plan and report, without game data (docs/tag-applied-check.md)."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))

import tag_applied_check as check  # noqa: E402

REGISTRY = {"rows": [
    # The #666 row before #718: applied for craft, missing for ships.
    {"tag": "SpaceUnit/Layer_Z_Adjust", "status": "partial", "types": ["craft"], "missing_types": ["ship"],
     "area": "movement", "ticket": 666, "check": {"perturb": "set:40"}},
    {"tag": "SpaceUnit/Max_Speed", "status": "applied", "area": "movement"},
    {"class": "GameConstants", "tag": "GameConstants/MaxRotationsSpace", "status": "applied", "area": "movement"},
    {"tag": "Faction/Color", "status": "applied", "area": "presentation"},
    {"tag": "SpaceUnit/Unread", "status": "todo", "area": "combat"},
    {"tag": "HardPoint/Health", "status": "applied", "types": "hardpoint", "area": "combat", "ticket": 650},
    {"tag": "SpaceUnit/Odd", "status": "applied", "types": ["ability"], "area": "combat"},
    {"tag": "SpaceUnit/Death", "status": "applied", "types": ["ship"], "area": "combat",
     "check": {"skip": "only a death applies it"}},
]}


def result(check_id: str, verdict: str, reason: str, **extra) -> dict:
    value = {"id": check_id, "verdict": verdict, "reason": reason, "objects": [], "without_tag": 0,
             "tables_changed": False, "first_tick": None, "kinds": {}, "ticks_run": 900, "workers_check": None,
             "seconds": 1.0}
    value.update(extra)
    return value


class PlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.checks = check.expand(check.registry_rows(REGISTRY))
        self.by = {(c["tag"], c["type"]): c for c in self.checks}

    def test_only_applied_and_partial_rows(self) -> None:
        self.assertNotIn(("SpaceUnit/Unread", "ship"), self.by)

    def test_partial_expects_its_types_and_not_the_missing(self) -> None:
        craft = self.by[("SpaceUnit/Layer_Z_Adjust", "craft")]
        ship = self.by[("SpaceUnit/Layer_Z_Adjust", "ship")]
        self.assertEqual((craft["expect"], ship["expect"]), ("changes", "no change"))
        self.assertTrue(craft["explicit"] and ship["explicit"])
        self.assertEqual(craft["change"], "set:40")

    def test_a_row_without_types_tries_every_type_of_its_class(self) -> None:
        types = [c["type"] for c in self.checks if c["tag"] == "SpaceUnit/Max_Speed"]
        self.assertEqual(types, list(check.UNIT_TYPES))
        self.assertFalse(self.by[("SpaceUnit/Max_Speed", "ship")]["explicit"])
        self.assertEqual([c["type"] for c in self.checks if c["tag"] == "GameConstants/MaxRotationsSpace"],
                         ["constants"])
        self.assertEqual(self.by[("HardPoint/Health", "hardpoint")]["expect"], "changes")

    def test_presets_are_not_run(self) -> None:
        self.assertIn("headless", self.by[("Faction/Color", "faction")]["preset"])
        self.assertIn("ability", self.by[("SpaceUnit/Odd", "ability")]["preset"])
        self.assertEqual(self.by[("SpaceUnit/Death", "ship")]["preset"], "only a death applies it")
        tsv = check.plan_tsv(self.checks).splitlines()
        self.assertEqual(tsv[0], "id\tclass\ttag\ttype\tchange")
        self.assertFalse(any("Faction/Color" in line or "SpaceUnit/Odd" in line for line in tsv))
        self.assertIn("\tSpaceUnit\tSpaceUnit/Layer_Z_Adjust\tship\tset:40", "\n".join(tsv))


class RegistrySchemaTests(unittest.TestCase):
    def test_the_registry_shape_expands_per_class_and_applied_types(self) -> None:
        rows = [{"tag": "Layer_Z_Adjust", "classes": ["SpaceUnit", "UniqueUnit"], "status": "partial",
                 "applied": [{"code": "src/units/unit_motion.cpp", "rules": ["LZ-01"], "types": ["craft"]}],
                 "missing_types": ["ship"], "area": "movement", "ticket": 666}]
        hints = check.load_hints(None)
        checks = check.expand(rows, hints)
        self.assertEqual([(c["class"], c["tag"], c["type"], c["expect"]) for c in checks], [
            ("SpaceUnit", "SpaceUnit/Layer_Z_Adjust", "craft", "changes"),
            ("SpaceUnit", "SpaceUnit/Layer_Z_Adjust", "ship", "no change"),
            ("UniqueUnit", "UniqueUnit/Layer_Z_Adjust", "craft", "changes"),
            ("UniqueUnit", "UniqueUnit/Layer_Z_Adjust", "ship", "no change"),
        ])

    def test_hints_file(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "hints.json"
            path.write_text(json.dumps({"hints": {"SpaceUnit/Layer_Z_Adjust": {"perturb": "set:40"},
                                                  "SpaceUnit/Death_Clone": {"skip": "a death"}}}), encoding="utf-8")
            hints = check.load_hints(path)
        rows = [{"tag": "layer_z_adjust", "classes": ["SpaceUnit"], "status": "applied",
                 "applied": [{"code": "x", "types": ["craft"]}]},
                {"tag": "Death_Clone", "classes": ["SpaceUnit"], "status": "applied", "applied": [{"code": "x"}]}]
        checks = check.expand(rows, hints)
        self.assertEqual(checks[0]["change"], "set:40")
        self.assertTrue(all(c.get("preset") == "a death" for c in checks[1:]))

    def test_code_targets_override_a_mislabelled_area(self) -> None:
        for code in ("apps/viewer/src/world_ui_view.cpp#bar", "src/presentation/draw.cpp", "src/scene/idle_tags.cpp"):
            rows = [{"tag": "Ship_Class", "classes": ["SpaceUnit"], "status": "applied", "area": "combat",
                     "applied": [{"code": code, "types": ["ship"]}]}]
            checks = check.expand(rows)
            self.assertIn("not checkable headless", checks[0]["preset"])
            self.assertEqual(len(check.plan_tsv(checks).splitlines()), 1)
            flat = check.expand([{"tag": "SpaceUnit/Ship_Class", "status": "applied", "code": code}])
            self.assertTrue(all("preset" in item for item in flat))


class ReportTests(unittest.TestCase):
    def setUp(self) -> None:
        self.checks = check.expand(check.registry_rows(REGISTRY))
        self.id = {(c["tag"], c["type"]): c["id"] for c in self.checks}

    def report(self, results: list[dict]) -> dict:
        return check.build_report(self.checks, {"ticks": 900, "seed": 1, "deterministic": True, "check_workers": 4},
                                  {r["id"]: r for r in results}, {"commit": "abc", "date": "d", "registry": "r"})

    def test_the_666_class_is_a_finding_either_way(self) -> None:
        lz_ship = self.id[("SpaceUnit/Layer_Z_Adjust", "ship")]
        lz_craft = self.id[("SpaceUnit/Layer_Z_Adjust", "craft")]
        # Before #718: ships unchanged, as the partial row says; no finding.
        before = self.report([result(lz_craft, "changes", "the battle differs from completed tick 10", first_tick=10),
                              result(lz_ship, "no change", "in the unit tables, the battle never differs")])
        self.assertEqual([f["tag"] for f in before["findings"]], [])
        # After #718: ships change, so the partial row is stale.
        after = self.report([result(lz_ship, "changes", "the battle differs from completed tick 1", first_tick=1)])
        self.assertEqual([(f["type"], f["finding"]) for f in after["findings"]],
                         [("ship", "changes on a type the registry says is missing")])

    def test_no_change_on_an_applied_type_is_a_finding(self) -> None:
        speed_ship = self.id[("SpaceUnit/Max_Speed", "ship")]
        report = self.report([result(speed_ship, "no change", "read, not applied: the unit tables are identical")])
        self.assertEqual([(f["tag"], f["type"], f["finding"]) for f in report["findings"]],
                         [("SpaceUnit/Max_Speed", "ship", "no change on an applied type")])

    def test_types_not_in_the_scene_drop_out_unless_named(self) -> None:
        station = self.id[("SpaceUnit/Max_Speed", "station")]
        report = self.report([result(station, "no change", "not exercised: the scene has no station of class SpaceUnit")])
        self.assertFalse(any(r["id"] == station for r in report["rows"]))
        self.assertEqual(report["findings"], [])
        # A row with no type in the scene keeps one line.
        report = self.report([result(self.id[("SpaceUnit/Max_Speed", t)], "no change",
                                     f"not exercised: the scene has no {t} of class SpaceUnit") for t in check.UNIT_TYPES])
        lz = [r for r in report["rows"] if r["tag"] == "SpaceUnit/Max_Speed"]
        self.assertEqual([r["verdict"] for r in lz], ["not in scene"])

    def test_not_exercised_is_not_a_finding(self) -> None:
        craft = self.id[("SpaceUnit/Max_Speed", "craft")]
        report = self.report([result(craft, "no change", "not exercised: no craft in the scene authors or inherits it")])
        self.assertEqual(report["findings"], [])
        self.assertEqual(report["counts"].get("not exercised"), 1)

    def test_worker_divergence_is_a_finding(self) -> None:
        speed_ship = self.id[("SpaceUnit/Max_Speed", "ship")]
        report = self.report([result(speed_ship, "changes", "x", workers_check="diverged at 4 workers, tick 7")])
        self.assertEqual(report["findings"][0]["finding"], "worker divergence")

    def test_markdown_and_one_draft_per_area(self) -> None:
        speed_ship = self.id[("SpaceUnit/Max_Speed", "ship")]
        speed_craft = self.id[("SpaceUnit/Max_Speed", "craft")]
        health = self.id[("HardPoint/Health", "hardpoint")]
        objects = [{"id": "Nebulon_B_Frigate", "from": ["2.2"], "to": ["4.4"], "read": True}]
        report = self.report([
            result(speed_ship, "no change", "read and dropped", objects=objects),
            result(speed_craft, "no change", "not read"),
            result(health, "no change", "not read"),
        ])
        text = check.render_report(report)
        self.assertIn("## Findings (3)", text)
        self.assertIn("Nebulon_B_Frigate 2.2 -> 4.4", text)
        drafts = check.issue_drafts(report)
        self.assertEqual(sorted(drafts), ["combat", "movement"])
        key, title, body = drafts["movement"]
        self.assertEqual(key, "area movement")
        self.assertEqual(title, "Tag applied-check: movement: 1 contradicted, 1 unproven claims")
        self.assertIn("<!-- tag-applied-check area movement -->", body)
        self.assertIn("Tracking: #649, #721, #791.", body)
        self.assertIn("#650", drafts["combat"][2])

    def test_results_file_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "results.jsonl"
            path.write_text(json.dumps({"baseline": {"ticks": 5, "deterministic": True}}) + "\n"
                            + json.dumps(result("c0001", "changes", "x")) + "\n", encoding="utf-8")
            baseline, results = check.read_results(path)
            self.assertEqual(baseline["ticks"], 5)
            self.assertEqual(results["c0001"]["verdict"], "changes")

    def test_no_baseline_node_never_contradicts_a_read(self) -> None:
        key = self.id[("SpaceUnit/Max_Speed", "ship")]
        report = self.report([result(key, "no change", "not authored; read unknown",
                                     objects=[{"id": "a", "from": [], "to": ["40"], "read": None}])])
        self.assertEqual(report["findings"][0]["evidence"], "unproven")
        body = check.issue_drafts(report)["movement"][2]
        self.assertIn("### Unproven", body)
        self.assertNotIn("### Contradicted", body)

    def test_changed_tables_are_unproven_until_the_battle_changes(self) -> None:
        key = self.id[("SpaceUnit/Max_Speed", "ship")]
        report = self.report([result(key, "no change", "in the unit tables", tables_changed=True,
                                     objects=[{"id": "a", "from": ["1"], "to": ["2"], "read": True}])])
        self.assertEqual(report["findings"][0]["evidence"], "unproven")
        self.assertIn("| unproven |", check.render_report(report))

    def test_fingerprint_ignores_run_date_order_and_window(self) -> None:
        entries = [dict(c, finding="no change on an applied type", evidence="unproven") for c in self.checks[:2]]
        digest = check.finding_fingerprint(entries)
        entries[0].update(reason="a later window", ticks_run=9000)
        self.assertEqual(digest, check.finding_fingerprint(list(reversed(entries))))
        entries[0]["evidence"] = "contradicted"
        self.assertNotEqual(digest, check.finding_fingerprint(entries))

    def test_latest_comment_marker_wins_over_issue_body(self) -> None:
        old, new = "a" * 64, "b" * 64
        client = mock.Mock(repo="owner/repo")
        client._request.return_value = [{"body": f"<!-- tag-applied-findings {new} -->"}]
        self.assertEqual(check.latest_fingerprint(client, 1, f"<!-- tag-applied-findings {old} -->"), new)

    def test_unchanged_nightly_never_comments(self) -> None:
        key = self.id[("SpaceUnit/Max_Speed", "ship")]
        report = self.report([result(key, "no change", "in the unit tables", tables_changed=True)])
        body = check.issue_drafts(report)["movement"][2]
        client = mock.Mock(repo="owner/repo")
        client._request.side_effect = [{"items": [{"number": 12, "body": body}]}, []]
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            (out / "report.json").write_text(json.dumps(report), encoding="utf-8")
            with mock.patch.dict(sys.modules, {"soak_issues": mock.Mock(client_from_environment=lambda: client)}):
                self.assertEqual(check.issues_command(type("Args", (), {"out": out, "file": True})()), 0)
        client.comment.assert_not_called()
        client.create.assert_not_called()


if __name__ == "__main__":
    unittest.main()
