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
    CATEGORY_EXCLUSIONS,
    CATEGORY_PATTERNS,
    CATEGORY_UNITS,
    Evidence,
    LabelledPath,
    NUMBER_RE,
    NUMERIC_TEXT_RE,
    RangeAccumulator,
    SIMPLE_ELEMENT_RE,
    decimal_text,
    sha256_path,
)

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
