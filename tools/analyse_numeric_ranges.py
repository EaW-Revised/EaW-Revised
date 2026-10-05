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

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


from tools.numeric_ranges.report import (
    SCHEMA_VERSION, NUMBER, NUMERIC_TEXT_RE, NUMBER_RE, SIMPLE_ELEMENT_RE, CATEGORY_PATTERNS, CATEGORY_EXCLUSIONS, CATEGORY_UNITS, AnalysisError, LabelledPath, Evidence, RangeAccumulator, decimal_text, optional_evidence, magnitude_evidence, parse_labelled_path, sha256_path, decimal_from_evidence, derive_headroom
)
from tools.numeric_ranges.xml import (
    parse_numeric_text, local_tag, categories_for_tag, xml_files, analyse_xml_roots
)
from tools.numeric_ranges.ted import (
    MegEntry, TedExtent, read_exact, read_meg_index, parse_ted_extent_bytes, ted_files, analyse_ted_sources, analyse_ted_archives
)

AnalysisError.__module__ = __name__
LabelledPath.__module__ = __name__
Evidence.__module__ = __name__
RangeAccumulator.__module__ = __name__
MegEntry.__module__ = __name__
TedExtent.__module__ = __name__
decimal_text.__module__ = __name__
optional_evidence.__module__ = __name__
magnitude_evidence.__module__ = __name__
parse_labelled_path.__module__ = __name__
sha256_path.__module__ = __name__
parse_numeric_text.__module__ = __name__
local_tag.__module__ = __name__
categories_for_tag.__module__ = __name__
xml_files.__module__ = __name__
analyse_xml_roots.__module__ = __name__
read_exact.__module__ = __name__
read_meg_index.__module__ = __name__
parse_ted_extent_bytes.__module__ = __name__
ted_files.__module__ = __name__
analyse_ted_sources.__module__ = __name__
analyse_ted_archives.__module__ = __name__
decimal_from_evidence.__module__ = __name__
derive_headroom.__module__ = __name__


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
