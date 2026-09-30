#!/usr/bin/env python3
"""#628, game-data tier: the tag registry against the game data (docs/tag-coverage.md).

Runs sim_headless --skirmish m2 with and without --tag-trace-out (the start must list and census
the same either way), then tag_coverage.py scene on the trace against the checked-in registry, and
tag_registry.py check-data (the tag inventory the gate is built on covers every element and attribute
name of the effective FoC XML). Prints SKIPPED: without EAWR_EAW_GAME_ROOT.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(arguments, capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    game_root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not game_root:
        print("SKIPPED: set EAWR_EAW_GAME_ROOT to a FoC installation")
        return 0
    args.work.mkdir(parents=True, exist_ok=True)
    plain_census = args.work / "census-untraced.json"
    traced_census = args.work / "census-traced.json"
    trace = args.work / "m2-tag-trace.json"
    for path in (plain_census, traced_census, trace):
        path.unlink(missing_ok=True)
    plain = run(args.program, "--skirmish", "m2", "--game-root", game_root, "--census-out", str(plain_census))
    traced = run(args.program, "--skirmish", "m2", "--game-root", game_root, "--census-out", str(traced_census),
                 "--tag-trace-out", str(trace))
    for label, completed in (("untraced", plain), ("traced", traced)):
        if completed.returncode != 0:
            print(f"FAIL: sim_headless --skirmish m2 ({label}) exit {completed.returncode}: {completed.stderr}")
            return 1
    if plain.stdout != traced.stdout or plain_census.read_bytes() != traced_census.read_bytes():
        print("FAIL: the M2 start differs with the tag trace on")
        return 1
    report = run(sys.executable, str(ROOT / "tools" / "inventory" / "tag_coverage.py"), "scene",
                 "--game-root", game_root, "--trace", str(trace),
                 "--statuses", str(ROOT / "docs" / "tag-coverage" / "statuses.json"),
                 "--out", str(args.work / "tag-coverage"))
    sys.stdout.write(report.stdout)
    sys.stdout.write(report.stderr)
    if report.returncode != 0:
        print(f"FAIL: tag_coverage.py scene exit {report.returncode}")
        return 1
    data = run(sys.executable, str(ROOT / "tools" / "inventory" / "tag_registry.py"), "check-data",
               "--game-root", game_root)
    sys.stdout.write(data.stdout)
    sys.stdout.write(data.stderr)
    if data.returncode != 0:
        print(f"FAIL: tag_registry.py check-data exit {data.returncode}")
        return 1
    print("tag registry: the M2 scene agrees with the registry and the inventory covers the game data")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
