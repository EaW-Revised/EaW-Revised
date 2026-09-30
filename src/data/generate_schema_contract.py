#!/usr/bin/env python3
"""Generate the compact runtime lookup from the accepted P0-09 contract."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any


SCHEMA_REVISION = "3e1b825a124fbc13b2293665f34a36dd4d4be80f"
PROFILE_IDS = {"eaw": 0, "foc": 1, "remake": 2}
NODE_KIND_IDS = {"element": 0, "attribute": 1}
STATUS_IDS = {"unknown": 0, "known": 1, "deprecated": 2}
TEXT_FIELDS = ("object_type", "tag_path", "tag_name", "schema_ref")
STRING_CHUNK_LIMIT = 60_000
STRING_CHUNK_OFFSET_BITS = 24


def cpp(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def cpp_blob_literal(value: str) -> str:
    """Encode UTF-8 bytes with source/execution-charset-independent escapes."""

    encoded: list[str] = []
    for byte in value.encode("utf-8") + b"\0":
        if byte == ord('"'):
            encoded.append(r'\"')
        elif byte == ord("\\"):
            encoded.append(r"\\")
        elif 0x20 <= byte <= 0x7E:
            encoded.append(chr(byte))
        else:
            # Three fixed octal digits cannot consume a following digit.
            encoded.append(f"\\{byte:03o}")
    return '"' + "".join(encoded) + '"'


def selected_rows(value: dict[str, Any]) -> tuple[list[dict[str, Any]], list[str]]:
    if value.get("schema_version") != 1:
        raise SystemExit("unsupported XML inventory schema version")
    revision = value.get("schema_revision")
    if revision != SCHEMA_REVISION:
        raise SystemExit(f"unexpected eaw-schema revision: {revision}")

    selected: dict[tuple[str, str, str, str, str], dict[str, object]] = {}
    object_types: set[str] = set()
    for row in value["rows"]:
        for field in ("profile", "object_type", "tag_path", "node_kind", "tag_name", "status"):
            if not isinstance(row.get(field), str):
                raise SystemExit(f"XML inventory row has invalid {field!r}")
        if row.get("schema_ref") is not None and not isinstance(row["schema_ref"], str):
            raise SystemExit("XML inventory row has invalid 'schema_ref'")
        if row["profile"] not in PROFILE_IDS:
            raise SystemExit(f"unsupported XML profile: {row['profile']}")
        if row["node_kind"] not in NODE_KIND_IDS:
            raise SystemExit(f"unsupported XML node kind: {row['node_kind']}")
        if row["status"] not in STATUS_IDS:
            raise SystemExit(f"unsupported XML schema status: {row['status']}")
        key = (
            row["profile"], row["object_type"].lower(), row["tag_path"].lower(),
            row["node_kind"], row["tag_name"].lower())
        previous = selected.get(key)
        if previous is None or STATUS_IDS[row["status"]] > STATUS_IDS[previous["status"]]:
            selected[key] = row
        object_types.add(row["object_type"])
    return [selected[key] for key in sorted(selected)], sorted(object_types, key=str.lower)


@dataclass(frozen=True)
class PackedContract:
    string_chunks: tuple[tuple[str, ...], ...]
    rows: tuple[tuple[int, int, int, int, int, int, int], ...]
    object_type_locators: tuple[int, ...]


def pack_contract(rows: list[dict[str, Any]], object_types: list[str]) -> PackedContract:
    locators: dict[str, int] = {}
    chunks: list[list[str]] = [[]]
    chunk_size = 0

    def intern(value: str) -> int:
        nonlocal chunk_size
        found = locators.get(value)
        if found is not None:
            return found
        byte_size = len(value.encode("utf-8")) + 1
        if byte_size > STRING_CHUNK_LIMIT:
            raise SystemExit("XML schema contains a string larger than the portable chunk limit")
        if chunks[-1] and chunk_size + byte_size > STRING_CHUNK_LIMIT:
            chunks.append([])
            chunk_size = 0
        chunk_index = len(chunks) - 1
        if chunk_index >= (1 << (32 - STRING_CHUNK_OFFSET_BITS)):
            raise SystemExit("XML schema string table exceeds 32-bit locators")
        locator = (chunk_index << STRING_CHUNK_OFFSET_BITS) | chunk_size
        locators[value] = locator
        chunks[-1].append(value)
        chunk_size += byte_size
        return locator

    packed_rows: list[tuple[int, int, int, int, int, int, int]] = []
    for row in rows:
        text_offsets = [intern(str(row[field] or "")) for field in TEXT_FIELDS]
        packed_rows.append((
            *text_offsets,
            PROFILE_IDS[row["profile"]],
            NODE_KIND_IDS[row["node_kind"]],
            STATUS_IDS[row["status"]],
        ))
    object_type_locators = tuple(intern(name) for name in object_types)
    return PackedContract(tuple(tuple(chunk) for chunk in chunks), tuple(packed_rows), object_type_locators)


def render_contract(value: dict[str, Any]) -> str:
    rows, object_types = selected_rows(value)
    packed = pack_contract(rows, object_types)

    lines = [
        "// Generated from plan/inventories/xml-tags.json; do not edit.",
        f"inline constexpr std::string_view schema_revision = {cpp(SCHEMA_REVISION)};",
        f"inline constexpr std::uint32_t schema_string_chunk_shift = {STRING_CHUNK_OFFSET_BITS};",
        "inline constexpr std::uint32_t schema_string_chunk_offset_mask = "
        "(1U << schema_string_chunk_shift) - 1U;",
    ]
    for index, chunk in enumerate(packed.string_chunks):
        lines.append(f"inline constexpr char schema_string_blob_{index}[] =")
        lines.extend(f"    {cpp_blob_literal(value)}" for value in chunk)
        lines.append(";")
    lines.append("inline constexpr const char* schema_string_blobs[] = {")
    for index in range(len(packed.string_chunks)):
        lines.append(f"    schema_string_blob_{index},")
    lines.append("};")
    lines.append("inline constexpr PackedSchemaRow schema_rows[] = {")
    for row in packed.rows:
        lines.append("    {%s}," % ", ".join(str(value) for value in row))
    lines.append("};")
    lines.append("inline constexpr std::uint32_t schema_object_type_locators[] = {")
    for locator in packed.object_type_locators:
        lines.append(f"    {locator},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    value = json.loads(args.input.read_text(encoding="utf-8"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(render_contract(value))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
