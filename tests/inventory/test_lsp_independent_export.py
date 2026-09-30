from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))
from lsp_independent_export import (IndependenceError, check_lsp_independence,  # noqa: E402
                                    compare, compare_custody, fragment_tree)

HASH_A = "a" * 64
HASH_B = "b" * 64


def lsp_value(index, name, value, provenance, source, line1, base=None, fragment=None):
    return {
        "index": index, "name": name, "value": value, "provenance": provenance,
        "source_object_id": source, "base_value": base,
        "fragment": fragment if fragment is not None else f"<{name}>{value}</{name}>",
        "origin": {"logical_path": "data/xml/units.xml", "line0": line1 - 1, "line1": line1,
                   "input_sha256": HASH_A},
        "origin_line_verified": True,
    }


def eawr_value(name, raw_text, provenance, source, line, displaced=None, children=None, attributes=None):
    return {
        "name": name, "raw_text": raw_text, "provenance": provenance, "source_object_id": source,
        "source": {"logical_path": "data/xml/units.xml", "line": line, "column": 5},
        "displaced_raw_text": displaced, "attributes": attributes or [], "children": children or [],
    }


def layer(object_id, line1, digest=HASH_A, element="Unit"):
    return {"object_id": object_id, "resolved_from": "workspace", "winner_order_dependent": False,
            "origin_element_name": element,
            "origin": {"logical_path": "data/xml/units.xml", "line1": line1, "input_sha256": digest}}


def fixture():
    lsp = {
        "producer": "tools/inventory/lsp_effective_export",
        "results": [{
            "requested_id": "Top", "found": True, "type_name": "GameObjectType",
            "structured_render_equals_rpc_xml": True,
            "rpc_result": {"chain": ["Top", "Base"], "xml_sha256": HASH_B},
            "chain": [layer("Top", 10), layer("Base", 2)],
            "values": [
                lsp_value(0, "Health", "50", "overridden", "Top", 12, base="100"),
                lsp_value(1, "Speed", "2", "inherited", "Base", 4),
                lsp_value(2, "Extra", "x", "added", "Top", 13),
            ],
        }],
    }
    manifest = {"hashed_by": "lsp-effective-export pre-index SHA-256 of original file bytes",
                "chain_input_files": [{"logical_path": "data/xml/units.xml", "sha256_before_index": HASH_A,
                                       "sha256_after_export": HASH_A,
                                       "lsp_project_file_hasher_sha256": HASH_A}]}
    eawr = {"samples": [{
        "requested_id": "Top", "object_id": "Top", "type_name": "Unit", "chain": ["Top", "Base"],
        "raw_chain": [
            {"id": "Top", "source": {"logical_path": "data/xml/units.xml", "line": 10}, "input_sha256": HASH_A},
            {"id": "Base", "source": {"logical_path": "data/xml/units.xml", "line": 2}, "input_sha256": HASH_A},
        ],
        "effective_values": [
            eawr_value("Health", " 50 ", "overridden", "Top", 12, displaced="100"),
            eawr_value("Speed", "2", "inherited", "Base", 4),
            eawr_value("Extra", "x", "added", "Top", 13),
        ],
    }]}
    public = [{"object_id": "Top", "input_sha256": [HASH_A]}]
    return lsp, manifest, eawr, public


def classes(result):
    return sorted(d["classification"] for d in result["matrix"][0]["differences"])


class IndependenceTests(unittest.TestCase):
    def test_accepts_harness_output(self) -> None:
        lsp, manifest, _, _ = fixture()
        check_lsp_independence(lsp, manifest)

    def test_rejects_eawr_fields_in_lsp_evidence(self) -> None:
        lsp, manifest, _, _ = fixture()
        lsp["results"][0]["raw_chain"] = []
        with self.assertRaises(IndependenceError):
            check_lsp_independence(lsp, manifest)

    def test_rejects_foreign_producer_and_copied_hashes(self) -> None:
        lsp, manifest, _, _ = fixture()
        foreign = dict(lsp, producer="out/xml/lsp-reference")
        with self.assertRaises(IndependenceError):
            check_lsp_independence(foreign, manifest)
        unhashed = dict(manifest, hashed_by="copied from xml_scan samples")
        with self.assertRaises(IndependenceError):
            check_lsp_independence(lsp, unhashed)
        broken = copy.deepcopy(manifest)
        broken["chain_input_files"][0]["lsp_project_file_hasher_sha256"] = HASH_B
        with self.assertRaises(IndependenceError):
            check_lsp_independence(lsp, broken)

    def test_custody_against_receipt(self) -> None:
        _, manifest, _, _ = fixture()
        receipt = {"includes": [{"logical_path": "data/xml/units.xml", "outcome": "loaded", "sha256": HASH_A}]}
        self.assertTrue(compare_custody(manifest, receipt)[0]["equal"])
        receipt["includes"][0]["sha256"] = HASH_B
        self.assertFalse(compare_custody(manifest, receipt)[0]["equal"])


class ComparisonTests(unittest.TestCase):
    def test_equal_export_has_no_divergence(self) -> None:
        result = compare(*fixture())
        row = result["matrix"][0]
        self.assertEqual(row["divergences"], 0)
        self.assertEqual(classes(result), ["type-name-granularity", "whitespace-trim"])
        self.assertEqual(row["eawr_values_trimmed_for_comparison"], 1)
        self.assertEqual(row["match_basis"]["value"]["exact"], 2)
        self.assertEqual(row["match_basis"]["value"]["whitespace-trim"], 1)
        self.assertTrue(row["chain_equal"] and row["input_hashes_equal"])
        self.assertEqual(row["provenance_counts"]["overridden"], 1)

    def test_detects_reordered_occurrences(self) -> None:
        lsp, manifest, eawr, public = fixture()
        values = lsp["results"][0]["values"]
        values[1], values[2] = values[2], values[1]
        result = compare(lsp, manifest, eawr, public)
        self.assertGreater(result["matrix"][0]["divergences"], 0)
        self.assertIn("ordered-name", classes(result))

    def test_detects_provenance_source_and_displaced_value(self) -> None:
        lsp, manifest, eawr, public = fixture()
        value = lsp["results"][0]["values"][0]
        value.update(provenance="merged", source_object_id="Base", base_value="99")
        found = classes(compare(lsp, manifest, eawr, public))
        for expected in ("provenance-kind", "source-object", "displaced-value"):
            self.assertIn(expected, found)

    def test_line_normalization_is_explicit(self) -> None:
        lsp, manifest, eawr, public = fixture()
        # A zero-based line reported as if it were one-based must not compare equal.
        lsp["results"][0]["values"][1]["origin"]["line1"] = 3
        self.assertIn("origin-line", classes(compare(lsp, manifest, eawr, public)))

    def test_chain_and_hash_divergence(self) -> None:
        lsp, manifest, eawr, public = fixture()
        lsp["results"][0]["chain"][1]["origin"]["input_sha256"] = HASH_B
        eawr["samples"][0]["chain"] = ["Top", "Other"]
        found = classes(compare(lsp, manifest, eawr, public))
        self.assertIn("chain", found)
        self.assertIn("input-hash", found)

    def test_line_endings_are_a_classified_representation(self) -> None:
        lsp, manifest, eawr, public = fixture()
        lsp["results"][0]["values"][1]["value"] = "a,\r\n\tb"
        lsp["results"][0]["values"][1]["fragment"] = "<Speed>a,\r\n\tb</Speed>"
        eawr["samples"][0]["effective_values"][1]["raw_text"] = "a,\n\tb"
        result = compare(lsp, manifest, eawr, public)
        self.assertEqual(result["matrix"][0]["divergences"], 0)
        self.assertIn("eol-normalization", classes(result))
        eawr["samples"][0]["effective_values"][1]["raw_text"] = "a, b"
        self.assertIn("value-text", classes(compare(lsp, manifest, eawr, public)))

    def test_nested_content_is_compared_structurally(self) -> None:
        lsp, manifest, eawr, public = fixture()
        fragment = ('<Abilities SubObjectList="Yes">\n  <!-- note -->\n  <Ability Name="A">\n'
                    '    <Type>DEPLOY</Type>\n  </Ability>\n</Abilities>')
        lsp["results"][0]["values"][1].update(name="Abilities", value="DEPLOY", fragment=fragment)
        lsp["results"][0]["values"][1]["origin"]["line1"] = 4
        nested = [{"name": "Ability", "raw_text": "", "line": 6, "attributes": [{"name": "Name", "value": "A"}],
                   "children": [{"name": "Type", "raw_text": "DEPLOY", "line": 7, "attributes": [],
                                 "children": []}]}]
        eawr["samples"][0]["effective_values"][1] = eawr_value(
            "Abilities", "", "inherited", "Base", 4, children=nested,
            attributes=[{"name": "SubObjectList", "value": "Yes"}])
        result = compare(lsp, manifest, eawr, public)
        row = result["matrix"][0]
        self.assertEqual(row["divergences"], 0, row["differences"])
        self.assertEqual(row["nested_elements_compared"], 2)
        self.assertIn("nested-inner-text", classes(result))

        nested[0]["children"][0]["raw_text"] = "TURBO"
        nested[0]["children"][0]["line"] = 8
        found = [d for d in compare(lsp, manifest, eawr, public)["matrix"][0]["differences"]
                 if d["classification"] == "nested-content"]
        self.assertEqual(sorted(d["field"] for d in found),
                         ["nested/Abilities/Ability[0]/Type[0].line", "nested/Abilities/Ability[0]/Type[0].text"])

    def test_empty_selection_is_preserved(self) -> None:
        lsp, manifest, eawr, public = fixture()
        public = []
        structured = dict(lsp, results=[])
        eawr["samples"] = []
        result = compare(structured, manifest, eawr, public)
        self.assertEqual(result["compared"], 0)
        self.assertEqual(result["objects_without_divergence"], 0)

    def test_missing_eawr_sample_now_fails_closed_on_id_binding(self) -> None:
        # An object that is in the selection but absent from EAWR now fails closed at the ID
        # binding stage rather than being recorded as a missing-in-eawr row.
        lsp, manifest, eawr, public = fixture()
        eawr["samples"] = []
        with self.assertRaisesRegex(IndependenceError, "EAWR sample IDs do not match selection"):
            compare(lsp, manifest, eawr, public)

    def test_fragment_tree_lines_are_absolute(self) -> None:
        tree = fragment_tree("<A x='1'>\r\n  <B>t</B>\r\n</A>", 40)
        self.assertEqual((tree["line"], tree["children"][0]["line"]), (40, 41))
        self.assertEqual(tree["attributes"], [["x", "1"]])


class ReviewFollowUpTests(unittest.TestCase):
    """Each case below compared as zero-divergence, or went unrecorded, before the fix."""

    def test_type_name_is_granularity_only_for_the_registry_type_and_authored_element(self) -> None:
        for lsp_type, eawr_type, element in (("SpaceUnit", "Unit", "Unit"),
                                             ("GameObjectType", "Totally_Different", "Unit"),
                                             ("GameObjectType", "Unit", None),
                                             ("GameObjectType", "Unit", "Other_Element")):
            lsp, manifest, eawr, public = fixture()
            lsp["results"][0]["type_name"] = lsp_type
            lsp["results"][0]["chain"][0]["origin_element_name"] = element
            eawr["samples"][0]["type_name"] = eawr_type
            result = compare(lsp, manifest, eawr, public)
            self.assertIn("type-name", classes(result), (lsp_type, eawr_type, element))
            self.assertNotIn("type-name-granularity", classes(result))
            self.assertGreater(result["matrix"][0]["divergences"], 0)

    def test_missing_eawr_structure_keys_fail_closed(self) -> None:
        for key in ("attributes", "children"):
            lsp, manifest, eawr, public = fixture()
            del eawr["samples"][0]["effective_values"][1][key]
            result = compare(lsp, manifest, eawr, public)
            self.assertIn("eawr-structure-missing", classes(result), key)
            self.assertGreater(result["matrix"][0]["divergences"], 0)

    def test_missing_structure_in_nested_child_fails_closed(self) -> None:
        lsp, manifest, eawr, public = nested_fixture()
        del eawr["samples"][0]["effective_values"][1]["children"][0]["children"][0]["attributes"]
        self.assertIn("eawr-structure-missing", classes(compare(lsp, manifest, eawr, public)))

    def test_nested_inner_text_requires_the_fragment_descendant_text(self) -> None:
        lsp, manifest, eawr, public = nested_fixture()
        self.assertEqual(compare(lsp, manifest, eawr, public)["matrix"][0]["divergences"], 0)
        lsp["results"][0]["values"][1]["value"] = "GARBAGE"
        result = compare(lsp, manifest, eawr, public)
        self.assertIn("value-text", classes(result))
        self.assertNotIn("nested-inner-text", classes(result))

    def test_nested_inner_text_requires_equal_direct_text(self) -> None:
        lsp, manifest, eawr, public = nested_fixture()
        value = lsp["results"][0]["values"][1]
        value["fragment"] = value["fragment"].replace('<Abilities SubObjectList="Yes">',
                                                      '<Abilities SubObjectList="Yes">stray')
        # The fragment's whole descendant text, so only the direct-text condition can reject it.
        value["value"] = "stray\n  \n  \n    DEPLOY"
        self.assertIn("value-text", classes(compare(lsp, manifest, eawr, public)))

    def test_nested_inner_text_is_not_applied_without_eawr_structure(self) -> None:
        lsp, manifest, eawr, public = nested_fixture()
        del eawr["samples"][0]["effective_values"][1]["children"]
        found = classes(compare(lsp, manifest, eawr, public))
        self.assertNotIn("nested-inner-text", found)
        self.assertIn("value-text", found)

    def test_eawr_whitespace_trim_is_classified_and_counted(self) -> None:
        lsp, manifest, eawr, public = fixture()
        eawr["samples"][0]["effective_values"][1]["raw_text"] = "\r\n 2 \t"
        eawr["samples"][0]["effective_values"][0]["displaced_raw_text"] = " 100\n"
        result = compare(lsp, manifest, eawr, public)
        row = result["matrix"][0]
        self.assertEqual(row["divergences"], 0)
        trims = [(d["field"], d["index"]) for d in row["differences"] if d["classification"] == "whitespace-trim"]
        self.assertEqual(sorted(trims), [("base_value", 0), ("value", 0), ("value", 1)])
        self.assertEqual(result["match_basis_totals"]["value"]["whitespace-trim"], 2)
        self.assertEqual(result["match_basis_totals"]["base_value"]["whitespace-trim"], 1)
        self.assertEqual(result["difference_classes"]["whitespace-trim"], 3)

    def test_non_xml_whitespace_is_not_trimmed(self) -> None:
        lsp, manifest, eawr, public = fixture()
        eawr["samples"][0]["effective_values"][1]["raw_text"] = "2 "
        self.assertIn("value-text", classes(compare(lsp, manifest, eawr, public)))

    def test_trimming_inside_eol_class_is_recorded(self) -> None:
        lsp, manifest, eawr, public = fixture()
        lsp["results"][0]["values"][1]["value"] = "a,\r\n\tb"
        eawr["samples"][0]["effective_values"][1]["raw_text"] = "\n a,\n\tb "
        difference = next(d for d in compare(lsp, manifest, eawr, public)["matrix"][0]["differences"]
                          if d["classification"] == "eol-normalization")
        self.assertTrue(difference["eawr_whitespace_trimmed"])

    def test_missing_input_hashes_on_both_sides_fail_closed(self) -> None:
        lsp, manifest, eawr, public = fixture()
        lsp["results"][0]["chain"][1]["origin"]["input_sha256"] = None
        eawr["samples"][0]["raw_chain"][1]["input_sha256"] = None
        result = compare(lsp, manifest, eawr, public)
        self.assertIn("input-hash-missing", classes(result))
        self.assertFalse(result["matrix"][0]["input_hashes_equal"])

    def test_fragment_root_keeps_direct_and_descendant_text(self) -> None:
        tree = fragment_tree("<A>t<!-- c --><B>u</B>v</A>", 1)
        self.assertEqual((tree["text"], tree["descendant_text"]), ("tv", "tuv"))


class SelectionIdBindingTests(unittest.TestCase):
    """The selected IDs must line up exactly, in order, on both sides (P0 #5)."""

    def test_good_fixture_still_compares(self) -> None:
        self.assertEqual(compare(*fixture())["compared"], 1)

    def test_duplicated_selected_id_is_rejected(self) -> None:
        lsp, manifest, eawr, public = fixture()
        public.append({"object_id": "Top", "input_sha256": [HASH_A]})
        with self.assertRaisesRegex(IndependenceError, "selection IDs are duplicated"):
            compare(lsp, manifest, eawr, public)

    def test_lsp_duplicate_id_replacing_another_is_rejected(self) -> None:
        lsp, manifest, eawr, public = fixture()
        public.append({"object_id": "Other", "input_sha256": [HASH_A]})
        lsp["results"].append(dict(lsp["results"][0]))
        with self.assertRaisesRegex(IndependenceError, "LSP result IDs do not match selection"):
            compare(lsp, manifest, eawr, public)

    def test_lsp_same_ids_in_wrong_order_is_rejected(self) -> None:
        lsp, manifest, eawr, public = fixture()
        public.append({"object_id": "Other", "input_sha256": [HASH_A]})
        other = copy.deepcopy(lsp["results"][0])
        other["requested_id"] = "Other"
        lsp["results"].insert(0, other)
        eawr_other = copy.deepcopy(eawr["samples"][0])
        eawr_other["requested_id"] = "Other"
        eawr["samples"].append(eawr_other)
        with self.assertRaisesRegex(IndependenceError, "LSP result IDs do not match selection"):
            compare(lsp, manifest, eawr, public)

    def test_eawr_duplicate_id_replacing_another_is_rejected(self) -> None:
        lsp, manifest, eawr, public = fixture()
        public.append({"object_id": "Other", "input_sha256": [HASH_A]})
        other = copy.deepcopy(lsp["results"][0])
        other["requested_id"] = "Other"
        lsp["results"].append(other)
        eawr["samples"].append(dict(eawr["samples"][0]))
        with self.assertRaisesRegex(IndependenceError, "EAWR sample IDs do not match selection"):
            compare(lsp, manifest, eawr, public)

    def test_eawr_same_ids_in_wrong_order_is_rejected(self) -> None:
        lsp, manifest, eawr, public = fixture()
        public.append({"object_id": "Other", "input_sha256": [HASH_A]})
        other = copy.deepcopy(lsp["results"][0])
        other["requested_id"] = "Other"
        lsp["results"].append(other)
        eawr_other = copy.deepcopy(eawr["samples"][0])
        eawr_other["requested_id"] = "Other"
        eawr["samples"].insert(0, eawr_other)
        with self.assertRaisesRegex(IndependenceError, "EAWR sample IDs do not match selection"):
            compare(lsp, manifest, eawr, public)


def nested_fixture():
    lsp, manifest, eawr, public = fixture()
    fragment = ('<Abilities SubObjectList="Yes">\n  <!-- note -->\n  <Ability Name="A">\n'
                '    <Type>DEPLOY</Type>\n  </Ability>\n</Abilities>')
    lsp["results"][0]["values"][1].update(name="Abilities", value="DEPLOY", fragment=fragment)
    lsp["results"][0]["values"][1]["origin"]["line1"] = 4
    nested = [{"name": "Ability", "raw_text": "", "line": 6, "attributes": [{"name": "Name", "value": "A"}],
               "children": [{"name": "Type", "raw_text": "DEPLOY", "line": 7, "attributes": [],
                             "children": []}]}]
    eawr["samples"][0]["effective_values"][1] = eawr_value(
        "Abilities", "", "inherited", "Base", 4, children=nested,
        attributes=[{"name": "SubObjectList", "value": "Yes"}])
    return lsp, manifest, eawr, public


if __name__ == "__main__":
    unittest.main()
