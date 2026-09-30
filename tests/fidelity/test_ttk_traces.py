#!/usr/bin/env python3
"""Time to kill: the #536 one-sided matchups S-43 to S-50 through `sim_headless --scenario`.

With EAWR_EAW_GAME_ROOT set, each scenario runs with its combat log (`--combat-out`,
docs/traces.md) and the test prints, per matchup, when the held target's shield empties, when each
of its hardpoints and its hull are destroyed, and the shots and hits of the shooter's weapons,
next to the FoC recordings (tests/fidelity/README.md, two seeds, 2026-09-28). The corvette-laser
case S-46, whose gap was the hit rate (space-damage DG-24, DG-36), must stay within 15 % of the
recorded range for its shield and hull, and so must S-45, whose hull since #669 falls only with
its hardpoints (HS-02, DG-39) as in FoC. In S-45, S-49 and S-50 the target's hull, as in every recording,
may fall only with its hardpoints (HS-02, DG-39, #669). S-46 also runs with 8 workers and must write the same
combat log. The recordings stay private; the recorded ticks below are what the test keeps.
"""

from __future__ import annotations

import argparse
import csv
import os
import pathlib
import subprocess
import tempfile
from typing import Dict, List, Optional, Tuple

# Recorded ticks, seeds 12345 and 4242 (min, max); None: not within the recording.
RECORDED: Dict[str, Dict[str, Tuple[int, int]]] = {
    "S-43": {"shield": (635, 701), "hull": (779, 825)},
    "S-45": {"shield": (1032, 1122), "hull": (1522, 1564)},
    "S-46": {"shield": (1353, 1472), "hull": (2360, 2428)},
    "S-47": {"shield": (2426, 2442)},
    "S-48": {"hull": (1374, 1374)},
    "S-49": {"hull": (2326, 2326)},
}
CHECKED: Dict[str, Tuple[str, ...]] = {"S-45": ("shield", "hull"), "S-46": ("shield", "hull")}
# #669 (space-damage DG-39): the held target's hull loses health only through the hardpoint
# coupling (space-hardpoints HS-02) while the shots aim at its hardpoints. In every recording of
# these cases (both seeds) each drop of the hull sits on the cap min(1, S/T + 0.2) of its maximum;
# the hull lost nothing directly.
HULL_FOLLOWS_HARDPOINTS = ("S-45", "S-49", "S-50")
HULL_CONSTRAINT = 0.2
WINDOW = 0.15
FAILURES: List[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def run(program: pathlib.Path, scenario: pathlib.Path, root: str, work: pathlib.Path, workers: int) -> Optional[bytes]:
    stem = f"{scenario.stem}.w{workers}"
    trace, combat = work / f"{stem}.csv", work / f"{stem}.combat.csv"
    completed = subprocess.run(
        [str(program), "--scenario", str(scenario), "--game-root", root, "--workers", str(workers),
         "--trace-out", str(trace), "--combat-out", str(combat)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
    )
    check(completed.returncode == 0, f"{stem}: sim_headless failed: {completed.stdout}{completed.stderr}")
    return combat.read_bytes() if completed.returncode == 0 else None


def summary(log: bytes) -> Dict[str, object]:
    """The target's shield-empty, hardpoint and hull death ticks; the shooter's shots and hits."""
    result: Dict[str, object] = {"shield": None, "hull": None, "hardpoints": {}, "shots": 0, "hits": 0,
                                 "hit_parts": {}}
    first: Dict[str, float] = {}
    for row in csv.DictReader(log.decode("utf-8").splitlines()):
        tick = int(row["tick"])
        if row["kind"] == "fired" and row["other"] == "target":
            result["shots"] += 1
        elif row["kind"] == "hit" and row["object"] == "target":
            result["hits"] += 1
            parts = result["hit_parts"]
            parts[row["part"]] = parts.get(row["part"], 0) + 1
            weapon = row["other"].split("/", 1)[1] if "/" in row["other"] else row["other"]
            phase = "up" if result["shield"] is None else "down"
            by = result.setdefault("by_weapon", {})
            key = f"{weapon}>{row['part']}@{phase}"
            by[key] = by.get(key, 0) + 1
        elif row["kind"] == "fired" and row["object"] != "target":
            weapon = row["part"]
            fired = result.setdefault("fired", {})
            fired[weapon] = fired.get(weapon, 0) + 1
        elif row["kind"] == "health" and row["object"] == "target":
            part, value = row["part"], float(row["value"])
            first.setdefault(part, value)
            if part == "shield":
                if value == 0 and result["shield"] is None and tick > 0:
                    result["shield"] = tick
            elif part != "hull" and value == 0 and first[part] > 0:
                result["hardpoints"].setdefault(part, tick)
        elif row["kind"] == "destroyed" and row["object"] == "target":
            result["hull"] = tick
    return result


def hull_over_time(log: bytes) -> Dict[str, object]:
    """The target's hull and hardpoints tick by tick: how much of the hull's loss the HS-02 cap
    does not explain (direct loss), and a sample every 150 ticks for the printout."""
    changes: Dict[int, List[Tuple[str, float]]] = {}
    for row in csv.DictReader(log.decode("utf-8").splitlines()):
        if row["kind"] == "health" and row["object"] == "target" and row["part"] != "shield":
            changes.setdefault(int(row["tick"]), []).append((row["part"], float(row["value"])))
    hull: Optional[float] = None
    maximum: Optional[float] = None
    hardpoints: Dict[str, float] = {}
    maxima: Dict[str, float] = {}
    share_before: Optional[float] = None
    direct = 0.0
    samples: List[Tuple[int, float, Dict[str, float]]] = []
    for tick in sorted(changes):
        hull_before = hull
        for part, value in changes[tick]:
            if part == "hull":
                hull = value
                if maximum is None:
                    maximum = value
            else:
                hardpoints[part] = value
                maxima.setdefault(part, value)
        total = sum(maxima.values())
        share = sum(hardpoints.values()) / total if total > 0 else None
        if hull is not None and hull_before is not None and maximum and hull < hull_before:
            # The coupling runs once a tick, so the cap may come from this tick's hardpoints or
            # the last tick's.
            caps = [min(1.0, x + HULL_CONSTRAINT) * maximum for x in (share, share_before) if x is not None]
            if all(abs(hull - cap) > 0.001 * maximum for cap in caps):
                direct += hull_before - hull
        share_before = share
        if hull is not None and (not samples or tick >= samples[-1][0] + 150):
            samples.append((tick, hull, dict(hardpoints)))
    return {"direct": direct, "samples": samples, "maximum": maximum}


def show(name: str, value: Optional[int], recorded: Optional[Tuple[int, int]]) -> str:
    ours = "-" if value is None else str(value)
    if recorded is None:
        return f"{name} {ours}"
    return f"{name} {ours} (recorded {recorded[0]}..{recorded[1]})"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: time-to-kill scenario traces (set EAWR_EAW_GAME_ROOT)")
        return 0
    scenarios = sorted(args.scenarios.glob("S-*-ttk-*.json"))
    check(len(scenarios) == 8, f"expected the eight time-to-kill scenarios S-43 to S-50, found {len(scenarios)}")
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        for scenario in scenarios:
            case = scenario.name[:4]
            log = run(args.program, scenario, root, work, 1)
            if log is None:
                continue
            result = summary(log)
            recorded = RECORDED.get(case, {})
            print(f"{scenario.stem}: " + "; ".join([
                show("shield empty", result["shield"], recorded.get("shield")),
                show("hull destroyed", result["hull"], recorded.get("hull")),
                "hardpoints destroyed " + (", ".join(f"{k} {v}" for k, v in sorted(result["hardpoints"].items())) or "none"),
                f"shots {result['shots']}, hits {result['hits']} ({result['hit_parts']})",
            ]))
            print(f"    fired {result.get('fired', {})}")
            print(f"    hits by weapon>part@shield {result.get('by_weapon', {})}")
            if case in CHECKED:
                for key in CHECKED[case]:
                    low, high = recorded[key]
                    value = result[key]
                    lower, upper = low * (1 - WINDOW), high * (1 + WINDOW)
                    check(value is not None and lower <= value <= upper,
                          f"{case}: {key} at {value}, recorded {low}..{high} (window {lower:.0f}..{upper:.0f})")
            if case in HULL_FOLLOWS_HARDPOINTS:
                over_time = hull_over_time(log)
                for tick, value, parts in over_time["samples"]:
                    print(f"    tick {tick}: hull {value:.0f}, " + ", ".join(f"{k} {v:.0f}" for k, v in sorted(parts.items())))
                print(f"    direct hull loss {over_time['direct']:.1f} (recorded 0)")
                check(over_time["direct"] <= 0.001 * (over_time["maximum"] or 1.0),
                      f"{case}: the hull lost {over_time['direct']:.1f} beyond the hardpoint cap (recorded 0, DG-39)")
            if case == "S-46":
                eight = run(args.program, scenario, root, work, 8)
                check(eight == log, "S-46: the combat log differs with 8 workers")
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("time-to-kill scenario traces passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
