#!/usr/bin/env python3
"""Prove that an intentionally injected project warning fails a preset build."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--preset", required=True)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()

    root = args.root.resolve()
    if re.fullmatch(r"[A-Za-z0-9_-]+", args.preset) is None:
        print("preset name contains unsupported path characters", file=sys.stderr)
        return 1
    binary = root / "out" / "warning-probe" / args.preset
    if binary.exists():
        shutil.rmtree(binary)

    configure = subprocess.run(
        [
            "cmake",
            "--preset",
            args.preset,
            "-B",
            str(binary),
            "-DEAWR_INJECT_PROJECT_WARNING=ON",
            "-DBUILD_TESTING=OFF",
        ],
        cwd=root,
        text=True,
    )
    if configure.returncode != 0:
        print("warning probe configuration failed before compilation", file=sys.stderr)
        return 1

    build = subprocess.run(
        ["cmake", "--build", str(binary), "--config", args.config, "--target", "eawr_core"],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(build.stdout, end="")
    if build.returncode == 0:
        print("deliberate project warning unexpectedly compiled successfully", file=sys.stderr)
        return 1
    if "EAWR deliberate project warning probe" not in build.stdout:
        print("build failed, but the deliberate warning was not observed", file=sys.stderr)
        return 1
    print("warnings-as-errors probe passed: the deliberate project warning failed the build")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
