#!/usr/bin/env python3
"""Build the names-only FoC Lua index from GhidrAssist registration output.

In Ghidra, open the FoC debug `StarWarsI.exe` and run
`tools/ghidra/FocLuaRegistrationDump.java` with an ignored TSV output path.
The Java script follows callers of the Lua member and global registration
functions, decompiles those callers, and records
literal names with the registering receiver class, including `LUA_PATH` and
`Script` in the Lua state initialiser. No binary addresses
or decompiler output are copied into this index.

The committed index names each receiver neutrally (`taskforce`, `story_event`, ...): only a
receiver the game's own Lua scripts spell keeps its name. The engine-to-neutral table is
an ignored research file (tab-separated `engine<TAB>neutral` lines, `#` comments), so no
committed file carries the engine's class names (clean-room rule, #834).

Example:
  python tools/generate_foc_lua_index.py out/research/foc-lua-registration.tsv \
      --receiver-names out/research/lua-receiver-names.tsv
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GLYPHX = ROOT / "plan/inventories/lua-declarations.json"
OUTPUT = ROOT / "plan/inventories/foc-lua-registrations.json"


WIDE_STRING_RECEIVER = "wide_string"


def receiver_names(path: Path) -> dict[str, str]:
    table = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 2 or not all(part.isidentifier() for part in parts):
            raise ValueError(f"invalid receiver name row {number}")
        table[parts[0]] = parts[1]
    return table


def generate(tsv: Path, names_table: dict[str, str], glyphx: Path = GLYPHX, output: Path = OUTPUT) -> dict:
    entries = []
    seen = set()
    for number, line in enumerate(tsv.read_text(encoding="utf-8").splitlines(), 1):
        parts = line.split("\t")
        if len(parts) != 3 or parts[0] not in {"global", "method"} or not parts[2]:
            raise ValueError(f"invalid registration row {number}")
        kind, receiver, name = parts
        if (kind == "global") != (receiver == ""):
            raise ValueError(f"invalid receiver in row {number}")
        if not name.isidentifier() or (receiver and not receiver.isidentifier()):
            raise ValueError(f"invalid visible name or receiver in row {number}")
        if receiver:
            if receiver not in names_table:
                raise ValueError(f"receiver in row {number} has no neutral name in the receiver table")
            receiver = names_table[receiver]
        key = (kind, receiver, name)
        if key in seen:
            raise ValueError(f"duplicate registration in row {number}")
        seen.add(key)
        entries.append({"exposed_name": name, "call_kind": kind,
                        "receiver_type": receiver or None})

    reference = json.loads(glyphx.read_text(encoding="utf-8"))["entries"]
    def names(rows: list[dict], kind: str) -> set[str]:
        return {row["exposed_name"] for row in rows if row["call_kind"] == kind}

    glyphx_globals, glyphx_methods = names(reference, "global"), names(reference, "method")
    foc_globals, foc_methods = names(entries, "global"), names(entries, "method")
    absent_methods = glyphx_methods - foc_methods
    wide_methods = {row["exposed_name"] for row in reference
                    if row["receiver_type"] == WIDE_STRING_RECEIVER}
    if (len(foc_globals - glyphx_globals), len(foc_methods - glyphx_methods),
            len(absent_methods)) != (20, 61, 22):
        raise ValueError("FoC/GlyphX visible-name differences do not match audit AU-117")
    if glyphx_globals - foc_globals or absent_methods != wide_methods:
        raise ValueError("unexpected missing GlyphX global or non-wide-string member")

    entries.sort(key=lambda row: (row["call_kind"], row["receiver_type"] or "",
                                  row["exposed_name"].casefold(), row["exposed_name"]))
    data = {
        "schema_version": 1,
        "source": "FoC debug build StarWarsI.exe",
        "extraction": "tools/ghidra/FocLuaRegistrationDump.java",
        "limitations": [
            "A name is visible on its receiver; signature, behavior, and mode availability are not established.",
            "Dynamically computed global names are not represented."
        ],
        "entries": entries,
    }
    output.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return {"entries": len(entries), "globals_added": 20,
            "members_added": 61, "wide_string_methods_absent": 22}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tsv", type=Path)
    parser.add_argument("--receiver-names", type=Path, required=True,
                        help="ignored engine-to-neutral receiver table (see the module docstring)")
    parser.add_argument("--glyphx", type=Path, default=GLYPHX)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    print(json.dumps(generate(args.tsv, receiver_names(args.receiver_names), args.glyphx, args.output),
                     sort_keys=True))


if __name__ == "__main__":
    main()
