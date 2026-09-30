#!/usr/bin/env python3
"""Focused negative controls for the metadata-only catalog evaluator."""

from __future__ import annotations

import copy
import hashlib
import json
import unittest

from tools.inventory.xml_acceptance import CATEGORIES, ROOTS, aggregate_p0_pass, evaluate, identity


def fixture(profile: str = "eaw") -> tuple[dict, dict, dict]:
    roots = []
    includes = []
    physical = []
    for category, path in ROOTS:
        source = f"base:loose:{path}"
        roots.append(dict(category=category, logical_path=path, outcome="loaded",
                          source_id=source, layer="base", sha256="a" * 64))
        physical.append(dict(logical_path=path, source_id=source, layer="base",
                             active_registry_file=True, parsed=True))
        name = ("data/xml/units/space/units_space_neutral_freighters.xml"
                if profile == "remake" and category == "game_objects" else f"data/xml/{category}.xml")
        source = f"base:loose:{name}"
        includes.append(dict(category=category, registry_path=path, logical_path=name,
                             include_order=0, outcome="loaded", source_id=source,
                             layer="base", sha256="b" * 64))
        physical.append(dict(logical_path=name, source_id=source, layer="base",
                             active_registry_file=True, parsed=True))
    diagnostics = []
    missing = []
    if profile == "eaw":
        path = "data/xml/cin_missing.xml"
        includes.insert(1, dict(category="game_objects", registry_path=ROOTS[0][1],
                                logical_path=path, include_order=1, outcome="missing",
                                source_id=None, layer=None, sha256=None))
        missing = [path]
        diagnostics.append(dict(code="EAWR-XML-0004", logical_path=path, line=None,
                                source_id=None, severity="error"))
    categories = {category: dict(raw=1, winners=1) for category in CATEGORIES}
    unresolved = []
    exception = []
    if profile == "remake":
        path = includes[0]["logical_path"]
        unresolved = [dict(code="EAWR-XML-0009", logical_path=path, line=389,
                           severity="error", source_id=includes[0]["source_id"],
                           message="variant cycle: Freighter_Acclamator_E -> Freighter_Acclamator_E")]
        exception = [dict(logical_path=path, line=389,
                          chain=["Freighter_Acclamator_E", "Freighter_Acclamator_E"],
                          input_sha256="b" * 64)]
    report = dict(schema_version=1, schema_revision="revision", profile=profile,
                  counts=dict(registry_includes=len(includes), physical_xml_records=len(physical),
                              definitions=6, diagnostics=len(diagnostics), unresolved=len(unresolved),
                              by_category=categories),
                  physical_inventory=physical, diagnostics=diagnostics, unresolved=unresolved,
                  registries=[dict(category=r["category"], registry_path=r["registry_path"],
                                   included_path=r["logical_path"], include_order=r["include_order"],
                                   loaded=r["outcome"] == "loaded", source_id=r["source_id"])
                              for r in includes])
    receipt = dict(schema_version=1, schema_revision="revision", profile=profile,
                   identity_algorithm="sha256-length-framed-v1", roots=roots, includes=includes)
    receipt["identity_sha256"] = identity(receipt)
    selected_profile = dict(profile=profile, profile_p0_pass=profile != "eaw",
                   input_receipt_identity_sha256=receipt["identity_sha256"],
                   registry_includes=len(includes), physical_xml_records=len(physical),
                   definitions=6, categories=categories,
                   diagnostics=dict(malformed=0, missing_registry_include=len(missing),
                                    duplicate_id=0, unknown=0, deprecated=0),
                   unresolved=len(unresolved))
    profiles = [selected_profile if p == profile else dict(profile=p, profile_p0_pass=p != "eaw")
                for p in ("eaw", "foc", "remake")]
    summary = dict(schema_revision="revision", evaluator_version=2, full_pass=False,
                   profiles=profiles,
                   registry_contract=dict(registry_file_sha256={profile: {
                       category: "a" * 64 for category, _ in ROOTS}}),
                   malformed_physical_xml={profile: []},
                   required_failures=dict(eaw_missing_registry_includes=missing,
                                          remake_variant_cycles=exception))
    return report, receipt, summary


class EvaluatorTests(unittest.TestCase):
    def test_profile_and_project_scope(self) -> None:
        results = []
        for profile in ("eaw", "foc", "remake"):
            with self.subTest(profile=profile):
                report, receipt, summary = fixture(profile)
                result = evaluate(profile, report, receipt, summary)
                self.assertEqual(result["profile_p0_pass"], profile != "eaw")
                results.append(result)
        self.assertFalse(aggregate_p0_pass(results))
        with self.assertRaises(ValueError):
            aggregate_p0_pass(results[1:])
        with self.assertRaises(ValueError):
            aggregate_p0_pass([results[1], results[1], results[2]])

    def test_missing_include_evidence_joins_receipt_and_diagnostic(self) -> None:
        report, receipt, summary = fixture()
        result = evaluate("eaw", report, receipt, summary)
        row = result["missing_include_dispositions"][0]
        self.assertEqual(row["logical_path"], receipt["includes"][1]["logical_path"])
        self.assertEqual(row["registry_sha256"], receipt["roots"][0]["sha256"])
        self.assertEqual(row["registry_source_id_sha256"],
                         hashlib.sha256(receipt["roots"][0]["source_id"].encode()).hexdigest())
        self.assertEqual(row["diagnostic_code"], report["diagnostics"][0]["code"])
        self.assertNotIn("source_id", row)
        self.assertNotIn("base:loose:", json.dumps(result))

    def test_approved_cycle_evidence_joins_exact_input(self) -> None:
        report, receipt, summary = fixture("remake")
        result = evaluate("remake", report, receipt, summary)
        row = result["approved_cycle_dispositions"][0]
        self.assertTrue(result["profile_p0_pass"])
        self.assertEqual(row["input_sha256"], receipt["includes"][0]["sha256"])
        self.assertEqual(row["chain"], summary["required_failures"]["remake_variant_cycles"][0]["chain"])
        self.assertEqual(row["diagnostic_code"], report["unresolved"][0]["code"])
        self.assertNotIn("source_id", row)

    def test_inactive_malformed_foc_does_not_fail_profile(self) -> None:
        report, receipt, summary = fixture("foc")
        path = "data/xml/inactive_bad.xml"
        source = "C:/Private/Game/" + path
        report["physical_inventory"].append(dict(logical_path=path, source_id=source,
                                                  layer="base", active_registry_file=False,
                                                  parsed=False))
        report["counts"]["physical_xml_records"] += 1
        report["diagnostics"].append(dict(code="EAWR-XML-0001", logical_path=path,
                                          line=9, source_id=source, severity="error"))
        report["counts"]["diagnostics"] += 1
        summary["profiles"][1]["physical_xml_records"] += 1
        summary["profiles"][1]["diagnostics"]["malformed"] = 1
        summary["malformed_physical_xml"]["foc"] = [dict(logical_path=path, line=9)]
        result = evaluate("foc", report, receipt, summary)
        self.assertTrue(result["profile_p0_pass"])
        self.assertEqual(result["malformed_inactive"], 1)
        self.assertNotIn("C:/Private", json.dumps(result))

    def test_omitted_failure_row_rejected_even_with_adjusted_count(self) -> None:
        report, receipt, summary = fixture()
        report["diagnostics"].clear()
        report["counts"]["diagnostics"] = 0
        summary["profiles"][0]["diagnostics"]["missing_registry_include"] = 0
        with self.assertRaises(ValueError):
            evaluate("eaw", report, receipt, summary)

    def test_missing_diagnostic_source_must_match_missing_receipt(self) -> None:
        report, receipt, summary = fixture()
        report["diagnostics"][0]["source_id"] = "unexpected:source"
        with self.assertRaises(ValueError):
            evaluate("eaw", report, receipt, summary)

    def test_project_status_cannot_claim_pass_with_eaw_missing(self) -> None:
        report, receipt, summary = fixture()
        summary["full_pass"] = True
        with self.assertRaises(ValueError):
            evaluate("eaw", report, receipt, summary)

    def test_wrong_profile_or_receipt_identity_rejected(self) -> None:
        report, receipt, summary = fixture()
        for mutation in (lambda r, q: r.update(profile="foc"),
                         lambda r, q: q.update(identity_sha256="0" * 64)):
            with self.subTest(mutation=mutation):
                changed_report, changed_receipt = copy.deepcopy(report), copy.deepcopy(receipt)
                mutation(changed_report, changed_receipt)
                with self.assertRaises(ValueError):
                    evaluate("eaw", changed_report, changed_receipt, summary)
        changed_receipt = copy.deepcopy(receipt)
        changed_receipt["includes"][0]["sha256"] = "c" * 64
        changed_receipt["identity_sha256"] = identity(changed_receipt)
        with self.assertRaises(ValueError):
            evaluate("eaw", report, changed_receipt, summary)

    def test_active_malformed_input_rejected(self) -> None:
        report, receipt, summary = fixture()
        report["physical_inventory"][1]["parsed"] = False
        with self.assertRaises(ValueError):
            evaluate("eaw", report, receipt, summary)

    def test_missing_root_rejected(self) -> None:
        report, receipt, summary = fixture()
        receipt["roots"].pop()
        receipt["identity_sha256"] = identity(receipt)
        with self.assertRaises(ValueError):
            evaluate("eaw", report, receipt, summary)

    def test_broadened_cycle_exception_rejected(self) -> None:
        report, receipt, summary = fixture("remake")
        summary["required_failures"]["remake_variant_cycles"].append(
            copy.deepcopy(summary["required_failures"]["remake_variant_cycles"][0]))
        with self.assertRaises(ValueError):
            evaluate("remake", report, receipt, summary)
        report, receipt, summary = fixture("remake")
        report["unresolved"][0]["source_id"] = "different:source"
        with self.assertRaises(ValueError):
            evaluate("remake", report, receipt, summary)
        report, receipt, summary = fixture("remake")
        report["unresolved"].append(dict(report["unresolved"][0], line=390))
        report["counts"]["unresolved"] = 2
        summary["profiles"][2]["unresolved"] = 2
        with self.assertRaises(ValueError):
            evaluate("remake", report, receipt, summary)


if __name__ == "__main__":
    unittest.main()
