#!/usr/bin/env python3
"""Run the S-97 squadron dogfight and the S-98 interception through `sim_headless --scenario` (#457, #607).

With EAWR_EAW_GAME_ROOT set, S-97 (an X-wing squadron attacks a TIE fighter squadron that has no
order) is staged on the FoC unit tables with 1, 2, 4 and 8 workers. Every worker count must write the
same trace, hashes and replay; the trace must be a complete scenario trace; and it must show what
the FoC recording shows (both runs identical, measured 2026-09-29): the craft close and fight at
short range (recorded: the mean distance from a craft to its nearest enemy stays between 31 and
119 units while both squadrons live), and the fight stays near the plane (FD-12: 90 % of the
recorded craft heights lie within 100 units of it, the extremes at 173; the remake 127 and 290,
space-fighters fidelity list). The trace has no craft targets, so the TIEs' turn on the X-wings
(recorded 84 ticks after the order) is checked by the contracts, not here. The outcome (FD-13)
holds the remake's losses, their ticks and hit rates where they are, below the recording's, so the
gap can only close; they guard DG-37 (the craft sphere's gate) and, with S-98, W-06a. S-98 (the X-wings intercept TIE bombers attacking a corvette) runs with 1 and
8 workers and must lose the bombers the recording loses, near its ticks, with the X-wings' shots
and hits near its shot log (W-06a). The recordings stay private, so this test checks those
outcomes, not every field; docs/behaviour/space-fighters.md explains the remaining differences.
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

SCENARIO = "S-97-xwing-vs-tie-dogfight"
INTERCEPT = "S-98-xwing-intercepts-bombers"
RECORDED_BAND = 100.0  # 5th to 95th percentile of the recorded craft heights, after tick 180
BAND_LIMIT = 150.0
RECORDED_EXTREME = 173.1
EXTREME_LIMIT = 320.0
ENGAGED_LIMIT = 150.0  # recorded: 31 to 119 at every second from tick 210 to 1500
RECORDED_FIRST_LOSS = 706  # FD-13: the recording's first craft lost (X-wing), both runs
RECORDED_LOSSES = {"xwing": 5, "tie": 2}  # by tick 1503 and 1800
# The remake (#607): first loss 448, no X-wing and all seven TIE fighters lost (FD-13). The limits
# hold that and may only move toward the recording.
FIRST_LOSS_LIMIT = 440
LOSS_LIMITS = {"xwing": (0, 5), "tie": (2, 7)}
# While every TIE fighter dies, the last one no earlier than this (the remake 1249). Without W-06a
# (#607 review: the old burst clock) the seventh TIE dies at 1070, so this and S-98 guard W-06a.
TIE_LAST_LOSS_LIMIT = 1200
HIT_RATE_LIMIT = 0.95  # recorded: X-wings 75 of 86 shots, TIE fighters 152 of 163 (#607 shot log)
RECORDED_BOMBER_DEATHS = [723, 929, 1110]  # S-98: three of four bombers by tick 1200
BOMBER_DEATH_WINDOW = 100  # the remake (#607): 685, 1022, 1030
RECORDED_INTERCEPT_SHOTS = (162, 98)  # the X-wings' shots and their hits on the bombers
INTERCEPT_SHARE = 0.2  # the remake: 155 and 94
SCALE = float(1 << 24)
FAILURES: List[str] = []

Rows = Dict[str, Dict[Tuple[int, str], str]]


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def run(program: pathlib.Path, scenario: pathlib.Path, root: str, work: pathlib.Path, workers: int,
        contacts: bool = False) -> Dict[str, bytes]:
    stem = f"{scenario.stem[:4]}.w{workers}"
    outputs = {"trace": work / f"{stem}.csv", "hashes": work / f"{stem}.hashes.csv", "replay": work / f"{stem}.eawr-replay",
               "combat": work / f"{stem}.combat.csv"}
    completed = subprocess.run(
        [str(program), "--scenario", str(scenario), "--game-root", root, "--workers", str(workers),
         "--trace-out", str(outputs["trace"]), "--hash-out", str(outputs["hashes"]),
         "--replay-out", str(outputs["replay"]), "--combat-out", str(outputs["combat"])],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
        env=dict(os.environ, EAWR_PROJECTILE_CONTACT_DIAGNOSTICS="1") if contacts else None,
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


def craft_of(rows: Rows) -> List[str]:
    return sorted(name for name in rows if "." in name and "/" not in name)


def check_dogfight(path: pathlib.Path) -> Dict[str, float]:
    rows = load(path)
    crafts = craft_of(rows)
    check(any(name.startswith("xwing.") for name in crafts) and any(name.startswith("tie.") for name in crafts),
          f"S-97: both squadrons' craft are traced, got {crafts}")
    ticks = sorted({tick for name in crafts for (tick, field) in rows[name] if field == "alive"})
    heights: List[float] = []
    engaged: List[float] = []
    for tick in ticks:
        live: Dict[str, Tuple[float, float, float]] = {}
        for name in crafts:
            fields = rows[name]
            if fields.get((tick, "alive")) != "1":
                continue
            live[name] = tuple(int(fields[(tick, f"pos.{axis}")]) / SCALE for axis in "xyz")
        if tick >= 180:
            heights.extend(point[2] for point in live.values())
        sides = {name.split(".")[0] for name in live}
        if tick >= 210 and tick % 30 == 0 and len(sides) == 2:
            nearest = [min(math.dist(point, other) for label, other in live.items() if label.split(".")[0] != name.split(".")[0])
                       for name, point in live.items()]
            engaged.append(sum(nearest) / len(nearest))
    heights.sort()
    band = 0.0
    extreme = 0.0
    if heights:
        band = max(abs(heights[int(len(heights) * 0.05)]), abs(heights[int(len(heights) * 0.95)]))
        extreme = max(abs(heights[0]), abs(heights[-1]))
    check(band <= BAND_LIMIT, f"S-97 (FD-12): 90 % of craft heights within {BAND_LIMIT}, got {band:.1f} (recorded {RECORDED_BAND})")
    check(extreme <= EXTREME_LIMIT,
          f"S-97 (FD-12): the highest and lowest craft within {EXTREME_LIMIT}, got {extreme:.1f} (recorded {RECORDED_EXTREME})")
    mean_engaged = sum(engaged) / len(engaged) if engaged else float("inf")
    check(mean_engaged <= ENGAGED_LIMIT, f"S-97 (FD-06): the craft fight at close range, mean nearest enemy {mean_engaged:.1f}")
    return {"band": band, "extreme": extreme, "engaged": mean_engaged}


def check_outcome(path: pathlib.Path, combat: bytes) -> str:
    """FD-13: who dies when, and how the shots land, against the recording's losses and ticks."""
    rows = load(path)
    deaths: Dict[str, List[int]] = {"xwing": [], "tie": []}
    for name in craft_of(rows):
        ticks = sorted(tick for (tick, field), value in rows[name].items() if field == "alive" and value == "0")
        if ticks:
            deaths[name.split(".")[0]].append(ticks[0])
    shots: Dict[str, int] = collections.Counter()
    hits: Dict[str, int] = collections.Counter()
    for row in csv.DictReader(combat.decode("utf-8").splitlines()):
        if row["kind"] == "fired":
            shots[row["object"].split(".")[0]] += 1
        elif row["kind"] == "hit":
            hits[row["other"].split(".")[0]] += 1
    first = min(deaths["xwing"] + deaths["tie"], default=None)
    summary = (f"first loss at {first} (recorded {RECORDED_FIRST_LOSS}), X-wings lost {len(deaths['xwing'])} "
               f"{sorted(deaths['xwing'])} (recorded {RECORDED_LOSSES['xwing']}), TIE fighters lost {len(deaths['tie'])} "
               f"{sorted(deaths['tie'])} (recorded {RECORDED_LOSSES['tie']}); X-wing shots {shots['xwing']} hits "
               f"{hits['xwing']}, TIE shots {shots['tie']} hits {hits['tie']}")
    check(first is None or first >= FIRST_LOSS_LIMIT, f"S-97 (FD-13): no craft lost before tick {FIRST_LOSS_LIMIT}: {summary}")
    for side, (low, high) in LOSS_LIMITS.items():
        check(low <= len(deaths[side]) <= high, f"S-97 (FD-13): {side} losses within {low} to {high}: {summary}")
    if len(deaths["tie"]) == LOSS_LIMITS["tie"][1]:
        check(max(deaths["tie"]) >= TIE_LAST_LOSS_LIMIT,
              f"S-97 (W-06a): the last TIE fighter lost no earlier than tick {TIE_LAST_LOSS_LIMIT}: {summary}")
    for side in ("xwing", "tie"):
        rate = hits[side] / shots[side] if shots[side] else 0.0
        check(rate <= HIT_RATE_LIMIT, f"S-97 (FD-13): {side} hit rate {rate:.2f} above {HIT_RATE_LIMIT}: {summary}")
    return summary


def check_intercept(path: pathlib.Path, combat: bytes) -> str:
    """S-98: the X-wings shoot down the TIE bombers that attack the corvette (FD-04, FD-13)."""
    rows = load(path)
    deaths = sorted(min(tick for (tick, field), value in rows[name].items() if field == "alive" and value == "0")
                    for name in craft_of(rows) if name.startswith("bomber.")
                    and any(field == "alive" and value == "0" for (tick, field), value in rows[name].items()))
    shots = hits = live_redirects = post_death_hits = same_tick_hits = 0
    death_ticks = {name: min(tick for (tick, field), value in fields.items() if field == "alive" and value == "0")
                   for name, fields in rows.items() if name.startswith("bomber.")
                   and any(field == "alive" and value == "0" for (_, field), value in fields.items())}
    for row in csv.DictReader(combat.decode("utf-8").splitlines()):
        if row["kind"] == "fired" and row["object"].startswith("xwing."):
            shots += 1
        elif row["kind"] == "hit" and row["other"].startswith("xwing.") and row["object"].startswith("bomber."):
            hits += 1
            selected = row.get("selected_target") or ""
            check(selected.startswith("bomber."), "S-98 (DG-30): each bomber hit names its original selected target")
            if selected.startswith("bomber.") and selected != row["object"]:
                tick = int(row["tick"])
                check((tick, "alive") in rows.get(selected, {}), "S-98 (DG-30): selected-target lifetime is traced")
                if rows.get(selected, {}).get((tick, "alive")) == "1":
                    live_redirects += 1
                elif death_ticks.get(selected, tick) < tick:
                    # Native detach clears target links but lets the projectile fly on (DG-30g).
                    post_death_hits += 1
                else:
                    # A tick snapshot cannot order the selected target's death against this hit.
                    same_tick_hits += 1
    summary = (f"bombers lost {len(deaths)} {deaths} (recorded {RECORDED_BOMBER_DEATHS}); X-wing shots {shots} "
               f"hits on bombers {hits} (recorded 162 and 98); live-target redirects {live_redirects}, "
               f"post-death hits {post_death_hits}, same-tick death/hits {same_tick_hits}")
    check(live_redirects == 0, f"S-98 (DG-30): no redirect while the selected target is alive: {summary}")
    check(len(deaths) == len(RECORDED_BOMBER_DEATHS)
          and all(abs(ours - recorded) <= BOMBER_DEATH_WINDOW for ours, recorded in zip(deaths, RECORDED_BOMBER_DEATHS)),
          f"S-98 (FD-13): three bombers lost, each within {BOMBER_DEATH_WINDOW} ticks of the recording: {summary}")
    for ours, recorded, name in ((shots, RECORDED_INTERCEPT_SHOTS[0], "shots"), (hits, RECORDED_INTERCEPT_SHOTS[1], "hits")):
        check(abs(ours - recorded) <= recorded * INTERCEPT_SHARE,
              f"S-98 (W-06a): the X-wings' {name} within {INTERCEPT_SHARE:.0%} of the recording: {summary}")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    parser.add_argument("--keep", type=pathlib.Path, help="copy the one-worker trace here")
    args = parser.parse_args()
    root = os.environ.get("EAWR_EAW_GAME_ROOT")
    if not root:
        print("SKIPPED: dogfight scenario trace (set EAWR_EAW_GAME_ROOT)")
        return 0
    scenario = args.scenarios / f"{SCENARIO}.json"
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        runs = {workers: run(args.program, scenario, root, work, workers) for workers in (1, 2, 4, 8)}
        if all(runs.values()):
            for workers in (2, 4, 8):
                for kind in ("trace", "hashes", "replay", "header", "combat"):
                    check(runs[workers][kind] == runs[1][kind], f"S-97: {kind} differs with {workers} workers")
            trace_path = runs[1]["path"].decode("utf-8")
            if args.keep:
                args.keep.write_bytes(runs[1]["trace"])
            compared = subprocess.run(
                [sys.executable, str(args.tool), trace_path, trace_path, "--scenario", str(scenario)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
            )
            check(compared.returncode == 0, f"S-97: not a complete scenario trace: {compared.stdout}")
            stats = check_dogfight(pathlib.Path(trace_path))
            print("S-97: heights 90 % within {band:.1f} (recorded 100), "
                  "extreme {extreme:.1f} (recorded 173), mean nearest enemy {engaged:.1f}".format(**stats))
            outcome = check_outcome(pathlib.Path(trace_path), runs[1]["combat"])
            print("S-97 outcome: " + outcome)
        intercept = args.scenarios / f"{INTERCEPT}.json"
        pair = {workers: run(args.program, intercept, root, work, workers, contacts=True) for workers in (1, 8)}
        if all(pair.values()):
            for kind in ("trace", "hashes", "replay", "header", "combat"):
                check(pair[8][kind] == pair[1][kind], f"S-98: {kind} differs with 8 workers")
            print("S-98 outcome: " + check_intercept(pathlib.Path(pair[1]["path"].decode("utf-8")), pair[1]["combat"]))
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    if FAILURES:
        return 1
    print("dogfight scenario trace passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
