#!/usr/bin/env python3
"""Inventory MTD atlas entries and XML texture references deterministically."""
from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import sys
import xml.parsers.expat
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from common import (TOOL_VERSION, ascii_fold, canonical_path, manifest_id, sha256,
                        source_hashes, write_json)
    from corpus import Corpus, CorpusError, Source
else:
    from .common import (TOOL_VERSION, ascii_fold, canonical_path, manifest_id, sha256,
                         source_hashes, write_json)
    from .corpus import Corpus, CorpusError, Source

RECORD_SIZE = 81
NAME_SIZE = 64
MAX_ENTRIES = 16 * 1024 * 1024
LSP_REVISION = "4461416d401b0f665bc1fe82aad60b90bd707fa9"
PETROGLYPH_TOOLS_REVISION = "3be4a58549897baa4c64aba8ffefc5eea574c495"
REDACTED_TEXTURE_REFERENCE = "<redacted>"
_URI_SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


class MtdInventoryError(RuntimeError):
    pass


@dataclass(frozen=True)
class MtdEntry:
    name: str
    x: int
    y: int
    width: int
    height: int
    has_alpha: bool

    def record(self) -> dict[str, Any]:
        return {
            "has_alpha": self.has_alpha,
            "name": self.name,
            "rectangle": {
                "height": self.height, "width": self.width,
                "x": self.x, "y": self.y,
            },
        }


@dataclass(frozen=True)
class AssetLocation:
    logical_path: str
    source_id: str
    origin: str
    layer_id: str
    size: int
    native_path: Path | None = None
    archive: Any | None = None
    member: Any | None = None

    def metadata(self) -> dict[str, Any]:
        return {
            "archive_path": self.archive.relative_path if self.archive else None,
            "layer_id": self.layer_id,
            "logical_path": self.logical_path,
            "origin": self.origin,
            "size": self.size,
            "source_id": self.source_id,
            "winner": True,
        }

    def read(self, corpus: Corpus) -> bytes:
        if self.native_path is not None:
            return self.native_path.read_bytes()
        if self.archive is None or self.member is None:
            raise MtdInventoryError(f"no payload source for {self.logical_path}")
        return corpus._read_member(self.archive, self.member)


class EffectiveAssetIndex:
    """Path/provenance index matching Corpus/VFS precedence without reading payloads."""

    def __init__(self, corpus: Corpus, profile: str):
        self._winners: dict[str, AssetLocation] = {}
        for layer in corpus._layers(profile):
            archives, _missing, _missing_native = corpus._archives(layer)
            loose: dict[str, AssetLocation] = {}
            for path in sorted(
                    (item for item in layer.data_root.rglob("*") if item.is_file()),
                    key=lambda item: ascii_fold(item.relative_to(layer.data_root).as_posix())):
                original = "Data/" + path.relative_to(layer.data_root).as_posix()
                logical = canonical_path(original)
                if logical in loose:
                    raise MtdInventoryError(
                        f"{layer.layer_id}: case-insensitive loose collision at {logical}")
                loose[logical] = AssetLocation(
                    logical, f"{layer.layer_id}:loose:{original}", "loose",
                    layer.layer_id, path.stat().st_size, native_path=path)
            for logical, location in sorted(loose.items()):
                self._winners.setdefault(logical, location)

            active = sorted(
                (archive for archive in archives if archive.active),
                key=lambda archive: archive.active_index or 0, reverse=True)
            for archive in active:
                for member in sorted(archive.members, key=lambda row: row.logical_path):
                    self._winners.setdefault(
                        member.logical_path,
                        AssetLocation(member.logical_path, archive.source_id, "archive",
                                      layer.layer_id, member.size,
                                      archive=archive, member=member))

    def get(self, logical_path: str) -> AssetLocation | None:
        try:
            return self._winners.get(canonical_path(logical_path))
        except ValueError:
            return None


def texture_dimensions(data: bytes, logical_path: str) -> tuple[str, int, int]:
    if data.startswith(b"DDS "):
        if len(data) < 128 or struct.unpack_from("<I", data, 4)[0] != 124:
            raise MtdInventoryError(f"{logical_path}: malformed DDS header")
        height, width = struct.unpack_from("<II", data, 12)
        kind = "dds"
    elif len(data) >= 18 and data[2] in (2, 3, 10, 11) and data[16] in (8, 24, 32):
        width, height = struct.unpack_from("<HH", data, 12)
        kind = "tga"
    else:
        raise MtdInventoryError(f"{logical_path}: bytes are neither supported DDS nor TGA")
    if width == 0 or height == 0:
        raise MtdInventoryError(f"{logical_path}: texture dimensions must be non-zero")
    return kind, width, height


def validate_rectangles(entries: Sequence[MtdEntry], width: int,
                        height: int) -> list[dict[str, Any]]:
    invalid: list[dict[str, Any]] = []
    for index, entry in enumerate(entries):
        if entry.x > width or entry.width > width - entry.x or \
                entry.y > height or entry.height > height - entry.y:
            invalid.append({
                "entry_index": index,
                "name": entry.name,
                "rectangle": entry.record()["rectangle"],
            })
    return invalid


def classify_texture_reference(value: str) -> tuple[str | None, str | None]:
    """Return stripped logical text or a non-sensitive rejection diagnostic."""
    if any((ord(character) < 0x20 and character not in "\t\n\r") or
           0x7f <= ord(character) < 0xa0 for character in value):
        return None, "control-bearing texture reference was redacted"
    stripped = value.strip()
    if not stripped:
        return None, None
    normalized = stripped.replace("\\", "/")
    if re.match(r"^[A-Za-z]:", normalized):
        return None, "drive-qualified texture reference was redacted"
    if normalized.startswith("//"):
        return None, "UNC texture reference was redacted"
    if normalized.startswith("/"):
        return None, "rooted texture reference was redacted"
    if _URI_SCHEME.match(stripped):
        return None, "URI texture reference was redacted"
    if any(part == ".." for part in normalized.split("/")):
        return None, "traversing texture reference was redacted"
    if ":" in normalized:
        return None, "invalid logical texture reference was redacted"
    try:
        canonical_path(normalized)
    except ValueError:
        return None, "invalid logical texture reference was redacted"
    return stripped, None


def texture_reference_candidates(value: str) -> list[str]:
    safe_value, _diagnostic = classify_texture_reference(value)
    if safe_value is None:
        return []
    normalized = safe_value.replace("\\", "/")
    if not normalized:
        return []
    folded = ascii_fold(normalized)
    if folded.startswith("data/"):
        base = normalized
    elif folded.startswith("art/textures/"):
        base = "data/" + normalized
    else:
        base = "data/art/textures/" + normalized
    try:
        base = canonical_path(base)
    except ValueError:
        return []
    suffix = Path(base).suffix.lower()
    if suffix:
        return [base] if suffix in (".dds", ".tga") else []
    return [base + ".dds", base + ".tga"]


def parse_mtd(data: bytes) -> list[MtdEntry]:
    if len(data) < 4:
        raise MtdInventoryError("truncated four-byte MTD header")
    count = struct.unpack_from("<I", data)[0]
    if count > MAX_ENTRIES:
        raise MtdInventoryError("MTD entry count exceeds safety limit")
    expected = 4 + count * RECORD_SIZE
    if len(data) < expected:
        raise MtdInventoryError(
            f"truncated MTD table: count requires {expected} bytes, found {len(data)}")
    if len(data) != expected:
        raise MtdInventoryError(
            f"MTD has {len(data) - expected} unexplained trailing bytes")
    result: list[MtdEntry] = []
    for index in range(count):
        offset = 4 + index * RECORD_SIZE
        raw_name = data[offset:offset + NAME_SIZE]
        terminator = raw_name.find(b"\0")
        if terminator <= 0:
            raise MtdInventoryError(f"entry {index} name is empty or unterminated")
        if any(raw_name[terminator:]):
            raise MtdInventoryError(f"entry {index} name padding is not zero-filled")
        try:
            name = raw_name[:terminator].decode("ascii")
        except UnicodeDecodeError as exc:
            raise MtdInventoryError(f"entry {index} name is not ASCII") from exc
        if any(ord(character) < 0x20 or ord(character) > 0x7e for character in name):
            raise MtdInventoryError(f"entry {index} name contains a control character")
        x, y, width, height = struct.unpack_from("<IIII", data, offset + NAME_SIZE)
        alpha = data[offset + RECORD_SIZE - 1]
        if width == 0 or height == 0:
            raise MtdInventoryError(f"entry {index} has an empty rectangle")
        if x + width > 0xffffffff or y + height > 0xffffffff:
            raise MtdInventoryError(f"entry {index} rectangle overflows uint32")
        if alpha not in (0, 1):
            raise MtdInventoryError(f"entry {index} alpha field is not boolean")
        result.append(MtdEntry(name, x, y, width, height, alpha == 1))
    return result


def _revision(root: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise MtdInventoryError("schema root is not a pinned Git checkout")
    return result.stdout.strip()


def texture_reference_tags(schema_root: Path) -> dict[str, list[str]]:
    """Return tag spelling to schema refs for referenceKind: textureFile blocks."""
    result: dict[str, list[str]] = defaultdict(list)
    for game in ("eaw", "foc"):
        tags_root = schema_root / game / "tags"
        if not tags_root.is_dir():
            # The pinned FoC schema currently inherits every tag from EaW and
            # therefore declares an empty tags list without a tags directory.
            continue
        for path in sorted(tags_root.glob("*.yaml"), key=lambda item: ascii_fold(item.name)):
            current_tag: str | None = None
            current_line = 0
            reference_kind: str | None = None

            def flush() -> None:
                if current_tag is not None and reference_kind == "textureFile":
                    ref = f"{game}/tags/{path.name}#L{current_line}"
                    result[ascii_fold(current_tag)].append(ref)

            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                match = re.match(r"^  - tag:\s*(.+?)\s*$", line)
                if match:
                    flush()
                    current_tag = match.group(1).strip("\"'")
                    current_line = number
                    reference_kind = None
                    continue
                if current_tag is not None:
                    match = re.match(r"^    referenceKind:\s*(.+?)\s*$", line)
                    if match:
                        reference_kind = match.group(1).strip("\"'")
            flush()
    return {key: sorted(set(value)) for key, value in sorted(result.items())}


def _safe_parser(parser: Any) -> None:
    def reject_doctype(*_args: Any) -> None:
        raise MtdInventoryError("XML contains forbidden DOCTYPE")

    def reject_entity(*_args: Any) -> int:
        raise MtdInventoryError("XML contains forbidden external entity")

    parser.StartDoctypeDeclHandler = reject_doctype
    parser.ExternalEntityRefHandler = reject_entity
    parser.SetParamEntityParsing(xml.parsers.expat.XML_PARAM_ENTITY_PARSING_NEVER)


def extract_texture_references(source: Source, tags: dict[str, list[str]]) -> list[dict[str, Any]]:
    if len(source.data) > 64 * 1024 * 1024:
        raise MtdInventoryError("XML exceeds 64 MiB policy")
    parser = xml.parsers.expat.ParserCreate()
    _safe_parser(parser)
    stack: list[tuple[str, int, int, list[str]]] = []
    result: list[dict[str, Any]] = []

    def start(name: str, _attributes: dict[str, str]) -> None:
        stack.append((name, parser.CurrentLineNumber, parser.CurrentColumnNumber + 1, []))

    def chars(value: str) -> None:
        if stack:
            stack[-1][3].append(value)

    def end(_name: str) -> None:
        name, line, column, parts = stack.pop()
        refs = tags.get(ascii_fold(name))
        if refs:
            for value in "".join(parts).split(","):
                safe_value, diagnostic = classify_texture_reference(value)
                if safe_value is None and diagnostic is None:
                    continue
                row = {
                    "column": column, "line": line,
                    "logical_path": source.logical_path,
                    "schema_refs": refs, "source_id": source.source_id,
                    "tag_name": name,
                    "value": safe_value if safe_value is not None
                    else REDACTED_TEXTURE_REFERENCE,
                }
                if diagnostic is not None:
                    row["diagnostic"] = diagnostic
                result.append(row)

    parser.StartElementHandler = start
    parser.CharacterDataHandler = chars
    parser.EndElementHandler = end
    try:
        parser.Parse(source.data, True)
    except xml.parsers.expat.ExpatError as exc:
        raise MtdInventoryError(
            f"XML parse error at {exc.lineno}:{exc.offset}: {exc}") from exc
    return result


def normalize_atlas_name(value: str) -> str:
    leaf = value.replace("\\", "/").rsplit("/", 1)[-1].strip()
    if not leaf:
        return ""
    if "." not in leaf:
        leaf += ".TGA"
    return leaf.upper()


def resolve_backing_page(corpus: Corpus, index: EffectiveAssetIndex,
                         source: Source, entries: Sequence[MtdEntry]) -> dict[str, Any]:
    stem = source.logical_path.rsplit(".", 1)[0]
    candidates = [stem + ".dds", stem + ".tga"]
    matches = [location for candidate in candidates
               if (location := index.get(candidate)) is not None]
    if len(matches) != 1:
        return {
            "candidates": candidates,
            "diagnostic": "both sibling page formats exist; explicit caller path required"
            if matches else "no sibling DDS or TGA page exists",
            "matches": [match.metadata() for match in matches],
            "rectangle_validation": "not_checked",
            "resolution": "ambiguous" if matches else "missing",
        }
    location = matches[0]
    try:
        data = location.read(corpus)
        kind, width, height = texture_dimensions(data, location.logical_path)
    except (MtdInventoryError, OSError) as exc:
        return {
            "candidates": candidates,
            "diagnostic": str(exc),
            "matches": [location.metadata()],
            "rectangle_validation": "not_checked",
            "resolution": "invalid_texture",
        }
    invalid = validate_rectangles(entries, width, height)
    return {
        "candidates": candidates,
        "page": {
            **location.metadata(),
            "format": kind,
            "height": height,
            "sha256": sha256(data),
            "width": width,
        },
        "rectangle_validation": "valid" if not invalid else "out_of_page",
        "invalid_rectangles": invalid,
        "resolution": "resolved",
    }


def _mtd_record(source: Source, entries: list[MtdEntry],
                backing_page: dict[str, Any] | None = None) -> dict[str, Any]:
    stem = source.logical_path.rsplit(".", 1)[0]
    return {
        **source.metadata(),
        **({"backing_page": backing_page} if backing_page is not None else {}),
        "backing_page_candidates": [stem + ".dds", stem + ".tga"],
        "entries": [entry.record() for entry in entries],
        "entry_count": len(entries),
    }


def generate(game_root: Path, mod_roots: Sequence[Path], schema_root: Path,
             xml_tags_path: Path, output: Path) -> dict[str, Any]:
    schema_revision = _revision(schema_root)
    tags = texture_reference_tags(schema_root)
    try:
        xml_tag_inventory = json.loads(xml_tags_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise MtdInventoryError(f"invalid XML tag inventory: {exc}") from exc
    observed_tag_names = {
        ascii_fold(row["tag_name"]) for row in xml_tag_inventory.get("rows", [])
        if row.get("node_kind") == "element"
    }
    selected_tags = {name: refs for name, refs in tags.items() if name in observed_tag_names}

    corpus = Corpus(game_root, mod_roots)
    profiles: list[dict[str, Any]] = []
    all_unresolved: list[dict[str, Any]] = []
    for profile in ("eaw", "foc", "remake"):
        asset_index = EffectiveAssetIndex(corpus, profile)
        raw_sources = list(corpus.iter_sources(profile, ".mtd", "raw"))
        raw_mount = corpus.last_manifest
        effective_sources = list(corpus.iter_sources(profile, ".mtd", "effective"))
        effective_mount = corpus.last_manifest
        parsed_by_identity: dict[tuple[str, str, str], list[MtdEntry]] = {}
        raw_mtds: list[dict[str, Any]] = []
        parse_failures: list[dict[str, Any]] = []
        for source in raw_sources:
            identity = (source.source_id, source.logical_path, source.sha256)
            try:
                entries = parse_mtd(source.data)
                parsed_by_identity[identity] = entries
                raw_mtds.append(_mtd_record(source, entries))
            except MtdInventoryError as exc:
                parse_failures.append({**source.metadata(), "diagnostic": str(exc)})

        atlas_names: dict[str, list[dict[str, str]]] = defaultdict(list)
        effective_mtds: list[dict[str, Any]] = []
        for source in effective_sources:
            identity = (source.source_id, source.logical_path, source.sha256)
            entries = parsed_by_identity.get(identity)
            if entries is None:
                continue
            effective_mtds.append(_mtd_record(
                source, entries,
                resolve_backing_page(corpus, asset_index, source, entries)))
            for entry in entries:
                atlas_names[normalize_atlas_name(entry.name)].append({
                    "logical_path": source.logical_path,
                    "name": entry.name,
                    "source_id": source.source_id,
                })

        grouped: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
        xml_parse_failures: list[dict[str, str]] = []
        for source in corpus.iter_sources(profile, ".xml", "effective"):
            try:
                for reference in extract_texture_references(source, selected_tags):
                    grouped[(ascii_fold(reference["tag_name"]), reference["value"],
                             reference.get("diagnostic", ""))].append(reference)
            except MtdInventoryError as exc:
                xml_parse_failures.append({
                    "logical_path": source.logical_path,
                    "source_id": source.source_id,
                    "diagnostic": str(exc),
                })

        xml_references: list[dict[str, Any]] = []
        unresolved: list[dict[str, Any]] = []
        for (_folded_tag, value, diagnostic), occurrences in sorted(
                grouped.items(),
                key=lambda item: (item[0][0], ascii_fold(item[0][1]), item[0][2])):
            rejected = bool(diagnostic)
            normalized = None if rejected else normalize_atlas_name(value)
            matches = [] if rejected else atlas_names.get(normalized, [])
            standalone_candidates = [] if rejected else texture_reference_candidates(value)
            standalone_matches = [] if rejected else [
                location.metadata() for candidate in standalone_candidates
                if (location := asset_index.get(candidate)) is not None
            ] if not matches else []
            if rejected:
                resolution = "rejected"
            elif matches:
                resolution = "atlas"
            elif len(standalone_matches) == 1:
                resolution = "standalone"
            else:
                resolution = "unresolved"
            row = {
                "atlas_matches": matches,
                "example": occurrences[0],
                "normalized_name": normalized,
                "occurrence_count": len(occurrences),
                "profile": profile,
                "resolution": resolution,
                "standalone_candidates": standalone_candidates,
                "standalone_matches": standalone_matches,
                "tag_name": occurrences[0]["tag_name"],
                "value": value,
            }
            if diagnostic:
                row["diagnostic"] = diagnostic
            xml_references.append(row)
            if resolution in ("rejected", "unresolved"):
                missing = {
                    "normalized_name": normalized,
                    "occurrence_count": len(occurrences),
                    "profile": profile,
                    "reason": diagnostic or
                    "reference is absent from every effective MTD and has no unique supported DDS/TGA VFS asset",
                    "site": occurrences[0],
                    "tag_name": occurrences[0]["tag_name"],
                    "value": value,
                }
                unresolved.append(missing)
                all_unresolved.append(missing)

        raw_mtds.sort(key=lambda row: (row["logical_path"], row["source_id"]))
        effective_mtds.sort(key=lambda row: (row["logical_path"], row["source_id"]))
        xml_references.sort(key=lambda row: (
            ascii_fold(row["tag_name"]), ascii_fold(row["value"]),
            row.get("diagnostic", "")))
        unresolved.sort(key=lambda row: (
            ascii_fold(row["tag_name"]), ascii_fold(row["value"]), row["reason"]))
        profiles.append({
            "counts": {
                "atlas_xml_reference_values": sum(
                    row["resolution"] == "atlas" for row in xml_references),
                "effective_icons": len(atlas_names),
                "effective_mtds": len(effective_mtds),
                "resolved_backing_pages": sum(
                    row.get("backing_page", {}).get("resolution") == "resolved"
                    for row in effective_mtds),
                "raw_mtds": len(raw_mtds) + len(parse_failures),
                "standalone_xml_reference_values": sum(
                    row["resolution"] == "standalone" for row in xml_references),
                "unresolved_xml_references": len(unresolved),
                "xml_reference_values": len(xml_references),
            },
            "effective_mount": effective_mount,
            "effective_mtds": effective_mtds,
            "mtd_parse_failures": parse_failures,
            "profile": profile,
            "raw_mount": raw_mount,
            "raw_mtds": raw_mtds,
            "unresolved": unresolved,
            "xml_parse_failures": xml_parse_failures,
            "xml_references": xml_references,
        })

    payload: dict[str, Any] = {
        "format": {
            "entry_layout": "name[64], x:u32, y:u32, width:u32, height:u32, has_alpha:u8",
            "entry_size": RECORD_SIZE,
            "flip_fields": "none; flip_x=false and flip_y=false are derived sampling orientation",
            "header": "count:u32 little-endian",
        },
        "manifest_id": "",
        "profiles": profiles,
        "provenance": {
            "lsp_revision": LSP_REVISION,
            "petroglyph_tools_revision": PETROGLYPH_TOOLS_REVISION,
            "schema_revision": schema_revision,
            "xml_tag_inventory_manifest_id": xml_tag_inventory.get("manifest_id"),
            "xml_tag_inventory_sha256": sha256(xml_tags_path.read_bytes()),
        },
        "resolution_policy": {
            "backing_page": "exactly one effective VFS sibling named <stem>.dds or <stem>.tga; dual-format siblings require an explicit caller path",
            "xml_reference": "reject and redact drive-qualified, rooted, UNC, URI, traversing, or control-bearing values; otherwise MTD name first, then exactly one supported DDS/TGA effective VFS asset under data/art/textures; otherwise unresolved",
        },
        "schema_version": 2,
        "selected_texture_reference_tags": [
            {"schema_refs": refs, "tag_name": name}
            for name, refs in selected_tags.items()
        ],
        "summary": {
            "resolved_backing_pages": sum(
                profile["counts"]["resolved_backing_pages"] for profile in profiles),
            "standalone_xml_reference_values": sum(
                profile["counts"]["standalone_xml_reference_values"] for profile in profiles),
            "mtd_parse_failures": sum(len(profile["mtd_parse_failures"]) for profile in profiles),
            "profiles": len(profiles),
            "unresolved_xml_reference_values": len(all_unresolved),
            "xml_parse_failures": sum(len(profile["xml_parse_failures"]) for profile in profiles),
        },
        "tool": "mtd_inventory",
        "tool_sources": source_hashes(Path(__file__).resolve().parent,
                                        ["common.py", "corpus.py", "mtd_inventory.py"]),
        "tool_version": TOOL_VERSION,
    }
    payload["manifest_id"] = manifest_id(payload)
    write_json(output, payload)
    return payload["summary"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--mod-root", required=True, action="append", type=Path)
    parser.add_argument("--schema-root", required=True, type=Path)
    parser.add_argument("--xml-tags", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        summary = generate(args.game_root, args.mod_root, args.schema_root,
                           args.xml_tags, args.out)
    except (CorpusError, MtdInventoryError, OSError, ValueError) as exc:
        print(f"mtd_inventory: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
