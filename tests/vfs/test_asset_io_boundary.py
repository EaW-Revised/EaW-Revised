#!/usr/bin/env python3
"""Keep runtime game-asset native I/O inside VFS/platform adapters."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


FORBIDDEN = re.compile(
    r"#\s*include\s*[<\"]filesystem[>\"]|"
    r"#\s*include\s*[<\"]fstream[>\"]|"
    r"\b(?:fopen|CreateFile[AW]?|ReadFile|std::ifstream|std::fstream|std::filesystem)\b"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    arguments = parser.parse_args()
    roots = [arguments.root / "src", arguments.root / "include" / "eawr"]
    violations: list[str] = []
    for source_root in roots:
        for path in source_root.rglob("*"):
            if path.suffix.lower() not in {".cpp", ".cc", ".cxx", ".hpp", ".h"}:
                continue
            relative = path.relative_to(arguments.root).as_posix()
            if relative.startswith(("src/vfs/", "src/platform/", "include/eawr/vfs/")):
                continue
            for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if FORBIDDEN.search(line):
                    violations.append(f"{relative}:{line_number}: native asset I/O surface: {line.strip()}")
    if violations:
        print("\n".join(violations), file=sys.stderr)
        return 1
    print("asset I/O boundary passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
