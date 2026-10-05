#!/usr/bin/env python3
"""Run the movement scenarios through `sim_headless --scenario` (P2-07, #70).

With EAWR_EAW_GAME_ROOT set, S-10 to S-13 and S-17 (TURBO, #76) are staged on the FoC unit
tables with 1, 2 and 4 workers. Every worker count must write the same trace, hashes and replay; each trace must
be a complete trace of its scenario (the comparer accepts it against itself); and the traces
must show the movement rules of docs/behaviour/space-movement.md. The original recordings stay
private, so this test checks the rules, not the per-field distance to them.

It also stages S-26 with both ships spawned by events and the attack order on the target's
spawn tick: the runner must submit the order after staging the target (#392).
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Dict, List, Tuple

ONE = 1 << 24
SCENARIOS = ("S-10-straight-move", "S-11-turn-in-place", "S-12-move-with-turn", "S-13-stop", "S-17-turbo-move")
FAILURES: List[str] = []

Trace = Dict[Tuple[int, str], int]


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def read(path: pathlib.Path) -> Trace:
    rows: Trace = {}
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            if row["object"] == "corvette":
                rows[(int(row["tick"]), row["field"])] = int(row["value"])
    return rows


def position(trace: Trace, tick: int) -> Tuple[float, float, float]:
    return tuple(trace[(tick, f"pos.{axis}")] / ONE for axis in "xyz")


def step(trace: Trace, tick: int) -> float:
    return math.dist(position(trace, tick), position(trace, tick - 1))


def yaw(trace: Trace, tick: int) -> float:
    return math.degrees(math.atan2(trace[(tick, "fwd.y")], trace[(tick, "fwd.x")]))


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
    result["header"] = outputs["trace"].with_suffix(".json").read_bytes()
    result["stderr"] = completed.stderr.encode("utf-8")
    result["path"] = str(outputs["trace"]).encode("utf-8")
    return result


def check_rules(traces: Dict[str, Trace]) -> None:
    s10, s11, s12, s13, s17 = (traces[name[:4]] for name in SCENARIOS)
    # MV-02, MV-13: an order at tick 30 moves from tick 32, a/2 = 0.03 in the first frame.
    check(step(s10, 31) == 0 and abs(step(s10, 32) - 0.03) < 1e-4, "S-10: the move starts at tick 32 with a/2")
    check(abs(step(s10, 200) - 3.72) < 1e-4, "S-10: cruise at 3.72 units per tick")
    check(all(s10[(tick, "pos.z")] == -20 * ONE for tick in range(600)), "S-10: the corvette stays at its layer z")
    end = position(s10, 599)
    check(abs(end[0]) < 0.1 and abs(end[1] + 1500) < 1e-3, f"S-10: arrives at (0, -1500): {end}")
    check(s10[(0, "fwd.z")] > 33000 and s10[(40, "fwd.z")] == 0, "S-10: staged pitch, level once moving")
    # MV-20, MV-21: turn in place at 1.5 / 2 degrees per frame, ending on 90 degrees.
    check(all(position(s11, tick) == position(s11, 0) for tick in range(300)), "S-11: no translation")
    check(abs(yaw(s11, 32) - 0.75) < 1e-3 and abs(yaw(s11, 151) - 90) < 1e-4 and abs(yaw(s11, 299) - 90) < 1e-4,
          f"S-11: 0.75 degrees per frame to 90: {yaw(s11, 32)}, {yaw(s11, 151)}")
    # MV-13 to MV-15: no turn before full speed, 15-degree arcs, the tangent match at 97.93 degrees.
    check(all(s12[(tick, "fwd.y")] == 0 for tick in range(32, 94)), "S-12: no turn before full speed")
    check(abs(yaw(s12, 94) - 1.5061) < 1e-3 and abs(yaw(s12, 153) - 90) < 1e-3,
          f"S-12: arcs of 15 degrees per 10 frames: {yaw(s12, 94)}, {yaw(s12, 153)}")
    check(abs(yaw(s12, 400) - 97.9296) < 1e-3, f"S-12: heads at the target after the match: {yaw(s12, 400)}")
    # MV-22: a stop at tick 120 still moves at 121, then holds.
    check(abs(step(s13, 121) - 3.72) < 1e-4 and all(step(s13, tick) == 0 for tick in range(122, 300)),
          "S-13: full step at 121, then stopped")
    # AB-24 (#76): TURBO at tick 60 doubles the corvette's speed, acceleration and deceleration.
    # The recording keeps S-10's track to tick 61, changes from tick 62 and cruises at 7.44006
    # (tests/fidelity/README.md, P2-06 final batch).
    check(all(position(s17, tick) == position(s10, tick) for tick in range(62)), "S-17: S-10's track until tick 61")
    check(step(s17, 62) > step(s10, 62) + 0.01, f"S-17: faster from tick 62: {step(s17, 62)} vs {step(s10, 62)}")
    check(abs(step(s17, 200) - 7.44) < 1e-3, f"S-17: cruise at 7.44 under TURBO: {step(s17, 200)}")


def spawned_attack_scenario(scenarios: pathlib.Path) -> dict:
    """S-26 with both ships spawned by events and the attack on the target's spawn tick (#392)."""
    scenario = json.loads((scenarios / "S-26-tartan-dead-astern-attack.json").read_text(encoding="utf-8"))
    scenario.pop("fire_windows")
    scenario["duration_ticks"] = 60
    for unit in scenario["units"]:
        unit["spawn"] = "event"
    scenario["events"] = [
        {"tick": 5, "action": "spawn", "unit": "shooter"},
        {"tick": 10, "action": "spawn", "unit": "target"},
        {"tick": 10, "action": "attack", "unit": "shooter", "target": "target"},
    ]
    return scenario


def check_spawned_attack(program: pathlib.Path, scenarios: pathlib.Path, tool: pathlib.Path, root: str,
                         work: pathlib.Path) -> None:
    """An attack order on an event-spawned target is submitted after the target is staged."""
    scenario = work / "S-26-spawned-attack.json"
    scenario.write_bytes(json.dumps(spawned_attack_scenario(scenarios), indent=2).encode("utf-8"))
    outputs = {}
    for workers in (1, 2, 4):
        trace = work / f"spawned-attack.w{workers}.csv"
        completed = subprocess.run(
            [str(program), "--scenario", str(scenario), "--game-root", root, "--workers", str(workers),
             "--trace-out", str(trace), "--hash-out", str(work / f"spawned-attack.w{workers}.hashes.csv")],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
        )
        check(completed.returncode == 0, f"spawned attack: sim_headless failed: {completed.stdout}{completed.stderr}")
        if completed.returncode != 0:
            return
        outputs[workers] = (trace.read_bytes(), trace.with_suffix(".hashes.csv").read_bytes())
    check(outputs[2] == outputs[1] and outputs[4] == outputs[1], "spawned attack: output differs across workers")
    trace = work / "spawned-attack.w1.csv"
    compared = subprocess.run(
        [sys.executable, str(tool), str(trace), str(trace), "--scenario", str(scenario)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
    )
    check(compared.returncode == 0, f"spawned attack: not a complete scenario trace: {compared.stdout}")
    rows: Dict[Tuple[int, str, str], str] = {}
    with trace.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            rows[(int(row["tick"]), row["object"], row["field"])] = row["value"]
    check([rows[(tick, "target", "alive")] for tick in (9, 10)] == ["0", "1"], "spawned attack: target spawns at 10")
    # Dead astern nothing is acquired idle; the tick-10 order is the target from tick 11 (A-01).
    targets = {tick: rows[(tick, "shooter/hp_tartan_cruiser_02", "target")] for tick in (10, 11, 59)}
    check(targets == {10: "", 11: "target", 59: "target"}, f"spawned attack: the order takes effect: {targets}")


def check_squadron_container_health(program: pathlib.Path, scenarios: pathlib.Path, root: str,
                                    work: pathlib.Path) -> None:
    """WSQ-60: all M2 container traces use local hull, independent of crew composition."""
    scenario = json.loads((scenarios / "S-10-straight-move.json").read_text(encoding="utf-8"))
    types = ("Rebel_X-Wing_Squadron", "Y-Wing_Squadron", "TIE_Fighter_Squadron",
             "TIE_Interceptor_Squadron", "TIE_Bomber_Squadron")
    owner = scenario["units"][0]["owner"]
    scenario["units"] = [
        {"label": f"team{i}", "type": kind, "owner": owner, "position": [i * 600, 0, 0],
         "facing_degrees": 0, "spawn": "start"}
        for i, kind in enumerate(types)
    ]
    scenario["events"] = []
    scenario["duration_ticks"] = 2
    scenario["expect"] = []
    path = work / "container-health.json"
    path.write_text(json.dumps(scenario), encoding="utf-8")
    outputs = {}
    for workers in (1, 2, 4, 8):
        trace = work / f"container-health.w{workers}.csv"
        completed = subprocess.run(
            [str(program), "--scenario", str(path), "--game-root", root, "--workers", str(workers),
             "--trace-out", str(trace)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, check=False,
        )
        check(completed.returncode == 0, f"container health: {completed.stdout}{completed.stderr}")
        if completed.returncode != 0:
            continue
        outputs[workers] = trace.read_bytes()
        with trace.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        for i, kind in enumerate(types):
            hull = [int(row["value"]) for row in rows if row["object"] == f"team{i}" and row["field"] == "hull"]
            check(hull == [150 * ONE] * 2, f"WSQ-60: {kind} container hull is 150 on every tick: {hull}")
            shields = [int(row["value"]) for row in rows if row["object"] == f"team{i}" and row["field"] == "shield"]
            check(shields == [0] * 2, f"WSQ-60: {kind} container reports zero shields, not crew shields: {shields}")
    check(len(outputs) == 4 and len(set(outputs.values())) == 1,
          "WSQ-60: all five containers produce identical traces with 1/2/4/8 workers")


def check_observed_initial_pose(program: pathlib.Path, scenarios: pathlib.Path, root: str,
                                work: pathlib.Path) -> None:
    """Recorded tick-zero craft pose overrides placement and survives worker partitioning."""
    scenario = json.loads((scenarios / "S-97-xwing-vs-tie-dogfight.json").read_text(encoding="utf-8"))
    scenario["duration_ticks"] = 2
    scenario["events"] = []
    craft = next(unit for unit in scenario["units"] if unit["label"] == "xwing.1")
    craft["position"] = [123.25, -456.5, 7]
    craft["facing_degrees"] = 67
    craft["apply_initial_pose"] = True
    outputs = {}
    path = work / "observed-initial-pose.json"
    path.write_text(json.dumps(scenario), encoding="utf-8")
    for workers in (1, 2, 4, 8):
        trace = work / f"initial-pose.w{workers}.csv"
        hashes = work / f"initial-pose.w{workers}.hashes.csv"
        completed = subprocess.run(
            [str(program), "--scenario", str(path), "--game-root", root, "--workers", str(workers),
             "--trace-out", str(trace), "--hash-out", str(hashes)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
        )
        check(completed.returncode == 0, f"initial pose: {completed.stdout}{completed.stderr}")
        if completed.returncode != 0:
            continue
        outputs[workers] = (trace.read_bytes(), hashes.read_bytes())
        with trace.open(newline="", encoding="utf-8") as stream:
            rows = {row["field"]: int(row["value"]) for row in csv.DictReader(stream)
                    if row["tick"] == "0" and row["object"] == "xwing.1"}
        check(tuple(rows[f"pos.{axis}"] for axis in "xyz") == (123.25 * ONE, -456.5 * ONE, 7 * ONE),
              "initial pose uses recorded world position without adding a layer or formation offset")
        actual_yaw = math.degrees(math.atan2(rows["fwd.y"], rows["fwd.x"]))
        check(abs(actual_yaw - 67) < 1e-4, f"initial pose yaw: {actual_yaw}")
    check(len(outputs) == 4 and len(set(outputs.values())) == 1,
          "initial pose trace and hashes agree on 1/2/4/8 workers")
    craft["label"] = "xwing.future"
    path.write_text(json.dumps(scenario), encoding="utf-8")
    rejected = subprocess.run([str(program), "--scenario", str(path), "--game-root", root,
                               "--trace-out", str(work / "invalid-initial-pose.csv")],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
    check(rejected.returncode != 0 and "matching tick-zero member" in rejected.stderr,
          "initial pose rejects an observed member absent from the initial setup")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: movement scenario traces (set EAWR_EAW_GAME_ROOT)")
        return 0
    traces: Dict[str, Trace] = {}
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        for name in SCENARIOS:
            scenario = args.scenarios / f"{name}.json"
            runs = {workers: run(args.program, scenario, root, work, workers) for workers in (1, 2, 4)}
            if not all(runs.values()):
                continue
            for workers in (2, 4):
                for kind in ("trace", "hashes", "replay", "header"):
                    check(runs[workers][kind] == runs[1][kind], f"{name}: {kind} differs with {workers} workers")
            trace_path = runs[1]["path"].decode("utf-8")
            compared = subprocess.run(
                [sys.executable, str(args.tool), trace_path, trace_path, "--scenario", str(scenario)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
            )
            check(compared.returncode == 0, f"{name}: not a complete scenario trace: {compared.stdout}")
            check("not modelled" not in runs[1]["stderr"].decode("utf-8"), f"{name}: every ability is modelled")
            traces[name[:4]] = read(pathlib.Path(trace_path))
        if len(traces) == len(SCENARIOS):
            check_rules(traces)
        check_spawned_attack(args.program, args.scenarios, args.tool, root, work)
        check_squadron_container_health(args.program, args.scenarios, root, work)
        check_observed_initial_pose(args.program, args.scenarios, root, work)
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("movement scenario traces passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
