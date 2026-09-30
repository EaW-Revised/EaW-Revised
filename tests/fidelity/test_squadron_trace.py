#!/usr/bin/env python3
"""Run the S-28 squadron attack through `sim_headless --scenario` (P2-12, #75).

With EAWR_EAW_GAME_ROOT set, S-28 (an Acclamator launching its squadrons at a Corellian
corvette) is staged on the FoC unit tables with 1, 2 and 4 workers. Every worker count must write
the same trace, hashes and replay; the trace must be a complete scenario trace; and it must show
what the three FoC recordings (tests/fidelity/README.md, measured 2026-09-27) show: the first
squadron within the first service interval, the second one launch delay (150 ticks) later, every
craft of a launch at the recorded bay point facing the recorded spawn vector, the corvette's
shield gone before it dies and its death within 20 % of the recorded tick 883. The recordings stay
private, so this test checks those outcomes, not every field; docs/behaviour/space-fighters.md
explains the remaining differences.
"""

from __future__ import annotations

import argparse
import collections
import csv
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Dict, List, Optional, Tuple

SCENARIO = "S-28-squadron-vs-capital"
RECORDED_DEATH = 883  # the corvette's first alive=0 tick in all three recordings
WINDOW = 0.2
DELAY = 150  # Acclamator Spawned_Squadron_Delay_Seconds 5 x 30 (FL-05)
# The recorded launch point and facing, Q24 raw (all three recordings, every launch).
LAUNCH_POINT = (-8129689600, -25165824000, -1992580864)
LAUNCH_FORWARD = (11992978, 0, -11732155)
POINT_TOLERANCE = 1024
# The remake's Q24 atan and sine land within 100 raw (6e-6) of the recorded binary32 facing.
FORWARD_TOLERANCE = 128
FAILURES: List[str] = []

Rows = Dict[str, Dict[Tuple[int, str], int]]


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def run(program: pathlib.Path, scenario: pathlib.Path, root: str, work: pathlib.Path, workers: int) -> Dict[str, bytes]:
    stem = f"S-28.w{workers}"
    outputs = {"trace": work / f"{stem}.csv", "hashes": work / f"{stem}.hashes.csv", "replay": work / f"{stem}.eawr-replay"}
    completed = subprocess.run(
        [str(program), "--scenario", str(scenario), "--game-root", root, "--workers", str(workers),
         "--trace-out", str(outputs["trace"]), "--hash-out", str(outputs["hashes"]),
         "--replay-out", str(outputs["replay"])],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
    )
    check(completed.returncode == 0, f"{stem}: sim_headless failed: {completed.stdout}{completed.stderr}")
    if completed.returncode != 0:
        return {}
    result = {name: path.read_bytes() for name, path in outputs.items()}
    result["header"] = outputs["trace"].with_suffix(".json").read_bytes()
    result["path"] = str(outputs["trace"]).encode("utf-8")
    return result


def load(path: pathlib.Path) -> Rows:
    rows: Rows = collections.defaultdict(dict)
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            rows[row["object"]][(int(row["tick"]), row["field"])] = int(row["value"])
    return rows


def first_tick(fields: Dict[Tuple[int, str], int], field: str, predicate) -> Optional[int]:
    ticks = sorted(tick for (tick, name), value in fields.items() if name == field and predicate(value))
    return ticks[0] if ticks else None


def check_squadrons(path: pathlib.Path) -> Tuple[List[int], Optional[int]]:
    rows = load(path)
    launches: Dict[int, List[str]] = collections.defaultdict(list)
    for name, fields in rows.items():
        if name in ("carrier", "corvette"):
            continue
        tick = first_tick(fields, "alive", lambda value: value == 1)
        if tick is not None:
            launches[tick].append(name)
    ticks = sorted(launches)
    check(len(ticks) >= 2, f"S-28: two launches, got {ticks}")
    if len(ticks) >= 2:
        check(ticks[0] < 30, f"S-28: the first launch at the first service (FL-01), got {ticks[0]}")
        check(ticks[1] - ticks[0] == DELAY, f"S-28: the second launch one delay later (FL-05), got {ticks[1] - ticks[0]}")
    for tick in ticks:
        for name in launches[tick]:
            fields = rows[name]
            point = tuple(fields[(tick, f"pos.{axis}")] for axis in "xyz")
            forward = tuple(fields[(tick, f"fwd.{axis}")] for axis in "xyz")
            check(all(abs(a - b) <= POINT_TOLERANCE for a, b in zip(point, LAUNCH_POINT)),
                  f"S-28: {name} starts at the recorded bay point (FL-06), got {point}")
            check(all(abs(a - b) <= FORWARD_TOLERANCE for a, b in zip(forward, LAUNCH_FORWARD)),
                  f"S-28: {name} faces the recorded spawn vector (FL-06), got {forward}")
    corvette = rows["corvette"]
    death = first_tick(corvette, "alive", lambda value: value == 0)
    emptied = first_tick(corvette, "shield", lambda value: value == 0)
    check(death is not None, "S-28: the squadrons destroy the corvette")
    if death is not None:
        low, high = RECORDED_DEATH * (1 - WINDOW), RECORDED_DEATH * (1 + WINDOW)
        check(low <= death <= high, f"S-28: the corvette dies at {death}, recorded {RECORDED_DEATH} (window {low:.0f}..{high:.0f})")
    check(emptied is not None and death is not None and emptied < death, "S-28: the corvette's shield falls before it dies")
    check(all(value == 1 for (tick, field), value in rows["carrier"].items() if field == "alive"), "S-28: the carrier survives")
    return ticks, death


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: squadron scenario trace (set EAWR_EAW_GAME_ROOT)")
        return 0
    scenario = args.scenarios / f"{SCENARIO}.json"
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        runs = {workers: run(args.program, scenario, root, work, workers) for workers in (1, 2, 4)}
        if all(runs.values()):
            for workers in (2, 4):
                for kind in ("trace", "hashes", "replay", "header"):
                    check(runs[workers][kind] == runs[1][kind], f"S-28: {kind} differs with {workers} workers")
            trace_path = runs[1]["path"].decode("utf-8")
            compared = subprocess.run(
                [sys.executable, str(args.tool), trace_path, trace_path, "--scenario", str(scenario)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
            )
            check(compared.returncode == 0, f"S-28: not a complete scenario trace: {compared.stdout}")
            ticks, death = check_squadrons(pathlib.Path(trace_path))
            print(f"S-28: launches at {ticks}, corvette destroyed at {death} (recorded {RECORDED_DEATH})")
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("squadron scenario trace passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
