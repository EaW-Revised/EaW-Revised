#!/usr/bin/env python3
"""Fixture builders and report checks for test_asset_validate.py."""

from __future__ import annotations

import hashlib
import json
import struct
import subprocess
from pathlib import Path


COUNT_KEYS = {
    "meshes", "bones", "materials", "animations",
    "vertices", "indices", "mips", "samples",
    "maps", "terrain_samples", "placements", "semantic_maps",
    "placements_crc_absent", "placements_crc_missing",
    "placements_crc_collision", "placements_crc_unique",
    "placements_model_renderable", "placements_model_unresolved",
    "placements_model_undeclared", "placements_model_unknown",
    "texture_references", "textures_resolved", "texture_slots_untextured",
    "sky_references", "skies_resolved", "skies_model_renderable",
}

CRC_BUCKETS = (
    "placements_crc_absent", "placements_crc_missing",
    "placements_crc_collision", "placements_crc_unique",
)
MODEL_BUCKETS = (
    "placements_model_renderable", "placements_model_unresolved",
    "placements_model_undeclared", "placements_model_unknown",
)
# Single-map inspect output, in order; the candidate ledger is the only addition.
INSPECT_MAP_KEYS = [
    "logical_path", "sha256", "stored_size", "kind", "semantic_complete",
    "terrain_width", "terrain_height", "terrain_samples", "terrain_materials",
    "environments", "water_records", "placements", "placements_crc_absent",
    "placements_crc_missing", "placements_crc_collision", "placements_crc_unique",
    "placements_model_renderable", "placements_model_unresolved",
    "placements_model_undeclared", "placements_model_unknown",
    "texture_references", "textures_resolved", "sky_references", "skies_resolved",
    "skies_model_renderable", "notices", "semantics", "unresolved_references",
    "environment_reference_ledger", "source_bounds_ledger", "environment_candidate_ledger",
]
RATE_KEYS = (
    "placement_type_resolution", "placement_model_renderability",
    "terrain_and_cloud_textures", "environment_skies", "environment_sky_models",
)


def chunk(identifier: int, payload: bytes, group: bool = False) -> bytes:
    size = len(payload) | (0x80000000 if group else 0)
    return struct.pack("<II", identifier, size) + payload


def mini(identifier: int, payload: bytes) -> bytes:
    return struct.pack("<BB", identifier, len(payload)) + payload


def ted_fixture() -> bytes:
    """An original synthetic space TED with one unresolvable placement.

    It exists so the map resolution fields are exercised by a record that is
    honestly unresolved rather than by an empty denominator.
    """
    root = mini(0, struct.pack("<I", 0x0201)) + mini(1, struct.pack("<I", 2))
    placed = (
        mini(0, struct.pack("<I", 4242))
        + mini(1, struct.pack("<I", 0xA1B2C3D4))
        + mini(4, struct.pack("<fff", 1.0, 2.0, 3.0))
        + mini(5, struct.pack("<fff", 0.0, 0.0, 45.0))
    )
    objects = chunk(1100, chunk(1113, chunk(1200, placed), group=True), group=True)
    main = chunk(256, b"", group=True) + chunk(258, chunk(1, objects, group=True), group=True)
    return struct.pack("<II", 0, len(root)) + root + chunk(1, main, group=True)


# A terrain material's texture mini is not guaranteed to hold a real string, so
# the fixture declares one that is not valid UTF-8.  The report must stay valid
# UTF-8 JSON and must still count the name as one unresolved reference.
BAD_TEXTURE_NAME = b"bad\xf6name.tga\x00"


def ted_land_fixture() -> bytes:
    """An original synthetic 2x1 land TED with one undecodable texture name."""
    root = mini(0, struct.pack("<I", 0x0201)) + mini(1, struct.pack("<I", 1))
    header = (
        mini(0, struct.pack("<I", 2)) + mini(1, struct.pack("<I", 1))
        + mini(4, struct.pack("<I", 2)) + mini(5, struct.pack("<I", 1))
    )
    materials = chunk(3, mini(12, BAD_TEXTURE_NAME), group=False)
    plane = struct.pack("<hBB", -3, 0, 17) + struct.pack("<hBB", 9, 0, 240)
    terrain = chunk(0, header) + chunk(2, materials, group=True) + chunk(5, plane)
    main = (
        chunk(256, b"", group=True)
        + chunk(257, terrain, group=True)
        + chunk(266, plane)
        + chunk(267, b"", group=True)
        + chunk(258, chunk(1, b"", group=True), group=True)
    )
    return struct.pack("<II", 0, len(root)) + root + chunk(1, main, group=True)


def dds_fixture() -> bytes:
    words = [
        0x20534444, 124, 0x100F, 1, 1, 4, 0, 1,
        *([0] * 11),
        32, 0x41, 0, 32,
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000,
        0x1000, 0, 0, 0, 0,
    ]
    return struct.pack("<" + "I" * len(words), *words) + bytes((3, 2, 1, 4))


def environment_ledger_fixture() -> bytes:
    """Four decoded environments with a malformed sibling at ordinal one."""
    root = mini(0, struct.pack("<I", 0x0201)) + mini(1, struct.pack("<I", 2))
    first = (mini(0x14, b"First\x00") + mini(0x19, b"MissingSky\x00")
             + mini(0x1a, b"NoModel\x00") + mini(0x2f, b"MissingCloud.tga\x00"))
    second = (mini(0x14, b"Second\x00") + mini(0x19, b"BadModel\x00")
              + mini(0x1a, b"GoodSky\x00") + mini(0x2f, b"GoodCloud.tga\x00"))
    third = (mini(0x14, b"Third\x00") + mini(0x19, b"GoodSky\x00")
             + mini(0x19, b"MissingSky\x00") + mini(0x2f, b"\x00"))
    fourth = mini(0x14, b"Fourth\x00") + mini(0x19, b"Bad\xf6\"Sky\x00")
    environments = b"".join(chunk(6, value) for value in (first, b"\x19", second, third, fourth))
    main = (chunk(256, chunk(4, environments, group=True), group=True)
            + chunk(258, chunk(1, b"", group=True), group=True))
    return struct.pack("<II", 0, len(root)) + root + chunk(1, main, group=True)


def float3(x: float, y: float, z: float) -> bytes:
    return struct.pack("<fff", x, y, z)


def float1(value: float) -> bytes:
    return struct.pack("<f", value)


def complete_candidate_minis() -> list[tuple[int, bytes]]:
    """Invented values in the candidate layout: five float3 then nine floats."""
    minis = [(identifier, float3(0.1 * identifier, 0.2, 0.3)) for identifier in range(5)]
    minis += [(identifier, float1(0.5 + identifier)) for identifier in range(5, 14)]
    return minis


# Candidate TED environments: one complete record wrapped in unknown IDs, a
# malformed sibling, one record with every failure kind and repeated IDs, and
# one record with no candidate mini at all.  Every value is invented.
CANDIDATE_ENVIRONMENTS: list[list[tuple[int, bytes]] | bytes] = [
    [(0x14, b"Complete\x00"), (0x0e, float1(9.0))] + complete_candidate_minis() + [(0x2f, b"Cloud.tga\x00")],
    b"\x19",
    [
        (0x14, b"Faulty\x00"),
        (0x01, float1(1.0) * 2),                                    # wrong_size: 8 of 12
        (0x02, float3(0.0, float("nan"), 0.0)),                     # nonfinite
        (0x03, float1(1.0)),                                        # malformed first ...
        (0x03, float3(1.0, 1.0, 1.0)),                              # ... valid later
        (0x04, float3(0.2, 0.2, 0.2)),                              # valid first ...
        (0x04, float3(float("inf"), 0.0, 0.0)),                     # ... nonfinite later
        (0x05, float1(float("-inf"))),                              # nonfinite
        (0x06, b""),                                                # wrong_size: 0 of 4
        (0x07, float3(1.0, 2.0, 3.0)),                              # wrong_size: float3 in a float slot
        (0x20, float1(4.0)),                                        # unknown ID
        (0x08, float1(0.0)), (0x09, float1(0.0)), (0x0a, float1(0.0)),
        (0x0b, float1(0.1)), (0x0c, float1(0.2)), (0x0d, float1(0.3)),
        (0x0b, float1(0.4)), (0x0b, b"\x00"),                       # repeated: first valid kept
    ],
    [(0x14, b"Empty\x00"), (0x19, b"NoSky\x00")],
]


def candidate_ledger_fixture() -> tuple[bytes, list[list[int]]]:
    """The TED and the independently laid out mini header offsets per record."""
    root = mini(0, struct.pack("<I", 0x0201)) + mini(1, struct.pack("<I", 2))
    payloads = [value if isinstance(value, bytes) else b"".join(mini(i, p) for i, p in value)
                for value in CANDIDATE_ENVIRONMENTS]
    environments = b"".join(chunk(6, payload) for payload in payloads)
    main = (chunk(256, chunk(4, environments, group=True), group=True)
            + chunk(258, chunk(1, b"", group=True), group=True))
    ted = struct.pack("<II", 0, len(root)) + root + chunk(1, main, group=True)
    # file header, root minis, then the 1, 256 and 4 chunk headers
    cursor = 8 + len(root) + 8 + 8 + 8
    offsets: list[list[int]] = []
    for value, payload in zip(CANDIDATE_ENVIRONMENTS, payloads):
        cursor += 8
        record: list[int] = []
        if not isinstance(value, bytes):
            position = cursor
            for _, field in value:
                record.append(position)
                position += 2 + len(field)
        offsets.append(record)
        cursor += len(payload)
    return ted, offsets


def expected_candidate_rows(minis: list[tuple[int, bytes]], offsets: list[int]) -> list[dict]:
    """First-occurrence, shape-only reading, derived here from the raw minis."""
    rows = []
    for identifier in range(14):
        expected = 12 if identifier < 5 else 4
        ordinals = [index for index, (field, _) in enumerate(minis) if field == identifier]
        row = {"field_id": identifier, "expected_size": expected, "occurrence_count": len(ordinals),
               "status": "missing", "observed_size": 0,
               "selected_field_ordinal": None, "selected_byte_offset": None}
        if ordinals:
            selected = ordinals[0]
            payload = minis[selected][1]
            row.update(selected_field_ordinal=selected, selected_byte_offset=offsets[selected],
                       observed_size=len(payload))
            if len(payload) != expected:
                row["status"] = "wrong_size"
            elif any(value != value or value in (float("inf"), float("-inf"))
                     for value in struct.unpack(f"<{expected // 4}f", payload)):
                row["status"] = "nonfinite"
            else:
                row["status"] = "decoded"
        rows.append(row)
    return rows


def check_environment_candidate_ledger(validator: Path, data: Path) -> None:
    logical = "data/art/maps/candidate.ted"
    ted, offsets = candidate_ledger_fixture()
    (data / "Art" / "Maps" / "Candidate.TED").write_bytes(ted)

    def inspect() -> tuple[bytes, dict]:
        completed = subprocess.run(
            [str(validator), "--profile", "eaw", "--game-root", str(data), "--inspect-map", logical],
            check=False, capture_output=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"candidate inspection failed: {completed.stderr!r}")
        if any(token in completed.stdout for token in (b"NaN", b"Infinity")):
            raise AssertionError("candidate ledger leaked a nonfinite number")
        return completed.stdout, json.loads(completed.stdout.decode("utf-8"))

    raw, loaded = inspect()
    if raw != inspect()[0]:
        raise AssertionError("candidate ledger JSON is not byte deterministic")
    if loaded["environment_reference_ledger"]["catalog_status"] != "loaded":
        raise AssertionError("the with-catalog candidate run did not load the catalog")
    ledger = loaded["environment_candidate_ledger"]
    if list(ledger) != ["schema_version", "mapping_status", "selected_environment", "environments"] or \
            ledger["schema_version"] != 1 or ledger["mapping_status"] != "unconfirmed" or \
            ledger["selected_environment"] is not None:
        raise AssertionError(f"candidate ledger identity changed or claimed a selection: {ledger}")
    decoded = [(ordinal, value) for ordinal, value in enumerate(CANDIDATE_ENVIRONMENTS)
               if not isinstance(value, bytes)]
    environments = ledger["environments"]
    if [environment["environment_ordinal"] for environment in environments] != [0, 2, 3]:
        raise AssertionError("malformed sibling did not leave an ordinal gap")
    for environment, (ordinal, minis) in zip(environments, decoded):
        if set(environment) != {"environment_ordinal", "fields"}:
            raise AssertionError(f"candidate environment shape changed: {environment}")
        expected = expected_candidate_rows(minis, offsets[ordinal])
        if environment["fields"] != expected:
            raise AssertionError(f"record {ordinal} candidate rows differ:\n"
                                 f"{environment['fields']}\n!=\n{expected}")
    complete, faulty, empty = (environment["fields"] for environment in environments)
    if any(row["status"] != "decoded" or row["occurrence_count"] != 1 for row in complete):
        raise AssertionError("the complete record did not decode every candidate field once")
    if [row["status"] for row in faulty] != [
            "missing", "wrong_size", "nonfinite", "wrong_size", "decoded", "nonfinite", "wrong_size",
            "wrong_size", "decoded", "decoded", "decoded", "decoded", "decoded", "decoded"]:
        raise AssertionError(f"faulty record statuses differ: {[row['status'] for row in faulty]}")
    # Malformed first, valid later: the first occurrence is still the one judged.
    if (faulty[3]["occurrence_count"], faulty[3]["observed_size"], faulty[3]["selected_field_ordinal"]) != (2, 4, 3):
        raise AssertionError(f"malformed-first duplicate was not first-occurrence: {faulty[3]}")
    if faulty[3]["selected_byte_offset"] != ted.index(mini(0x03, float1(1.0))):
        raise AssertionError("selected byte offset does not name the first TED mini header")
    if (faulty[4]["occurrence_count"], faulty[4]["selected_field_ordinal"]) != (2, 5):
        raise AssertionError(f"valid-first duplicate did not keep the first mini: {faulty[4]}")
    if (faulty[11]["occurrence_count"], faulty[11]["selected_field_ordinal"]) != (3, 14):
        raise AssertionError(f"thrice-repeated ID lost its count or first ordinal: {faulty[11]}")
    if any(row != {"field_id": index, "expected_size": 12 if index < 5 else 4, "occurrence_count": 0,
                   "status": "missing", "observed_size": 0, "selected_field_ordinal": None,
                   "selected_byte_offset": None} for index, row in enumerate(empty)):
        raise AssertionError("a record with no candidate minis invented a field")

    # The candidate ledger depends only on TED bytes, never on the XML catalog.
    registry = data / "XML" / "GameObjectFiles.xml"
    parked = registry.with_suffix(".parked")
    registry.rename(parked)
    try:
        raw_without, without = inspect()
    finally:
        parked.rename(registry)
    if without["environment_reference_ledger"]["catalog_status"] != "unavailable":
        raise AssertionError("parking the registry did not make the catalog unavailable")
    if without["environment_candidate_ledger"] != ledger:
        raise AssertionError("XML catalog availability changed the candidate ledger")
    marker = b'"environment_candidate_ledger":'
    if raw[raw.index(marker):] != raw_without[raw_without.index(marker):]:
        raise AssertionError("candidate ledger bytes differ with and without the catalog")


def source_bounds_fixture(root_extra: bytes = b"", volume_stream: bytes | None = None) -> bytes:
    root = mini(0, struct.pack("<I", 0x0201)) + mini(1, struct.pack("<I", 2)) + root_extra
    main = chunk(256, b"", group=True) + chunk(258, chunk(1, b"", group=True), group=True)
    if volume_stream is not None:
        main += chunk(259, volume_stream)
    return struct.pack("<II", 0, len(root)) + root + chunk(1, main, group=True)


def check_source_bounds_ledger(validator: Path, data: Path) -> None:
    logical = "data/art/maps/bounds.ted"
    target = data / "Art" / "Maps" / "Bounds.TED"

    def inspect(ted: bytes) -> tuple[bytes, dict]:
        target.write_bytes(ted)
        completed = subprocess.run(
            [str(validator), "--profile", "eaw", "--game-root", str(data), "--inspect-map", logical],
            check=False, capture_output=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"bounds inspection failed: {completed.stderr!r}")
        if any(token in completed.stdout for token in (b"NaN", b"Infinity", str(data).encode())):
            raise AssertionError("ledger leaked a nonfinite number or native host path")
        return completed.stdout, json.loads(completed.stdout.decode("utf-8"),
                                            parse_constant=lambda value: (_ for _ in ()).throw(AssertionError(value)))

    extent_a = mini(0x10, struct.pack("<f", 4000.0))
    extent_b = mini(0x11, struct.pack("<f", 3000.0))
    entries = [
        (0, struct.pack("<fff", 0, 0, -10)),
        (1, struct.pack("<fff", 40, 60, 90)),
        (0, struct.pack("<fff", -5, -5, -5)),
        (1, struct.pack("<fff", 5, 5, 5)),
        (2, struct.pack("<fff", 9, 9, 9)),
        (4, struct.pack("<fff", 1, 1, 1)),
        (5, struct.pack("<fff", float("nan"), 2, 3)),
        (6, struct.pack("<fff", 4, 5, 6)),
        (7, struct.pack("<f", 1)),
        (8, struct.pack("<fff", 4, 5, 6)),
    ]
    stream = b"".join(mini(identifier, payload) for identifier, payload in entries)
    ted = source_bounds_fixture(extent_a + extent_b, stream)
    raw, loaded = inspect(ted)
    if raw != inspect(ted)[0]:
        raise AssertionError("source bounds JSON is not byte deterministic")
    ledger = loaded["source_bounds_ledger"]
    if ledger["schema_version"] != 1 or ledger["source_coordinates"] != "unchanged_ted" or \
            ledger["interpretation"] != {"kind": "structural_source_bounds", "selected_camera": None,
                                          "playable_boundary": None, "inferred_volume_meaning": False}:
        raise AssertionError("source bounds claimed a camera, boundary, or volume meaning")
    first_offset = 8 + len(mini(0, struct.pack("<I", 0x0201))) + len(mini(1, struct.pack("<I", 2)))
    if ledger["declared_extents"] != {"first_field_id": 16, "first_byte_offset": first_offset,
                                     "first": 4000, "second_field_id": 17,
                                     "second_byte_offset": first_offset + len(extent_a), "second": 3000}:
        raise AssertionError(f"extent values or independently calculated mini offsets differ: {ledger['declared_extents']}")
    stream_start = 8 + struct.unpack_from("<I", ted, 4)[0] + 8 + len(chunk(256, b"", group=True)) \
        + len(chunk(258, chunk(1, b"", group=True), group=True)) + 8
    offsets = []
    cursor = stream_start
    for identifier, payload in entries:
        offsets.append(cursor)
        cursor += 2 + len(payload)
    expected_volumes = [
        {"pair_ordinal": 0, "min_field_id": 0, "min_byte_offset": offsets[0],
         "max_field_id": 1, "max_byte_offset": offsets[1],
         "minimum": [0, 0, -10], "maximum": [40, 60, 90]},
        {"pair_ordinal": 1, "min_field_id": 0, "min_byte_offset": offsets[2],
         "max_field_id": 1, "max_byte_offset": offsets[3],
         "minimum": [-5, -5, -5], "maximum": [5, 5, 5]},
    ]
    if ledger["volumes"] != expected_volumes:
        raise AssertionError(f"ordered source volume pairs differ: {ledger['volumes']}")
    expected_fields = [{"field_id": identifier, "byte_offset": offset, "payload_length": len(payload),
                        "participates_in_pair": index < 4}
                       for index, ((identifier, payload), offset) in enumerate(zip(entries, offsets))]
    if ledger["volume_fields"] != expected_fields:
        raise AssertionError("retained field metadata lost order, offsets, sizes or pair participation")
    if ledger["notices"] or b"payload" in raw.replace(b'"payload_length"', b""):
        raise AssertionError("valid ledger emitted a bounds notice or raw payload bytes")

    # Older/reversed vectors, absent section and incomplete/invalid extents stay visible.
    old_stream = mini(0, struct.pack("<I", 1)) + mini(3, struct.pack("<fff", 9, 9, 9)) \
        + mini(4, struct.pack("<fff", 1, 1, 1)) + mini(6, struct.pack("<fff", 0, 0, 0))
    old = inspect(source_bounds_fixture(volume_stream=old_stream))[1]["source_bounds_ledger"]
    if old["declared_extents"] is not None or old["volumes"] or len(old["volume_fields"]) != 4 or \
            any(field["participates_in_pair"] for field in old["volume_fields"]):
        raise AssertionError("older standalone or reversed vectors were paired")
    absent = inspect(source_bounds_fixture())[1]["source_bounds_ledger"]
    if absent["volumes"] or absent["volume_fields"] or \
            [notice["issue_code"] for notice in absent["notices"]] != ["optional_section_absent"]:
        raise AssertionError("absent 1/259 notice or empty arrays differ")
    half = inspect(source_bounds_fixture(root_extra=extent_a))[1]["source_bounds_ledger"]
    invalid = inspect(source_bounds_fixture(root_extra=mini(0x10, struct.pack("<f", float("inf"))) + extent_b))[1]["source_bounds_ledger"]
    for name, item in (("half", half), ("invalid", invalid)):
        if item["declared_extents"] is not None or \
                "declared_extents_invalid" not in [notice["issue_code"] for notice in item["notices"]]:
            raise AssertionError(f"{name} extents invented a pair or lost its notice")
    duplicated = inspect(source_bounds_fixture(root_extra=extent_a + extent_a + extent_b))[1]["source_bounds_ledger"]
    duplicate_codes = [notice["issue_code"] for notice in duplicated["notices"]]
    if duplicated["declared_extents"] is not None or duplicate_codes[:2] != ["duplicate_root_field", "declared_extents_invalid"]:
        raise AssertionError(f"duplicate extent was selected or its original notices were lost: {duplicated}")

    # The bounds ledger depends only on TED bytes, even when the XML catalog changes.
    before = inspect(ted)[1]["source_bounds_ledger"]
    registry = data / "XML" / "GameObjectFiles.xml"
    parked = registry.with_suffix(".parked")
    registry.rename(parked)
    try:
        after = inspect(ted)[1]["source_bounds_ledger"]
    finally:
        parked.rename(registry)
    if before != after:
        raise AssertionError("XML catalog availability changed source bounds")


def check_environment_ledger(validator: Path, data: Path, old_ted: bytes) -> None:
    logical_path = "data/art/maps/ledger.ted"
    ted = environment_ledger_fixture()
    (data / "Art" / "Maps" / "Ledger.TED").write_bytes(ted)

    def inspect(path: str):
        completed = subprocess.run(
            [str(validator), "--profile", "eaw", "--game-root", str(data), "--inspect-map", path],
            check=False, capture_output=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"inspect-map failed: {completed.stderr!r}")
        return completed.stdout, json.loads(completed.stdout.decode("utf-8"))

    unavailable_raw, unavailable = inspect(logical_path)
    if unavailable_raw != inspect(logical_path)[0]:
        raise AssertionError("repeated unavailable-catalog inspection is not byte-identical")
    if unavailable["environment_reference_ledger"]["catalog_status"] != "unavailable":
        raise AssertionError("missing game-object registry was reported as loaded")
    if [row["status"] for row in unavailable["environment_reference_ledger"]["references"]] != [
            "missing_catalog_object", "missing_catalog_object", "unresolved_texture",
            "missing_catalog_object", "missing_catalog_object", "unresolved_texture",
            "missing_catalog_object", "undeclared", "undeclared",
            "missing_catalog_object", "undeclared", "undeclared"]:
        raise AssertionError("unavailable catalog assigned resolved sky statuses")
    if unavailable["sha256"] != hashlib.sha256(ted).hexdigest():
        raise AssertionError("inspect-map TED hash differs from independently calculated hash")
    empty = inspect("data/art/maps/fixture.ted")[1]["environment_reference_ledger"]
    if empty != {"schema_version": 1, "catalog_status": "unavailable", "references": []}:
        raise AssertionError(f"map with no environments invented ledger rows: {empty}")
    if hashlib.sha256(old_ted).hexdigest() != inspect("data/art/maps/fixture.ted")[1]["sha256"]:
        raise AssertionError("existing inspect-map TED hash changed")
    if list(unavailable) != INSPECT_MAP_KEYS:
        raise AssertionError(f"inspect-map keys changed beyond the additive candidate ledger: {list(unavailable)}")
    no_candidates = inspect("data/art/maps/fixture.ted")[1]["environment_candidate_ledger"]
    if no_candidates != {"schema_version": 1, "mapping_status": "unconfirmed",
                         "selected_environment": None, "environments": []}:
        raise AssertionError(f"map with no environments invented candidate rows: {no_candidates}")
    sky_only = unavailable["environment_candidate_ledger"]["environments"]
    if [environment["environment_ordinal"] for environment in sky_only] != [0, 2, 3, 4] or \
            any(row["status"] != "missing" or row["selected_byte_offset"] is not None
                for environment in sky_only for row in environment["fields"]) or \
            any([row["field_id"] for row in environment["fields"]] != list(range(14)) for environment in sky_only):
        raise AssertionError("sky-only environments did not report fourteen missing candidate fields")

    xml = data / "XML"
    xml.mkdir()
    (xml / "GameObjectFiles.xml").write_text(
        "<Game_Object_Files><File>ledger_a.xml</File><File>ledger_b.xml</File></Game_Object_Files>",
        encoding="utf-8",
    )
    (xml / "ledger_a.xml").write_text(
        "<SpaceProps>\n<SpaceProp Name=\"GoodSky\">\n"
        "<Space_Model_Name>old_sky.alo</Space_Model_Name>\n</SpaceProp>\n</SpaceProps>\n",
        encoding="utf-8",
    )
    (xml / "ledger_b.xml").write_text(
        "<SpaceProps>\n<SpaceProp Name=\"NoModel\"/>\n"
        "<SpaceProp Name=\"BadModel\">\n<Space_Model_Name>missing_sky.alo</Space_Model_Name>\n</SpaceProp>\n"
        "<SpaceProp Name=\"GoodSky\">\n<Space_Model_Name>good_sky.alo</Space_Model_Name>\n</SpaceProp>\n"
        "</SpaceProps>\n",
        encoding="utf-8",
    )
    (data / "Art" / "Models" / "good_sky.alo").write_bytes(b"probe-only model")
    (data / "Art" / "Textures" / "GoodCloud.DDS").write_bytes(b"probe-only texture")
    loaded_raw, loaded = inspect(logical_path)
    if loaded_raw != inspect(logical_path)[0]:
        raise AssertionError("repeated inspect-map output is not byte-identical")
    ledger = loaded["environment_reference_ledger"]
    if ledger["schema_version"] != 1 or ledger["catalog_status"] != "loaded":
        raise AssertionError(f"loaded ledger identity changed: {ledger}")
    rows = ledger["references"]
    if len(rows) != 12:
        raise AssertionError(f"expected three rows per decoded environment, got {len(rows)}")
    fields = ("primary_sky", "secondary_sky", "cloud_texture")
    field_ids = (0x19, 0x1a, 0x2f)
    for index, row in enumerate(rows):
        expected = {"environment_ordinal": (0, 2, 3, 4)[index // 3],
                    "field": fields[index % 3], "field_id": field_ids[index % 3]}
        if any(row[key] != value for key, value in expected.items()):
            raise AssertionError(f"ledger row {index} lost order or identity: {row}")
        if set(row) != {"environment_ordinal", "field", "field_id",
                        "declaration_byte_offset", "reference_name", "catalog_source",
                        "model_name", "status"}:
            raise AssertionError(f"ledger row {index} has wrong shape: {row}")
    statuses = ["missing_catalog_object", "undeclared_model", "unresolved_texture",
                "unresolved_model", "resolved", "resolved",
                "missing_catalog_object", "undeclared", "undeclared",
                "missing_catalog_object", "undeclared", "undeclared"]
    if [row["status"] for row in rows] != statuses:
        raise AssertionError(f"ledger statuses changed: {[row['status'] for row in rows]}")
    if rows[0]["reference_name"] != "MissingSky" or rows[6]["reference_name"] != "MissingSky":
        raise AssertionError("repeated unresolved sky names were collapsed")
    first_offset = ted.index(mini(0x19, b"MissingSky\x00"))
    last_offset = ted.rindex(mini(0x19, b"MissingSky\x00"))
    if (rows[0]["declaration_byte_offset"], rows[6]["declaration_byte_offset"]) != (first_offset, last_offset):
        raise AssertionError("TED mini offset does not name the effective last-valid declaration")
    if rows[7]["reference_name"] is not None or rows[7]["declaration_byte_offset"] is not None:
        raise AssertionError("absent field did not retain null name and offset")
    if rows[8]["reference_name"] != "" or rows[8]["declaration_byte_offset"] != ted.index(mini(0x2f, b"\x00")):
        raise AssertionError("explicit empty field was conflated with an absent field")
    if rows[9]["reference_name"] != 'Badö"Sky' or b"\\u00f6" not in loaded_raw or b'\\\"Sky' not in loaded_raw:
        raise AssertionError("invalid authored bytes did not round-trip through UTF-8 JSON escapes")
    for index, line, model in ((1, 2, None), (3, 3, "missing_sky.alo"), (4, 6, "good_sky.alo")):
        source = rows[index]["catalog_source"]
        if source is None or source["logical_path"] != "data/xml/ledger_b.xml" or source["line"] != line:
            raise AssertionError(f"row {index} did not cite the effective XML object declaration: {source}")
        if set(source) != {"logical_path", "source_id", "layer_id", "line", "column"}:
            raise AssertionError(f"catalog source shape changed: {source}")
        if not source["source_id"] or source["layer_id"] != "base" or source["column"] != 2:
            raise AssertionError(f"catalog source provenance changed: {source}")
        if rows[index]["model_name"] != model:
            raise AssertionError(f"row {index} selected wrong model: {rows[index]}")
    if any(rows[index]["catalog_source"] is not None or rows[index]["model_name"] is not None
           for index in (0, 2, 5, 6, 7, 8, 9, 10, 11)):
        raise AssertionError("non-catalog rows invented XML provenance or models")
    if loaded["sha256"] != hashlib.sha256(ted).hexdigest() or loaded["logical_path"] != logical_path:
        raise AssertionError("inspect-map enclosing TED identity changed")
    unchanged = ("logical_path", "sha256", "stored_size", "kind", "semantic_complete",
                 "terrain_width", "terrain_height", "terrain_samples", "terrain_materials",
                 "environments", "water_records", "placements", "placements_crc_absent",
                 "placements_crc_missing", "placements_crc_collision", "placements_crc_unique",
                 "placements_model_renderable", "placements_model_unresolved",
                 "placements_model_undeclared", "placements_model_unknown", "notices", "semantics")
    if any(loaded[key] != unavailable[key] for key in unchanged):
        raise AssertionError("catalog availability changed unrelated inspect-map values")
    if (loaded["sky_references"], loaded["skies_resolved"], loaded["skies_model_renderable"],
        loaded["texture_references"], loaded["textures_resolved"]) != (6, 3, 1, 2, 1):
        raise AssertionError("existing inspect-map aggregate counters changed")
    unresolved = loaded["unresolved_references"]
    if unresolved["sky_object_ids"] != ["MissingSky", 'Badö"Sky'] or \
            unresolved["model_names"] != ["missing_sky.alo"] or \
            unresolved["texture_names"] != ["MissingCloud.tga"]:
        raise AssertionError(f"existing unresolved summaries changed: {unresolved}")


def add_counts(target: dict[str, int], source: dict[str, int]) -> None:
    for key in COUNT_KEYS:
        target[key] += source[key]
