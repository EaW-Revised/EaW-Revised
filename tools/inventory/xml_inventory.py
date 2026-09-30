#!/usr/bin/env python3
"""Generate deterministic P0-09 XML shape and schema-status inventories."""
from __future__ import annotations

import argparse
import json
import sys
import xml.parsers.expat
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from common import TOOL_VERSION, ascii_fold, canonical_path, manifest_id, sha256, source_hashes, write_json
    from corpus import Corpus, CorpusError, Source
    from schema_index import SchemaError, build_schema_index
else:
    from .common import TOOL_VERSION, ascii_fold, canonical_path, manifest_id, sha256, source_hashes, write_json
    from .corpus import Corpus, CorpusError, Source
    from .schema_index import SchemaError, build_schema_index


class XmlInventoryError(RuntimeError): pass


def _configure_safe_parser(parser: Any, context: str) -> None:
    """Reject every DTD/entity path after Expat performs encoding detection."""
    def reject_doctype(*_args: Any) -> None:
        raise XmlInventoryError(f"{context} contains forbidden DOCTYPE")

    def reject_entity(*_args: Any) -> int:
        raise XmlInventoryError(f"{context} contains forbidden external entity")

    parser.StartDoctypeDeclHandler = reject_doctype
    parser.ExternalEntityRefHandler = reject_entity
    parser.SetParamEntityParsing(xml.parsers.expat.XML_PARAM_ENTITY_PARSING_NEVER)


@dataclass(frozen=True)
class Occurrence:
    object_type: str
    tag_path: str
    node_kind: str
    tag_name: str
    line: int
    column: int
    type_reason: str | None
    order: int


class SchemaLookup:
    def __init__(self, value: dict[str, Any]):
        self.value = value; self.revision = value["revision"]
        self.types: dict[str, str] = {}
        for row in value["types"]:
            self.types.setdefault(ascii_fold(row["name"]), row["name"])
        self.tags: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
        for row in value["tags"]:
            self.tags[(ascii_fold(row["object_type"]), ascii_fold(row["tag"]))].append(row)
        self.metafiles: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for row in value["metafiles"]: self.metafiles[row["path"]].append(row)

    def meta(self, logical_path: str, profile: str) -> list[dict[str, Any]]:
        return [r for r in self.metafiles.get(ascii_fold(logical_path), []) if r["game"] == "eaw" or profile != "eaw"]

    def status(self, profile: str, object_type: str, tag: str) -> tuple[str, str | None, list[str]]:
        candidates = [r for r in self.tags.get((ascii_fold(object_type), ascii_fold(tag)), [])
                      if r["game"] == "eaw" or profile != "eaw"]
        if not candidates: return "unknown", None, []
        candidates.sort(key=lambda r: (not r["deprecated"], r["game"], r["schema_ref"]))
        selected = candidates[0]
        status = "deprecated" if any(r["deprecated"] for r in candidates) else "known"
        applicability = sorted({r["game"] for r in candidates} | {r["available_since"] for r in candidates if r["available_since"]})
        return status, selected["schema_ref"], applicability


def parse_xml(source: Source, profile: str, schema: SchemaLookup,
              forced_types: tuple[str, ...] = ()) -> list[Occurrence]:
    if len(source.data) > 64 * 1024 * 1024: raise XmlInventoryError("XML exceeds 64 MiB policy")
    parser = xml.parsers.expat.ParserCreate()
    _configure_safe_parser(parser, "XML")
    parser.buffer_text = True
    stack: list[str] = []; contexts: list[tuple[str, int, str | None]] = []
    occurrences: list[Occurrence] = []; order = 0; meta = schema.meta(source.logical_path, profile)

    def start(name: str, attributes: dict[str, str]) -> None:
        nonlocal order
        depth = len(stack); stack.append(name)
        canonical_type = schema.types.get(ascii_fold(name))
        reason: str | None = None
        if canonical_type:
            object_type, base_depth = canonical_type, depth
        elif len(forced_types) == 1 and depth <= 1:
            # A file-registry type applies to each definition directly below the
            # collection root.  The concrete element spelling remains in tag_path.
            object_type, base_depth = forced_types[0], depth
        elif depth == 0:
            singleton = [m for m in meta if m["meta_file_type"] == "singleton" and len(m["types"]) == 1]
            if singleton: object_type, base_depth = singleton[0]["types"][0], depth
            else:
                object_type, base_depth = name, depth
                reason = "actual root/config type is not a registered schema object type"
        elif depth == 1:
            direct = [m for m in meta if m["meta_file_type"] == "directContent" and len(m["types"]) == 1]
            if direct: object_type, base_depth = direct[0]["types"][0], depth
            else: object_type, base_depth, reason = contexts[-1]
        else: object_type, base_depth, reason = contexts[-1]
        contexts.append((object_type, base_depth, reason))
        path = "/".join(stack[base_depth:])
        order += 1; occurrences.append(Occurrence(object_type, path, "element", name,
                                                  parser.CurrentLineNumber, parser.CurrentColumnNumber + 1,
                                                  reason, order))
        for attr in attributes:  # Expat preserves source attribute order on supported builds.
            order += 1; occurrences.append(Occurrence(object_type, path + "/@" + attr, "attribute", attr,
                                                       parser.CurrentLineNumber, parser.CurrentColumnNumber + 1,
                                                       reason, order))

    def end(_name: str) -> None:
        stack.pop(); contexts.pop()

    parser.StartElementHandler = start; parser.EndElementHandler = end
    try: parser.Parse(source.data, True)
    except xml.parsers.expat.ExpatError as exc:
        raise XmlInventoryError(f"XML parse error at {exc.lineno}:{exc.offset}: {exc}") from exc
    return occurrences


def registry_type_hints(sources: list[Source], profile: str, schema: SchemaLookup) -> dict[str, tuple[str, ...]]:
    """Resolve pinned schema file registries without inventing types from filenames."""
    by_path = {source.logical_path: source for source in sources}
    hints: dict[str, set[str]] = defaultdict(set)
    for meta_path, entries in schema.metafiles.items():
        applicable = [row for row in entries if row["meta_file_type"] == "fileRegistry" and
                      (row["game"] == "eaw" or profile != "eaw")]
        source = by_path.get(meta_path)
        if not applicable or source is None:
            continue
        parser = xml.parsers.expat.ParserCreate(); stack: list[str] = []; text_parts: list[str] = []
        _configure_safe_parser(parser, f"registry {meta_path}")
        values: list[str] = []
        def start(name: str, _attrs: dict[str, str]) -> None:
            stack.append(name)
            if ascii_fold(name) == "file": text_parts.clear()
        def chars(value: str) -> None:
            if stack and ascii_fold(stack[-1]) == "file": text_parts.append(value)
        def end(name: str) -> None:
            if ascii_fold(name) == "file": values.append("".join(text_parts).strip())
            stack.pop()
        parser.StartElementHandler = start; parser.CharacterDataHandler = chars; parser.EndElementHandler = end
        try: parser.Parse(source.data, True)
        except xml.parsers.expat.ExpatError as exc:
            raise XmlInventoryError(f"schema registry {meta_path} is malformed: {exc}") from exc
        parent = meta_path.rsplit("/", 1)[0]
        for value in values:
            if not value: continue
            raw = value.replace("\\", "/")
            logical = ascii_fold(raw) if ascii_fold(raw).startswith("data/") else ascii_fold(parent + "/" + raw)
            try:
                logical = canonical_path(logical)
            except ValueError:
                continue
            for row in applicable:
                hints[logical].update(row["types"])
    return {path: tuple(sorted(types, key=ascii_fold)) for path, types in hints.items()}


def site(source: Source, occurrence: Occurrence) -> dict[str, Any]:
    return {"column": occurrence.column, "line": occurrence.line, "logical_path": source.logical_path,
            "order": occurrence.order, "source_id": source.source_id}


def site_key(value: dict[str, Any]) -> tuple[Any, ...]:
    return (value["source_id"], value["logical_path"], value["order"], value["line"], value["column"])


def generate(game_root: Path, mod_root: Path | Sequence[Path],
             schema_root: Path, out: Path) -> dict[str, Any]:
    schema_value = build_schema_index(schema_root); schema = SchemaLookup(schema_value)
    corpus = Corpus(game_root, mod_root); rows: list[dict[str, Any]] = []; unresolved: list[dict[str, Any]] = []
    profile_manifests: list[dict[str, Any]] = []
    for profile in ("eaw", "foc", "remake"):
        raw_sources = list(corpus.iter_sources(profile, ".xml", "raw")); raw_mount = corpus.last_manifest
        effective_sources = list(corpus.iter_sources(profile, ".xml", "effective")); effective_mount = corpus.last_manifest
        type_hints = registry_type_hints(effective_sources, profile, schema)
        effective_identity = {(s.source_id, s.logical_path, s.sha256) for s in effective_sources}
        raw_outcomes: list[dict[str, Any]] = []
        grouped: dict[tuple[str, str, str, str], list[tuple[Source, Occurrence]]] = defaultdict(list)
        schema_matches: list[dict[str, Any]] = []
        for source in raw_sources:
            outcome = {**source.metadata(), "diagnostic": None, "occurrences": 0, "parsed": False}
            try:
                occurrences = parse_xml(source, profile, schema, type_hints.get(source.logical_path, ())); outcome["parsed"] = True
                outcome["occurrences"] = len(occurrences)
                outcome["occurrence_sequence_sha256"] = sha256(json.dumps(
                    [[o.object_type, o.tag_path, o.node_kind, o.tag_name, o.line, o.column, o.order]
                     for o in occurrences], ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
                if (source.source_id, source.logical_path, source.sha256) in effective_identity:
                    for occurrence in occurrences:
                        grouped[(occurrence.object_type, occurrence.tag_path,
                                 occurrence.node_kind, occurrence.tag_name)].append((source, occurrence))
            except XmlInventoryError as exc: outcome["diagnostic"] = {
                "code": "EAWR-INV-XML-0001", "message": str(exc)}
            raw_outcomes.append(outcome)
        raw_by_key = {(r["source_id"], r["logical_path"], r["sha256"]): r for r in raw_outcomes}
        outcomes = [raw_by_key[(s.source_id, s.logical_path, s.sha256)] for s in effective_sources]
        for (object_type, tag_path, node_kind, tag_name), values in sorted(
                grouped.items(), key=lambda item: tuple(ascii_fold(str(x)) for x in item[0])):
            locations = sorted((site(source, occurrence) for source, occurrence in values), key=site_key)
            status, schema_ref, applicability = schema.status(profile, object_type, tag_name) if node_kind == "element" else ("unknown", None, [])
            file_count = len({(s.source_id, s.logical_path) for s, _ in values})
            row = {"example": {k: locations[0][k] for k in ("source_id", "logical_path", "line", "column")},
                   "file_count": file_count, "node_kind": node_kind, "object_type": object_type,
                   "profile": profile, "schema_ref": schema_ref, "status": status,
                   "tag_name": tag_name, "tag_path": tag_path, "usage_count": len(values)}
            rows.append(row)
            if schema_ref is not None:
                schema_matches.append({"applicability": applicability, "node_kind": node_kind,
                                       "object_type": object_type, "schema_ref": schema_ref,
                                       "tag_name": tag_name, "tag_path": tag_path})
            if status == "unknown":
                reasons = sorted({o.type_reason for _, o in values if o.type_reason})
                unresolved.append({"node_kind": node_kind, "object_type": object_type, "profile": profile,
                                   "reasons": reasons or ["tag/context absent from pinned schema"],
                                   "tag_name": tag_name, "tag_path": tag_path})
        profile_manifests.append({"effective": effective_mount, "effective_file_count": len(effective_sources),
                                  "file_outcomes": outcomes,
                                  "parse_failures": sum(not r["parsed"] for r in outcomes), "profile": profile,
                                  "raw": raw_mount, "raw_file_count": len(raw_sources),
                                  "raw_file_outcomes": raw_outcomes, "registry_type_hints": [
                                      {"logical_path": path, "types": list(types)} for path, types in sorted(type_hints.items())],
                                  "schema_matches": schema_matches,
                                  "raw_parse_failures": sum(not r["parsed"] for r in raw_outcomes)})
    rows.sort(key=lambda r: (r["profile"], ascii_fold(r["object_type"]), ascii_fold(r["tag_path"]),
                             r["node_kind"], ascii_fold(r["tag_name"])))
    unresolved.sort(key=lambda r: (r["profile"], ascii_fold(r["object_type"]), ascii_fold(r["tag_path"]), r["node_kind"]))
    manifest: dict[str, Any] = {"manifest_id": "", "profiles": profile_manifests,
                                "schema_files": schema_value["files"], "schema_revision": schema.revision,
                                "schema_version": 1, "tool": "xml_inventory",
                                "tool_sources": source_hashes(Path(__file__).resolve().parent,
                                                              ["common.py", "corpus.py", "schema_index.py", "xml_inventory.py"]),
                                "tool_version": TOOL_VERSION}
    manifest["manifest_id"] = manifest_id(manifest)
    tags = {"manifest_id": manifest["manifest_id"], "rows": rows, "schema_revision": schema.revision, "schema_version": 1}
    unknown = {"manifest_id": manifest["manifest_id"], "rows": unresolved, "schema_version": 1}
    write_json(out / "xml-tags.json", tags); write_json(out / "xml-unresolved.json", unknown)
    write_json(out / "xml-manifest.json", manifest)
    return {"manifest_id": manifest["manifest_id"], "parse_failures": sum(p["parse_failures"] for p in profile_manifests),
            "rows": len(rows), "unresolved": len(unresolved)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--mod-root", required=True, action="append", type=Path,
                        help="MODPATH root; repeat in leaf-first dependency order")
    parser.add_argument("--schema-root", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path); args = parser.parse_args(argv)
    try: summary = generate(args.game_root, args.mod_root, args.schema_root, args.out)
    except (CorpusError, OSError, SchemaError, ValueError, XmlInventoryError) as exc:
        print(f"xml_inventory: {exc}", file=sys.stderr); return 2
    print(json.dumps(summary, sort_keys=True)); return 0


if __name__ == "__main__": raise SystemExit(main())
