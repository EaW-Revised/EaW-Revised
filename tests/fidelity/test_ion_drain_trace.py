#!/usr/bin/env python3
"""Run the S-51 ion drain through `sim_headless --scenario` (#561).

With EAWR_EAW_GAME_ROOT set, S-51 (an MC80 whose turbolaser hardpoints are destroyed by script
attacks a Tartan that holds fire) is staged on the FoC unit tables with 1, 2, 4 and 8 workers.
Every worker count must write the same trace, hashes and replay, and the trace must be a
complete scenario trace. The outcome must match the recorded FoC reference
(tests/fidelity/README.md, measured 2026-09-29):
- only the two forward ion cannons fire, and the first one that lands takes exactly 80 off the
  Tartan's shield: the hardpoint's own 40 (space-damage DG-25) at the corvette shield multiplier
  of 2, not the projectile's 20;
- the Tartan's hull never drops and it stays alive (EN-07, DP-06);
- once its shield is empty, the overflow drains its energy and the shield stays down: recorded,
  it never climbs past 20.25 again.
When the shield empties depends on how many bolts miss. FoC loses most of them at this range
(recorded: 140 bolts, about a dozen hits, empty at tick 713), and the remake's projectiles catch
more (space-damage G-D6, #536). So that tick is printed, not checked.
The original recordings stay private, so this test checks the outcome, not every field.
"""

from __future__ import annotations

import argparse
import csv
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Dict, List

SCENARIO = "S-51-mc80-ion-drains-tartan"
ONE = 1 << 24  # Q24
RECORDED_FIRST_HIT = 80 * ONE  # every landed ion bolt, in both recordings
RECORDED_EMPTY = 713  # the Tartan's first shield=0 tick
SHIELD_CEILING = 40 * ONE  # half a bolt; the recordings stay at or below 20.25 once empty
FAILURES: List[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def run(program: pathlib.Path, scenario: pathlib.Path, root: str, work: pathlib.Path, workers: int) -> Dict[str, bytes]:
    stem = f"S-51.w{workers}"
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


def series(path: pathlib.Path, unit: str, field: str) -> Dict[int, str]:
    rows: Dict[int, str] = {}
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            if row["object"] == unit and row["field"] == field:
                rows[int(row["tick"])] = row["value"]
    return rows


def check_drain(path: pathlib.Path) -> None:
    alive = series(path, "tartan", "alive")
    hull = series(path, "tartan", "hull")
    shield = {tick: int(value) for tick, value in series(path, "tartan", "shield").items()}
    ticks = sorted(shield)
    check(bool(ticks), "S-51: the trace has the Tartan's shield")
    if not ticks:
        return
    check(all(value == "1" for value in alive.values()), "S-51: the Tartan survives, as recorded (DP-06)")
    check(len(set(hull.values())) == 1, "S-51: the Tartan's hull never drops, as recorded (EN-07, DP-06)")
    drops = [(tick, shield[previous] - shield[tick]) for previous, tick in zip(ticks, ticks[1:]) if shield[tick] < shield[previous]]
    check(bool(drops), "S-51: an ion bolt reaches the Tartan's shield")
    if drops:
        tick, amount = drops[0]
        check(abs(amount - RECORDED_FIRST_HIT) <= 1024,
              f"S-51: the first hit at {tick} takes {amount / ONE:.3f} off the shield, recorded 80 (DG-25: 40 x2)")
    emptied = next((tick for tick in ticks if shield[tick] == 0), None)
    check(emptied is not None, "S-51: the Tartan's shield empties, as recorded")
    if emptied is not None:
        peak = max(shield[tick] for tick in ticks if tick >= emptied)
        check(peak <= SHIELD_CEILING,
              f"S-51: after it empties at {emptied} the shield climbs back to {peak / ONE:.3f}; recorded at most 20.25 (the drain)")
    print(f"S-51: first hit {drops[0][0] if drops else None} ({drops[0][1] / ONE if drops else 0:.3f}), "
          f"shield empty at {emptied} (recorded {RECORDED_EMPTY})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: ion drain scenario trace (set EAWR_EAW_GAME_ROOT)")
        return 0
    scenario = args.scenarios / f"{SCENARIO}.json"
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        runs = {workers: run(args.program, scenario, root, work, workers) for workers in (1, 2, 4, 8)}
        if all(runs.values()):
            for workers in (2, 4, 8):
                for kind in ("trace", "hashes", "replay", "header"):
                    check(runs[workers][kind] == runs[1][kind], f"S-51: {kind} differs with {workers} workers")
            trace_path = runs[1]["path"].decode("utf-8")
            compared = subprocess.run(
                [sys.executable, str(args.tool), trace_path, trace_path, "--scenario", str(scenario)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
            )
            check(compared.returncode == 0, f"S-51: not a complete scenario trace: {compared.stdout}")
            check_drain(pathlib.Path(trace_path))
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("ion drain scenario trace passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
