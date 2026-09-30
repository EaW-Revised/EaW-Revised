from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))
from corpus import Source  # noqa: E402
from xml_inventory import (SchemaLookup, XmlInventoryError, parse_xml,  # noqa: E402
                           registry_type_hints)


SCHEMA = {
    "revision": "synthetic",
    "files": [], "metafiles": [],
    "types": [{"name": "Unit", "game": "eaw"}],
    "tags": [
        {"available_since": None, "deprecated": False, "game": "eaw", "object_type": "Unit",
         "schema_ref": "eaw/tags/Unit.yaml#L2", "tag": "Value"},
        {"available_since": None, "deprecated": True, "game": "eaw", "object_type": "Unit",
         "schema_ref": "eaw/tags/Unit.yaml#L4", "tag": "Old"},
    ],
}


def source(data: bytes) -> Source:
    return Source("data/xml/test.xml", "test:loose:Data/XML/Test.xml", "loose", "test", data, "x")


class XmlTests(unittest.TestCase):
    def test_order_context_attributes_and_lines(self) -> None:
        xml = b'<Root>\n<Unit Name="A"><Group><Value>1</Value><Value>2</Value></Group><Old/></Unit>\n</Root>'
        schema = SchemaLookup(SCHEMA); rows = parse_xml(source(xml), "eaw", schema)
        values = [r for r in rows if r.tag_name == "Value"]
        self.assertEqual([r.tag_path for r in values], ["Unit/Group/Value", "Unit/Group/Value"])
        self.assertEqual([r.line for r in values], [2, 2])
        attribute = next(r for r in rows if r.node_kind == "attribute")
        self.assertEqual(attribute.tag_path, "Unit/@Name")
        self.assertEqual(schema.status("eaw", "Unit", "Value")[0], "known")
        self.assertEqual(schema.status("eaw", "Unit", "Old")[0], "deprecated")

    def test_malformed_and_doctype_fail(self) -> None:
        schema = SchemaLookup(SCHEMA)
        for value in (b"<Root>", b'<!DOCTYPE x SYSTEM "file:///x"><Root/>'):
            with self.subTest(value=value):
                with self.assertRaises(XmlInventoryError): parse_xml(source(value), "eaw", schema)

    def test_registry_forced_type_starts_each_definition_context(self) -> None:
        schema = SchemaLookup(SCHEMA)
        rows = parse_xml(source(b"<Units><SpaceUnit><Value>1</Value></SpaceUnit></Units>"),
                         "eaw", schema, ("Unit",))
        value = next(row for row in rows if row.tag_name == "Value")
        self.assertEqual((value.object_type, value.tag_path), ("Unit", "SpaceUnit/Value"))

    def test_registry_doctype_rejection_uses_parser_encoding_detection(self) -> None:
        value = {**SCHEMA, "metafiles": [{
            "game": "eaw", "meta_file_type": "fileRegistry",
            "path": "data/xml/registry.xml", "types": ["Unit"],
        }]}
        schema = SchemaLookup(value)
        encoded = '<!DOCTYPE x [<!ENTITY y SYSTEM "file:///x">]><Root/>'.encode("utf-32")
        with self.assertRaises(XmlInventoryError):
            registry_type_hints([Source("data/xml/registry.xml", "registry", "loose", "base",
                                        encoded, "fixture")], "eaw", schema)


if __name__ == "__main__": unittest.main()
