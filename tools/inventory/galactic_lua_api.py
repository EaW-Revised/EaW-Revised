#!/usr/bin/env python3
"""Inventory the conservative stock GC script closure without executing Lua.

Winning source bytes are checked against the committed FoC manifest. Only
metadata is emitted: no source, local paths, assets or debug-build internals.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.inventory.lua_calls import lex_lua, parse_source_calls
from tools.inventory.lua_inventory import is_standard

ROOT = Path(__file__).resolve().parents[2]
INFRASTRUCTURE = {
    "Create_Thread", "Create_Thread.Kill", "Create_Thread.Kill_All", "GetThreadID",
    "GetEvent", "GetEvent.Params", "GetEvent.Reset", "ThreadValue", "ThreadValue.Set",
    "ThreadValue.Reset", "_ScriptExit", "GetCurrentTime", "GetCurrentTime.Frame",
    "Register_Event", "Cancel_Event", "require",
}


def is_root(path: str) -> bool:
    # Include mixed-mode story/intervention scripts deliberately: inventory
    # coverage is an upper bound, never a claim of runtime GC reachability.
    return (path.startswith("data/scripts/story/")
            or path.startswith("data/scripts/interventions/")
            or path.startswith("data/scripts/ai/galacticmode/")
            or (path.startswith("data/scripts/ai/") and path.count("/") == 3)
            or (path.startswith("data/scripts/freestore/") and "galactic" in Path(path).stem))


def dependencies(data: bytes) -> tuple[set[str], list[int]]:
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = data.decode("cp1252")
    tokens = lex_lua(text)
    modules: set[str] = set()
    dynamic: list[int] = []
    for index, token in enumerate(tokens[:-1]):
        if token.kind != "name" or token.value != "require":
            continue
        if index and tokens[index - 1].value in {".", ":", "function"}:
            continue
        value = tokens[index + 1]
        parenthesized = value.value == "("
        if not parenthesized and value.kind != "string":
            continue
        if value.value == "(":
            value = tokens[index + 2]
        end = index + (3 if parenthesized else 2)
        literal = value.kind == "string" and (not parenthesized or tokens[end].value in {")", ","})
        if literal:
            text = value.value
            modules.add((text[2:-2] if text.startswith("[[") else text[1:-1]).lower())
        else:
            dynamic.append(token.line)
    return modules, dynamic


def generate(effective: Path, manifest: dict, registrations: dict) -> dict:
    profile = next(p for p in manifest["profiles"] if p["profile"] == "foc")
    files = {row["logical_path"]: row for row in profile["file_outcomes"]}
    by_module: dict[str, list[str]] = defaultdict(list)
    for path in files:
        by_module[Path(path).stem].append(path)
    seeds = sorted(path for path in files if is_root(path))
    pending = list(seeds)
    selected: dict[str, dict] = {}
    parsed = {}
    unresolved = []
    while pending:
        path = pending.pop()
        if path in selected:
            continue
        row = files[path]
        data = (effective / path).read_bytes()
        if hashlib.sha256(data).hexdigest() != row["sha256"]:
            raise ValueError(f"winning source differs from the committed manifest: {path}")
        calls, helpers = parse_source_calls(data)
        modules, dynamic = dependencies(data)
        edges = []
        for module in sorted(modules):
            # Library names are globally unique in the stock closure. Preserve
            # ambiguity instead of silently picking a search-path winner.
            candidates = by_module.get(Path(module).stem, [])
            if len(candidates) != 1:
                unresolved.append({"path": path, "module": module,
                                   "reason": "missing or ambiguous required module"})
            else:
                edges.append(candidates[0])
                pending.append(candidates[0])
        unresolved.extend({"path": path, "line": line, "reason": "dynamic require"} for line in dynamic)
        selected[path] = {"path": path, "sha256": row["sha256"], "source_id": row["source_id"],
                          "root": path in seeds, "requires": sorted(edges)}
        parsed[path] = calls, helpers

    # Helpers are scoped to each root's dependency closure, not to all stock
    # modules: an unrelated script definition cannot hide an engine call.
    def helper_closure(path: str) -> tuple[set[str], set[str]]:
        found, todo, helpers = set(), [path], set()
        while todo:
            current = todo.pop()
            if current in found:
                continue
            found.add(current)
            helpers.update(parsed[current][1])
            todo.extend(selected[current]["requires"])
        return found, helpers

    contexts: dict[str, list[set[str]]] = defaultdict(list)
    for seed in seeds:
        members, helpers = helper_closure(seed)
        for member in members:
            contexts[member].append(helpers)

    declared_globals = {r["exposed_name"] for r in registrations["entries"] if r["call_kind"] == "global"}
    declared_members = {r["exposed_name"] for r in registrations["entries"] if r["call_kind"] in {"method", "member"}}
    grouped = defaultdict(list)
    for path in sorted(selected):
        calls, _ = parsed[path]
        helpers = set.intersection(*contexts[path])
        for call in calls:
            if call.reason or call.symbol is None:
                unresolved.append({"path": path, "line": call.line, "reason": call.reason or "unknown callable"})
                continue
            symbol, kind = call.symbol, call.call_kind
            registered = (symbol in declared_globals if kind == "global" else
                          symbol.split(".")[-1] in declared_members)
            if symbol in {"Free_Random", "GameRandom.Free_Random"}:
                status, target = "forbidden", "docs/lua-sandbox.md"
            elif symbol in INFRASTRUCTURE:
                status, target = "bound", "src/script/authoritative/bindings.cpp"
            elif kind == "global" and symbol in helpers:
                status, target = "script", "stock dependency closure"
            elif is_standard(symbol, kind):
                status, target = "partial", "docs/lua-sandbox.md"
            elif registered:
                status, target = "missing", "galactic host adapters (legacy #1991)"
            else:
                status, target = "unresolved", "unregistered callable or context-dependent script helper"
            original = symbol
            if kind in {"dotted", "method"} and symbol not in INFRASTRUCTURE and not is_standard(symbol, kind):
                # Object variable names do not define distinct engine APIs.
                symbol, kind = symbol.split(".")[-1], "member"
            grouped[(symbol, kind, status, target, registered)].append({"path": path, "line": call.line, "symbol": original})
    rows = []
    for (symbol, kind, status, target, registered), locations in sorted(grouped.items()):
        by_path = defaultdict(list)
        for location in locations:
            by_path[location["path"]].append(location["line"])
        rows.append({"symbol": symbol, "call_kind": kind, "status": status, "target": target,
                     "registered_engine_name": registered, "call_count": len(locations),
                     "receiver_expressions": sorted({loc["symbol"] for loc in locations if loc["symbol"] != symbol}),
                     "locations": [{"path": path, "lines": sorted(lines), "call_count": len(lines)}
                                   for path, lines in sorted(by_path.items())]})
    return {"schema_version": 1, "source_manifest_id": manifest["manifest_id"],
            "scope": "conservative GC roots: galactic-mode and top-level AI, galactic freestore, all story and intervention scripts; literal require closure",
            "limitations": ["Mixed-mode story and intervention roots include tactical calls; runtime reachability is not established.",
                            "Static dynamic calls and ambiguous requires remain unresolved.",
                            "Standard library calls are partial until checked against sandbox policy; script helpers require their stock modules.",
                            "Tactical world bindings do not count as galactic bindings."],
            "rules": ["WGA-32", "WGA-37..47", "WST-53..57"],
            "counts": {"roots": len(seeds), "scripts": len(selected), "calls": sum(r["call_count"] for r in rows),
                       "unresolved": len(unresolved), "statuses": dict(sorted(Counter(r["status"] for r in rows).items()))},
            "scripts": [selected[p] for p in sorted(selected)], "rows": rows,
            "unresolved": sorted(unresolved, key=lambda r: json.dumps(r, sort_keys=True))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--effective", type=Path, required=True, help="unpacked winning FoC view (research only)")
    parser.add_argument("--manifest", type=Path, default=ROOT / "plan/inventories/lua-manifest.json")
    parser.add_argument("--registrations", type=Path, default=ROOT / "plan/inventories/foc-lua-registrations.json")
    parser.add_argument("--out", type=Path, default=ROOT / "plan/inventories/galactic-lua-api.json")
    parser.add_argument("--fault-header", type=Path, default=ROOT / "src/script/authoritative/galactic_api_inventory.hpp")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    result = generate(args.effective, json.loads(args.manifest.read_text(encoding="utf-8")),
                      json.loads(args.registrations.read_text(encoding="utf-8")))
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    declared_globals = {r["exposed_name"] for r in json.loads(args.registrations.read_text(encoding="utf-8"))["entries"]
                        if r["call_kind"] == "global"}
    globals_ = sorted({r["symbol"] for r in result["rows"] if r["status"] == "missing" and r["call_kind"] == "global"}
                      | {name for r in result["rows"] if r["status"] == "missing"
                         for name in r["receiver_expressions"] if name.split(".")[0] in declared_globals})
    header = ("#pragma once\n\n// Generated by tools/inventory/galactic_lua_api.py; names only, no stock source.\n"
              "#include <array>\n#include <string_view>\n\nnamespace eawr::script::authoritative::detail {\n"
              f"inline constexpr std::array<std::string_view, {len(globals_)}> missing_galactic_globals{{{{\n"
              + "".join(f'    "{name}",\n' for name in globals_)
              + "}};\n} // namespace eawr::script::authoritative::detail\n")
    if args.check:
        if args.out.read_text(encoding="utf-8") != rendered or args.fault_header.read_text(encoding="utf-8") != header:
            raise SystemExit("galactic Lua inventory is stale")
    else:
        args.out.write_text(rendered, encoding="utf-8", newline="\n")
        args.fault_header.write_text(header, encoding="utf-8", newline="\n")
    print(json.dumps(result["counts"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
