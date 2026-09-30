#!/usr/bin/env python3
"""Fixtures for tools/check_lua_numeric_boundary.py."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

FLOATING = """
namespace eawr::script::numeric {
inline long long twice(long long value) { return static_cast<long long>(value * 2.0); }
}
"""

INTEGER_ONLY = """
#include <cstdint>
namespace eawr::script::numeric {
inline std::uint64_t twice(std::uint64_t value) { return value << 1U; }
}
"""

MATH_INCLUDE = """
#include <cmath>
namespace eawr::script::numeric {
inline int one() { return 1; }
}
"""


def run(root: Path, clang: str, source: str, directory: Path, name: str) -> subprocess.CompletedProcess[str]:
    path = directory / name
    path.write_text(source, encoding="utf-8")
    return subprocess.run(
        [sys.executable, str(root / "tools/check_lua_numeric_boundary.py"), "--root", str(root), "--clang", clang,
         "--jobs", "1", "--files", str(path)],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        floating = run(args.root, args.clang, FLOATING, directory, "floating.cpp")
        if floating.returncode != 1 or "LUA_NUMERIC_FLOATING_TYPE" not in floating.stderr or \
                "LUA_NUMERIC_FLOATING_LITERAL" not in floating.stderr:
            failures.append(f"floating fixture was not rejected:\n{floating.stdout}{floating.stderr}")
        clean = run(args.root, args.clang, INTEGER_ONLY, directory, "integer_only.cpp")
        if clean.returncode != 0:
            failures.append(f"integer-only fixture was rejected:\n{clean.stdout}{clean.stderr}")
        math = run(args.root, args.clang, MATH_INCLUDE, directory, "math_include.cpp")
        if math.returncode != 1 or "LUA_NUMERIC_MATH_INCLUDE" not in math.stderr:
            failures.append(f"math include fixture was not rejected:\n{math.stdout}{math.stderr}")
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
