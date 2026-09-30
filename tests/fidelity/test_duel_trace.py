#!/usr/bin/env python3
"""Run the S-15 duel through `sim_headless --scenario` (P2-11, #74).

With EAWR_EAW_GAME_ROOT set, S-15 (Nebulon-B against Tartan) is staged on the FoC unit tables
with 1, 2 and 4 workers. Every worker count must write the same trace, hashes and replay; the
trace must be a complete scenario trace; and the duel must end like the recorded FoC reference
(tests/fidelity/README.md, measured 2026-09-26): the Nebulon-B wins, and the Tartan's shield
empties and the Tartan dies within 15 % of the recorded ticks 698 and 805 (the remake: 615 and
718, 11 % early). docs/behaviour/space-damage.md G-D6 explains the remaining gap: FoC loses
about one shot in seven in flight, and the remake's collision box catches them. The original
recordings stay private, so this test checks the outcome, not every field.
"""

from __future__ import annotations

import argparse
import csv
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Dict, List, Optional, Tuple

SCENARIO = "S-15-duel-damage"
RECORDED_DEATH = 805  # the Tartan's alive=0 tick in all three P2-06 recordings
RECORDED_EMPTY = 698  # the Tartan's first shield=0 tick in all three
WINDOW = 0.15
FAILURES: List[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def run(program: pathlib.Path, scenario: pathlib.Path, root: str, work: pathlib.Path, workers: int) -> Dict[str, bytes]:
    stem = f"S-15.w{workers}"
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


def first(rows: Dict[int, str], predicate) -> Optional[int]:
    return next((tick for tick in sorted(rows) if predicate(rows[tick])), None)


def check_duel(path: pathlib.Path) -> Tuple[Optional[int], Optional[int]]:
    frigate_alive = series(path, "frigate", "alive")
    tartan_alive = series(path, "tartan", "alive")
    tartan_shield = series(path, "tartan", "shield")
    frigate_hull = series(path, "frigate", "hull")
    death = first(tartan_alive, lambda value: value == "0")
    emptied = first(tartan_shield, lambda value: int(value) == 0)
    check(all(value == "1" for value in frigate_alive.values()), "S-15: the Nebulon-B survives, as recorded")
    check(death is not None, "S-15: the Tartan dies")
    if death is not None:
        low, high = RECORDED_DEATH * (1 - WINDOW), RECORDED_DEATH * (1 + WINDOW)
        check(low <= death <= high, f"S-15: the Tartan dies at {death}, recorded {RECORDED_DEATH} (window {low:.0f}..{high:.0f})")
    check(emptied is not None and death is not None and emptied < death, "S-15: the Tartan's shield empties before it dies")
    if emptied is not None:
        low, high = RECORDED_EMPTY * (1 - WINDOW), RECORDED_EMPTY * (1 + WINDOW)
        check(low <= emptied <= high,
              f"S-15: the Tartan's shield empties at {emptied}, recorded {RECORDED_EMPTY} (window {low:.0f}..{high:.0f})")
    check(len(set(frigate_hull.values())) == 1, "S-15: the Nebulon-B's shield keeps its hull whole, as recorded")
    return death, emptied


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: duel scenario trace (set EAWR_EAW_GAME_ROOT)")
        return 0
    scenario = args.scenarios / f"{SCENARIO}.json"
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        runs = {workers: run(args.program, scenario, root, work, workers) for workers in (1, 2, 4)}
        if all(runs.values()):
            for workers in (2, 4):
                for kind in ("trace", "hashes", "replay", "header"):
                    check(runs[workers][kind] == runs[1][kind], f"S-15: {kind} differs with {workers} workers")
            trace_path = runs[1]["path"].decode("utf-8")
            compared = subprocess.run(
                [sys.executable, str(args.tool), trace_path, trace_path, "--scenario", str(scenario)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
            )
            check(compared.returncode == 0, f"S-15: not a complete scenario trace: {compared.stdout}")
            death, emptied = check_duel(pathlib.Path(trace_path))
            print(f"S-15: Tartan shield empty at {emptied} (recorded {RECORDED_EMPTY}), "
                  f"destroyed at {death} (recorded {RECORDED_DEATH})")
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("duel scenario trace passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
