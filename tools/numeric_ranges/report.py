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
