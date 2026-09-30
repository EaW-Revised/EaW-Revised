#!/usr/bin/env python3
"""Build a deterministic, narrow index of the pinned MIT eaw-schema YAML files."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from common import TOOL_VERSION, ascii_fold, sha256, write_json
else:
    from .common import TOOL_VERSION, ascii_fold, sha256, write_json


class SchemaError(RuntimeError): pass


def _scalar(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        try: return json.loads(value)
        except json.JSONDecodeError: pass
    if len(value) >= 2 and value[0] == value[-1] == "'": return value[1:-1].replace("''", "'")
    return value


def _revision(root: Path) -> str:
    result = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True)
    if result.returncode != 0: raise SchemaError("schema root is not a pinned git checkout")
    return result.stdout.strip()


def _manifest(root: Path, game: str) -> dict[str, Any]:
    path = root / game / "_index.json"
    if not path.is_file(): raise SchemaError(f"missing {game}/_index.json")
    try: value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc: raise SchemaError(f"invalid {game}/_index.json: {exc}") from exc
    return value


def _parse_types(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []; current: dict[str, Any] | None = None
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = re.match(r"^  - typeName:\s*(.+?)\s*$", line)
        if match:
            if current: rows.append(current)
            current = {"name": _scalar(match.group(1)), "name_tag": None, "line": number}
        elif current:
            match = re.match(r"^    nameTag:\s*(.+?)\s*$", line)
            if match: current["name_tag"] = _scalar(match.group(1))
    if current: rows.append(current)
    return rows


def _parse_tags(path: Path, game: str, relative: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []; current: dict[str, Any] | None = None
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = re.match(r"^  - tag:\s*(.+?)\s*$", line)
        if match:
            if current: rows.append(current)
            current = {"available_since": None, "deprecated": False, "game": game,
                       "line": number, "tag": _scalar(match.group(1)), "value_type": None,
                       "schema_ref": f"{game}/{relative}#L{number}"}
            continue
        if current:
            match = re.match(r"^    (type|availableSince|deprecated):\s*(.+?)\s*$", line)
            if match:
                key, value = match.groups(); value = _scalar(value)
                if key == "type": current["value_type"] = value
                elif key == "availableSince": current["available_since"] = value
                else: current["deprecated"] = ascii_fold(value) == "true"
    if current: rows.append(current)
    return rows


def _parse_metafiles(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []; current: dict[str, Any] | None = None; collecting = False
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = re.match(r"^  - path:\s*(.+?)\s*$", line)
        if match:
            if current: rows.append(current)
            current = {"line": number, "path": ascii_fold(_scalar(match.group(1)).replace("\\", "/")),
                       "meta_file_type": None, "types": []}; collecting = False; continue
        if not current: continue
        match = re.match(r"^    metaFileType:\s*(.+?)\s*$", line)
        if match: current["meta_file_type"] = _scalar(match.group(1)); collecting = False; continue
        if re.match(r"^    types:\s*$", line): collecting = True; continue
        match = re.match(r"^      -\s*(.+?)\s*$", line)
        if collecting and match: current["types"].append(_scalar(match.group(1))); continue
        if line.startswith("    ") and not line.startswith("      "): collecting = False
    if current: rows.append(current)
    return rows


def build_schema_index(root: Path) -> dict[str, Any]:
    license_path = root / "LICENSE"
    if not license_path.is_file() or not license_path.read_text(encoding="utf-8").startswith("MIT License"):
        raise SchemaError("eaw-schema LICENSE is missing or is not MIT")
    revision = _revision(root); files: list[dict[str, str]] = []; types: list[dict[str, Any]] = []
    tags: list[dict[str, Any]] = []; metafiles: list[dict[str, Any]] = []
    for game in ("eaw", "foc"):
        manifest = _manifest(root, game)
        for rel in manifest.get("types", []):
            path = root / game / rel
            types.extend({**row, "game": game, "schema_ref": f"{game}/{rel}#L{row['line']}"} for row in _parse_types(path))
            files.append({"path": f"{game}/{rel}", "sha256": sha256(path.read_bytes())})
        for rel in manifest.get("tags", []):
            path = root / game / rel; object_type = Path(rel).stem
            tags.extend({**row, "object_type": object_type} for row in _parse_tags(path, game, rel))
            files.append({"path": f"{game}/{rel}", "sha256": sha256(path.read_bytes())})
        for rel in manifest.get("meta", []):
            path = root / game / rel
            metafiles.extend({**row, "game": game, "schema_ref": f"{game}/{rel}#L{row['line']}"} for row in _parse_metafiles(path))
            files.append({"path": f"{game}/{rel}", "sha256": sha256(path.read_bytes())})
        index_path = root / game / "_index.json"
        files.append({"path": f"{game}/_index.json", "sha256": sha256(index_path.read_bytes())})
    files.sort(key=lambda r: ascii_fold(r["path"])); types.sort(key=lambda r: (r["game"], ascii_fold(r["name"])))
    tags.sort(key=lambda r: (r["game"], ascii_fold(r["object_type"]), ascii_fold(r["tag"])))
    metafiles.sort(key=lambda r: (r["game"], r["path"], r["meta_file_type"] or ""))
    return {"files": files, "license": "MIT", "metafiles": metafiles, "revision": revision,
            "schema_version": 1, "tags": tags, "tool_version": TOOL_VERSION, "types": types}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--schema-root", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path); args = parser.parse_args(argv)
    try: write_json(args.out, build_schema_index(args.schema_root))
    except (OSError, SchemaError, ValueError) as exc: print(f"schema_index: {exc}", file=sys.stderr); return 2
    return 0


if __name__ == "__main__": raise SystemExit(main())

