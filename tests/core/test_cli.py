#!/usr/bin/env python3
"""Verify that the P0-01 CLI reports replay unavailability as a real failure."""

from __future__ import annotations

import argparse
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    args = parser.parse_args()

    completed = subprocess.run(
        [args.program, "--replay", "synthetic/replay.eawr"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(completed.stdout, end="")
    if completed.returncode != 3:
        print(f"expected replay-unavailable exit code 3, got {completed.returncode}", file=sys.stderr)
        return 1
    expected = "EAWR-SIM-0001 [error] synthetic/replay.eawr"
    if expected not in completed.stdout or "replay execution is unavailable until P0-04" not in completed.stdout:
        print("expected replay-unavailable diagnostic was not emitted", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
