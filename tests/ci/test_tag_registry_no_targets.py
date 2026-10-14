"""Per-kind exclusions must be sourced and cannot conceal an applied consumer."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/inventory"))
import tag_registry as registry
import tag_report


class NoTargetTests(unittest.TestCase):
    def fixture(self):
        return {"schema_version": 2, "evidence": {"MT-01": "debug build: ship flight never reads the value"},
                "rows": [{"tag": "Flight_Tag", "classes": ["SpaceUnit"], "area": "movement", "status": "applied",
                          "applied": [{"code": "src/flight.cpp#flight_step", "types": ["craft"], "rules": ["MT-01"]}],
                          "no_targets": [{"types": ["ship"], "status": "foc-ignores", "evidence": "MT-01"}]}]}

    def problems(self, value):
        return "\n".join(registry.registry_problems(value, None, None))

    def test_valid_exclusion_and_old_rows(self):
        value = self.fixture()
        self.assertEqual(self.problems(value), "")
        del value["rows"][0]["no_targets"]
        self.assertEqual(self.problems(value), "")

    def test_inline_debug_evidence_is_accepted(self):
        value = self.fixture()
        value["rows"][0]["no_targets"][0]["evidence"] = "debug build: no ordinary ship-flight consumer"
        self.assertEqual(self.problems(value), "")

    def test_missing_application_still_reports_the_existing_gate_error(self):
        value = self.fixture()
        value["rows"][0]["applied"] = None
        self.assertIn("lists the code that applies it", self.problems(value))

    def test_malformed_exclusions_are_rejected(self):
        cases = [([], "nonempty array"), ({}, "nonempty array"), ([None], "entry needs"),
                 ([{"types": ["ship"], "status": "foc-ignores"}], "entry needs")]
        for exclusions, diagnostic in cases:
            with self.subTest(exclusions=exclusions):
                value = self.fixture()
                value["rows"][0]["no_targets"] = exclusions
                self.assertIn(diagnostic, self.problems(value))

    def test_invalid_kinds_status_and_evidence_are_rejected(self):
        cases = [("types", [], "distinct unit kinds"), ("types", ["unknown"], "distinct unit kinds"),
                 ("types", [None], "distinct unit kinds"), ("types", ["ship", "ship"], "distinct unit kinds"),
                 ("types", ["station", "ship"], "distinct unit kinds"),
                 ("status", "deferred", "status must be foc-ignores"),
                 ("evidence", "", "evidence must be nonempty"),
                 ("evidence", None, "evidence must be nonempty"),
                 ("evidence", "MISSING", "not defined"),
                 ("evidence", "stock authoring only", "cite the debug build")]
        for field, replacement, diagnostic in cases:
            with self.subTest(field=field, replacement=replacement):
                value = self.fixture()
                value["rows"][0]["no_targets"][0][field] = replacement
                self.assertIn(diagnostic, self.problems(value))

    def test_overlap_and_unsourced_reference_are_rejected(self):
        for change in ("applied", "whole", "duplicate", "source", "missing", "todo"):
            with self.subTest(change=change):
                value = self.fixture()
                row = value["rows"][0]
                if change == "applied":
                    row["applied"][0]["types"] = ["ship"]
                elif change == "whole":
                    row["applied"][0]["types"] = []
                elif change == "duplicate":
                    row["no_targets"].append(copy.deepcopy(row["no_targets"][0]))
                elif change == "source":
                    value["evidence"]["MT-01"] = "stock authoring only"
                elif change == "missing":
                    row.update(status="partial", ticket=843, missing_types=["ship"])
                else:
                    row.update(status="todo", ticket=843)
                self.assertIn({"source": "cite the debug build", "todo": "only an applied or partial row"}.get(
                    change, "overlap"), self.problems(value))

    def test_separate_missing_consumer_does_not_hide_flight_exclusion(self):
        value = self.fixture()
        value["rows"][0].update(status="partial", ticket=843,
                               missing_types=["ship attack positioning with Keep_Moving_In_Battle"])
        self.assertEqual(self.problems(value), "")
        rendered = tag_report.render(value)["movement.md"]
        self.assertIn("ship: foc-ignores", rendered)
        self.assertIn("missing: ship attack positioning with Keep_Moving_In_Battle", rendered)

    def test_report_exposes_resolved_evidence_and_kind(self):
        value = self.fixture()
        value["evidence"]["MT-01"] += " | [source]"
        rendered = tag_report.render(value)["movement.md"]
        self.assertIn("ship: foc-ignores", rendered)
        self.assertIn("ship: debug build: ship flight never reads the value &#124; &#91;source&#93;", rendered)
        self.assertNotIn("missing: ship", rendered)


if __name__ == "__main__":
    unittest.main()
