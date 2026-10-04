#!/usr/bin/env python3
"""Seeds the tag registry's `applied` evidence from the sources (docs/tag-coverage.md).

For each XML tag name a loader mentions as a string literal, finds where the loader stores the value (the field it
assigns) and where else the code uses that field. A use outside the loader that is not the identity hash, a test or
the load trace counts as the tag being applied: the value reaches the simulation or the presentation. The scan is
a text scan, so it is a seed for a reviewed registry, not proof: `tag_registry.py` marks what it produces `auto`,
and the perturbation check (tools/soak, out/specs/tag-applied-check) is the mechanical proof.

    python tools/inventory/tag_apply_scan.py --out out/tag-coverage/apply-scan.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

SOURCE_ROOTS = ("src", "include", "apps/viewer/src")
SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".inl")
# Files that read a field without applying it: the identity hash, the load trace, census and report writers.
NOT_APPLYING = re.compile(r"(identity|tag_trace|census|_report|snapshot_encode|encoding)\.", re.IGNORECASE)
# Readers that compare upper-cased tag names.
UPPER_FILES = ("src/presentation/audio/sfx.cpp",)
# Field names too common to follow by name alone.
GENERIC = {"name", "id", "type", "text", "value", "count", "size", "range", "color", "colour", "path", "mode",
           "kind", "data", "model", "index", "flags", "list", "items", "entries", "time", "speed", "scale", "rate",
           "min", "max", "offset", "position", "radius", "height", "width", "angle", "duration", "delay", "state"}
LITERAL = re.compile(r'"([A-Za-z][A-Za-z0-9_]{2,})"')
ASSIGN = re.compile(r"((?:[A-Za-z_]\w*(?:\.|->))*[A-Za-z_]\w*)\s*=(?!=)")
MEMBER = re.compile(r"(?:\.|->)([A-Za-z_]\w*)\s*$")
LOCAL_DECL = re.compile(r"\b(?:const\s+)?(?:auto|Fixed|int|bool|float|double|std::\w+(?:<[^>]*>)?)&?\s+([A-Za-z_]\w*)\s*=")
IF_INIT = re.compile(r"\(\s*(?:const\s+)?auto\s*&?\s*([A-Za-z_]\w*)\s*=")
ARG_SINK = re.compile(r'"[^"]+"\s*,\s*(&(?:[A-Za-z_]\w*(?:::|\.|->))*[A-Za-z_]\w*|(?:[A-Za-z_]\w*(?:\.|->))+[A-Za-z_]\w*)\s*[,)}]')
LOCAL_ARG = re.compile(r'"[^"]+"\s*,\s*([A-Za-z_]\w*)\s*[,)]')
CALL_SINK = re.compile(r'\)\s*\w+\(\s*&?((?:[A-Za-z_]\w*(?:\.|->))+[A-Za-z_]\w*)\s*[,)]')


@dataclass
class ParseSite:
    file: str
    line: int
    sink: str | None
    text: str


@dataclass
class Scan:
    sites: dict[str, list[ParseSite]] = field(default_factory=lambda: defaultdict(list))
    uses: dict[str, list[tuple[str, int]]] = field(default_factory=lambda: defaultdict(list))


def source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for base in SOURCE_ROOTS:
        directory = root / base
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                relative = path.relative_to(root).as_posix()
                if "/bin/" in relative and "project" in relative:
                    continue
                files.append(path)
    return files


def last_member(expression: str) -> str:
    return re.split(r"\.|->|::", expression)[-1]


def sink_of(lines: list[str], index: int, tag: str) -> str | None:
    """The field (or local) the value of the tag literal on lines[index] is stored into."""
    line = lines[index]
    position = line.find(f'"{tag}"')
    before = line[:position]
    after = line[position:]
    # `member = read("Tag", ...)`, `x.y = ... "Tag"`
    assigned = [m for m in ASSIGN.finditer(before)]
    if assigned:
        return assigned[-1].group(1)
    if index > 0:  # a wrapped statement: the assignment ends the previous line
        previous = lines[index - 1].rstrip()
        match = re.search(r"((?:[A-Za-z_]\w*(?:\.|->))*[A-Za-z_]\w*)\s*=\s*$", previous)
        if match:
            return match.group(1)
    # `set("Tag", goal.time_limit)`, `scalar("Tag", looks.height, false)`
    arg = ARG_SINK.match(after)
    if arg:
        return arg.group(1).lstrip("&")
    call = CALL_SINK.search(after)
    if call:
        return call.group(1)
    local_arg = LOCAL_ARG.match(after)
    if local_arg and local_arg.group(1) not in ("true", "false", "nullptr"):
        return local_arg.group(1)
    # `if (const auto v = text("Tag")) looks.x = f(*v);`
    init = IF_INIT.search(before)
    if init:
        local = init.group(1)
        tail = after
        for follow in lines[index:index + 3]:
            tail += " " + follow
        for match in ASSIGN.finditer(tail):
            if re.search(rf"\b{re.escape(local)}\b", tail[match.end():match.end() + 120]):
                return match.group(1)
        return local
    return None


def resolve_local(lines: list[str], index: int, local: str) -> str | None:
    """A plain local's field: the next assignment of the local to a member within a few lines."""
    pattern = re.compile(rf"((?:[A-Za-z_]\w*(?:\.|->))+[A-Za-z_]\w*)\s*=\s*[^;=]*\b{re.escape(local)}\b")
    for follow in lines[index + 1:index + 16]:
        match = pattern.search(follow)
        if match:
            return match.group(1)
    return None


def scan_sources(root: Path, tags: set[str]) -> Scan:
    """Parse sites for the tag names (exact spellings the inventory has; FoC ignores case, the loaders do not need to), and every member-access use of a name in the sources."""
    result = Scan()
    upper_spelling = {name.upper(): name for name in tags}
    files = source_files(root)
    texts = {path.relative_to(root).as_posix(): path.read_text(encoding="utf-8", errors="replace").splitlines()
             for path in files}
    for relative, lines in texts.items():
        for index, line in enumerate(lines):
            if line.lstrip().startswith("//"):
                continue
            for match in LITERAL.finditer(line):
                literal = match.group(1)
                tag = literal
                if tag not in tags:
                    # Files that match upper-cased keys (the SFX event reader) name a tag in capitals.
                    spelled = upper_spelling.get(literal) if relative in UPPER_FILES else None
                    if spelled is None:
                        continue
                    tag = spelled
                sink = sink_of(lines, index, literal)
                if sink and "." not in sink and "->" not in sink and "::" not in sink:
                    sink = resolve_local(lines, index, sink) or sink
                result.sites[tag.lower()].append(ParseSite(relative, index + 1, sink, line.strip()[:160]))
    fields = {last_member(site.sink).lower() for sites in result.sites.values() for site in sites if site.sink}
    member_use = re.compile(r"(?:\.|->)\s*([A-Za-z_]\w*)")
    plain_use = re.compile(r"\b([A-Za-z_]\w*)\b")
    for relative, lines in texts.items():
        for index, line in enumerate(lines):
            code = line.split("//")[0]
            for match in member_use.finditer(code):
                name = match.group(1).lower()
                if name in fields:
                    result.uses[name].append((relative, index + 1))
            # designated initialisers and same-scope uses of a member name inside its own class
            if code.lstrip().startswith("."):
                for match in plain_use.finditer(code):
                    name = match.group(1).lower()
                    if name in fields:
                        result.uses[name].append((relative, index + 1))
    return result


def applying(uses: Iterable[tuple[str, int]], parse_files: set[str],
             parse_lines: frozenset[tuple[str, int]] = frozenset()) -> list[tuple[str, int]]:
    """The uses that apply the value: outside the identity hash and friends, tests and the parse sites' own files
    (a loader that copies the value into a table is not applying it)."""
    found = []
    for relative, line in uses:
        if (relative, line) in parse_lines:
            continue  # the assignment that stores the value is not a use of it
        if NOT_APPLYING.search(Path(relative).name + "."):
            continue
        if "/tests/" in relative or relative.startswith("tests/"):
            continue
        same_unit_loader = relative.startswith("src/units/unit_tables") and any(
            path.startswith("src/units/unit_tables") for path in parse_files)
        if (relative in parse_files or same_unit_loader) and relative.startswith(("src/units/", "src/data/", "src/script/foc/ai_data")):
            continue
        found.append((relative, line))
    return found


def local_uses(lines: list[str], index: int, local: str, window: int = 90) -> int:
    """How many later lines of the same function read the local the tag's value went into."""
    pattern = re.compile(rf"\b{re.escape(local)}\b")
    count = 0
    for follow in lines[index + 1:index + 1 + window]:
        if follow.startswith("}"):
            break  # the function's end
        count += len(pattern.findall(follow.split("//")[0]))
    return count


def analyse(root: Path, names: set[str]) -> dict[str, dict]:
    """Per lower-cased tag name: its parse sites and the files that apply the field they fill."""
    scan = scan_sources(root, names)
    texts = {path.relative_to(root).as_posix(): path.read_text(encoding="utf-8", errors="replace").splitlines()
             for path in source_files(root)}
    out: dict[str, dict] = {}
    for tag, sites in scan.sites.items():
        parse_files = {site.file for site in sites}
        parse_lines = frozenset((site.file, site.line + offset) for site in sites for offset in (0, 1))
        entries = []
        for site in sites:
            entry = {"file": site.file, "line": site.line, "sink": site.sink, "text": site.text}
            if site.sink and not any(mark in site.sink for mark in (".", "->", "::")):
                entry["local"] = True
                entry["local_uses"] = local_uses(texts[site.file], site.line - 1, site.sink)
            if site.sink:
                field_name = last_member(site.sink).lower()
                uses = applying(scan.uses.get(field_name, []), parse_files, parse_lines)
                entry["field"] = last_member(site.sink)
                entry["generic"] = field_name in GENERIC
                entry["uses"] = sorted({f"{f}" for f, _ in uses})
                entry["use_lines"] = len(uses)
            entries.append(entry)
        out[tag] = {"sites": entries}
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--inventory", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    inventory = args.inventory or args.repo / "plan" / "inventories" / "xml-tags.json"
    rows = json.loads(inventory.read_text(encoding="utf-8"))["rows"]
    names = {row["tag_name"] for row in rows if row["profile"] == "foc"}
    result = analyse(args.repo, names)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=1, sort_keys=True), encoding="utf-8")
    print(f"tag_apply_scan: {len(result)} tags with a parse site")
    return 0


if __name__ == "__main__":
    sys.exit(main())
