from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))
from corpus import Source  # noqa: E402
from mtd_inventory import (MtdEntry, MtdInventoryError,  # noqa: E402
                           REDACTED_TEXTURE_REFERENCE, classify_texture_reference,
                           extract_texture_references, normalize_atlas_name, parse_mtd,
                           texture_dimensions,
                           texture_reference_candidates, validate_rectangles)


def fixture_bytes() -> bytes:
    text = (ROOT / "tests" / "assets" / "fixtures" / "two-icons.mtd.hex").read_text()
    tokens = [token for line in text.splitlines() if not line.startswith("#")
              for token in line.split()]
    return bytes.fromhex("".join(tokens))


class MtdInventoryTests(unittest.TestCase):
    def test_two_icon_fixture(self) -> None:
        entries = parse_mtd(fixture_bytes())
        self.assertEqual([entry.name for entry in entries], ["I_ALPHA.TGA", "I_OPAQUE.TGA"])
        self.assertEqual((entries[0].x, entries[0].y, entries[0].width, entries[0].height),
                         (1, 2, 16, 8))
        self.assertTrue(entries[0].has_alpha)
        self.assertFalse(entries[1].has_alpha)

    def test_truncated_and_malformed_records_fail_closed(self) -> None:
        value = fixture_bytes()
        with self.assertRaisesRegex(MtdInventoryError, "truncated"):
            parse_mtd(value[:-1])
        malformed = bytearray(value)
        malformed[4 + 80] = 2
        with self.assertRaisesRegex(MtdInventoryError, "alpha"):
            parse_mtd(bytes(malformed))
        unterminated = bytearray(value)
        unterminated[4:68] = b"A" * 64
        with self.assertRaisesRegex(MtdInventoryError, "unterminated"):
            parse_mtd(bytes(unterminated))

    def test_reference_extraction_and_normalization(self) -> None:
        source = Source("data/xml/test.xml", "fixture", "loose", "test",
                        b"<Root><Icon_Name>i_alpha</Icon_Name></Root>", "hash")
        rows = extract_texture_references(source, {"icon_name": ["schema#L1"]})
        self.assertEqual(len(rows), 1)
        self.assertEqual(normalize_atlas_name(rows[0]["value"]), "I_ALPHA.TGA")

    def test_doctype_is_rejected(self) -> None:
        source = Source("data/xml/test.xml", "fixture", "loose", "test",
                        b"<!DOCTYPE x><Root/>", "hash")
        with self.assertRaisesRegex(MtdInventoryError, "DOCTYPE"):
            extract_texture_references(source, {})

    def test_texture_dimensions_and_rectangle_validation(self) -> None:
        tga = bytearray(18 + 4 * 2 * 3)
        tga[2] = 2
        tga[12:16] = bytes((4, 0, 2, 0))
        tga[16] = 24
        self.assertEqual(texture_dimensions(bytes(tga), "page.tga"), ("tga", 4, 2))
        dds = bytearray(128)
        dds[:4] = b"DDS "
        dds[4:8] = (124).to_bytes(4, "little")
        dds[12:16] = (2).to_bytes(4, "little")
        dds[16:20] = (4).to_bytes(4, "little")
        self.assertEqual(texture_dimensions(bytes(dds), "page.dds"), ("dds", 4, 2))
        entries = [
            MtdEntry("inside", 3, 1, 1, 1, True),
            MtdEntry("outside", 3, 1, 2, 1, False),
        ]
        invalid = validate_rectangles(entries, 4, 2)
        self.assertEqual([(row["entry_index"], row["name"]) for row in invalid],
                         [(1, "outside")])
        with self.assertRaisesRegex(MtdInventoryError, "neither supported"):
            texture_dimensions(b"bad", "page.tga")

    def test_standalone_texture_candidates_are_bounded(self) -> None:
        self.assertEqual(texture_reference_candidates("I_ICON"), [
            "data/art/textures/i_icon.dds", "data/art/textures/i_icon.tga"])
        self.assertEqual(texture_reference_candidates("Art/Textures/I_ICON.TGA"),
                         ["data/art/textures/i_icon.tga"])
        self.assertEqual(texture_reference_candidates("Data/Art/Textures/I_ICON.DDS"),
                         ["data/art/textures/i_icon.dds"])
        self.assertEqual(texture_reference_candidates("../escape.tga"), [])
        self.assertEqual(texture_reference_candidates("unsupported.png"), [])

    def test_nonportable_texture_references_are_rejected_before_classification(self) -> None:
        rejected = [
            r"C:\Users\PRIVATE\Game\Art\Textures\I_PRIVATE.TGA",
            r"C:Users\PRIVATE\I_PRIVATE.TGA",
            "/home/private/I_PRIVATE.TGA",
            r"\private\I_PRIVATE.TGA",
            r"\\server\private\I_PRIVATE.TGA",
            "//server/private/I_PRIVATE.TGA",
            "file:///home/private/I_PRIVATE.TGA",
            "https://example.invalid/I_PRIVATE.TGA",
            "../private/I_PRIVATE.TGA",
            "folder/../private/I_PRIVATE.TGA",
            "folder/\x7fprivate.tga",
            "I_PRIVATE.TGA\x85",
        ]
        for value in rejected:
            with self.subTest(value=value):
                safe_value, diagnostic = classify_texture_reference(value)
                self.assertIsNone(safe_value)
                self.assertIsNotNone(diagnostic)
                self.assertEqual(texture_reference_candidates(value), [])

        for value in ("I_ICON", r"Subdir\I_ICON.TGA",
                      "Data/Art/Textures/MixedCase.DDS"):
            with self.subTest(value=value):
                self.assertEqual(classify_texture_reference(value), (value, None))

    def test_rejected_reference_is_redacted_in_serializable_occurrence(self) -> None:
        private_values = [
            r"C:\Users\PRIVATE\I_PRIVATE.TGA",
            "/home/private/I_PRIVATE.TGA",
            r"\\server\private\I_PRIVATE.TGA",
            "file:///home/private/I_PRIVATE.TGA",
        ]
        xml = "<Root>" + "".join(
            f"<Icon_Name>{value}</Icon_Name>" for value in private_values) + "</Root>"
        source = Source("data/xml/test.xml", "fixture", "loose", "test",
                        xml.encode("utf-8"), "hash")
        rows = extract_texture_references(source, {"icon_name": ["schema#L1"]})
        self.assertEqual(len(rows), len(private_values))
        self.assertTrue(all(row["value"] == REDACTED_TEXTURE_REFERENCE for row in rows))
        self.assertTrue(all("diagnostic" in row for row in rows))
        serialized = json.dumps(rows)
        for private_value in ("PRIVATE", "/home/", "\\\\server", "file://"):
            self.assertNotIn(private_value, serialized)


if __name__ == "__main__":
    unittest.main()
