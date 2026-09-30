#!/usr/bin/env python3
"""Run the rear-arc and attack-order cases S-22 to S-27, S-32, S-33 and the level-1 station arc
cases S-34 to S-42 through `sim_headless --scenario` (#361, #516).

With EAWR_EAW_GAME_ROOT set, each case is staged on the FoC unit tables; S-26 and S-27 run with
1, 2, 4 and 8 workers, which must write the same trace, hashes and replay. Each trace must be a
complete scenario trace that keeps its fire windows (docs/traces.md): every silent one, and
every other one that closes before either ship dies. The checks are the recorded FoC behaviour of
docs/behaviour/space-weapon-fire.md (A-04 to A-07, the S-22 to S-27 table): an idle shooter
keeps yaw 0 and only the hardpoints that reach the target fire; an ordered Tartan keeps yaw 0
through tick 32, turns in place from tick 33 at 0.84 degrees per frame, the short way, settles
on the recorded heading at the recorded tick without moving, and then fires all five
hardpoints. The scenario runner does not apply the target's hold_fire and invulnerable flags,
so the target fires back and a ship may die: hardpoint participation is counted before that,
and the windows a death overlaps are printed, not checked. The original recordings stay private; the recorded values below are the note's.
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile
from collections import defaultdict
from typing import Dict, List, Optional, Tuple

ONE = 1 << 24
FAILURES: List[str] = []
# case: (scenario stem, hardpoints that fire, recorded final yaw, recorded settle tick)
CASES = {
    "S-22": ("S-22-tartan-dead-astern-idle", set(), 0.0, None),
    "S-23": ("S-23-tartan-port-quarter-idle", {"hp_tartan_cruiser_01", "hp_tartan_cruiser_04"}, 0.0, None),
    "S-24": ("S-24-nebulon-dead-astern-idle", set(), 0.0, None),
    "S-25": ("S-25-nebulon-port-quarter-idle", {"hp_nebulon_weapon_fl", "hp_nebulon_weapon_bl"}, 0.0, None),
    "S-26": ("S-26-tartan-dead-astern-attack", {f"hp_tartan_cruiser_0{index}" for index in range(5)}, -179.1471, 246),
    "S-27": ("S-27-tartan-port-quarter-attack", {f"hp_tartan_cruiser_0{index}" for index in range(5)}, 166.0536, 230),
    # #516: the Acclamator's missile (FC) and torpedo (BC) launchers are bow weapons (W-12).
    "S-32": ("S-32-acclamator-port-beam-idle", {"hp_acclamator_weapon_fl", "hp_acclamator_weapon_bl"}, 0.0, None),
    "S-33": ("S-33-acclamator-port-bow-idle", {"hp_acclamator_weapon_fl", "hp_acclamator_weapon_bl",
                                              "hp_acclamator_weapon_fc", "hp_acclamator_weapon_bc"}, 0.0, None),
    # #516 follow-up: level-1 station arcs, one idle target per bearing (each station's
    # Targeting_Priority_Set is unauthored, so a shared multi-target scenario ties every
    # candidate at priority 1.0 and the opportunity scan can stick on one that never actually
    # points there; a single target avoids that).
    "S-34": ("S-34-rebel-station-ccm-lc", {"hp_rebel_station_one_ccm", "hp_rebel_station_one_lc"}, 0.0, None),
    "S-35": ("S-35-rebel-station-lc-only", {"hp_rebel_station_one_lc"}, 0.0, None),
    "S-36": ("S-36-rebel-station-ccm-only", {"hp_rebel_station_one_ccm"}, 0.0, None),
    "S-37": ("S-37-rebel-station-tbl-only", {"hp_rebel_station_one_tbl"}, 0.0, None),
    "S-38": ("S-38-rebel-station-lc-tbl", {"hp_rebel_station_one_lc", "hp_rebel_station_one_tbl"}, 0.0, None),
    "S-39": ("S-39-empire-station-l01-only", {"hp_empire_station_one_00", "hp_empire_station_one_01"}, 0.0, None),
    "S-40": ("S-40-empire-station-l01-l02",
             {"hp_empire_station_one_00", "hp_empire_station_one_01"}, 0.0, None),
    "S-41": ("S-41-empire-station-l02-only", {"hp_empire_station_one_00", "hp_empire_station_one_02"}, 0.0, None),
    "S-42": ("S-42-empire-station-missile-only", {"hp_empire_station_one_00"}, 0.0, None),
}
# case: (hardpoint, issue) known to fire where FoC's does not, only because the runner does not
# apply the target's invulnerable flag (#575). In S-40 the station shoots away the Nebulon-B's
# nearest hardpoint, the engines (-90.9 degrees from hp_02's bore, outside its 90-degree
# half-cone). W-05 then aims at the next nearest, BL (-88.3, inside), and hp_02 starts firing.
# FoC's invulnerable target keeps every hardpoint. The hardpoint's fire windows aren't checked. It
# must stay silent until the target first loses hull, and must fire after that: once it
# doesn't, remove the entry.
EXPECTED_FAIL = {"S-40": ("hp_empire_station_one_02", "#575")}
YAW_TOLERANCE = 0.01  # degrees
RATE = 0.84           # degrees per frame: Max_Rate_Of_Turn 1.4 x 1.2 over the corvette slowdown 2


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
    result["header"] = outputs["trace"].with_suffix(".json").read_bytes()
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


def yaw(rows, tick: int) -> float:
    return math.degrees(math.atan2(rows[("shooter", "fwd.y")][tick], rows[("shooter", "fwd.x")][tick]))


def first_death(path: pathlib.Path) -> Optional[int]:
    """The first tick on which the shooter or the target is no longer alive."""
    rows = read(path)
    ticks = [tick for unit in ("shooter", "target") for tick, value in rows[(unit, "alive")].items() if value == 0]
    return min(ticks) if ticks else None


def first_hull_loss(path: pathlib.Path) -> Optional[int]:
    """The first tick on which the target's hull is below its tick-0 value."""
    hull = read(path)[("target", "hull")]
    return next((tick for tick in sorted(hull) if hull[tick] < hull[0]), None)


def split_windows(scenario: dict, death: Optional[int],
                  expected_fail: Optional[str] = None) -> Tuple[List[dict], List[Tuple[int, dict]]]:
    """The fire windows a death cannot break (silent ones, and those closing before `death`), and
    the numbered rest. A window of an expected-fail hardpoint goes to the rest."""
    def kept(window: dict) -> bool:
        if expected_fail is not None and f"shooter/{expected_fail}" in window["hardpoints"]:
            return False
        return death is None or window["min_shots"] == 0 or window["to_tick"] < death

    windows = list(enumerate(scenario["fire_windows"]))
    return [window for _, window in windows if kept(window)], [(index, window) for index, window in windows
                                                               if not kept(window)]


def load_tool(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("compare_traces", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def window_shots(path: pathlib.Path, window: dict) -> Dict[str, int]:
    rows = read(path)
    return {label: sum(value for tick, value in rows[(label, "shots")].items()
                       if window["from_tick"] <= tick <= window["to_tick"])
            for label in window["hardpoints"]}


def check_case(case: str, path: pathlib.Path) -> str:
    _, firing, final, settle = CASES[case]
    rows = read(path)
    ticks = sorted(rows[("shooter", "fwd.x")])
    alive = rows[("shooter", "alive")]
    death: Optional[int] = next((tick for tick in ticks if alive.get(tick) == 0), None)
    live = [tick for tick in ticks if death is None or tick < death]
    start = (rows[("shooter", "pos.x")][0], rows[("shooter", "pos.y")][0])
    check(all((rows[("shooter", "pos.x")][tick], rows[("shooter", "pos.y")][tick]) == start for tick in live),
          f"{case}: the shooter does not translate")
    if settle is None:
        check(all(yaw(rows, tick) == 0.0 for tick in live), f"{case}: an idle shooter keeps yaw 0 (A-05)")
    else:
        check(all(yaw(rows, tick) == 0.0 for tick in range(0, 33)), f"{case}: yaw 0 through tick 32 (A-04)")
        direction = 1.0 if final > 0 else -1.0
        check(abs(yaw(rows, 33) - direction * RATE) < 1e-3, f"{case}: tick 33 turns {RATE} degrees the short way")
        moved = [tick for tick in range(1, live[-1] + 1) if yaw(rows, tick) != yaw(rows, tick - 1)]
        check(bool(moved) and moved[-1] == settle, f"{case}: the turn settles at tick {moved[-1] if moved else None}, "
                                                  f"recorded {settle}")
        check(abs(yaw(rows, live[-1]) - final) <= YAW_TOLERANCE,
              f"{case}: final yaw {yaw(rows, live[-1]):.4f}, recorded {final}")
    fired = {obj.split("/", 1)[1] for (obj, field), series in rows.items()
             if field == "shots" and obj.startswith("shooter/") and any(series.get(tick, 0) for tick in live)}
    if case in EXPECTED_FAIL:
        hardpoint, issue = EXPECTED_FAIL[case]
        shots = rows[(f"shooter/{hardpoint}", "shots")]
        first = next((tick for tick in live if shots.get(tick, 0)), None)
        loss = first_hull_loss(path)
        check(first is not None, f"{case}: {hardpoint} no longer fires, as recorded: remove its expected-fail ({issue})")
        if first is not None:
            check(loss is not None and first > loss,
                  f"{case}: {hardpoint} fires at tick {first}, before the target first loses hull ({loss})")
            print(f"{case}: XFAIL {hardpoint} fires from tick {first}, after the target first loses hull at tick "
                  f"{loss}; FoC's invulnerable target never does ({issue})")
        fired.discard(hardpoint)
    check(fired == firing, f"{case}: hardpoints that fire {sorted(fired)}, recorded {sorted(firing)}")
    total = sum(value for (obj, field), series in rows.items() if field == "shots" and obj.startswith("shooter/")
                for tick, value in series.items() if tick in live)
    return f"{case}: final yaw {yaw(rows, live[-1]):.4f}, {total} shots before tick {live[-1] + 1}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: attack turn scenario traces (set EAWR_EAW_GAME_ROOT)")
        return 0
    tool = load_tool(args.tool)
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        for case, (stem, _, _, settle) in CASES.items():
            scenario = args.scenarios / f"{stem}.json"
            counts = (1, 2, 4, 8) if settle is not None else (1,)
            runs = {workers: run(args.program, scenario, root, work, workers) for workers in counts}
            if not all(runs.values()):
                continue
            for workers in counts[1:]:
                for kind in ("trace", "hashes", "replay", "header"):
                    check(runs[workers][kind] == runs[1][kind], f"{case}: {kind} differs with {workers} workers")
            trace_path = runs[1]["path"].decode("utf-8")
            # The comparer checks the whole scenario, then its fire windows in order. Windows a
            # death can break depend on the hold_fire and invulnerable flags the runner does not
            # apply: they may fail, every other one must pass.
            death = first_death(pathlib.Path(trace_path))
            expected_fail = EXPECTED_FAIL.get(case, (None, None))[0]
            kept, rest = split_windows(json.loads(scenario.read_text(encoding="utf-8")), death, expected_fail)
            compared = subprocess.run(
                [sys.executable, str(args.tool), trace_path, trace_path, "--scenario", str(scenario)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
            )
            known = compared.returncode == 1 and any(f": fire window {index}," in compared.stdout for index, _ in rest)
            check(compared.returncode == 0 or known, f"{case}: not a complete scenario trace: {compared.stdout}")
            try:
                tool.check_fire_rows(pathlib.Path(trace_path), tool.read_trace(pathlib.Path(trace_path)),
                                     {"fire_windows": kept})
            except tool.Divergence as error:
                check(False, f"{case}: {error}")
            print(check_case(case, pathlib.Path(trace_path)))
            print(f"{case}: {len(kept)} of {len(kept) + len(rest)} fire windows checked (first death: tick {death})")
            for index, window in rest:
                shots = window_shots(pathlib.Path(trace_path), window)
                low, high = window["min_shots"], window["max_shots"]
                missed = {label: count for label, count in shots.items()
                          if count < low or (high is not None and count > high)}
                if missed:
                    why = (f"expected-fail ({EXPECTED_FAIL[case][1]})" if expected_fail is not None
                           and f"shooter/{expected_fail}" in window["hardpoints"] else f"after the death at tick {death}")
                    print(f"{case}: fire window {index} (ticks {window['from_tick']}..{window['to_tick']}, "
                          f"{low}..{high}) not kept {why}: {missed}")
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("attack turn scenario traces passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
