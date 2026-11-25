#!/usr/bin/env python3
"""Same-host A/B table for the per-frame perf traces of a remake_bench run made with --arm (#8058).

    python tools/performance/frame_ab_table.py <rig-run-dir> [--window-ticks A,B] [--control off] [--md out.md]

Reads <rig-run-dir>/<scene>--<arm>--<repeat>.csv (the --eawr-perf-trace files), keeps the frames inside the scene's tick
window (or --window-ticks) and prints, per arm: runs, frames, mean / median / p95 / p99 of wall_frame_ms, the mean
viewer_main_ms and outside_viewer_ms, and the per-run means in run order so a drift or order effect is visible. The last
block is the arm-minus-control difference of the per-run means. Measurement only: it reads files below the run directory.
"""
from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import statistics


def percentile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    position = (len(ordered) - 1) * q
    low, high = int(position), min(int(position) + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def read_run(path: pathlib.Path, window: tuple[int, int] | None) -> dict:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    kept = []
    for row in rows:
        try:
            tick = int(float(row["tick"]))
            frame_ms = float(row["wall_frame_ms"])
        except (KeyError, ValueError):
            continue
        if frame_ms <= 0.0:
            continue
        if window and not window[0] <= tick <= window[1]:
            continue
        kept.append(row)
    wall = [float(r["wall_frame_ms"]) for r in kept]
    main = [float(r["viewer_main_ms"]) for r in kept if r.get("viewer_main_ms")]
    outside = [float(r["outside_viewer_ms"]) for r in kept if r.get("outside_viewer_ms")]
    return dict(frames=len(wall), mean=statistics.fmean(wall) if wall else float("nan"),
                median=statistics.median(wall) if wall else float("nan"), p95=percentile(wall, 0.95),
                p99=percentile(wall, 0.99), main=statistics.fmean(main) if main else float("nan"),
                outside=statistics.fmean(outside) if outside else float("nan"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run_dir")
    parser.add_argument("--window-ticks", help="A,B (default: the window recorded in the run's spec)")
    parser.add_argument("--control", default="off")
    parser.add_argument("--md")
    args = parser.parse_args()
    root = pathlib.Path(args.run_dir)
    windows: dict[str, tuple[int, int]] = {}
    spec = next(iter(root.glob("_spec.json")), None)
    if spec:
        for run in json.loads(spec.read_text(encoding="utf-8-sig")).get("runs", []):
            window = run.get("benchmark", {}).get("window")
            if window and len(window) == 2:
                windows[run["name"]] = (int(window[0]), int(window[1]))
    forced = tuple(int(v) for v in args.window_ticks.split(",")) if args.window_ticks else None
    pattern = re.compile(r"^(?P<scene>.+)--(?P<arm>[^-]+(?:-[^-]+)*)--(?P<n>\d+)\.csv$")
    arms: dict[str, list[tuple[int, dict]]] = {}
    for path in sorted(root.glob("*.csv")):
        match = pattern.match(path.name)
        if not match or path.name.endswith((".movers.csv", ".render.csv", ".old.csv", "-diag.csv")):
            continue
        name = path.stem
        arms.setdefault(match["arm"], []).append((int(match["n"]), read_run(path, forced or windows.get(name))))
    lines = ["| arm | runs | frames | mean ms | median | p95 | p99 | main ms | outside ms | per-run means (repeat order) |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    means: dict[str, float] = {}
    for arm, runs in arms.items():
        runs.sort()
        frames = sum(r["frames"] for _, r in runs)
        mean_of_means = statistics.fmean(r["mean"] for _, r in runs)
        means[arm] = mean_of_means
        lines.append(f"| {arm} | {len(runs)} | {frames} | {mean_of_means:.2f} | "
                     f"{statistics.fmean(r['median'] for _, r in runs):.2f} | {statistics.fmean(r['p95'] for _, r in runs):.2f} | "
                     f"{statistics.fmean(r['p99'] for _, r in runs):.2f} | {statistics.fmean(r['main'] for _, r in runs):.2f} | "
                     f"{statistics.fmean(r['outside'] for _, r in runs):.2f} | "
                     + ", ".join(f"{r['mean']:.2f}" for _, r in runs) + " |")
    if args.control in means:
        lines.append("")
        for arm, value in means.items():
            if arm != args.control:
                lines.append(f"{arm} minus {args.control}: {value - means[args.control]:+.2f} ms "
                             f"({(value / means[args.control] - 1) * 100:+.1f} %)")
    text = "\n".join(lines)
    print(text)
    if args.md:
        pathlib.Path(args.md).write_text(text + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
