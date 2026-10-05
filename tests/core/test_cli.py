#!/usr/bin/env python3
"""Verify that the CLI reports a missing replay as a real I/O failure."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="eawr-missing-replay-") as temporary:
        missing = Path(temporary) / "missing.eawr-replay"
        output = Path(temporary) / "hashes.csv"
        completed = subprocess.run(
            [args.program, "--replay", str(missing), "--hash-out", str(output)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if output.exists():
            print("missing replay produced a hash output", file=sys.stderr)
            return 1
    print(completed.stdout, end="")
    if completed.returncode != 3:
        print(f"expected replay I/O exit code 3, got {completed.returncode}", file=sys.stderr)
        return 1
    if str(missing) not in completed.stdout or "could not open replay input" not in completed.stdout:
        print("expected missing-replay diagnostic was not emitted", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
