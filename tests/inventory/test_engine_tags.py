from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/inventory"))
from engine_tags import EngineTags
from corpus import Source
import xml_inventory
from engine_tag_catalog import build
import engine_tag_catalog
from test_xml import SCHEMA


def catalog(*tags):
    return {"schema_version": 1, "evidence": {"ET-01": "debug build"}, "rows": [
        {"tag": tag, "table_kind": "unit", "value_type": "float", "evidence": "ET-01"}
        for tag in tags]}


class EngineTagTests(unittest.TestCase):
    def test_table_owner_is_required_even_when_schema_knows_another_pair(self):
        lookup = xml_inventory.SchemaLookup(SCHEMA, EngineTags(catalog("Value", "Unauthored")))
        for profile in ("foc", "remake"):
            with self.subTest(profile=profile):
                self.assertEqual(lookup.status(profile, "uNiT", "UNAUTHORED")[0], "known-engine")
                self.assertEqual(lookup.status(profile, "Faction", "Unauthored")[0], "unknown")
                self.assertEqual(lookup.status(profile, "Faction", "Value")[0], "unknown")
        value = catalog("Unauthored")
        value["rows"][0]["table_kind"] = "faction"
        lookup = xml_inventory.SchemaLookup(SCHEMA, EngineTags(value))
        self.assertEqual(lookup.status("foc", "Unit", "Unauthored")[0], "unknown")
        self.assertEqual(lookup.status("foc", "Faction", "Unauthored")[0], "known-engine")

    def test_shipped_owner_aliases_and_distinct_tables(self):
        tags = EngineTags.load()
        for object_type, tag in (("GameObjectType", "Turret_Rotate_To_Rest_Delay_Sec"),
                                 ("GameObjectType", "SFXEvent_Defend"),
                                 ("CorruptPlanetAbility", "Transition_Time_In_Secs"),
                                 ("CorruptPlanetAbility", "Required_Corruption_Level")):
            with self.subTest(object_type=object_type, tag=tag):
                self.assertTrue(tags.recognizes("foc", object_type, tag))
                self.assertFalse(tags.recognizes("foc", "UnregisteredType", tag))

    def test_generator_writes_lf_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rows, review, out = (root / name for name in ("rows.tsv", "review.json", "catalog.json"))
            rows.write_text("table\tname\tparser\nowner\tTag\t8\n", encoding="utf-8")
            review.write_text(json.dumps({"tables": {"owner": {"kind": "unit", "evidence": "ET-01"}},
                                          "value_types": {"8": "float"}, "evidence": {"ET-01": "debug build"}}), encoding="utf-8")
            argv = ["engine_tag_catalog", "--rows", str(rows), "--review", str(review), "--out", str(out)]
            with mock.patch.object(sys, "argv", argv):
                engine_tag_catalog.main()
            generated = out.read_bytes()
            self.assertNotIn(b"\r", generated)
            self.assertTrue(generated.endswith(b"\n"))
            self.assertEqual(json.loads(generated)["rows"][0]["tag"], "Tag")

    def test_schema_precedence_case_folding_and_profile_scope(self):
        lookup = xml_inventory.SchemaLookup(SCHEMA, EngineTags(catalog("Value", "Old", "Unauthored")))
        self.assertEqual(lookup.status("foc", "Unit", "Value")[0], "known")
        self.assertEqual(lookup.status("foc", "Unit", "Old")[0], "deprecated")
        self.assertEqual(lookup.status("remake", "Unit", "UNAUTHORED"), ("known-engine", None, ["foc"]))
        self.assertEqual(lookup.status("eaw", "Unit", "Unauthored"), ("unknown", None, []))
        self.assertEqual(lookup.status("foc", "Unit", "Typo")[0], "unknown")
        tags = EngineTags(catalog("Key"))
        self.assertFalse(tags.recognizes("foc", "Unit", "\u212aey"))

    def test_generated_inventory_keeps_unknowns_and_attributes_unresolved(self):
        data = b'<Root><Unit Unauthored="1"><Unauthored>2</Unauthored><Typo>3</Typo></Unit><Faction><Unauthored/></Faction></Root>'
        source = Source("data/xml/test.xml", "synthetic", "loose", "base", data, "fixture")
        class Corpus:
            def __init__(self, *_): self.last_manifest = {}
            def iter_sources(self, *_): return iter([source])
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(xml_inventory, "Corpus", Corpus), \
                mock.patch.object(xml_inventory, "build_schema_index", return_value={**SCHEMA, "types": SCHEMA["types"] + [{"name": "Faction", "game": "eaw"}]}), \
                mock.patch.object(xml_inventory.EngineTags, "load", return_value=EngineTags(catalog("Unauthored"))):
            out = Path(tmp)
            xml_inventory.generate(out, out, out, out)
            rows = json.loads((out / "xml-tags.json").read_text())["rows"]
            unresolved = json.loads((out / "xml-unresolved.json").read_text())["rows"]
            known = [r for r in rows if r["tag_name"] == "Unauthored" and r["node_kind"] == "element" and r["object_type"] == "Unit"]
            self.assertEqual({r["profile"]: r["status"] for r in known},
                             {"eaw": "unknown", "foc": "known-engine", "remake": "known-engine"})
            self.assertFalse(any(r["profile"] != "eaw" and r["tag_name"] == "Unauthored"
                                 and r["node_kind"] == "element" and r["object_type"] == "Unit" for r in unresolved))
            self.assertEqual(sum(r["tag_name"] == "Unauthored" and r["object_type"] == "Faction" for r in unresolved), 3)
            self.assertTrue(any(r["tag_name"] == "Unauthored" and r["node_kind"] == "attribute" for r in unresolved))
            self.assertEqual(sum(r["tag_name"] == "Typo" for r in unresolved), 3)
            self.assertIn("engine_tags_sha256", json.loads((out / "xml-manifest.json").read_text()))

    def test_catalog_rejects_duplicates_and_missing_evidence(self):
        with self.assertRaises(ValueError): EngineTags(catalog("Same", "same"))
        value = catalog("Tag"); value["evidence"] = {}
        with self.assertRaises(ValueError): EngineTags(value)

    def test_generation_translates_private_tokens_and_requires_complete_review(self):
        rows = [{"table": "private-owner", "name": "Tag", "parser": "8"}]
        review = {"tables": {"private-owner": {"kind": "synthetic", "evidence": "ET-01"}},
                  "value_types": {"8": "float"}, "evidence": {"ET-01": "debug build"}}
        value = build(rows * 2, review)
        self.assertEqual(len(value["rows"]), 1)
        self.assertNotIn("private-owner", json.dumps(value))
        review["value_types"] = {}
        with self.assertRaises(KeyError): build(rows, review)

    def test_shipped_catalog_has_the_unauthored_seed_names(self):
        tags = EngineTags.load()
        for tag in ("Cancel_Special_Ability", "Corrupt_Planet_Ability", "Required_Corruption_Level",
                    "Transition_Time_In_Secs", "Fadeout_End_Frame", "Space_Position_LeftTop",
                    "Land_Position_LeftTop", "Overrun_Auto_Resolve_Multiple", "Respawn_Tied_To_Structure",
                    "Turret_Rotate_To_Rest_Delay_Sec", "SFXEvent_Defend", "SFXEvent_Undeploy", "SFXEvent_Retreat_Cancel",
                    "SFXEvent_Retreat_Engines_Jump", "SFXEvent_Shields_Go_Down", "SFXEvent_Shields_Go_Back_Up",
                    "SFXEvent_Select_With_Non_Hero_Ability", "SFXEvent_Move_With_Non_Hero_Ability"):
            with self.subTest(tag=tag): self.assertIn(tag.lower(), tags.tags)
        self.assertFalse(tags.recognizes("foc", "GameObjectType", "Perceptual_Function_Name"))
        self.assertFalse(tags.recognizes("foc", "GameObjectType", "Defend"))


if __name__ == "__main__": unittest.main()
