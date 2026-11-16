#!/usr/bin/env python3
"""Summarise an EAWR_GC_TICK_TRACE JSONL series (galactic live session, #6933).

Per phase: calls, p50/p95/max of the per-tick wall ms (inclusive) and the summed exclusive ms (the table sorts by it). Then the spike ticks (total above --spike-ms),
the interval between consecutive spikes in ticks and in wall seconds (a fixed interval points to a scheduled cycle,
an irregular one to allocation or a script timer), and which phase carried each spike.

Usage: python tools/performance/gc_tick_trace_summary.py <trace.jsonl> [--spike-ms 33] [--skip-ticks 0] [--markdown]
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def load(path: Path, skip_ticks: int) -> list[dict]:
    rows = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            print(f"skipping unparsable line {number}", file=sys.stderr)
            continue
        if row.get("tick", 0) > skip_ticks:
            rows.append(row)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--spike-ms", type=float, default=33.0)
    parser.add_argument("--skip-ticks", type=int, default=0, help="ignore ticks up to this number (load/warm-up)")
    parser.add_argument("--markdown", action="store_true")
    args = parser.parse_args()
    rows = load(args.trace, args.skip_ticks)
    if not rows:
        print("no rows", file=sys.stderr)
        return 1
    totals = [row["total_ms"] for row in rows]
    sources = [row["source_ms"] for row in rows]
    steps = [row["step_ms"] for row in rows]
    span_s = (rows[-1]["t_ms"] - rows[0]["t_ms"]) / 1000.0
    out = []
    out.append(f"ticks {len(rows)} (tick {rows[0]['tick']}..{rows[-1]['tick']}), wall {span_s:.1f} s, "
               f"{len(rows) / span_s:.1f} ticks/s" if span_s > 0 else f"ticks {len(rows)}")
    out.append(f"total ms p50 {percentile(totals, .5):.2f} p95 {percentile(totals, .95):.2f} p99 {percentile(totals, .99):.2f} "
               f"max {max(totals):.2f} mean {statistics.fmean(totals):.2f}")
    out.append(f"command-source p50 {percentile(sources, .5):.2f} max {max(sources):.2f}; "
               f"session-step p50 {percentile(steps, .5):.2f} p95 {percentile(steps, .95):.2f} max {max(steps):.2f}")

    per_phase: dict[str, list[float]] = {}
    exclusive_sum: dict[str, float] = {}
    calls: dict[str, int] = {}
    for row in rows:
        for group in ("phases", "cpu_summed"):
            for name, (count, inclusive, exclusive) in row.get(group, {}).items():
                key = name if group == "phases" else "cpu." + name
                per_phase.setdefault(key, []).append(inclusive)
                exclusive_sum[key] = exclusive_sum.get(key, 0.0) + exclusive
                calls[key] = calls.get(key, 0) + count
    header = ["phase", "ticks seen", "calls", "p50 ms", "p95 ms", "max ms", "sum ms", "sum excl ms"]
    table = []
    for name, values in per_phase.items():
        table.append([name, str(len(values)), str(calls[name]), f"{percentile(values, .5):.3f}",
                      f"{percentile(values, .95):.3f}", f"{max(values):.3f}", f"{sum(values):.1f}",
                      f"{exclusive_sum[name]:.1f}"])
    table.sort(key=lambda r: -float(r[7]))  # exclusive first: where the time is spent, not who encloses it
    out.append("")
    if args.markdown:
        out.append("| " + " | ".join(header) + " |")
        out.append("|" + "|".join(["---"] * len(header)) + "|")
        out.extend("| " + " | ".join(r) + " |" for r in table)
    else:
        out.append("  ".join(header))
        out.extend("  ".join(r) for r in table)

    spikes = [row for row in rows if row["total_ms"] >= args.spike_ms]
    out.append("")
    out.append(f"spikes (total >= {args.spike_ms:.0f} ms): {len(spikes)} of {len(rows)}")
    previous = None
    tick_gaps: list[int] = []
    wall_gaps: list[float] = []
    for row in spikes:
        # Rank by EXCLUSIVE time (v[2]): an enclosing scope such as tick_game_step always wins by inclusive time.
        top = sorted(((v[2], k) for k, v in row.get("phases", {}).items()), reverse=True)[:3]
        label = ", ".join(f"{k} {ms:.0f}" for ms, k in top) or "no gc_profile phase"
        gap = ""
        if previous is not None:
            tick_gaps.append(row["tick"] - previous["tick"])
            wall_gaps.append((row["t_ms"] - previous["t_ms"]) / 1000.0)
            gap = f"  +{tick_gaps[-1]} ticks / +{wall_gaps[-1]:.2f} s"
        out.append(f"  tick {row['tick']} t={row['t_ms'] / 1000.0:.2f}s total {row['total_ms']:.0f} "
                   f"(source {row['source_ms']:.1f}, step {row['step_ms']:.0f}): {label}{gap}")
        previous = row
    if tick_gaps:
        out.append(f"spike interval: ticks p50 {statistics.median(tick_gaps):.0f} min {min(tick_gaps)} max {max(tick_gaps)}; "
                   f"wall s p50 {statistics.median(wall_gaps):.2f} min {min(wall_gaps):.2f} max {max(wall_gaps):.2f}")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
