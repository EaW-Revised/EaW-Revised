#!/usr/bin/env python3
"""Clean-room check (#825): no original FoC class, method or field names in tracked files.

Committed comments and docs cite behaviour-note rule IDs and "the debug build", never the
engine's own names. This applies the pre-push grep of out/specs/common.md to whole tracked
files. Our own identifiers and third-party names are allow-listed below; a new hit is either
reworded (preferred) or, when it is our own or a public API name, added to ALLOWED_TOKENS.
Usage: python tools/cleanroom_check.py [--list]. Exit 1 on any unallowed hit.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PATTERN = re.compile(
    r"[A-Z][A-Za-z]+(?:Class|Wrapper)|[A-Za-z]+Class::[A-Za-z_]+|\bal[A-Z][A-Za-z]+(?:::|\b)"
    r"|[A-Za-z]+::[A-Z][a-z]+_[A-Z]|\bm_[a-z]+_[a-z]+|MOD_TYPE_|0x14[0-9A-Fa-f]{7}"
)

# Vendored code and this tool's own pattern. The inventories are scanned too (#867 review F1).
SKIP_PREFIXES = ("third_party/", "tools/cleanroom_check.py")
# Research tooling takes the engine names it matches at run time (#834), so no file is skipped.
SKIP_FILES: set[str] = set()
# Our own, standard-library, OS-API or public names. An engine-shaped name is allowed only when the
# game's own mod-facing data spells it (#867 review F2): the FoC Lua scripts or XML.
ALLOWED_TOKENS = {
    # Ours (tests, the rig's PowerShell), Godot's, Python's and the Windows API's.
    "InputClass", "PixelClass", "WeightClass", "GraphicalWrapper", "GDExtensionClassLibraryPtr",
    "setUpClass", "tearDownClass", "PriorityClass", "GetPriorityClass", "SetPriorityClass",
    "SchedulingClass", "RegisterClassW", "pcPriClassBase", "PriClass",
    # Shader asset names the game's models reference (alDefault.fx, alMissingShader).
    "alDefault", "alMissingShader",
    # Lua receiver types the game's own scripts spell (GameScoring.lua, GalacticFreeStore.lua).
    "GameObjectWrapper", "GameObjectTypeWrapper", "PlayerWrapper",
    # XML tag and value names of the FoC data (docs/tag-coverage/statuses.json).
    "MovementClass", "MovementClassType", "UnitCollisionClass",
}
# Lua API names the game's scripts call (plan/inventories/lua-api.json, built from those scripts),
# and asset file names the data refers to (an icon .tga, an effect .fx).
LUA_API = ROOT / "plan" / "inventories" / "lua-api.json"
ASSET_NAME = re.compile(r"^[\w.-]*\.(?:fx|fxh|fxo|tga|dds|alo|ala|xml|lua|meg|mtd|wav|mp3|bik|ted)\b", re.I)


def lua_api_names() -> set[str]:
    if not LUA_API.is_file():
        return set()
    rows = json.loads(LUA_API.read_text(encoding="utf-8"))["rows"]
    return {part for row in rows for part in row["symbol"].split(".")}
# Whole lines to skip (none today; a negative assertion that names an engine term would go here).
ALLOWED_LINES: tuple[str, ...] = ()


def tracked_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def line_tokens(line: str, allowed: set[str] | frozenset[str] = frozenset()) -> list[tuple[str, int, int]]:
    """Each engine-style word of the line that is not allowed, with its span (tools/oss uses it too)."""
    if any(marker in line for marker in ALLOWED_LINES):
        return []
    found = []
    for match in PATTERN.finditer(line):
        start, end = match.span()
        while start > 0 and (line[start - 1].isalnum() or line[start - 1] == "_"):
            start -= 1
        while end < len(line) and (line[end].isalnum() or line[end] == "_"):
            end += 1
        token = line[start:end]
        head = token.split("::")[0]
        if token in ALLOWED_TOKENS or head in ALLOWED_TOKENS or token in allowed or head in allowed:
            continue
        if token.startswith("Process") and token.endswith("PriorityClass"):
            continue
        if ASSET_NAME.match(line[end:]) or (start > 0 and line[start - 1] in "-/"):
            continue  # part of an asset file name the data refers to
        found.append((token, start, end))
    return found


def hits(rel: str, allowed: set[str] | frozenset[str] = frozenset()) -> list[tuple[int, str, str]]:
    if rel.startswith(SKIP_PREFIXES) or rel in SKIP_FILES:
        return []
    try:
        text = (ROOT / rel).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return []
    return [(number, token, line.strip()[:120])
            for number, line in enumerate(text.splitlines(), 1) for token, _, _ in line_tokens(line, allowed)]


def scan() -> list[tuple[str, int, str, str]]:
    allowed = frozenset(lua_api_names())
    return [(rel, n, tok, line) for rel in tracked_files() for n, tok, line in hits(rel, allowed)]


def main(argv: list[str]) -> int:
    found = scan()
    for rel, number, token, line in found:
        print(f"{rel}:{number}: {token}: {line}")
    if found:
        print(f"cleanroom_check: {len(found)} hit(s); reword to a rule ID and 'the debug build'.")
        return 1
    print("cleanroom_check: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
