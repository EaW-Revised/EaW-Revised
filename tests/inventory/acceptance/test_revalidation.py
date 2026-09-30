"""Targeted post-remediation checks for the P0-08/P0-09 acceptance gate.

This module is intentionally independent of generated inventory rows.  It checks
the repaired lexical/XML safety boundaries and treats the remediation report's
canonical hashes as external expected evidence rather than regenerating them.
"""
from __future__ import annotations

import hashlib
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))

from common import canonical_bytes, manifest_id  # noqa: E402
from corpus import Source  # noqa: E402
from lua_calls import LuaError, parse_source_calls  # noqa: E402
from xml_inventory import SchemaLookup, XmlInventoryError, parse_xml, registry_type_hints  # noqa: E402


EXPECTED = {
    "lua-api.json": "77b957769db3593c14586ce6288baa1d35f270cde6c5a944e228354936416468",
    "lua-unresolved.json": "85c0f3c27ff96702a952ea73827bd91adfd50dd7558aa9419af4942208dce0c4",
    "lua-manifest.json": "d8898233a211d2164bf94a95574c2ddd5cb0c3891eac84ea0bf638c3ae982b35",
    "xml-tags.json": "38a3e3bfb3afb96db1a62e7df70ab0530e9001f4bb1a734f2835506a094a04e5",
    "xml-unresolved.json": "61656a37bdee3da67f8a68471d366c6951f41305001129a329ffff3f6c1bfe3b",
    "xml-manifest.json": "2d5c9c78b253bcb45366f925166add05f62077ea4cda08b0de88fee81b29b64a",
}


def source(data: bytes, path: str = "data/xml/revalidation.xml") -> Source:
    return Source(path, "acceptance:fixture", "loose", "acceptance", data,
                  hashlib.sha256(data).hexdigest())


class RepairedBoundaryTests(unittest.TestCase):
    def test_cr_lf_short_string_boundary(self) -> None:
        for malformed in (b'Call("raw\nnewline")', b'Call("raw\rnewline")',
                          b'Call("raw\r\nnewline")'):
            with self.subTest(malformed=malformed):
                with self.assertRaises(LuaError):
                    parse_source_calls(malformed)
        for valid in (b'Call("escaped\\n")', b'Call("continued\\\nline")',
                      b'Call("continued\\\r\nline")'):
            with self.subTest(valid=valid):
                calls, _ = parse_source_calls(valid)
                self.assertEqual([call.symbol for call in calls], ["Call"])

    def test_document_doctype_rejected_after_encoding_detection(self) -> None:
        schema = SchemaLookup({"revision": "fixture", "files": [], "metafiles": [],
                               "types": [], "tags": []})
        for encoding in ("utf-8", "utf-16", "utf-32"):
            with self.subTest(encoding=encoding):
                bad = '<!DOCTYPE x SYSTEM "file:///outside"><Root/>'.encode(encoding)
                with self.assertRaises(XmlInventoryError):
                    parse_xml(source(bad), "eaw", schema)

    def test_registry_doctype_rejected_and_unicode_registry_accepted(self) -> None:
        schema = SchemaLookup({"revision": "fixture", "files": [], "metafiles": [{
            "game": "eaw", "meta_file_type": "fileRegistry",
            "path": "data/xml/registry.xml", "types": ["Unit"],
        }], "types": [{"name": "Unit", "game": "eaw"}], "tags": []})
        for encoding in ("utf-8", "utf-16"):
            with self.subTest(encoding=encoding):
                valid = '<Registry>\u03a9\u00e9<File>unit.xml</File></Registry>'.encode(encoding)
                registry = source(valid, "data/xml/registry.xml")
                self.assertEqual(registry_type_hints([registry], "eaw", schema),
                                 {"data/xml/unit.xml": ("Unit",)})
        # CPython's bundled Expat does not accept UTF-32 documents; retain the
        # fail-closed safety check for that unsupported family below.
        encoding = "utf-32"
        with self.assertRaises(XmlInventoryError):
            registry_type_hints([source('<Registry>\u03a9</Registry>'.encode(encoding),
                                       "data/xml/registry.xml")], "eaw", schema)
        for encoding in ("utf-8", "utf-16", "utf-32"):
            with self.subTest(doctype_encoding=encoding):
                bad = '<!DOCTYPE x SYSTEM "file:///outside"><Registry/>' .encode(encoding)
                with self.assertRaises(XmlInventoryError):
                    registry_type_hints([source(bad, "data/xml/registry.xml")], "eaw", schema)

    def test_canonical_outputs_and_manifest_tool_identity(self) -> None:
        for name, expected in EXPECTED.items():
            path = ROOT / "plan" / "inventories" / name
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertEqual(actual, expected, name)
            value = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(path.read_bytes(), canonical_bytes(value), name)
            if name.endswith("manifest.json"):
                self.assertEqual(manifest_id(value), value["manifest_id"])
                self.assertEqual(value["tool_version"], "1.0.0")
                self.assertEqual(value["tool"], name.split("-", 1)[0] + "_inventory")
                self.assertTrue(value["tool_sources"])
        lua = json.loads((ROOT / "plan/inventories/lua-manifest.json").read_text(encoding="utf-8"))
        xml = json.loads((ROOT / "plan/inventories/xml-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(xml["schema_revision"], "3e1b825a124fbc13b2293665f34a36dd4d4be80f")
        self.assertEqual([(p["profile"], p["raw_file_count"], p["effective_file_count"])
                          for p in lua["profiles"]],
                         [("eaw", 520, 260), ("foc", 1168, 370), ("remake", 2120, 709)])
        self.assertEqual([(p["profile"], p["raw_file_count"], p["effective_file_count"],
                           p["parse_failures"]) for p in xml["profiles"]],
                         [("eaw", 375, 367, 10), ("foc", 1036, 639, 4),
                          ("remake", 2473, 1740, 18)])


if __name__ == "__main__":
    unittest.main()
