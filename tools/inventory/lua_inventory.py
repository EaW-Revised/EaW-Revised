#!/usr/bin/env python3
"""Generate deterministic P0-08 Lua API inventory metadata."""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, Sequence

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from common import TOOL_VERSION, manifest_id, sha256, source_hashes, write_json
    from corpus import Corpus, CorpusError, Source
    from lua_calls import CallSite, LuaError, PGLUA_HEADER, analyze_pglua, parse_source_calls
else:
    from .common import TOOL_VERSION, manifest_id, sha256, source_hashes, write_json
    from .corpus import Corpus, CorpusError, Source
    from .lua_calls import CallSite, LuaError, PGLUA_HEADER, analyze_pglua, parse_source_calls

STANDARD_GLOBALS = {
    "assert", "collectgarbage", "dofile", "error", "gcinfo", "getfenv", "getmetatable",
    "ipairs", "loadfile", "loadlib", "loadstring", "next", "pairs", "pcall", "print",
    "rawequal", "rawget", "rawset", "require", "setfenv", "setmetatable", "tonumber",
    "tostring", "type", "unpack", "xpcall",
}
STANDARD_PREFIXES = {"coroutine", "debug", "io", "math", "os", "string", "table"}
FOC_INDEX = Path(__file__).resolve().parents[2] / "plan/inventories/foc-lua-registrations.json"


def location(source: Source, call: CallSite) -> dict[str, Any]:
    kind = "pglua" if call.prototype_id is not None else "text"
    return {
        "column": call.column,
        "line": call.line,
        "logical_path": source.logical_path,
        "pc": call.pc,
        "prototype_id": call.prototype_id,
        "source_id": source.source_id,
        "source_kind": kind,
    }


def location_key(value: dict[str, Any]) -> tuple[Any, ...]:
    return (value["source_id"], value["logical_path"], value["source_kind"],
            value["prototype_id"] if value["prototype_id"] is not None else -1,
            value["pc"] if value["pc"] is not None else -1,
            value["line"] if value["line"] is not None else -1,
            value["column"] if value["column"] is not None else -1)


class DeclarationIndex:
    def __init__(self, path: Path):
        raw = path.read_bytes()
        self.sha256 = sha256(raw)
        value = json.loads(raw.decode("utf-8"))
        rows = value.get("entries", value.get("records"))
        if value.get("schema_version") != 1 or not isinstance(rows, list):
            raise ValueError("declaration index must use schema version 1 with entries")
        self.meta = {"reference": value.get("reference", value.get("source")),
                     "scope": value.get("scope", value.get("extraction"))}
        self.rows = rows
        self.foc_registrations = "source" in value

    def match(self, symbol: str, kind: str) -> tuple[str, str | None, list[dict[str, Any]], list[str]]:
        name = symbol.split(".")[-1] if kind == "dotted" else symbol
        kinds = {"method", "member"} if self.foc_registrations and kind == "dotted" else {kind}
        matches = [r for r in self.rows if r.get("exposed_name") == name and r.get("call_kind") in kinds]
        if not matches:
            return "absent", None, [], []
        receivers = sorted({r["receiver_type"] for r in matches if r.get("receiver_type")})
        locations = sorted({(loc["file"], loc["line"]) for r in matches for loc in r.get("reference_locations", [])})
        refs = [{"file": f, "line": n} for f, n in locations]
        if not self.foc_registrations and kind == "method" and len(receivers) != 1:
            return "unresolved", None, refs, receivers
        receiver_type = receivers[0] if kind == "method" and len(receivers) == 1 else None
        return "present", receiver_type, refs, receivers


def index_for_profile(profile: str, glyphx: DeclarationIndex,
                      foc: DeclarationIndex) -> DeclarationIndex:
    return glyphx if profile == "eaw" else foc


def is_standard(symbol: str, kind: str) -> bool:
    return (kind == "global" and symbol in STANDARD_GLOBALS) or (
        kind == "dotted" and symbol.split(".", 1)[0] in STANDARD_PREFIXES)


def parse_one(source: Source) -> tuple[list[CallSite], set[str], dict[str, Any]]:
    if source.data.startswith(PGLUA_HEADER):
        calls, detail = analyze_pglua(source.data)
        return calls, set(), {"format": "pglua", **detail}
    calls, helpers = parse_source_calls(source.data)
    return calls, helpers, {"format": "text", "instruction_count": None,
                            "prototype_count": None, "unsupported_instructions": []}


def generate(game_root: Path, mod_root: Path | Sequence[Path],
             reference_index: Path, out: Path, foc_index: Path = FOC_INDEX) -> dict[str, Any]:
    glyphx_declarations = DeclarationIndex(reference_index)
    foc_declarations = DeclarationIndex(foc_index)
    corpus = Corpus(game_root, mod_root)
    all_rows: list[dict[str, Any]] = []
    all_unresolved: list[dict[str, Any]] = []
    profile_manifests: list[dict[str, Any]] = []

    for profile in ("eaw", "foc", "remake"):
        # FoC and mods running on FoC use the original engine's registration
        # names. The base EaW profile retains the GlyphX reference index.
        declarations = index_for_profile(profile, glyphx_declarations, foc_declarations)
        raw_sources = list(corpus.iter_sources(profile, ".lua", "raw"))
        raw_mount = corpus.last_manifest
        effective_sources = list(corpus.iter_sources(profile, ".lua", "effective"))
        effective_mount = corpus.last_manifest
        outcomes: list[dict[str, Any]] = []
        raw_outcomes: list[dict[str, Any]] = []
        grouped: dict[tuple[str, str], list[tuple[Source, CallSite]]] = defaultdict(list)

        effective_identity = {(s.source_id, s.logical_path, s.sha256) for s in effective_sources}
        for source in raw_sources:
            outcome: dict[str, Any] = {**source.metadata(), "diagnostic": None, "parsed": False}
            try:
                calls, helpers, detail = parse_one(source)
                outcome.update(detail); outcome["parsed"] = True; outcome["call_sites"] = len(calls)
                if (source.source_id, source.logical_path, source.sha256) in effective_identity:
                    for call in calls:
                        loc = location(source, call)
                        if call.reason or call.symbol is None or call.call_kind is None:
                            all_unresolved.append({"location": loc, "profile": profile,
                                                   "reason": call.reason or "unknown callable", "symbol": call.symbol})
                            continue
                        if call.call_kind == "global" and call.symbol in helpers:
                            all_unresolved.append({"location": loc, "profile": profile,
                                                   "reason": "script-defined helper excluded", "symbol": call.symbol})
                            continue
                        if is_standard(call.symbol, call.call_kind):
                            continue
                        grouped[(call.symbol, call.call_kind)].append((source, call))
            except (LuaError, ValueError) as exc:
                outcome["diagnostic"] = {
                    "code": "EAWR-INV-LUA-0002" if source.data.startswith(PGLUA_HEADER) else "EAWR-INV-LUA-0001",
                    "column": getattr(exc, "column", None), "line": getattr(exc, "line", None),
                    "message": str(exc), "offset": getattr(exc, "offset", None),
                    "prototype": getattr(exc, "prototype", None),
                }
            raw_outcomes.append(outcome)

        # Effective entries must all have a raw outcome; copy in precedence-sorted order.
        raw_by_key = {(r["source_id"], r["logical_path"], r["sha256"]): r for r in raw_outcomes}
        for source in effective_sources:
            outcomes.append(raw_by_key[(source.source_id, source.logical_path, source.sha256)])

        call_sites: list[dict[str, Any]] = []
        for (symbol, kind), sites in sorted(grouped.items(), key=lambda item: item[0]):
            ref_status, receiver_type, refs, receiver_types = declarations.match(symbol, kind)
            locations = sorted((location(source, call) for source, call in sites), key=location_key)
            classification = "engine" if ref_status == "present" else "candidate"
            row = {
                "call_count": len(locations), "call_kind": kind, "classification": classification,
                "example": locations[0], "profile": profile, "receiver_type": receiver_type,
                "reference_locations": refs, "reference_status": ref_status, "symbol": symbol,
            }
            if declarations.foc_registrations:
                row["receiver_types"] = receiver_types
            all_rows.append(row)
            for (source, call), loc in zip(sorted(sites, key=lambda pair: location_key(location(pair[0], pair[1]))), locations):
                call_sites.append({"call_kind": kind, "location": loc,
                                   "receiver_provenance": call.receiver, "symbol": symbol})
        profile_manifests.append({
            "call_sites": sorted(call_sites, key=lambda x: (x["symbol"], x["call_kind"], location_key(x["location"]))),
            "effective": effective_mount,
            "effective_file_count": len(effective_sources),
            "file_outcomes": outcomes,
            "parse_failures": sum(not row["parsed"] for row in outcomes),
            "profile": profile,
            "raw": raw_mount,
            "raw_file_count": len(raw_sources),
            "raw_file_outcomes": raw_outcomes,
            "raw_parse_failures": sum(not row["parsed"] for row in raw_outcomes),
        })

    all_rows.sort(key=lambda r: (r["profile"], r["symbol"], r["call_kind"], r["receiver_type"] or ""))
    all_unresolved.sort(key=lambda r: (r["profile"], r.get("symbol") or "", location_key(r["location"]), r["reason"]))
    manifest: dict[str, Any] = {
        "declaration_index": {"metadata": glyphx_declarations.meta, "sha256": glyphx_declarations.sha256},
        "foc_declaration_index": {"metadata": foc_declarations.meta, "sha256": foc_declarations.sha256},
        "manifest_id": "",
        "profiles": profile_manifests,
        "schema_version": 1,
        "tool": "lua_inventory",
        "tool_sources": source_hashes(Path(__file__).resolve().parent,
                                       ["common.py", "corpus.py", "lua_calls.py", "lua_inventory.py"]),
        "tool_version": TOOL_VERSION,
    }
    manifest["manifest_id"] = manifest_id(manifest)
    api = {"manifest_id": manifest["manifest_id"], "rows": all_rows, "schema_version": 2}
    unresolved = {"manifest_id": manifest["manifest_id"], "rows": all_unresolved, "schema_version": 2}
    write_json(out / "lua-api.json", api)
    write_json(out / "lua-unresolved.json", unresolved)
    write_json(out / "lua-manifest.json", manifest)
    return {"rows": len(all_rows), "unresolved": len(all_unresolved),
            "parse_failures": sum(p["parse_failures"] for p in profile_manifests),
            "manifest_id": manifest["manifest_id"]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--mod-root", required=True, action="append", type=Path,
                        help="MODPATH root; repeat in leaf-first dependency order")
    parser.add_argument("--reference-index", required=True, type=Path,
                        help="GlyphX index for the base EaW profile")
    parser.add_argument("--foc-index", type=Path, default=FOC_INDEX,
                        help="FoC registration names for FoC and Remake profiles")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        summary = generate(args.game_root, args.mod_root, args.reference_index, args.out, args.foc_index)
    except (CorpusError, LuaError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"lua_inventory: {exc}", file=sys.stderr); return 2
    print(json.dumps(summary, sort_keys=True)); return 0


if __name__ == "__main__": raise SystemExit(main())
