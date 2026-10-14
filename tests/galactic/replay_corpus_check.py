#!/usr/bin/env python3
"""Run every committed EAWRGCR fixture against pins on 1/2/4/8 workers."""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
from compare_replay_hashes import compare, compare_galactic_bundle, galactic_cases, read_rows


def check(program: str, corpus: pathlib.Path, output: pathlib.Path, target: str) -> None:
    directory = output / f"replay-evidence-{target}"
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "target.txt").write_bytes((target + "\n").encode())
    for name, replay, pins in galactic_cases(corpus):
        for workers in (1, 2, 4, 8):
            hashes = directory / f"{name}-workers-{workers}.hashes.csv"
            # No stale output can count as evidence when a runner fails to write its result.
            hashes.unlink(missing_ok=True)
            done = subprocess.run([program, "--galactic-replay", str(replay), "--workers", str(workers),
                                   "--hash-out", str(hashes)], capture_output=True, text=True, timeout=120)
            if done.returncode:
                raise ValueError(f"{name}/workers-{workers}: runner exit {done.returncode}: {done.stdout}{done.stderr}")
            # Fail early and retain the exact case, worker count and first divergent tick.
            compare(f"{name}/pins", pins,
                    f"{name}/{target}/workers-{workers}", read_rows(hashes))
    compare_galactic_bundle(output, corpus, (target,))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    parser.add_argument("--corpus", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, help="retain replay evidence for batch comparison")
    parser.add_argument("--target", default="local")
    args = parser.parse_args()
    if not args.target or any(character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for character in args.target):
        parser.error("target must be an alphanumeric name with optional hyphens or underscores")
    try:
        if args.out is not None:
            check(args.program, args.corpus, args.out, args.target)
        else:
            with tempfile.TemporaryDirectory() as directory:
                check(args.program, args.corpus, pathlib.Path(directory), args.target)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1
    print("galactic corpus pins and 1/2/4/8 worker parity passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
