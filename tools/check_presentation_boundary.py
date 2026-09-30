#!/usr/bin/env python3
"""UI-07: presentation may consume snapshots/commands, never a live tactical session.

The command sink interface must expose command values, not TacticalSession. Its
implementation belongs outside presentation; no presentation-side exemption is needed.
Follow project-local includes so aliases and wrapper headers cannot bypass the gate.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re

ROOTS = ("src/presentation", "include/eawr/presentation", "src/data/ui", "include/eawr/data/ui",
         "apps/viewer/src", "apps/viewer/project")
SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".h", ".inc", ".gd"}
COMMENTS = re.compile(r'//[^\n]*|/\*.*?\*/', re.DOTALL)
INCLUDES = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
COMPUTED_INCLUDE = re.compile(r'^\s*#\s*include\s+[^<"\s]', re.MULTILINE)
SESSION = re.compile(r'\bTacticalSession\b')


def violations(root: Path, entries: list[Path]) -> list[str]:
    found = []
    visited = set()

    def visit(path: Path, chain: tuple[str, ...]) -> None:
        path = path.resolve()
        if path in visited:
            return
        visited.add(path)
        text = COMMENTS.sub("", path.read_text(encoding="utf-8"))
        label = path.relative_to(root).as_posix()
        if COMPUTED_INCLUDE.search(text):
            found.append(" -> ".join((*chain, label)) + ": UI-07 computed include cannot be audited")
        if SESSION.search(text):
            found.append(" -> ".join((*chain, label)) + ": UI-07 live TacticalSession access")
        for match in INCLUDES.finditer(text):
            name = match.group(1)
            for candidate in (path.parent / name, root / "include" / name, root / "src" / name):
                candidate = candidate.resolve()
                if candidate.is_relative_to(root) and candidate.is_file():
                    visit(candidate, (*chain, label))
                    break

    for entry in entries:
        visit(entry, ())
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    entries = [path for directory in ROOTS for path in (root / directory).rglob("*")
               if path.is_file() and path.suffix in SUFFIXES]
    missing = [directory for directory in ROOTS if not (root / directory).is_dir()]
    errors = [f"missing presentation boundary root: {directory}" for directory in missing]
    errors.extend(violations(root, entries))
    if errors:
        print("\n".join(errors))
        return 1
    print(f"UI-07 presentation boundary accepted ({len(entries)} entry files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
