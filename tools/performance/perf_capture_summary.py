#!/usr/bin/env python3
"""Summarise a perf telemetry feed or capture (#8058), or an EAWR_GC_TICK_TRACE series (#6933).

Reads one of
  * a telemetry feed: JSON Lines, the files a run with --eawr-telemetry <dir> writes (docs/performance/perf-feed.md);
  * a capture: the Chrome trace JSON that Shift+F3 writes into <dir>/captures/;
  * a GC tick trace: the older JSONL of EAWR_GC_TICK_TRACE (galactic live session only).
Several feed segments may be given; they are read in the order named.

Prints, per simulation source (tactical, galactic): tick percentiles and delivered tick rate, the frame percentiles (when
the file has frames), the hitch list with the spacing to the previous hitch (ticks and wall seconds) and the phase that
carries the excess, the repeating-pattern readout (spacing, tick-locked or wall-locked fold, dominant phase, marker
match) and the verdict of the core rules (tick-speed against frame-time). The per-phase table of a GC trace is kept.
The pattern and verdict are computed here from the records with the same rules the game's analyzer uses
(include/eawr/core/perf_diag/pattern.hpp and verdict.hpp); the feed's own recorded lines are printed beside them.

Usage: python tools/performance/perf_capture_summary.py <feed.jsonl|capture.json|trace.jsonl> [more segments]
           [--spike-ms N] [--skip-ticks 0] [--markdown] [--frame-budget-ms 16.7]
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------- the model


@dataclass
class Tick:
    n: int
    t_us: int
    ms: float
    lag: float = 0.0
    lock: float = 0.0
    cpu: float = -1.0
    iv: float = 33.3
    flags: int = 0
    phases: dict = field(default_factory=dict)  # name -> ms


@dataclass
class Frame:
    n: int
    t_us: int
    ms: float
    main: float = -1.0
    render: float = -1.0
    gpu: float = -1.0
    lock: float = 0.0
    stale: bool = False
    fight: dict = field(default_factory=dict)  # u, eu, atk, proj, fx, part, dc, obj (empty when the frame carried none)


@dataclass
class Source:
    label: str
    workers: int = 0
    ticks: list = field(default_factory=list)
    markers: list = field(default_factory=list)  # (kind, tick)
    hitches: list = field(default_factory=list)  # recorded by the game: dicts
    verdicts: list = field(default_factory=list)  # recorded text lines
    patterns: list = field(default_factory=list)


@dataclass
class Capture:
    kind: str = "feed"  # feed | chrome | gc-trace
    header: dict = field(default_factory=dict)
    sources: dict = field(default_factory=dict)  # id -> Source
    frames: list = field(default_factory=list)
    frame_hitches: list = field(default_factory=list)
    pc: list = field(default_factory=list)
    contexts: list = field(default_factory=list)  # `context` records, oldest first
    threads: list = field(default_factory=list)  # `thr` records: {"t", "th": [[name, share, labeled], ...]}
    mem: list = field(default_factory=list)  # `mem` records
    dropped: dict = field(default_factory=dict)  # src -> last total
    gc_phase_table: list = field(default_factory=list)  # (name, per-tick inclusive list, exclusive sum, calls)
    notes: list = field(default_factory=list)

    def source(self, ident) -> Source:
        return self.sources.setdefault(ident, Source(label=str(ident)))


# ---------------------------------------------------------------- loading


def percentile(values, fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def _json_lines(path: Path):
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            yield json.loads(line)
        except json.JSONDecodeError:
            print(f"skipping unparsable line {number} of {path.name}", file=sys.stderr)


def load_feed(paths, skip_ticks: int) -> Capture:
    capture = Capture(kind="feed")
    names: dict = {}  # (src, id) -> name
    for path in paths:
        for row in _json_lines(path):
            kind = row.get("r")
            src = row.get("src")
            if kind == "header":
                capture.header = capture.header or row
            elif kind == "source":
                capture.source(src).label = row.get("label", str(src))
                capture.source(src).workers = row.get("workers", capture.source(src).workers)
            elif kind == "phase-name":
                names[(src, row["id"])] = row["name"]
            elif kind == "tick":
                if row["n"] <= skip_ticks:
                    continue
                phases = {names.get((src, i), f"phase{i}"): ms for i, ms in row.get("ph", [])}
                capture.source(src).ticks.append(Tick(row["n"], row["t"], row["ms"], row.get("lag", 0.0), row.get("lock", 0.0),
                                                      row.get("cpu", -1.0), row.get("iv", 33.3), row.get("fl", 0), phases))
            elif kind == "marker":
                if not row.get("fs"):
                    capture.source(src).markers.append((row["k"], row["n"]))
            elif kind == "hitch":
                if row.get("kind") == "frame":
                    capture.frame_hitches.append(row)
                else:
                    capture.source(src).hitches.append(row)
            elif kind == "frame":
                fight = {k: row[k] for k in ("u", "eu", "atk", "proj", "fx", "part", "dc", "obj") if k in row}
                capture.frames.append(Frame(row["n"], row["t"], row["ms"], row.get("main", -1.0), row.get("render", -1.0),
                                            row.get("gpu", -1.0), row.get("lock", 0.0), bool(row.get("stale")), fight))
            elif kind == "pc":
                capture.pc.append(row)
            elif kind == "context":
                if not capture.contexts or any(capture.contexts[-1].get(k) != v for k, v in row.items() if k != "t"):
                    capture.contexts.append(row)
            elif kind == "thr":
                capture.threads.append(row)
            elif kind == "mem":
                capture.mem.append(row)
            elif kind == "verdict":
                capture.source(src).verdicts.append(row.get("text", ""))
            elif kind == "pattern":
                capture.source(src).patterns.append(row.get("text", ""))
            elif kind == "dropped":
                capture.dropped[src] = row.get("total", 0)
    return capture


def load_chrome(path: Path, skip_ticks: int) -> Capture:
    data = json.loads(path.read_text(encoding="utf-8"))
    events = data["traceEvents"] if isinstance(data, dict) else data
    other = data.get("otherData", {}) if isinstance(data, dict) else {}
    capture = Capture(kind="chrome", header={"schema": other.get("schema"), "build": other.get("build"), "engine": other.get("engine")})
    track_label: dict = {}
    for event in events:
        if event.get("ph") == "M" and event.get("name") == "thread_name":
            track_label[event["tid"]] = event["args"]["name"]
    by_tick: dict = {}
    for event in events:
        phase, tid = event.get("ph"), event.get("tid", 0)
        label = track_label.get(tid, "")
        group = tid // 10000 if tid >= 10000 else None
        if phase == "X" and event.get("name") == "tick" and group is not None:
            args = event.get("args", {})
            number = args.get("tick", 0)
            if number <= skip_ticks:
                continue
            source = capture.source(group - 1)
            source.label = label.replace("ticks: ", "", 1) or source.label
            tick = Tick(number, int(event["ts"] + event["dur"]), event["dur"] / 1000.0, args.get("lag_ms", 0.0), 0.0, args.get("cpu", -1.0))
            source.ticks.append(tick)
            by_tick[(group - 1, number)] = tick
        elif phase == "X" and group is not None and event.get("name") != "tick":
            number = event.get("args", {}).get("tick")
            tick = by_tick.get((group - 1, number))
            if tick is not None:
                tick.phases[event["name"]] = event["dur"] / 1000.0
        elif phase == "i" and group is not None and event.get("name") != "hitch":
            capture.source(group - 1).markers.append((event["name"], event.get("args", {}).get("tick", 0)))
        elif phase == "X" and event.get("name") == "frame":
            args = event.get("args", {})
            capture.frames.append(Frame(args.get("frame", 0), int(event["ts"] + event["dur"]), event["dur"] / 1000.0,
                                        args.get("main_ms", -1.0), args.get("render_cpu_ms", -1.0), args.get("gpu_ms", -1.0),
                                        args.get("ui_lock_ms", 0.0)))
    for source in capture.sources.values():
        source.ticks.sort(key=lambda t: t.n)
    capture.frames.sort(key=lambda f: f.n)
    if other.get("pattern"):
        capture.notes.append("recorded pattern: " + other["pattern"])
    return capture


def load_gc_trace(rows: list, skip_ticks: int) -> Capture:
    capture = Capture(kind="gc-trace")
    source = capture.source("galactic")
    source.label = "galactic"
    per_phase: dict = {}
    exclusive: dict = {}
    calls: dict = {}
    for row in rows:
        if row.get("tick", 0) <= skip_ticks:
            continue
        phases = {}
        for group in ("phases", "cpu_summed"):
            for name, (count, inclusive, excl) in row.get(group, {}).items():
                key = name if group == "phases" else "cpu." + name
                per_phase.setdefault(key, []).append(inclusive)
                exclusive[key] = exclusive.get(key, 0.0) + excl
                calls[key] = calls.get(key, 0) + count
                if group == "phases":
                    phases[name] = excl  # exclusive: an enclosing scope always wins by inclusive time
        source.ticks.append(Tick(row["tick"], int(row["t_ms"] * 1000), row["total_ms"], phases=phases,
                                 iv=33.3))
        source.ticks[-1].phases["command-source"] = row.get("source_ms", 0.0)
        source.ticks[-1].phases["session-step"] = row.get("step_ms", 0.0)
    capture.gc_phase_table = [(name, values, exclusive[name], calls[name]) for name, values in per_phase.items()]
    return capture


def load(paths, skip_ticks: int) -> Capture:
    first = paths[0]
    text = first.read_text(encoding="utf-8")
    head = text.lstrip()[:200]
    if head.startswith('{"traceEvents"') or head.startswith("[") or '"traceEvents"' in head:
        return load_chrome(first, skip_ticks)
    if '"r":"header"' in head or '"r": "header"' in head or '"r":"' in head:
        return load_feed(paths, skip_ticks)
    rows = [row for row in _json_lines(first)]
    if rows and "source_ms" in rows[0]:
        return load_gc_trace(rows, skip_ticks)
    raise SystemExit(f"{first}: not a telemetry feed, a capture or a GC tick trace")


# ---------------------------------------------------------------- hitches (the game's rule, design 3.1)


class RollingMedian:
    def __init__(self, size: int = 256):
        self.size, self.values = size, []

    def push(self, value: float):
        self.values.append(value)
        if len(self.values) > self.size:
            self.values.pop(0)

    def median(self) -> float:
        return statistics.median(self.values) if self.values else 0.0


def detect_hitches(ticks: list, spike_ms: float | None = None) -> list:
    """Hitch rows: dict(tick, t_us, ms, median, phase, excess, gap_ticks, gap_s). The dominant phase is the phase with
    the largest excess over its own median of the last 256 ticks (the biggest phase is always the same one)."""
    window = RollingMedian()
    phase_windows: dict = {}
    hitches = []
    for tick in ticks:
        median = window.median()
        warm = len(window.values) >= 32
        limit = max(2 * median, median + 5.0, 4.0)
        if spike_ms is not None:
            hit = tick.ms >= spike_ms
        else:
            hit = bool(tick.flags & 1) or (warm and (tick.ms > limit or (tick.iv > 0 and tick.lag > tick.iv)))
        best_name, best_excess = "", -math.inf
        for name, ms in tick.phases.items():
            values = phase_windows.setdefault(name, RollingMedian()).values
            excess = ms - statistics.median(values) if len(values) >= 16 else 0.0
            if excess > best_excess:
                best_name, best_excess = name, excess
        if hit:
            if best_excess <= 0 and tick.phases:  # no warm median yet: the largest phase is the best available guess
                best_name = max(tick.phases, key=tick.phases.get)
                best_excess = tick.phases[best_name]
            hitches.append(dict(tick=tick.n, t_us=tick.t_us, ms=tick.ms, median=median, phase=best_name,
                                excess=max(best_excess, 0.0)))
        window.push(tick.ms)
        for name, ms in tick.phases.items():
            phase_windows.setdefault(name, RollingMedian()).push(ms)
    for previous, current in zip(hitches, hitches[1:]):
        current["gap_ticks"] = current["tick"] - previous["tick"]
        current["gap_s"] = (current["t_us"] - previous["t_us"]) / 1e6
    return hitches


# ---------------------------------------------------------------- pattern readout (pattern.hpp)

CADENCE = [(3, "gc ai planning/execution (aligned)"), (4, "capture-point progress"), (16, "fog-grid decay"),
           (30, "one-second services (damage rate, lua ServiceRate, scans)"), (90, "shield/energy recharge, gc hints"),
           (150, "attack positioning, mp hash exchange"), (300, "gc ai perception/learning (aligned)"),
           (900, "gc gambling evaluation"), (1350, "fiscal cycle (galactic day)")]
MIN_OCCURRENCES, DOMINANT_SHARE, FOLD_SHARE, GAP_SLACK_TICKS, GAP_SLACK_RATIO, MARKER_SHARE = 4, 0.60, 0.70, 2, 0.03, 0.50
LUA_MARKERS = ("lua.memory-walk", "lua.collect")


def _share_of(total: int, share: float) -> int:
    return math.ceil(total * share - 1e-9)


def _dominant_gap(gaps: list, slack_floor: int):
    ordered = sorted(gaps)
    best_count, best_mid, best_gap = 0, 0, 0
    for candidate in sorted(set(g for g in ordered if g > 0)):
        slack = max(slack_floor, int(candidate * GAP_SLACK_RATIO))
        inside = [g for g in ordered if candidate - slack <= g <= candidate + slack]
        if len(inside) > best_count:
            best_count, best_mid, best_gap = len(inside), inside[len(inside) // 2], candidate
    valid = best_count >= MIN_OCCURRENCES and best_count >= _share_of(len(ordered), DOMINANT_SHARE)
    return valid, (best_mid if best_count else best_gap), best_count


def _fold(values: list, period: int, slack: int):
    best_locked, best_centre = 0, 0
    residues = sorted(v % period for v in values)
    for centre in set(residues):
        count = 0
        for r in residues:
            d = abs(r - centre)
            if min(d, period - d) <= slack:
                count += 1
        if count > best_locked:
            best_locked, best_centre = count, centre
    return best_locked, best_centre


def _refine(valid: bool, gap: int, span: int) -> int:
    if not valid or gap == 0 or span < gap:
        return gap
    cycles = (span + gap // 2) // gap
    return gap if cycles == 0 else (span + cycles // 2) // cycles


def analyze_pattern(hitches: list, markers: list) -> dict:
    """The readout of the core analyzer for the last 256 hitches. `markers` = [(kind, tick)]."""
    samples = hitches[-256:]
    out = dict(kind="none", hitches=len(samples), period=0, phase=0, locked=0, speed_ambiguous=False, cadence=None,
               phase_name="", phase_count=0, phase_excess=0.0, marker=None, marker_count=0, allocation_driven=False)
    by_tick: dict = {}
    for kind, tick in markers:
        by_tick.setdefault(tick, set()).add(kind)
    counts: dict = {}
    for sample in samples:
        kinds = set()
        for tick in range(sample["tick"] - 2, sample["tick"] + 3):
            kinds |= by_tick.get(tick, set())
        for kind in kinds:
            counts[kind] = counts.get(kind, 0) + 1
    phases: dict = {}
    for sample in samples:
        if sample["phase"]:
            phases.setdefault(sample["phase"], []).append(sample["excess"])
    if phases:
        name = max(phases, key=lambda n: (len(phases[n]), sum(phases[n]) / len(phases[n])))
        out.update(phase_name=name, phase_count=len(phases[name]), phase_excess=sum(phases[name]) / len(phases[name]))
    if counts:
        kind = max(sorted(counts), key=lambda k: counts[k])
        if counts[kind] >= _share_of(len(samples), MARKER_SHARE):
            out.update(marker=kind, marker_count=counts[kind])
    if len(samples) < MIN_OCCURRENCES + 1:
        return out
    ticks = [s["tick"] for s in samples]
    walls = [s["t_us"] // 1000 for s in samples]
    tick_gaps = [b - a for a, b in zip(ticks, ticks[1:])]
    wall_gaps = [b - a for a, b in zip(walls, walls[1:])]
    t_valid, t_gap, _ = _dominant_gap(tick_gaps, GAP_SLACK_TICKS)
    w_valid, w_gap, _ = _dominant_gap(wall_gaps, 1)
    tick_period = _refine(t_valid, t_gap, ticks[-1] - ticks[0])
    wall_period = _refine(w_valid, w_gap, walls[-1] - walls[0])
    candidates = []
    min_fold = lambda slack: 3 * (2 * slack + 1)
    for period in ([tick_period, tick_period // 2, tick_period * 2] if t_valid else []) + [c[0] for c in CADENCE]:
        if period >= min_fold(1) and period not in candidates:
            candidates.append(period)
    tick_best = (0, 0, 0)  # locked, period, centre
    for period in candidates:
        locked, centre = _fold(ticks, period, 1)
        if locked > tick_best[0]:
            tick_best = (locked, period, centre)
    tick_ok = tick_best[1] != 0 and tick_best[0] >= _share_of(len(samples), FOLD_SHARE) and tick_best[0] >= MIN_OCCURRENCES
    wall_best, wall_ok = (0, 0, 0), False
    if w_valid:
        slack = max(1, int(wall_period * GAP_SLACK_RATIO))
        if wall_period >= min_fold(slack):
            locked, centre = _fold(walls, wall_period, slack)
            wall_best = (locked, wall_period, centre)
        wall_ok = wall_best[1] != 0 and wall_best[0] >= _share_of(len(samples), FOLD_SHARE)
    if tick_ok:
        out.update(kind="tick-locked", locked=tick_best[0], period=tick_best[1], phase=tick_best[2], speed_ambiguous=wall_ok)
        for period, what in CADENCE:
            if period == tick_best[1]:
                out["cadence"] = what
    elif wall_ok:
        out.update(kind="wall-locked", locked=wall_best[0], period=wall_best[1], phase=wall_best[2])
    out["allocation_driven"] = out["kind"] == "none" and out["marker"] in LUA_MARKERS
    return out


def format_pattern(p: dict) -> str:
    if p["kind"] == "none":
        line = f"pattern: none ({p['hitches']} hitches, irregular)"
        if p["allocation_driven"]:
            line += ", allocation-driven (lua memory)"
    else:
        unit = "ticks" if p["kind"] == "tick-locked" else "ms"
        line = f"pattern: every {p['period']} {unit} ({p['locked']}/{p['hitches']}, {p['kind']}, phase {p['phase']})"
        if p["speed_ambiguous"]:
            line += ", constant speed: change speed to tell tick- from wall-locked"
        if p["cadence"]:
            line += ", cadence " + p["cadence"]
    if p["phase_name"]:
        line += f", {p['phase_name']} +{p['phase_excess']:.1f} ms"
    if p["marker"]:
        line += f", marker {p['marker']} {p['marker_count']}/{p['hitches']}"
    return line


# ---------------------------------------------------------------- verdict (verdict.hpp)


def evaluate_verdict(stats: dict) -> str:
    """Rules 0..10 of design 2.7, first match wins in the sim group and in the frame group. Missing inputs are None."""
    if stats.get("idle"):
        return "idle"
    sim_slow = stats["delivered"] < 0.95
    frame_slow = stats["frame_p95"] > stats["budget"]
    if not sim_slow and not frame_slow:
        return "ok"
    load = stats["tick_p95"] / stats["interval"] if stats["interval"] > 0 else 0.0
    w_sim = stats["lock_ms"] / stats["mean_tick"] if stats["mean_tick"] > 0 else 0.0
    w_ui = stats["ui_lock_ms"] / stats["mean_frame"] if stats["mean_frame"] > 0 else 0.0
    cpu, others = stats.get("cpu"), stats.get("others")
    sim_rule, sim_text, sim_unknown = 0, "", False
    if sim_slow:
        phase = stats.get("top_phase") or "tick"
        if load >= 0.9 and cpu is not None and cpu >= 0.8:
            sim_rule, sim_text = 2, f"sim-bound: {phase} {stats.get('top_ms', 0.0):.1f} ms"
        elif w_sim >= 0.2:
            sim_rule, sim_text = 3, f"coupled: sim waits on frame (session lock {stats['lock_ms']:.1f} ms)"
        elif cpu is not None and cpu < 0.6 and others is not None and others >= 0.25:
            sim_rule, sim_text = 4, f"external: other processes {others * 100:.0f} %"
        elif cpu is not None and cpu < 0.6:
            sim_rule, sim_text = 5, "coupled: sim thread starved of CPU (own threads)"
        elif cpu is not None:
            sim_rule, sim_text = 6, "sim-bound (unattributed)"
        else:
            sim_unknown = True
    frame_rule, frame_text = 0, ""
    if frame_slow:
        gpu, render, p95 = stats.get("gpu"), stats.get("render"), stats["frame_p95"]
        if gpu is not None and gpu >= 0.85 * p95:
            frame_rule, frame_text = 7, f"render-bound: GPU {gpu:.1f} ms"
        elif render is not None and render >= 0.85 * p95:
            frame_rule, frame_text = 8, f"render-bound: render thread {render:.1f} ms"
        elif w_ui >= 0.2:
            frame_rule, frame_text = 9, f"coupled: frame waits on sim (lock {stats['ui_lock_ms']:.1f} ms)"
        else:
            main = stats.get("main") or 0.0
            frame_rule, frame_text = 10, f"render-bound: main thread (viewer {main:.1f} ms, engine not measured)"
    if sim_unknown and frame_rule == 0:
        line = "unknown: sim slow, on-CPU share not measured"
    elif sim_rule and frame_rule:
        coupled = sim_rule == 3 or frame_rule == 9 or stats.get("correlation", 0) > 0.5 or stats.get("coincidence", 0) > 0.5
        line = ("coupled: " if coupled else "sim-bound + render-bound (independent): ") + sim_text + "; " + frame_text
    elif sim_rule:
        line = sim_text
    else:
        line = frame_text
    if sim_unknown and not line.startswith("unknown"):
        line += "; sim slow, cause unknown (on-CPU share not measured)"
    if sim_slow and stats.get("stale", 0) > 0.10:
        line += " (picture holds)"
    return line


def window_stats(source: Source, frames: list, pc: list, budget: float, last_s: float = 5.0) -> dict:
    """The stats of the last `last_s` seconds of the series (the game's window)."""
    if len(source.ticks) < 8:
        return dict(idle=True)
    end = source.ticks[-1].t_us
    window = [t for t in source.ticks if t.t_us + last_s * 1e6 >= end]
    span = (window[-1].t_us - window[0].t_us) / 1e6
    interval = window[-1].iv
    if len(window) < 8 or span < 1.0 or interval <= 0:
        return dict(idle=True)
    totals = [t.ms for t in window]
    cpus = [min(1.0, t.cpu) for t in window if t.cpu >= 0]
    p95 = percentile(totals, 0.95)
    tally: dict = {}
    for tick in window:
        if tick.ms < p95 or not tick.phases:
            continue
        name = max(tick.phases, key=tick.phases.get)
        tally.setdefault(name, []).append(tick.phases[name])
    top = max(tally, key=lambda n: len(tally[n])) if tally else ""
    stats = dict(idle=False, delivered=(len(window) - 1) / span / (1000.0 / interval), interval=interval, tick_p95=p95,
                 mean_tick=sum(totals) / len(totals), lock_ms=sum(t.lock for t in window) / len(window),
                 cpu=(sum(cpus) / len(cpus) if len(cpus) >= len(window) / 2 else None), top_phase=top,
                 top_ms=(sum(tally[top]) / len(tally[top]) if top else 0.0), budget=budget, frame_p95=0.0, mean_frame=0.0,
                 ui_lock_ms=0.0, stale=0.0, gpu=None, render=None, main=None, others=None)
    recent = [f for f in frames if f.t_us + last_s * 1e6 >= frames[-1].t_us] if frames else []
    if len(recent) >= 8:
        stats["frame_p95"] = percentile([f.ms for f in recent], 0.95)
        stats["mean_frame"] = sum(f.ms for f in recent) / len(recent)
        stats["ui_lock_ms"] = sum(f.lock for f in recent) / len(recent)
        stats["stale"] = sum(1 for f in recent if f.stale) / len(recent)
        for key, attr in (("main", "main"), ("render", "render"), ("gpu", "gpu")):
            values = [getattr(f, attr) for f in recent if getattr(f, attr) >= 0]
            stats[key] = sum(values) / len(values) if values else None
    if pc:
        others = pc[-1].get("others", -1.0)
        stats["others"] = others if others is not None and others >= 0 else None
    return stats


# ---------------------------------------------------------------- report


def _table(header: list, rows: list, markdown: bool) -> list:
    if markdown:
        return ["| " + " | ".join(header) + " |", "|" + "|".join(["---"] * len(header)) + "|"] + ["| " + " | ".join(r) + " |" for r in rows]
    return ["  ".join(header)] + ["  ".join(r) for r in rows]


def correlation(xs: list, ys: list) -> float | None:
    """Pearson correlation; None when either series is constant or shorter than 3."""
    if len(xs) < 3 or len(xs) != len(ys):
        return None
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if sx == 0 or sy == 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)


def context_lines(capture: Capture) -> list:
    """The PC and settings context (the latest `context` record) in a few readable lines."""
    if not capture.contexts:
        return []
    c = capture.contexts[-1]
    out = ["", "== context"]
    p, e = c.get("p_cores"), c.get("e_cores")
    split = f" ({p} performance + {e} efficiency cores)" if e else ""
    out.append(f"cpu: {c.get('cpu') or '?'}, {c.get('cores', '?')} cores / {c.get('logical', '?')} logical processors{split}"
               + ("" if c.get("topology", True) else " (topology not read)"))
    if c.get("gpu"):
        out.append(f"gpu: {c['gpu']} ({c.get('gpu_type', '?')}), driver {c.get('gpu_driver') or '?'}, api {c.get('gpu_api') or '?'}, "
                   f"{c.get('renderer', '?')} on {c.get('rd_driver', '?')}")
    res = c.get("res")
    out.append(f"display: {'x'.join(str(v) for v in res) if res else '?'} {c.get('window', '?')}, vsync {c.get('vsync', '?')}, "
               f"fps cap {c.get('fps_cap', '?')}, render thread {c.get('render_thread', '?')} (setting {c.get('render_thread_setting', '?')})")
    video = c.get("video")
    if video:
        out.append("video: " + ", ".join(f"{k} {v}" for k, v in video.items()))
    threads = c.get("threads")
    if threads:
        parts = [f"hardware {threads.get('hardware', '?')}"]
        for name in ("sim", "particles"):
            pool = threads.get(name)
            if pool:
                parts.append(f"{name} {pool['resolved']} ({pool['mode']}, automatic would be {pool['automatic']})")
        out.append("threads: " + ", ".join(parts))
    workers = [f"{s.label} {s.workers}" for s in capture.sources.values() if s.workers]
    if workers:
        out.append("simulation workers in use: " + ", ".join(workers))
    return out


def thread_lines(capture: Capture, markdown: bool) -> list:
    """Per-thread CPU: each thread mean and max share of one core over the capture (1.0 = one thread-equivalent)."""
    if not capture.threads:
        return []
    shares: dict = {}
    labeled: dict = {}
    for row in capture.threads:
        for name, share, is_labeled in row.get("th", []):
            shares.setdefault(name, []).append(share)
            labeled[name] = labeled.get(name, False) or bool(is_labeled)
    seconds = len(capture.threads)
    rows = []
    for name, values in shares.items():
        # A second without a row means the thread was below the reporting floor or not alive: count it as 0.
        padded = values + [0.0] * max(0, seconds - len(values))
        rows.append((statistics.fmean(padded), name, max(values), percentile(padded, 0.95), labeled[name]))
    rows.sort(reverse=True)
    total = [sum(share for _, share, _ in row.get("th", [])) for row in capture.threads]
    out = ["", f"== threads (cpu share of one core, {seconds} s; labeled = named by the game)"]
    out.extend(_table(["thread", "mean", "p95", "max", "labeled"],
                      [[n, f"{m:.2f}", f"{p95:.2f}", f"{mx:.2f}", "yes" if lab else "no"] for m, n, mx, p95, lab in rows[:25]], markdown))
    out.append(f"sum of listed threads: mean {statistics.fmean(total):.2f} cores, max {max(total):.2f}")
    named = [r for r in rows if r[4]]
    if named:
        out.append(f"hottest labeled thread: {named[0][1]} mean {named[0][0]:.2f}, max {named[0][2]:.2f}")
    return out


def mem_lines(capture: Capture) -> list:
    if not capture.mem:
        return []
    commit = [m["commit_mb"] for m in capture.mem]
    ws = [m["ws_mb"] for m in capture.mem]
    faults = [m["pf_s"] for m in capture.mem if m.get("pf_s", -1) >= 0]
    out = ["", f"== memory ({len(capture.mem)} s)",
           f"commit MiB: first {commit[0]}, last {commit[-1]}, max {max(commit)}; working set MiB: first {ws[0]}, last {ws[-1]}, max {max(ws)}"]
    if faults:
        out.append(f"page faults per s: p50 {percentile(faults, .5):.0f} p95 {percentile(faults, .95):.0f} max {max(faults):.0f}")
    return out


def fight_lines(capture: Capture, markdown: bool) -> list:
    """Fight intensity per frame and its link to the frame time: correlation and frame time by intensity quartile."""
    frames = [f for f in capture.frames if f.fight]
    if len(frames) < 8:
        return []
    out = ["", f"== fight intensity ({len(frames)} frames)"]
    keys = [("u", "own units"), ("eu", "enemy units"), ("atk", "attacking"), ("proj", "projectiles"), ("fx", "effects"),
            ("part", "particles"), ("dc", "draw calls"), ("obj", "visible instances")]
    rows = []
    for key, label in keys:
        values = [f.fight.get(key, 0) for f in frames]
        r = correlation([float(v) for v in values], [f.ms for f in frames])
        rows.append([label, f"{percentile(values, .5):.0f}", f"{percentile(values, .95):.0f}", f"{max(values)}",
                     "-" if r is None else f"{r:+.2f}"])
    out.extend(_table(["counter", "p50", "p95", "max", "corr with frame ms"], rows, markdown))
    # Frame time by intensity: the frames split into four equal groups by (units + enemy units + projectiles + particles / 10).
    def score(f):
        d = f.fight
        return d.get("u", 0) + d.get("eu", 0) + d.get("proj", 0) + d.get("part", 0) / 10.0
    ordered = sorted(frames, key=score)
    quarter = max(1, len(ordered) // 4)
    groups = [ordered[i * quarter:(i + 1) * quarter if i < 3 else len(ordered)] for i in range(4)]
    out.append("")
    out.append("frame time by fight intensity (score = units + projectiles + particles / 10, quartiles of frames):")
    table = []
    for i, group in enumerate(groups, 1):
        ms = [f.ms for f in group]
        mains = [f.main for f in group if f.main >= 0]
        renders = [f.render for f in group if f.render >= 0]
        gpus = [f.gpu for f in group if f.gpu >= 0]
        table.append([f"Q{i}", f"{score(group[0]):.0f}-{score(group[-1]):.0f}", f"{statistics.fmean(ms):.1f}", f"{percentile(ms, .95):.1f}",
                      f"{statistics.fmean(mains):.1f}" if mains else "-", f"{statistics.fmean(renders):.1f}" if renders else "-",
                      f"{statistics.fmean(gpus):.1f}" if gpus else "-"])
    out.extend(_table(["quartile", "score", "frame mean", "frame p95", "main", "render", "gpu"], table, markdown))
    return out


def report(capture: Capture, spike_ms: float | None, markdown: bool, budget: float) -> list:
    out = []
    if capture.header:
        out.append(f"{capture.kind}: schema {capture.header.get('schema')}, build {capture.header.get('build') or '?'}, "
                   f"engine {capture.header.get('engine') or '?'}")
    out.extend(context_lines(capture))
    if capture.kind == "gc-trace" and spike_ms is None:
        spike_ms = 33.0  # the old tool's default
    for ident, source in capture.sources.items():
        if not source.ticks:
            continue
        ticks = source.ticks
        totals = [t.ms for t in ticks]
        span_s = (ticks[-1].t_us - ticks[0].t_us) / 1e6
        rate = f", {len(ticks) / span_s:.1f} ticks/s" if span_s > 0 else ""
        out.append("")
        out.append(f"== {source.label}: ticks {len(ticks)} (tick {ticks[0].n}..{ticks[-1].n}), wall {span_s:.1f} s{rate}")
        out.append(f"tick ms p50 {percentile(totals, .5):.2f} p95 {percentile(totals, .95):.2f} p99 {percentile(totals, .99):.2f} "
                   f"max {max(totals):.2f} mean {statistics.fmean(totals):.2f}")
        if capture.kind == "gc-trace":
            sources = [t.phases.get("command-source", 0.0) for t in ticks]
            steps = [t.phases.get("session-step", 0.0) for t in ticks]
            out.append(f"command-source p50 {percentile(sources, .5):.2f} max {max(sources):.2f}; "
                       f"session-step p50 {percentile(steps, .5):.2f} p95 {percentile(steps, .95):.2f} max {max(steps):.2f}")
        if capture.gc_phase_table:
            rows = [[n, str(len(v)), str(c), f"{percentile(v, .5):.3f}", f"{percentile(v, .95):.3f}", f"{max(v):.3f}",
                     f"{sum(v):.1f}", f"{e:.1f}"] for n, v, e, c in capture.gc_phase_table]
            rows.sort(key=lambda r: -float(r[7]))  # exclusive first: where the time is spent, not who encloses it
            out.append("")
            out.extend(_table(["phase", "ticks seen", "calls", "p50 ms", "p95 ms", "max ms", "sum ms", "sum excl ms"], rows, markdown))
        elif any(t.phases for t in ticks):
            sums: dict = {}
            for t in ticks:
                for name, ms in t.phases.items():
                    sums.setdefault(name, []).append(ms)
            rows = sorted(([n, str(len(v)), f"{percentile(v, .5):.3f}", f"{percentile(v, .95):.3f}", f"{max(v):.3f}", f"{sum(v):.1f}"]
                           for n, v in sums.items()), key=lambda r: -float(r[5]))[:20]
            out.append("")
            out.extend(_table(["phase", "ticks seen", "p50 ms", "p95 ms", "max ms", "sum ms"], rows, markdown))
        hitches = detect_hitches(ticks, spike_ms)
        label = f"ticks >= {spike_ms:.0f} ms" if spike_ms is not None else "tick hitches"
        out.append("")
        out.append(f"{label}: {len(hitches)} of {len(ticks)}")
        for h in hitches[:200]:
            gap = f"  +{h['gap_ticks']} ticks / +{h['gap_s']:.2f} s" if "gap_ticks" in h else ""
            phase = f"{h['phase']} +{h['excess']:.1f} ms" if h["phase"] else "no phase rows"
            out.append(f"  tick {h['tick']} t={h['t_us'] / 1e6:.2f}s {h['ms']:.1f} ms (median {h['median']:.1f}): {phase}{gap}")
        if len(hitches) > 200:
            out.append(f"  ... {len(hitches) - 200} more")
        gaps = [h["gap_ticks"] for h in hitches if "gap_ticks" in h]
        walls = [h["gap_s"] for h in hitches if "gap_s" in h]
        if gaps:
            out.append(f"hitch interval: ticks p50 {statistics.median(gaps):.0f} min {min(gaps)} max {max(gaps)}; "
                       f"wall s p50 {statistics.median(walls):.2f} min {min(walls):.2f} max {max(walls):.2f}")
        out.append(format_pattern(analyze_pattern(hitches, source.markers)))
        if source.patterns:
            out.append("recorded " + source.patterns[-1])
        stats = window_stats(source, capture.frames, capture.pc, budget)
        out.append("verdict: " + evaluate_verdict(stats) + (f" (recorded: {source.verdicts[-1]})" if source.verdicts else ""))
    if capture.frames:
        frames = [f.ms for f in capture.frames]
        out.append("")
        out.append(f"== frames {len(frames)}: ms p50 {percentile(frames, .5):.2f} p95 {percentile(frames, .95):.2f} "
                   f"p99 {percentile(frames, .99):.2f} max {max(frames):.2f}")
        frame_hitches = []
        window = RollingMedian()
        for f in capture.frames:
            median = window.median()
            if len(window.values) >= 32 and f.ms > max(2 * median, median + 8.3):
                frame_hitches.append((f.n, f.ms, median))
            window.push(f.ms)
        out.append(f"frame hitches: {len(frame_hitches)}")
        for n, ms, median in frame_hitches[:50]:
            out.append(f"  frame {n} {ms:.1f} ms (median {median:.1f})")
    out.extend(fight_lines(capture, markdown))
    out.extend(thread_lines(capture, markdown))
    out.extend(mem_lines(capture))
    if capture.pc:
        others = [p["others"] for p in capture.pc if p.get("others", -1) >= 0]
        if others:
            out.append(f"pc: other processes cpu p50 {percentile(others, .5) * 100:.0f} % max {max(others) * 100:.0f} %")
    for src, total in capture.dropped.items():
        out.append(f"dropped records ({'frames' if src == -1 else 'source ' + str(src)}): {total}")
    out.extend(capture.notes)
    return out


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--spike-ms", type=float, default=None, help="call every tick at or above this a hitch (default: the game's rule)")
    parser.add_argument("--skip-ticks", type=int, default=0, help="ignore ticks up to this number (load/warm-up)")
    parser.add_argument("--frame-budget-ms", type=float, default=16.7)
    parser.add_argument("--markdown", action="store_true")
    args = parser.parse_args(argv)
    capture = load(args.files, args.skip_ticks)
    if not any(s.ticks for s in capture.sources.values()) and not capture.frames:
        print("no rows", file=sys.stderr)
        return 1
    print("\n".join(report(capture, args.spike_ms, args.markdown, args.frame_budget_ms)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
