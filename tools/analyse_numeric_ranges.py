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


SCHEMA_VERSION = 2
NUMBER = r"[+-]?(?:(?:\d+(?:\.\d*)?)|(?:\.\d+))(?:[eE][+-]?\d+)?"
NUMERIC_TEXT_RE = re.compile(rf"^\s*({NUMBER})(?:\s*[,;\s]\s*({NUMBER}))*\s*$")
NUMBER_RE = re.compile(NUMBER)
SIMPLE_ELEMENT_RE = re.compile(
    r"<(?P<tag>[A-Za-z_][\w:.-]*)(?:\s[^<>]*?)?>(?P<text>[^<>]*)</(?P=tag)\s*>",
    re.IGNORECASE,
)

CATEGORY_PATTERNS: dict[str, tuple[re.Pattern[str], ...]] = {
    "coordinate": tuple(
        re.compile(pattern)
        for pattern in (
            r"(?:^|_)(?:POSITION|COORDINATE|COORDINATES)(?:_|$)",
            r"(?:^|_)LOCATION$",
            r"(?:^|_)(?:X|Y|Z)_COORDINATE(?:_|$)",
            r"(?:^|_)BOUNDING_(?:BOX|SPHERE)(?:_|$)",
        )
    ),
    "speed": tuple(
        re.compile(pattern)
        for pattern in (
            r"(?:^|_)(?:SPEED|VELOCITY)(?:_|$)",
            r"(?:^|_)(?:ACCELERATION|DECELERATION)(?:_|$)",
            r"(?:^|_)TURN_RATE(?:_|$)",
        )
    ),
    "duration": tuple(
        re.compile(pattern)
        for pattern in (
            r"(?:^|_)(?:DURATION|DELAY|INTERVAL|LIFETIME|COOLDOWN)(?:_|$)",
            r"(?:^|_)(?:TIME|PERIOD)(?:_|$)",
            r"(?:^|_)(?:RECHARGE|RELOAD|BUILD)_TIME(?:_|$)",
        )
    ),
    "damage": (re.compile(r"(?:^|_)DAMAGE(?:_|$)"),),
    "multiplier": tuple(
        re.compile(pattern)
        for pattern in (
            r"(?:^|_)MULTIPLIER(?:_|$)",
            r"(?:^|_)FACTOR(?:_|$)",
        )
    ),
}

CATEGORY_EXCLUSIONS: dict[str, tuple[str, ...]] = {
    "coordinate": ("LOCATION_FOLLOWS",),
    "speed": ("PERCENT", "MULTIPLIER", "FACTOR", "_MOD", "SCROLL_SPEED"),
    "duration": ("MULTIPLIER", "PERCENT"),
    "damage": (
        "PERCENT",
        "MULTIPLIER",
        "FACTOR",
        "MODIFIER",
        "_FRAME",
        "_TYPE",
        "_RANGE",
        "_RATE",
        "PER_SECOND",
        "_BONUS",
        "_TIME",
        "_DELAY",
        "_INTERVAL",
        "_RADIUS",
        "_ARC",
        "_QUADRATIC",
        "THRESHOLD",
        "_MOD",
        "ALTERNATE",
        "MAX_SECS",
    ),
    "multiplier": (),
}

CATEGORY_UNITS = {
    "coordinate": "XML source coordinate units; tag semantics and axis conventions vary",
    "speed": "XML source units per engine-defined time unit; conversion is not established",
    "duration": "XML source time units; individual tags may use seconds, frames, or ticks",
    "damage": "XML source damage/hit-point units",
    "multiplier": "dimensionless when the matched tag is semantically a multiplier",
}


class AnalysisError(RuntimeError):
    """Input is structurally invalid and cannot be analysed safely."""


@dataclass(frozen=True)
class LabelledPath:
    label: str
    path: Path


@dataclass(frozen=True)
class Evidence:
    value: Decimal
    source: str
    source_sha256: str
    tag: str
    component: int
    arity: int

    def as_json(self) -> dict[str, object]:
        return {
            "value": decimal_text(self.value),
            "source": self.source,
            "source_sha256": self.source_sha256,
            "tag": self.tag,
            "component": self.component,
            "arity": self.arity,
        }


@dataclass
class RangeAccumulator:
    count: int = 0
    min_evidence: Evidence | None = None
    max_evidence: Evidence | None = None
    smallest_nonzero_evidence: Evidence | None = None
    max_absolute_evidence: Evidence | None = None
    tags: set[str] = field(default_factory=set)

    def add(self, evidence: Evidence) -> None:
        self.count += 1
        self.tags.add(evidence.tag.split("[", 1)[0])
        if self.min_evidence is None or evidence.value < self.min_evidence.value:
            self.min_evidence = evidence
        if self.max_evidence is None or evidence.value > self.max_evidence.value:
            self.max_evidence = evidence
        if evidence.value != 0 and (
            self.smallest_nonzero_evidence is None
            or abs(evidence.value) < abs(self.smallest_nonzero_evidence.value)
        ):
            self.smallest_nonzero_evidence = evidence
        if self.max_absolute_evidence is None or abs(evidence.value) > abs(
            self.max_absolute_evidence.value
        ):
            self.max_absolute_evidence = evidence

    def as_json(self, units: str) -> dict[str, object]:
        return {
            "units": units,
            "component_count": self.count,
            "matched_tags": sorted(self.tags),
            "minimum": optional_evidence(self.min_evidence),
            "maximum": optional_evidence(self.max_evidence),
            "smallest_nonzero_magnitude": magnitude_evidence(
                self.smallest_nonzero_evidence
            ),
            "maximum_magnitude": magnitude_evidence(self.max_absolute_evidence),
        }


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


def decimal_text(value: Decimal) -> str:
    if value == 0:
        return "0"
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text


def optional_evidence(value: Evidence | None) -> dict[str, object] | None:
    return None if value is None else value.as_json()


def magnitude_evidence(value: Evidence | None) -> dict[str, object] | None:
    if value is None:
        return None
    result = value.as_json()
    result["value"] = decimal_text(abs(value.value))
    if value.value < 0:
        result["source_value"] = decimal_text(value.value)
    return result


def parse_labelled_path(argument: str) -> LabelledPath:
    if "=" not in argument:
        raise argparse.ArgumentTypeError("expected LABEL=PATH")
    label, raw_path = argument.split("=", 1)
    label_parts = label.split("/")
    if (
        not label
        or "\\" in label
        or label.startswith("/")
        or any(part in ("", ".", "..") for part in label_parts)
    ):
        raise argparse.ArgumentTypeError("LABEL must be a safe non-empty relative path")
    if not raw_path:
        raise argparse.ArgumentTypeError("PATH must not be empty")
    return LabelledPath(label, Path(raw_path))


def sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_numeric_text(text: str | None) -> tuple[Decimal, ...] | None:
    if text is None or not NUMERIC_TEXT_RE.fullmatch(text):
        return None
    result: list[Decimal] = []
    try:
        for match in NUMBER_RE.finditer(text):
            result.append(Decimal(match.group(0)))
    except InvalidOperation:
        return None
    return tuple(result) or None


def local_tag(tag: str) -> str:
    return tag.rsplit("}", 1)[-1].upper()


def categories_for_tag(tag: str) -> tuple[str, ...]:
    return tuple(
        category
        for category, patterns in CATEGORY_PATTERNS.items()
        if any(pattern.search(tag) for pattern in patterns)
        and not any(excluded in tag for excluded in CATEGORY_EXCLUSIONS[category])
    )


def xml_files(root: Path) -> list[Path]:
    return sorted(
        (path for path in root.rglob("*") if path.is_file() and path.suffix.lower() == ".xml"),
        key=lambda path: path.relative_to(root).as_posix().lower(),
    )


def analyse_xml_roots(roots: Sequence[LabelledPath]) -> dict[str, object]:
    accumulators = {name: RangeAccumulator() for name in CATEGORY_PATTERNS}
    corpus_rows: list[dict[str, object]] = []
    fallback_files: list[dict[str, object]] = []
    unreadable_files: list[dict[str, str]] = []
    negative_duration_counts: dict[tuple[str, Decimal], int] = {}
    negative_duration_sources: dict[tuple[str, Decimal], set[str]] = {}
    defense_duration_counts: dict[Decimal, int] = {}
    defense_duration_sources: dict[Decimal, set[str]] = {}

    for labelled in roots:
        if not labelled.path.is_dir():
            raise AnalysisError(f"XML root is not a directory: {labelled.path}")
        manifest = hashlib.sha256()
        files = xml_files(labelled.path)
        processed_count = 0
        xml_document_count = 0
        fallback_count = 0
        for path in files:
            relative = path.relative_to(labelled.path).as_posix()
            source = f"{labelled.label}/{relative}"
            file_hash = sha256_path(path)
            manifest.update(relative.encode("utf-8"))
            manifest.update(b"\0")
            manifest.update(file_hash.encode("ascii"))
            manifest.update(b"\n")
            elements: Iterable[tuple[str, str | None]]
            try:
                root = ET.parse(path).getroot()
                elements = ((element.tag, element.text) for element in root.iter())
                xml_document_count += 1
            except ET.ParseError as error:
                try:
                    raw_text = path.read_text(encoding="utf-8-sig", errors="strict")
                except (OSError, UnicodeError) as read_error:
                    unreadable_files.append({"source": source, "error": str(read_error)})
                    continue
                matches = list(SIMPLE_ELEMENT_RE.finditer(raw_text))
                elements = ((match.group("tag"), match.group("text")) for match in matches)
                fallback_files.append(
                    {
                        "source": source,
                        "xml_error": str(error),
                        "complete_simple_elements_scanned": len(matches),
                    }
                )
                fallback_count += 1
            except OSError as error:
                unreadable_files.append({"source": source, "error": str(error)})
                continue
            processed_count += 1
            occurrence: dict[str, int] = {}
            for raw_tag, text in elements:
                tag = local_tag(raw_tag)
                categories = categories_for_tag(tag)
                if not categories:
                    continue
                values = parse_numeric_text(text)
                if values is None:
                    continue
                occurrence[tag] = occurrence.get(tag, 0) + 1
                evidence_tag = f"{tag}[{occurrence[tag]}]"
                for component, value in enumerate(values):
                    evidence = Evidence(
                        value=value,
                        source=source,
                        source_sha256=file_hash,
                        tag=evidence_tag,
                        component=component,
                        arity=len(values),
                    )
                    for category in categories:
                        accumulators[category].add(evidence)
                    if "duration" in categories and value < 0:
                        key = (tag, value)
                        negative_duration_counts[key] = negative_duration_counts.get(key, 0) + 1
                        negative_duration_sources.setdefault(key, set()).add(source)
                    if tag == "DEFENSE_DURATION_IN_SECS":
                        defense_duration_counts[value] = defense_duration_counts.get(value, 0) + 1
                        defense_duration_sources.setdefault(value, set()).add(source)
        corpus_rows.append(
            {
                "label": labelled.label,
                "file_count": len(files),
                "processed_file_count": processed_count,
                "xml_document_count": xml_document_count,
                "fallback_file_count": fallback_count,
                "manifest_sha256": manifest.hexdigest(),
                "manifest_definition": "SHA-256 over sorted relative-path NUL file-sha256 LF records",
            }
        )

    return {
        "corpora": corpus_rows,
        "fallback_files": fallback_files,
        "unreadable_files": unreadable_files,
        "notable_values": {
            "negative_duration_components_by_tag": [
                {
                    "tag": tag,
                    "value": decimal_text(value),
                    "component_count": count,
                    "source_file_count": len(negative_duration_sources[(tag, value)]),
                }
                for (tag, value), count in sorted(
                    negative_duration_counts.items(), key=lambda item: (item[0][0], item[0][1])
                )
            ],
            "defense_duration_in_secs_distribution": [
                {
                    "value": decimal_text(value),
                    "component_count": count,
                    "source_file_count": len(defense_duration_sources[value]),
                }
                for value, count in sorted(defense_duration_counts.items())
            ],
        },
        "categories": {
            name: accumulator.as_json(CATEGORY_UNITS[name])
            for name, accumulator in accumulators.items()
        },
    }


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


def decimal_from_evidence(category: dict[str, object], key: str) -> Decimal | None:
    evidence = category.get(key)
    if not isinstance(evidence, dict):
        return None
    value = evidence.get("value")
    return abs(Decimal(value)) if isinstance(value, str) else None


def derive_headroom(xml: dict[str, object], ted: dict[str, object]) -> dict[str, object]:
    categories = xml["categories"]
    assert isinstance(categories, dict)
    ted_max_record = ted.get("maximum_declared_axis_extent")
    ted_max = (
        Decimal(ted_max_record["value"])
        if isinstance(ted_max_record, dict) and isinstance(ted_max_record.get("value"), str)
        else None
    )
    damage = decimal_from_evidence(categories["damage"], "maximum_magnitude")
    multiplier = decimal_from_evidence(categories["multiplier"], "maximum_magnitude")
    speed = decimal_from_evidence(categories["speed"], "maximum_magnitude")
    duration = decimal_from_evidence(categories["duration"], "maximum_magnitude")

    products: list[dict[str, str]] = []
    if ted_max is not None:
        products.extend(
            [
                {
                    "expression": "max_ted_axis_extent^2",
                    "value": decimal_text(ted_max * ted_max),
                    "meaning": "single coordinate-square screening bound",
                },
                {
                    "expression": "2 * max_ted_axis_extent^2",
                    "value": decimal_text(Decimal(2) * ted_max * ted_max),
                    "meaning": "conservative 2D dot/squared-distance intermediate screening bound",
                },
                {
                    "expression": "3 * max_ted_axis_extent^2",
                    "value": decimal_text(Decimal(3) * ted_max * ted_max),
                    "meaning": "conservative 3D dot-product intermediate screening bound",
                },
            ]
        )
    if damage is not None and multiplier is not None:
        products.append(
            {
                "expression": "max_abs_damage * max_abs_multiplier",
                "value": decimal_text(damage * multiplier),
                "meaning": "cross-tag screening bound; compatibility of the two extrema is not established",
            }
        )
    if speed is not None and duration is not None:
        products.append(
            {
                "expression": "max_abs_speed * max_abs_duration",
                "value": decimal_text(speed * duration),
                "meaning": "cross-tag screening bound; source time units and compatibility are not established",
            }
        )
    return {
        "screening_products": products,
        "unresolved_accumulation_bounds": [
            "maximum simultaneous contributors to accumulated damage/economy totals",
            "maximum simulation lifetime or tick count",
            "fixed simulation tick rate and per-tag time-unit conversions",
            "maximum path length, waypoint count, and repeated transform composition count",
            "whether coordinates are centred, corner-origin, or may legally exceed declared TED extents",
        ],
        "interpretation": "Products are magnitude screens, not asserted runtime operation pairs. Accumulation headroom remains open until the listed counts are bounded.",
    }


def build_inventory(
    xml_roots: Sequence[LabelledPath],
    ted_archives: Sequence[LabelledPath],
    ted_roots: Sequence[LabelledPath] = (),
) -> dict[str, object]:
    xml = analyse_xml_roots(xml_roots)
    ted = analyse_ted_sources(ted_archives, ted_roots)
    return {
        "schema_version": SCHEMA_VERSION,
        "method": {
            "xml_numbers": "exact decimal parsing of wholly numeric leaf text; no binary float or locale conversion",
            "xml_categories": {
                category: [pattern.pattern for pattern in patterns]
                for category, patterns in CATEGORY_PATTERNS.items()
            },
            "xml_category_exclusions": {
                category: list(exclusions)
                for category, exclusions in CATEGORY_EXCLUSIONS.items()
            },
            "ted": "MEG v1 index parsing and loose-file enumeration plus bounded root-TLV width/height decoding",
        },
        "xml": xml,
        "ted": ted,
        "headroom": derive_headroom(xml, ted),
    }


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--xml-root",
        action="append",
        type=parse_labelled_path,
        default=[],
        metavar="LABEL=PATH",
        help="XML corpus root; may be repeated",
    )
    parser.add_argument(
        "--ted-archive",
        action="append",
        type=parse_labelled_path,
        default=[],
        metavar="LABEL=PATH",
        help="MEG archive containing TED entries; may be repeated",
    )
    parser.add_argument(
        "--ted-root",
        action="append",
        type=parse_labelled_path,
        default=[],
        metavar="LABEL=PATH",
        help="root containing loose TED files; may be repeated",
    )
    parser.add_argument("--output", type=Path, help="write JSON to this path instead of stdout")
    parser.add_argument("--compact", action="store_true", help="omit pretty indentation")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = make_parser().parse_args(argv)
    if not arguments.xml_root and not arguments.ted_archive and not arguments.ted_root:
        print("at least one --xml-root, --ted-archive, or --ted-root is required", file=sys.stderr)
        return 2
    try:
        inventory = build_inventory(
            arguments.xml_root, arguments.ted_archive, arguments.ted_root
        )
    except (AnalysisError, OSError) as error:
        print(f"range analysis failed: {error}", file=sys.stderr)
        return 1
    serialized = json.dumps(
        inventory,
        indent=None if arguments.compact else 2,
        sort_keys=True,
        ensure_ascii=False,
    ) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        with arguments.output.open("w", encoding="utf-8", newline="\n") as output:
            output.write(serialized)
    else:
        sys.stdout.write(serialized)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
