#!/usr/bin/env python3
"""Run the S-52 Y-wing ion shot through `sim_headless --scenario` (#561).

With EAWR_EAW_GAME_ROOT set, S-52 is staged on the FoC unit tables with 1, 2, 4 and 8 workers.
In it, a Y-wing squadron attacks a Tartan that holds fire and cruises under a move order, and
fires its ION_CANNON_SHOT at the Tartan at tick 150.
Every worker count must write the same trace, hashes and replay, and the trace must be a
complete scenario trace. The outcome must match the recorded FoC reference
(tests/fidelity/README.md, measured 2026-09-29):
- the first thing to reach the Tartan's shield takes exactly 50 off it, within 10 % of the
  recorded tick 401;
- the Tartan keeps its 4.2 units per tick cruise on every tick from 100 to the end, stun or no
  stun: the stun never cuts a move already under way (space-damage IS-05);
- the Tartan survives.
The original recording stays private, so this test checks the outcome, not every field.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Dict, List

SCENARIO = "S-52-ywing-ion-shot-stuns-tartan"
ONE = 1 << 24  # Q24
RECORDED_FIRST_HIT = 50 * ONE
RECORDED_FIRST_TICK = 401
WINDOW = 0.1
CRUISE = 4.2  # units per tick; recorded on every tick from 100 to 1199
CRUISE_TOLERANCE = 0.01
FAILURES: List[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def run(program: pathlib.Path, scenario: pathlib.Path, root: str, work: pathlib.Path, workers: int) -> Dict[str, bytes]:
    stem = f"S-52.w{workers}"
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


def fields(path: pathlib.Path, unit: str) -> Dict[int, Dict[str, int]]:
    rows: Dict[int, Dict[str, int]] = {}
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            if row["object"] == unit:
                rows.setdefault(int(row["tick"]), {})[row["field"]] = int(row["value"])
    return rows


def check_shot(path: pathlib.Path) -> None:
    tartan = fields(path, "tartan")
    ticks = sorted(tartan)
    check(bool(ticks), "S-52: the trace has the Tartan")
    if not ticks:
        return
    check(all(row["alive"] == 1 for row in tartan.values()), "S-52: the Tartan survives, as recorded")
    drops = [(tick, tartan[previous]["shield"] - tartan[tick]["shield"])
             for previous, tick in zip(ticks, ticks[1:]) if tartan[tick]["shield"] < tartan[previous]["shield"]]
    check(bool(drops), "S-52: something reaches the Tartan's shield")
    if drops:
        tick, amount = drops[0]
        check(abs(amount - RECORDED_FIRST_HIT) <= 1024,
              f"S-52: the first hit at {tick} takes {amount / ONE:.3f} off the shield, recorded 50")
        check(abs(tick - RECORDED_FIRST_TICK) <= WINDOW * RECORDED_FIRST_TICK,
              f"S-52: the first hit lands at {tick}, recorded {RECORDED_FIRST_TICK}")
    slow = []
    for previous, tick in zip(ticks, ticks[1:]):
        if tick <= 100:
            continue
        step = math.hypot(tartan[tick]["pos.x"] - tartan[previous]["pos.x"],
                          tartan[tick]["pos.y"] - tartan[previous]["pos.y"]) / ONE
        if abs(step - CRUISE) > CRUISE_TOLERANCE:
            slow.append((tick, round(step, 3)))
    check(not slow, f"S-52: the Tartan leaves its recorded {CRUISE} cruise (IS-05) at {slow[:5]}")
    print(f"S-52: first hit {drops[0][0] if drops else None} ({drops[0][1] / ONE if drops else 0:.3f}; "
          f"recorded {RECORDED_FIRST_TICK}), hull at the end {tartan[ticks[-1]]['hull'] / ONE:.2f} (recorded 599.98)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: ion shot scenario trace (set EAWR_EAW_GAME_ROOT)")
        return 0
    scenario = args.scenarios / f"{SCENARIO}.json"
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        runs = {workers: run(args.program, scenario, root, work, workers) for workers in (1, 2, 4, 8)}
        if all(runs.values()):
            for workers in (2, 4, 8):
                for kind in ("trace", "hashes", "replay", "header"):
                    check(runs[workers][kind] == runs[1][kind], f"S-52: {kind} differs with {workers} workers")
            trace_path = runs[1]["path"].decode("utf-8")
            compared = subprocess.run(
                [sys.executable, str(args.tool), trace_path, trace_path, "--scenario", str(scenario)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
            )
            check(compared.returncode == 0, f"S-52: not a complete scenario trace: {compared.stdout}")
            check_shot(pathlib.Path(trace_path))
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("ion shot scenario trace passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
