#!/usr/bin/env python3
"""Run the S-57 squadron group move through `sim_headless --scenario` (#599).

With EAWR_EAW_GAME_ROOT set, S-57 (four X-wing squadrons abreast, 200 units apart, moved 2400
units east by one command) is staged on the FoC unit tables with 1, 2 and 4 workers. Every worker
count must write the same trace, hashes and replay, and the trace must be a complete scenario
trace. It prints, for the flight (until a squadron is 200 units short of the destination), the
closest craft of different squadrons and the lane crossings: two squadrons swapping sides of the
move while less than a squadron's width apart along it. It does not bound them: on FoC's rules
(docs/behaviour/space-fighters.md FO-09 to FO-11, from the debug build) a wide abreast line keeps
every squadron at `Max_Speed` while it is far off its lane, so the rows form late and two rows'
lanes pass close. The retail reference is the owner's capture of such a line (#629, video only),
which shows the same late close pass of the two middle squadrons; the recorder's Lua orders move
one object each and cannot stage a group move. C-27 bounds the spacing and the crossings where the
squadrons start close.
"""

from __future__ import annotations

import argparse
import collections
import csv
import math
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Dict, List, Tuple

SCENARIO = "S-57-xwing-group-move"
SQUADRONS = ("a", "b", "c", "d")
ORDER_TICK = 30
DESTINATION_X = 1200.0
FLIGHT_END = DESTINATION_X - 200.0  # the flight ends once a squadron is this far east
WIDTH = 60.0  # a squadron's width across the move, for a crossing
SCALE = float(1 << 24)
FAILURES: List[str] = []

Rows = Dict[str, Dict[Tuple[int, str], str]]


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def run(program: pathlib.Path, scenario: pathlib.Path, root: str, work: pathlib.Path, workers: int) -> Dict[str, bytes]:
    stem = f"S-57.w{workers}"
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
            rows[row["object"]][(int(row["tick"]), row["field"])] = row["value"]
    return rows


def point(rows: Rows, name: str, tick: int) -> Tuple[float, float, float] | None:
    fields = rows.get(name, {})
    if fields.get((tick, "alive")) != "1":
        return None
    return tuple(int(fields[(tick, f"pos.{axis}")]) / SCALE for axis in "xyz")  # type: ignore[return-value]


def check_flight(path: pathlib.Path) -> Dict[str, float]:
    rows = load(path)
    crafts = sorted(name for name in rows if "." in name and name.split(".")[0] in SQUADRONS)
    check(len(crafts) == 20, f"S-57: the four squadrons' twenty craft are traced, got {len(crafts)}")
    ticks = sorted({tick for (tick, field) in rows.get("a", {}) if field == "alive"})
    closest = math.inf
    crossings = 0
    end = ORDER_TICK
    previous: Dict[str, Tuple[float, float, float]] = {}
    for tick in ticks:
        if tick <= ORDER_TICK:
            continue
        centres = {name: point(rows, name, tick) for name in SQUADRONS}
        if any(centre is None for centre in centres.values()):
            check(False, f"S-57: a squadron is gone at tick {tick}")
            break
        if max(centre[0] for centre in centres.values()) > FLIGHT_END:  # type: ignore[index]
            break
        end = tick
        live = {name: point(rows, name, tick) for name in crafts}
        for a, pa in live.items():
            for b, pb in live.items():
                if pa is None or pb is None or a >= b or a.split(".")[0] == b.split(".")[0]:
                    continue
                closest = min(closest, math.dist(pa, pb))
        for index, a in enumerate(SQUADRONS):
            for b in SQUADRONS[index + 1:]:
                if a in previous and b in previous:
                    before = previous[a][1] - previous[b][1]
                    now = centres[a][1] - centres[b][1]  # type: ignore[index]
                    along = abs(centres[a][0] - centres[b][0])  # type: ignore[index]
                    if (before < 0) != (now < 0) and along < WIDTH:
                        crossings += 1
        previous = centres  # type: ignore[assignment]
    check(end > ORDER_TICK + 60, f"S-57: the squadrons fly for at least two seconds, flight ended at tick {end}")
    return {"closest": closest, "crossings": float(crossings), "end": float(end)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    parser.add_argument("--keep", type=pathlib.Path, help="copy the one-worker trace here")
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: group move scenario trace (set EAWR_EAW_GAME_ROOT)")
        return 0
    scenario = args.scenarios / f"{SCENARIO}.json"
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        runs = {workers: run(args.program, scenario, root, work, workers) for workers in (1, 2, 4)}
        if all(runs.values()):
            for workers in (2, 4):
                for kind in ("trace", "hashes", "replay", "header"):
                    check(runs[workers][kind] == runs[1][kind], f"S-57: {kind} differs with {workers} workers")
            trace_path = runs[1]["path"].decode("utf-8")
            if args.keep:
                args.keep.write_bytes(runs[1]["trace"])
            compared = subprocess.run(
                [sys.executable, str(args.tool), trace_path, trace_path, "--scenario", str(scenario)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
            )
            check(compared.returncode == 0, f"S-57: not a complete scenario trace: {compared.stdout}")
            stats = check_flight(pathlib.Path(trace_path))
            print("S-57: in flight to tick {end:.0f}, closest craft of different squadrons {closest:.2f}, "
                  "lane crossings {crossings:.0f}".format(**stats))
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("group move scenario trace passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
