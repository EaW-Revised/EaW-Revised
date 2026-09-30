"""P1-09 tactical camera constant inventory.

Bounded, read-only, offline. It records only tag names, their literal values and
the logical VFS source path each value won from. It never records a native path,
a game root, archive bytes or any other installation-identifying material.

Two effective sources supply every consumed constant:

- ``data/xml/tacticalcameras.xml`` — the per-mode camera definitions
  (``Land_Mode``, ``Space_Mode``, ``Unlocked``): zoom distance limits, the pitch
  and distance splines, pitch/yaw/FOV limits and their per-mouse-unit and
  per-zoom-unit rates, smoothing times, clip planes and bounds buffers.
- ``data/xml/gameconstants.xml`` — the global tactical scroll constants:
  min/max scroll speed, the edge and offscreen scroll regions, the push-scroll
  modifier, and the scroll acceleration/deceleration factors.

Values are retained as their exact source lexeme plus, where the lexeme is
numeric, a parsed number. Petroglyph writes trailing ``f`` suffixes and mixes
``,`` and space separators inside the spline lists; both are preserved in
``value`` and normalised only in the parsed form.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
import xml.parsers.expat
from pathlib import Path
from typing import Any

try:
    from .common import TOOL_VERSION, manifest_id, sha256, source_hashes, write_json
    from .corpus import Corpus, CorpusError
except ImportError:  # direct script/test import
    from common import TOOL_VERSION, manifest_id, sha256, source_hashes, write_json
    from corpus import Corpus, CorpusError

PROFILES = ("eaw", "foc", "remake")
CAMERA_SOURCE = "data/xml/tacticalcameras.xml"
CONSTANTS_SOURCE = "data/xml/gameconstants.xml"
MAX_XML_BYTES = 64 * 1024 * 1024

# Camera definitions are consumed by name. An unexpected definition name is
# recorded rather than silently dropped.
KNOWN_MODES = ("Land_Mode", "Space_Mode", "Unlocked")

# Exactly the global tactical scroll tags this model consumes. Strategic and
# galactic scroll tags are deliberately out of scope for a tactical camera.
SCROLL_TAGS = (
    "Tactical_Min_Scroll_Speed",
    "Tactical_Max_Scroll_Speed",
    "Tactical_Edge_Scroll_Region",
    "Tactical_Offscreen_Scroll_Region",
    "Push_Scroll_Speed_Modifier",
    "Scroll_Acceleration_Factor",
    "Scroll_Deceleration_Factor",
    "Land_Tactical_Camera_Locked",
    "Space_Tactical_Camera_Locked",
)

_NUMBER = re.compile(r"^[+-]?(\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[fF]?$")
_SPLINE_SPLIT = re.compile(r"[,\s]+")


class CameraInventoryError(RuntimeError):
    pass


def _reject_unsafe_xml(data: bytes) -> None:
    """Reject DOCTYPE and external entities before any tree is built."""
    if len(data) > MAX_XML_BYTES:
        raise CameraInventoryError("XML exceeds 64 MiB policy")
    parser = xml.parsers.expat.ParserCreate()

    def reject_doctype(*_args: Any) -> None:
        raise CameraInventoryError("XML contains forbidden DOCTYPE")

    def reject_entity(*_args: Any) -> int:
        raise CameraInventoryError("XML contains forbidden external entity")

    parser.StartDoctypeDeclHandler = reject_doctype
    parser.ExternalEntityRefHandler = reject_entity
    parser.SetParamEntityParsing(xml.parsers.expat.XML_PARAM_ENTITY_PARSING_NEVER)
    parser.Parse(data, True)


def parse_number(text: str) -> float | None:
    """Parse one Petroglyph scalar lexeme, tolerating a trailing f suffix."""
    stripped = text.strip()
    if not _NUMBER.match(stripped):
        return None
    return float(stripped.rstrip("fF"))


def parse_spline(text: str) -> list[list[float]] | None:
    """Parse a flat control-point list into (fraction, value) pairs.

    Petroglyph mixes ',' and ' ' separators inside one list, so the separator
    carries no structure: the numbers are read as a flat sequence and taken two
    at a time. An odd count or a non-numeric token is a rejected spline.
    """
    tokens = [token for token in _SPLINE_SPLIT.split(text.strip()) if token]
    if not tokens or len(tokens) % 2 != 0:
        return None
    values: list[float] = []
    for token in tokens:
        number = parse_number(token)
        if number is None:
            return None
        values.append(number)
    points = [[values[index], values[index + 1]] for index in range(0, len(values), 2)]
    # Control-point fractions must be non-decreasing for a piecewise-linear
    # evaluation to be single valued.
    for earlier, later in zip(points, points[1:]):
        if later[0] < earlier[0]:
            return None
    return points


def _field_record(tag: str, text: str, logical_path: str, layer_id: str) -> dict[str, Any]:
    record: dict[str, Any] = {
        "layer_id": layer_id,
        "logical_path": logical_path,
        "tag": tag,
        "value": text,
    }
    number = parse_number(text)
    if number is not None:
        record["number"] = number
    spline = parse_spline(text)
    if spline is not None and len(spline) > 1:
        record["points"] = spline
    return record


def _winning_source(corpus: Corpus, profile: str, logical_path: str) -> Any:
    for source in corpus.iter_sources(profile, ".xml", "effective"):
        if source.winner and source.logical_path == logical_path:
            return source
    raise CameraInventoryError(f"{profile}: no effective winner for {logical_path}")


def collect_profile(corpus: Corpus, profile: str) -> dict[str, Any]:
    camera_source = _winning_source(corpus, profile, CAMERA_SOURCE)
    _reject_unsafe_xml(camera_source.data)
    root = ET.fromstring(camera_source.data.decode("utf-8", "replace"))
    if root.tag != "TacticalCameras":
        raise CameraInventoryError(
            f"{profile}: {CAMERA_SOURCE} root element is {root.tag!r}, expected TacticalCameras")

    modes: list[dict[str, Any]] = []
    unexpected_modes: list[str] = []
    for definition in root:
        if definition.tag != "TacticalCamera":
            continue
        name = definition.get("Name") or definition.get("name") or ""
        if name not in KNOWN_MODES:
            unexpected_modes.append(name)
            continue
        fields = [
            _field_record(field.tag, (field.text or "").strip(),
                          camera_source.logical_path, camera_source.layer_id)
            for field in definition
            if (field.text or "").strip()
        ]
        modes.append({
            "fields": sorted(fields, key=lambda row: row["tag"]),
            "name": name,
        })

    constants_source = _winning_source(corpus, profile, CONSTANTS_SOURCE)
    _reject_unsafe_xml(constants_source.data)
    constants_root = ET.fromstring(constants_source.data.decode("utf-8", "replace"))
    scroll: list[dict[str, Any]] = []
    missing_scroll_tags: list[str] = []
    seen = {element.tag: (element.text or "").strip()
            for element in constants_root.iter()
            if element.tag in SCROLL_TAGS and (element.text or "").strip()}
    for tag in SCROLL_TAGS:
        if tag not in seen:
            missing_scroll_tags.append(tag)
            continue
        scroll.append(_field_record(tag, seen[tag], constants_source.logical_path,
                                    constants_source.layer_id))

    return {
        "camera_source": {
            "layer_id": camera_source.layer_id,
            "logical_path": camera_source.logical_path,
            "sha256": camera_source.sha256,
        },
        "constants_source": {
            "layer_id": constants_source.layer_id,
            "logical_path": constants_source.logical_path,
            "sha256": constants_source.sha256,
        },
        "counts": {
            "camera_fields": sum(len(mode["fields"]) for mode in modes),
            "modes": len(modes),
            "scroll_constants": len(scroll),
        },
        "missing_scroll_tags": sorted(missing_scroll_tags),
        "modes": sorted(modes, key=lambda row: row["name"]),
        "profile": profile,
        "scroll_constants": sorted(scroll, key=lambda row: row["tag"]),
        "unexpected_camera_definitions": sorted(unexpected_modes),
    }


def generate(game_root: Path, mod_roots: list[Path], output: Path) -> dict[str, Any]:
    corpus = Corpus(game_root, mod_roots)
    profiles = [collect_profile(corpus, profile) for profile in PROFILES]
    payload: dict[str, Any] = {
        "derivation": {
            "pan_speed": "lerp(Tactical_Min_Scroll_Speed, Tactical_Max_Scroll_Speed) on the normalised distance; derived, pending timed original measurement",
            "pitch_no_spline": "below Pitch_Zoom_Begin_Fraction blend Pitch_When_Zoomed_In to Pitch_Default, otherwise Pitch_Default + Pitch_Per_Zoom_Unit * (t - max(begin, 0)), clamped to [Pitch_Min, Pitch_Max]",
            "pitch_spline": "Use_Splines=yes evaluates Pitch_Spline piecewise-linearly and supersedes Pitch_Min/Pitch_Max; derived, pending original capture",
            "spline_format": "flat numeric sequence read two at a time as (zoom fraction, value); ',' and ' ' separators are interchangeable",
            "zoom_parameter": "t in [0,1]; 0 is fully zoomed in (Distance_Min) and 1 is fully zoomed out (Distance_Max)",
        },
        "manifest_id": "",
        "profiles": profiles,
        "schema_version": 1,
        "sources": {
            "camera": CAMERA_SOURCE,
            "constants": CONSTANTS_SOURCE,
        },
        "summary": {
            "camera_fields": sum(profile["counts"]["camera_fields"] for profile in profiles),
            "missing_scroll_tags": sum(
                len(profile["missing_scroll_tags"]) for profile in profiles),
            "modes": sum(profile["counts"]["modes"] for profile in profiles),
            "profiles": len(profiles),
            "scroll_constants": sum(
                profile["counts"]["scroll_constants"] for profile in profiles),
            "unexpected_camera_definitions": sum(
                len(profile["unexpected_camera_definitions"]) for profile in profiles),
        },
        "tool": "camera_inventory",
        "tool_sources": source_hashes(Path(__file__).resolve().parent,
                                      ["common.py", "corpus.py", "camera_inventory.py"]),
        "tool_version": TOOL_VERSION,
    }
    payload["manifest_id"] = manifest_id(payload)
    write_json(output, payload)
    return payload["summary"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--mod-root", required=True, action="append", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        summary = generate(args.game_root, args.mod_root, args.out)
    except (CorpusError, CameraInventoryError, ET.ParseError, OSError, ValueError) as exc:
        print(f"camera_inventory: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


__all__ = [
    "CameraInventoryError",
    "collect_profile",
    "generate",
    "main",
    "parse_number",
    "parse_spline",
    "sha256",
]
