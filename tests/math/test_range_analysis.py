from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = ROOT / "tools" / "analyse_numeric_ranges.py"
SPEC = importlib.util.spec_from_file_location("analyse_numeric_ranges", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
ANALYSER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYSER
SPEC.loader.exec_module(ANALYSER)
FIXTURES = Path(__file__).with_name("range-analysis")


def fixture_bytes(name: str) -> bytes:
    tokens = []
    for line in (FIXTURES / name).read_text(encoding="utf-8").splitlines():
        tokens.extend(line.split("#", 1)[0].split())
    return bytes.fromhex("".join(tokens))


def write_meg(path: Path, name: str, payload: bytes) -> None:
    encoded_name = name.encode("latin-1")
    start = 8 + 2 + len(encoded_name) + 20
    blob = bytearray(struct.pack("<II", 1, 1))
    blob += struct.pack("<H", len(encoded_name)) + encoded_name
    blob += struct.pack("<IIIII", 0, 0, len(payload), start, 0)
    blob += payload
    path.write_bytes(blob)


class NumericTextTests(unittest.TestCase):
    def test_accepts_exact_decimal_vectors(self) -> None:
        self.assertEqual(
            ANALYSER.parse_numeric_text(" -1.25, .5; 2e3 "),
            (Decimal("-1.25"), Decimal(".5"), Decimal("2e3")),
        )

    def test_rejects_mixed_enum_text(self) -> None:
        self.assertIsNone(ANALYSER.parse_numeric_text("1, ENERGY"))

    def test_decimal_text_does_not_round_to_context_precision(self) -> None:
        value = Decimal("123456789012345678901234567890.000")
        self.assertEqual(
            ANALYSER.decimal_text(value), "123456789012345678901234567890"
        )

    def test_magnitude_is_absolute_but_preserves_signed_source_value(self) -> None:
        accumulator = ANALYSER.RangeAccumulator()
        accumulator.add(
            ANALYSER.Evidence(Decimal("-1"), "fixture", "0" * 64, "POSITION[1]", 0, 1)
        )
        result = accumulator.as_json("fixture units")
        self.assertEqual(result["smallest_nonzero_magnitude"]["value"], "1")
        self.assertEqual(result["smallest_nonzero_magnitude"]["source_value"], "-1")

    def test_absolute_modifier_is_not_a_multiplier(self) -> None:
        self.assertNotIn(
            "multiplier", ANALYSER.categories_for_tag("ABSOLUTE_INCOME_MODIFIER")
        )

    def test_damage_percentage_is_not_direct_damage(self) -> None:
        self.assertNotIn("damage", ANALYSER.categories_for_tag("DAMAGE_PERCENTAGE"))
        self.assertNotIn("damage", ANALYSER.categories_for_tag("DAMAGE_MOD"))
        self.assertNotIn(
            "damage", ANALYSER.categories_for_tag("PROJECTILE_DAMAGE_DELAY_SECS")
        )


class TedTests(unittest.TestCase):
    def test_decodes_typed_extent_record(self) -> None:
        extent = ANALYSER.parse_ted_extent_bytes(fixture_bytes("ted-map-header.hex"))
        self.assertIsNotNone(extent)
        self.assertEqual((extent.width, extent.height), (Decimal("8960"), Decimal("4800")))
        self.assertEqual((extent.root_payload_size, extent.width_property_offset), (21, 14))

    def test_rejects_ambiguous_extent_records(self) -> None:
        record = bytearray(fixture_bytes("ted-map-header.hex"))
        record[4:8] = struct.pack("<I", 33)
        record.extend(record[14:26])
        with self.assertRaises(ANALYSER.AnalysisError):
            ANALYSER.parse_ted_extent_bytes(bytes(record))

    def test_does_not_scan_extent_bytes_outside_root(self) -> None:
        record = fixture_bytes("ted-map-header.hex")
        self.assertIsNone(ANALYSER.parse_ted_extent_bytes(b"prefix" + record))

    def test_rejects_truncated_root_without_searching_payload(self) -> None:
        record = bytearray(fixture_bytes("ted-map-header.hex"))
        record[4:8] = struct.pack("<I", 255)
        self.assertIsNone(ANALYSER.parse_ted_extent_bytes(bytes(record)))


class IntegrationTests(unittest.TestCase):
    def test_original_fixtures_produce_ranges_and_missing_ted_bound(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            xml_root = temporary_path / "xml"
            xml_root.mkdir()
            (xml_root / "fixture.xml").write_bytes(
                (FIXTURES / "range-sample.xml").read_bytes()
            )
            good_meg = temporary_path / "maps.meg"
            write_meg(good_meg, r"DATA\ART\MAPS\FIXTURE.TED", fixture_bytes("ted-map-header.hex"))
            missing_meg = temporary_path / "missing.meg"
            write_meg(missing_meg, r"DATA\ART\MAPS\MISSING.TED", b"no extent record")

            inventory = ANALYSER.build_inventory(
                [ANALYSER.LabelledPath("fixture-xml", xml_root)],
                [
                    ANALYSER.LabelledPath("fixture-maps.meg", good_meg),
                    ANALYSER.LabelledPath("missing-maps.meg", missing_meg),
                ],
            )

            categories = inventory["xml"]["categories"]
            self.assertEqual(categories["coordinate"]["minimum"]["value"], "-3000")
            self.assertEqual(categories["coordinate"]["maximum"]["value"], "2000")
            self.assertEqual(
                categories["damage"]["smallest_nonzero_magnitude"]["value"],
                "0.000244140625",
            )
            self.assertEqual(categories["multiplier"]["minimum"]["value"], "-2")
            self.assertEqual(
                inventory["ted"]["maximum_declared_axis_extent"]["value"], "8960"
            )
            self.assertEqual(len(inventory["ted"]["missing_extent_records"]), 1)
            self.assertEqual(
                inventory["headroom"]["screening_products"][2]["value"],
                str(3 * 8960 * 8960),
            )

    def test_loose_ted_root_is_enumerated_separately(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "MAP.TED").write_bytes(fixture_bytes("ted-map-header.hex"))
            inventory = ANALYSER.build_inventory(
                [], [], [ANALYSER.LabelledPath("loose", root)]
            )
            self.assertEqual(inventory["ted"]["ted_entry_count"], 1)
            self.assertEqual(inventory["ted"]["extent_record_count"], 1)
            self.assertEqual(inventory["ted"]["loose_corpora"][0]["file_count"], 1)
            self.assertEqual(
                inventory["ted"]["loose_corpora"][0]["files"][0]["source"],
                "loose/MAP.TED",
            )
            self.assertEqual(
                inventory["ted"]["structural_observations"][
                    "extent_pair_at_root_payload_minus_7_and_minus_1_count"
                ],
                1,
            )

    def test_malformed_xml_uses_bounded_complete_element_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            xml_root = Path(temporary)
            (xml_root / "fragment.xml").write_text(
                "<Objects><Damage>2.5</Damage><Broken>", encoding="utf-8"
            )
            result = ANALYSER.analyse_xml_roots(
                [ANALYSER.LabelledPath("fragment", xml_root)]
            )
            self.assertEqual(result["categories"]["damage"]["maximum"]["value"], "2.5")
            self.assertEqual(len(result["fallback_files"]), 1)
            self.assertEqual(result["unreadable_files"], [])
            self.assertEqual(result["corpora"][0]["xml_document_count"], 0)
            self.assertEqual(result["corpora"][0]["fallback_file_count"], 1)
            self.assertEqual(result["corpora"][0]["processed_file_count"], 1)

    def test_notable_duration_distributions_are_reproducible(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "durations.xml").write_text(
                "<R><Time_Limit>-1</Time_Limit>"
                "<Defense_Duration_In_Secs>30.0</Defense_Duration_In_Secs>"
                "<Defense_Duration_In_Secs>999999999999999.0</Defense_Duration_In_Secs>"
                "</R>",
                encoding="utf-8",
            )
            result = ANALYSER.analyse_xml_roots(
                [ANALYSER.LabelledPath("fixture", root)]
            )
            notable = result["notable_values"]
            self.assertEqual(
                notable["negative_duration_components_by_tag"],
                [
                    {
                        "tag": "TIME_LIMIT",
                        "value": "-1",
                        "component_count": 1,
                        "source_file_count": 1,
                    }
                ],
            )
            self.assertEqual(
                [row["value"] for row in notable["defense_duration_in_secs_distribution"]],
                ["30", "999999999999999"],
            )


if __name__ == "__main__":
    unittest.main()
