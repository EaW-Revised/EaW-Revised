#!/usr/bin/env python3
"""Measure numeric ranges in Empire at War XML and TED map metadata.

The program deliberately reports observed data rather than assigning gameplay
semantics that the source files do not establish.  Decimal XML values are parsed
with ``decimal.Decimal``; TED metadata is decoded from a bounded root-property
record and never treated as an XML-derived map bound.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from decimal import Decimal, InvalidOperation, localcontext
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator, Sequence

from tools.numeric_ranges.report import (
    AnalysisError,
    LabelledPath,
    decimal_text,
    sha256_path,
)

@dataclass(frozen=True)
class MegEntry:
    name: str
    size: int
    start: int


@dataclass(frozen=True)
class TedExtent:
    width: Decimal
    height: Decimal
    root_payload_size: int
    width_property_offset: int
    height_property_offset: int


def read_exact(stream: BinaryIO, size: int, context: str) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise AnalysisError(f"truncated {context}: expected {size} bytes, got {len(data)}")
    return data


def read_meg_index(stream: BinaryIO, file_size: int) -> list[MegEntry]:
    header = read_exact(stream, 8, "MEG header")
    filename_count, file_count = struct.unpack("<II", header)
    if filename_count > 1_000_000 or file_count > 1_000_000:
        raise AnalysisError("implausible MEG table counts")
    names: list[str] = []
    for index in range(filename_count):
        length = struct.unpack("<H", read_exact(stream, 2, f"MEG filename {index} length"))[0]
        raw_name = read_exact(stream, length, f"MEG filename {index}")
        names.append(raw_name.decode("latin-1"))
    entries: list[MegEntry] = []
    for index in range(file_count):
        _, _, size, start, name_index = struct.unpack(
            "<IIIII", read_exact(stream, 20, f"MEG entry {index}")
        )
        if name_index >= len(names):
            raise AnalysisError(f"MEG entry {index} has invalid filename index {name_index}")
        if start > file_size or size > file_size - start:
            raise AnalysisError(f"MEG entry {index} extends beyond the archive")
        entries.append(MegEntry(names[name_index], size, start))
    return entries


def parse_ted_extent_bytes(data: bytes) -> TedExtent | None:
    """Decode direct-child extent properties from a bounded TED root record.

    The header is a little-endian 32-bit root id and payload size followed by
    one-byte property ids and one-byte payload sizes.  Requiring a complete,
    exactly bounded root record prevents an extent-like byte sequence deeper in
    a map payload from being accepted as header metadata.
    """
    if len(data) < 8:
        return None
    root_id, payload_size = struct.unpack_from("<II", data)
    if root_id != 0 or payload_size > 4096 or payload_size > len(data) - 8:
        return None
    end = 8 + payload_size
    position = 8
    properties: list[tuple[int, int, int, bytes]] = []
    while position < end:
        if end - position < 2:
            return None
        property_offset = position
        property_id, property_size = struct.unpack_from("BB", data, position)
        position += 2
        if property_size > end - position:
            return None
        payload = data[position : position + property_size]
        properties.append((property_id, property_size, property_offset, payload))
        position += property_size

    pairs: list[tuple[tuple[int, int, int, bytes], tuple[int, int, int, bytes]]] = []
    for first, second in zip(properties, properties[1:]):
        if first[0:2] == (0x10, 4) and second[0:2] == (0x11, 4):
            pairs.append((first, second))
    if not pairs:
        return None
    if len(pairs) != 1:
        raise AnalysisError(f"ambiguous TED root extent properties: {len(pairs)} pairs")
    first, second = pairs[0]
    width = struct.unpack("<f", first[3])[0]
    height = struct.unpack("<f", second[3])[0]
    if not (
        math.isfinite(width)
        and math.isfinite(height)
        and 0 < width <= 1_000_000
        and 0 < height <= 1_000_000
    ):
        return None
    return TedExtent(
        Decimal.from_float(width),
        Decimal.from_float(height),
        payload_size,
        first[2],
        second[2],
    )


def ted_files(root: Path) -> list[Path]:
    return sorted(
        (path for path in root.rglob("*") if path.is_file() and path.suffix.lower() == ".ted"),
        key=lambda path: path.relative_to(root).as_posix().lower(),
    )


def analyse_ted_sources(
    archives: Sequence[LabelledPath], loose_roots: Sequence[LabelledPath]
) -> dict[str, object]:
    archive_rows: list[dict[str, object]] = []
    maximum: dict[str, object] | None = None
    maximum_value = Decimal(0)
    total_entries = 0
    parsed_entries = 0
    missing: list[dict[str, object]] = []
    root_size_counts: dict[int, int] = {}
    property_offset_counts: dict[tuple[int, int], int] = {}
    terminal_extent_layout_count = 0

    def consider_extent(
        extent: TedExtent,
        source: str,
        provenance: dict[str, object],
    ) -> None:
        nonlocal maximum, maximum_value, terminal_extent_layout_count
        root_size_counts[extent.root_payload_size] = (
            root_size_counts.get(extent.root_payload_size, 0) + 1
        )
        offsets = (extent.width_property_offset, extent.height_property_offset)
        property_offset_counts[offsets] = property_offset_counts.get(offsets, 0) + 1
        if offsets == (extent.root_payload_size - 7, extent.root_payload_size - 1):
            terminal_extent_layout_count += 1
        for axis, value in (("width", extent.width), ("height", extent.height)):
            if maximum is None or value > maximum_value:
                maximum_value = value
                maximum = {
                    "value": decimal_text(value),
                    "axis": axis,
                    "source": source,
                    "width": decimal_text(extent.width),
                    "height": decimal_text(extent.height),
                    "root_payload_size": extent.root_payload_size,
                    "width_property_offset": extent.width_property_offset,
                    "height_property_offset": extent.height_property_offset,
                    **provenance,
                }

    for labelled in archives:
        if not labelled.path.is_file():
            raise AnalysisError(f"TED archive is not a file: {labelled.path}")
        archive_hash = sha256_path(labelled.path)
        file_size = labelled.path.stat().st_size
        archive_entries = 0
        archive_parsed = 0
        with labelled.path.open("rb") as stream:
            entries = read_meg_index(stream, file_size)
            for entry in entries:
                if not entry.name.lower().endswith(".ted"):
                    continue
                total_entries += 1
                archive_entries += 1
                stream.seek(entry.start)
                header = read_exact(stream, min(entry.size, 4096), f"TED entry {entry.name}")
                extent = parse_ted_extent_bytes(header)
                source = f"{labelled.label}!/{entry.name.replace(chr(92), '/')}"
                if extent is None:
                    missing.append(
                        {
                            "source": source,
                            "archive_sha256": archive_hash,
                            "entry_offset": entry.start,
                            "entry_size": entry.size,
                            "reason": "no bounded root TED width/height property pair",
                        }
                    )
                    continue
                parsed_entries += 1
                archive_parsed += 1
                consider_extent(
                    extent,
                    source,
                    {
                        "archive_sha256": archive_hash,
                        "entry_offset": entry.start,
                        "entry_size": entry.size,
                    },
                )
        archive_rows.append(
            {
                "label": labelled.label,
                "relative_path": labelled.label,
                "sha256": archive_hash,
                "size_bytes": file_size,
                "ted_entry_count": archive_entries,
                "extent_record_count": archive_parsed,
            }
        )

    loose_rows: list[dict[str, object]] = []
    for labelled in loose_roots:
        if not labelled.path.is_dir():
            raise AnalysisError(f"loose TED root is not a directory: {labelled.path}")
        files = ted_files(labelled.path)
        manifest = hashlib.sha256()
        root_parsed = 0
        file_rows: list[dict[str, object]] = []
        for path in files:
            relative = path.relative_to(labelled.path).as_posix()
            source = f"{labelled.label}/{relative}"
            file_hash = sha256_path(path)
            manifest.update(relative.encode("utf-8"))
            manifest.update(b"\0")
            manifest.update(file_hash.encode("ascii"))
            manifest.update(b"\n")
            total_entries += 1
            with path.open("rb") as stream:
                header = stream.read(4096)
            extent = parse_ted_extent_bytes(header)
            if extent is None:
                missing.append(
                    {
                        "source": source,
                        "source_sha256": file_hash,
                        "entry_size": path.stat().st_size,
                        "reason": "no bounded root TED width/height property pair",
                    }
                )
                continue
            parsed_entries += 1
            root_parsed += 1
            consider_extent(
                extent,
                source,
                {"source_sha256": file_hash, "entry_size": path.stat().st_size},
            )
            file_rows.append(
                {
                    "source": source,
                    "source_sha256": file_hash,
                    "size_bytes": path.stat().st_size,
                    "width": decimal_text(extent.width),
                    "height": decimal_text(extent.height),
                    "root_payload_size": extent.root_payload_size,
                    "width_property_offset": extent.width_property_offset,
                    "height_property_offset": extent.height_property_offset,
                }
            )
        loose_rows.append(
            {
                "label": labelled.label,
                "file_count": len(files),
                "extent_record_count": root_parsed,
                "manifest_sha256": manifest.hexdigest(),
                "manifest_definition": "SHA-256 over sorted relative-path NUL file-sha256 LF records",
                "files": file_rows,
            }
        )

    return {
        "units": "TED declared tactical-map source units; origin and real-world conversion are not established",
        "record_signature": "bounded root TLV properties 10 04 <little-endian binary32 width>, then 11 04 <little-endian binary32 height>",
        "record_scope": "one adjacent property pair among direct children of root id 0; root payload must end exactly within the first 4096 bytes",
        "structural_observations": {
            "extent_pair_at_root_payload_minus_7_and_minus_1_count": terminal_extent_layout_count,
            "root_payload_size_counts": [
                {"bytes": size, "count": count}
                for size, count in sorted(root_size_counts.items())
            ],
            "property_offset_pair_counts": [
                {
                    "width_property_offset": offsets[0],
                    "height_property_offset": offsets[1],
                    "count": count,
                }
                for offsets, count in sorted(property_offset_counts.items())
            ],
        },
        "archives": archive_rows,
        "loose_corpora": loose_rows,
        "ted_entry_count": total_entries,
        "extent_record_count": parsed_entries,
        "maximum_declared_axis_extent": maximum,
        "missing_extent_records": missing,
    }


def analyse_ted_archives(archives: Sequence[LabelledPath]) -> dict[str, object]:
    return analyse_ted_sources(archives, ())
