#!/usr/bin/env python3
"""Run original P0-01 boundary fixtures without modifying the CTest suite."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


NEGATIVE = {
    "auto_deduction_from_call.cpp": "SIM_BOUNDARY_FLOATING_TYPE",
    "direct_close.cpp": "SIM_BOUNDARY_DIRECT_IO",
    "direct_create_file.cpp": "SIM_BOUNDARY_DIRECT_IO",
    "direct_fopen.cpp": "SIM_BOUNDARY_DIRECT_IO",
    "direct_remove.cpp": "SIM_BOUNDARY_DIRECT_IO",
    "direct_read_write.cpp": "SIM_BOUNDARY_DIRECT_IO",
    "floating_literal_suffixes.cpp": "SIM_BOUNDARY_FLOATING_LITERAL",
    "include_backslash_path.cpp": "SIM_BOUNDARY_FORBIDDEN_INCLUDE",
    "include_dot_path.cpp": "SIM_BOUNDARY_FORBIDDEN_INCLUDE",
    "long_double_alias.cpp": "SIM_BOUNDARY_FLOATING_TYPE",
    "template_float_alias.cpp": "SIM_BOUNDARY_FLOATING_TYPE",
    "template_float_instantiation.cpp": "SIM_BOUNDARY_FLOATING_TYPE",
}
POSITIVE = (
    "comment_words.cpp",
    "integer_identifier.cpp",
    "renderer_math.cpp",
    "template_integer_fixed.cpp",
)


def invoke(checker: Path, root: Path, clang: str, fixture: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(checker),
            "--root",
            str(root),
            "--clang",
            clang,
            "--files",
            str(fixture),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    checker = root / "tools" / "check_sim_boundary.py"
    fixture_root = root / "tests" / "acceptance" / "p0_01"
    failures = 0

    for name, expected in NEGATIVE.items():
        result = invoke(checker, root, args.clang, fixture_root / "negative" / name)
        if result.returncode == 0 or expected not in result.stdout:
            print(f"FAIL negative/{name}: expected {expected} rejection", file=sys.stderr)
            print(result.stdout, file=sys.stderr)
            failures += 1
        else:
            print(f"PASS negative/{name}: rejected with {expected}")

    for name in POSITIVE:
        result = invoke(checker, root, args.clang, fixture_root / "positive" / name)
        if result.returncode != 0:
            print(f"FAIL positive/{name}: expected acceptance", file=sys.stderr)
            print(result.stdout, file=sys.stderr)
            failures += 1
        else:
            print(f"PASS positive/{name}: accepted")

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
