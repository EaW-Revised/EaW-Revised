#!/usr/bin/env python3
"""Run the order cases S-29 to S-31 through `sim_headless --scenario` (#452).

With EAWR_EAW_GAME_ROOT set, each case is staged on the FoC unit tables with 1, 2 and 4 workers,
which must write the same trace, hashes and replay, and each trace must be a complete scenario
trace (docs/traces.md). The checks are the rules of docs/behaviour/space-orders.md; no original
recording exists yet (OR-U4), so these scenarios are also the recording specs:

- S-29: two Tartans ordered to attack a corvette 3000 units away close on it (OR-05), stop between
  0.9 and 1.0 of their 800-unit attack distance from it, and kill it; each flies the path planned
  from the order without re-planning it, so its heading reverses at most twice (C-07, #662).
- S-30: a Tartan guarding an Acclamator holds while it is within 750 units (OR-14), starts to
  follow within one check interval (10 frames) of it drifting farther, and stops 675 to 750
  units from it.
- S-31: a Tartan attack-moving past a corvette fires at it while it moves (OR-11), never stops on
  the way and ends on its point.
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
from collections import defaultdict
from typing import Dict, List, Tuple

ONE = 1 << 24
FAILURES: List[str] = []
CASES = {
    "S-29": "S-29-tartan-attack-out-of-range",
    "S-30": "S-30-tartan-guard-acclamator",
    "S-31": "S-31-tartan-attack-move-past-corvette",
}
ATTACK_DISTANCE = 800.0  # Tartan_Patrol_Cruiser's last authored Targeting_Max_Attack_Distance
GUARD_RANGE = 750.0      # gameconstants.xml Space_Guard_Range
CHECK_FRAMES = 10        # gameconstants.xml MovementReevaluationFrameCount
TURN_EPSILON = 0.01      # degrees per tick below which a unit is not turning
# One avoidance detour turns away and back once; #662's re-planned approach reversed 41 times.
MAX_APPROACH_FLIPS = 2


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def run(program: pathlib.Path, scenario: pathlib.Path, root: str, work: pathlib.Path, workers: int) -> Dict[str, bytes]:
    stem = f"{scenario.stem[:4]}.w{workers}"
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
    result["path"] = str(outputs["trace"]).encode("utf-8")
    return result


def read(path: pathlib.Path) -> Dict[Tuple[str, str], Dict[int, int]]:
    rows: Dict[Tuple[str, str], Dict[int, int]] = defaultdict(dict)
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            if row["field"] == "target":
                continue  # a label, or empty
            rows[(row["object"], row["field"])][int(row["tick"])] = int(row["value"])
    return rows


def position(rows, label: str, tick: int) -> Tuple[float, float]:
    return rows[(label, "pos.x")][tick] / ONE, rows[(label, "pos.y")][tick] / ONE


def gap(rows, a: str, b: str, tick: int) -> float:
    (ax, ay), (bx, by) = position(rows, a, tick), position(rows, b, tick)
    return math.hypot(ax - bx, ay - by)


def moved(rows, label: str, tick: int) -> bool:
    return tick > 0 and position(rows, label, tick) != position(rows, label, tick - 1)


def turn_flips(rows, label: str, first: int, last: int) -> int:
    """How often the unit's turn changes direction (left to right or back) in ticks first..last."""
    fx, fy = rows[(label, "fwd.x")], rows[(label, "fwd.y")]
    flips, turning = 0, 0
    for tick in range(max(first, 1), last + 1):
        step = math.degrees(math.atan2(fy[tick], fx[tick]) - math.atan2(fy[tick - 1], fx[tick - 1]))
        step = (step + 180.0) % 360.0 - 180.0
        direction = (step > TURN_EPSILON) - (step < -TURN_EPSILON)
        if direction and turning and direction != turning:
            flips += 1
        turning = direction or turning
    return flips


def check_s29(rows, end: int) -> str:
    death = next((tick for tick in range(end) if rows[("target", "alive")][tick] == 0), None)
    check(death is not None, "S-29: the corvette dies")
    last = (death or end) - 1
    for label in ("shooter", "wing"):
        check(not any(moved(rows, label, tick) for tick in range(1, 32)), f"S-29: {label} holds until the order acts")
        check(any(moved(rows, label, tick) for tick in range(32, 60)), f"S-29: {label} closes on the target")
        stop = max(tick for tick in range(1, last + 1) if moved(rows, label, tick))
        distance = gap(rows, label, "target", last)
        check(0.9 * ATTACK_DISTANCE - 10.0 <= distance <= ATTACK_DISTANCE and stop < last,
              f"S-29: {label} stops inside its attack distance ({distance:.1f} units at tick {last}, last move {stop})")
        # OR-06, C-07 (#662): the approach is planned once and flown as planned; a check that
        # re-plans it swings the heading off course and back every few checks.
        flips = turn_flips(rows, label, 31, stop)
        check(flips <= MAX_APPROACH_FLIPS,
              f"S-29: {label} flies its approach without swinging ({flips} turn reversals up to tick {stop})")
        check(rows[(label, "alive")][end - 1] == 1, f"S-29: {label} survives")
    return f"S-29: the corvette dies at tick {death}; the Tartans stop {gap(rows, 'shooter', 'target', last):.1f} and {gap(rows, 'wing', 'target', last):.1f} from it"


def check_s30(rows, end: int) -> str:
    far = next(tick for tick in range(end) if gap(rows, "guard", "guarded", tick) > GUARD_RANGE)
    first = next((tick for tick in range(1, end) if moved(rows, "guard", tick)), None)
    check(first is not None and far < first <= far + CHECK_FRAMES + 2,
          f"S-30: the guard holds within the guard range and follows within a check (drift at {far}, move at {first})")
    final = gap(rows, "guard", "guarded", end - 1)
    check(0.9 * GUARD_RANGE - 1.0 <= final <= GUARD_RANGE and not moved(rows, "guard", end - 1),
          f"S-30: the guard stops within the guard range ({final:.1f} units)")
    return f"S-30: drift past {GUARD_RANGE:.0f} at tick {far}, the guard moves at {first}, ends {final:.1f} away"


def check_s31(rows, end: int) -> str:
    shots_moving = 0
    for (label, field), values in rows.items():
        if field != "shots" or not label.startswith("shooter/"):
            continue
        shots_moving += sum(count for tick, count in values.items() if moved(rows, "shooter", tick))
    check(shots_moving > 0, "S-31: the Tartan fires at the corvette while it moves")
    arrival = next(tick for tick in range(32, end) if position(rows, "shooter", tick) == position(rows, "shooter", end - 1))
    halted = [tick for tick in range(33, arrival) if not moved(rows, "shooter", tick)]
    check(not halted, f"S-31: the Tartan never stops before its point (halted at {halted[:5]})")
    x, y = position(rows, "shooter", end - 1)
    check(math.hypot(x - 2500.0, y + 1500.0) < 1.0, f"S-31: the Tartan ends on its point ({x:.2f}, {y:.2f})")
    return f"S-31: {shots_moving} shots while moving, arrival at tick {arrival}"


CHECKS = {"S-29": check_s29, "S-30": check_s30, "S-31": check_s31}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: order scenario traces (set EAWR_EAW_GAME_ROOT)")
        return 0
    with tempfile.TemporaryDirectory(prefix="eawr-orders-") as scratch:
        work = pathlib.Path(scratch)
        for case, stem in CASES.items():
            scenario = arguments.scenarios / f"{stem}.json"
            runs = {workers: run(arguments.program, scenario, root, work, workers) for workers in (1, 2, 4)}
            if not all(runs.values()):
                continue
            for workers in (2, 4):
                for name in ("trace", "hashes", "replay"):
                    check(runs[workers][name] == runs[1][name], f"{case}: {workers} workers change the {name}")
            trace_path = runs[1]["path"].decode("utf-8")
            compared = subprocess.run(
                [sys.executable, str(arguments.tool), trace_path, trace_path, "--scenario", str(scenario)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
            )
            check(compared.returncode == 0, f"{case}: not a complete scenario trace: {compared.stdout}")
            rows = read(pathlib.Path(trace_path))
            end = max(rows[("shooter" if case != "S-30" else "guard", "alive")]) + 1
            print(CHECKS[case](rows, end))
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("order scenario traces passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
