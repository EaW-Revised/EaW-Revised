"""Tech-up report for galactic quality-run logs: does each AI faction reach its maximum galactic tech level?

    python -m gc_quality.techup <log files or folders> [--out report.md] [--deadline 27000]

Reads the galactic technology account of every AI player (`current_tech` / `maximum_tech` of the per-player galactic
economy account, logged as "tech" rows at the first tick and at every change, and as `tech`/`mx` in the periodic
samples): never a tactical value. A faction counts as teched up when its current level reaches its maximum level within
the deadline (27,000 ticks = 15 minutes). Also reports the first tick of every level, the level at 5/10/15 minutes, and
which tech goals the native AI held (goal function, plan, running plan) from the "tgoal" and "goals" rows.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from . import logs

TICKS_PER_MINUTE = 1800
MARKS = (9000, 18000, 27000)  # 5, 10, 15 minutes
_DETAIL = re.compile(r"^player (\d+) goal=(\d+) definition=(\S+) target=(\S+) desire=(\S+) active=(\d|true|false) plan=(\S+) running=(\d+)")
_FUNCTION = re.compile(r"; ([A-Za-z0-9_]+) n=(\d+) active=(\d+) planned=(\d+) running=(\d+) desire<=([-0-9.e+]+)")
_SUMMARY = re.compile(r"^player (\d+) ")


def tech_track(run: logs.Run, player: int) -> list[tuple[int, int, int]]:
    """(tick, level, maximum) changes of one player, from the tech rows, else from the samples."""
    rows = [(int(r["t"]), int(r["tech"]), int(r.get("mx", -1))) for r in run.tech if int(r["p"]) == player]
    if rows:
        return sorted(rows)
    out: list[tuple[int, int, int]] = []
    for s in run.samples:
        if int(s["p"]) == player and (not out or out[-1][1] != int(s["tech"])):
            out.append((int(s["t"]), int(s["tech"]), int(s.get("mx", -1))))
    return out


def level_at(track: list[tuple[int, int, int]], tick: int) -> int | None:
    value = None
    for t, level, _ in track:
        if t <= tick:
            value = level
    return value


def tech_rows(run: logs.Run, deadline: int) -> list[dict]:
    rows = []
    for player in run.ai_players:
        track = tech_track(run, player)
        if not track:
            rows.append({"player": player, "faction": run.faction(player), "no_data": True})
            continue
        start = track[0][1]
        maximum = max(m for _, _, m in track)
        top = max(level for _, level, _ in track)
        first: dict[int, int] = {}
        for t, level, _ in track:
            for step in range(start + 1, level + 1):
                first.setdefault(step, t)
        reached_cap = next((t for t, level, m in track if m > 0 and level >= m), None)
        rows.append({"player": player, "faction": run.faction(player), "start": start, "max_cap": maximum, "top": top,
                     "first": first, "marks": {m: level_at(track, m) for m in MARKS},
                     "teched_up": reached_cap is not None and reached_cap <= deadline, "cap_tick": reached_cap,
                     "starts_at_cap": start >= maximum > 0})
    return rows


def goal_rows(run: logs.Run) -> dict[int, dict]:
    """Per player: tech goals from the tgoal rows, tech functions from the periodic summaries, all function names."""
    out: dict[int, dict] = {}
    for player in run.ai_players:
        out[player] = {"details": {}, "functions": {}, "summary_ticks": []}
    for row in run.tgoals:
        match = _DETAIL.match(row.get("line", ""))
        if not match:
            continue
        player, _goal, definition, _target, desire, active, plan, running = match.groups()
        entry = out.setdefault(int(player), {"details": {}, "functions": {}, "summary_ticks": []})["details"].setdefault(
            definition, {"samples": 0, "first": int(row["t"]), "active": 0, "planned": 0, "running": 0, "plans": set(), "max_desire": 0.0,
                         "first_running": None, "first_planned": None})
        entry["samples"] += 1
        isactive = active in ("1", "true")
        entry["active"] += 1 if isactive else 0
        if plan != "none":
            entry["planned"] += 1
            entry["plans"].add(plan)
            if entry["first_planned"] is None:
                entry["first_planned"] = int(row["t"])
        if int(running):
            entry["running"] += 1
            if entry["first_running"] is None:
                entry["first_running"] = int(row["t"])
        try:
            entry["max_desire"] = max(entry["max_desire"], float(desire))
        except ValueError:
            pass
    for row in run.goals:
        line = row.get("line", "")
        head = _SUMMARY.match(line)
        if not head or " goals," not in line:
            continue
        player = int(head.group(1))
        entry = out.setdefault(player, {"details": {}, "functions": {}, "summary_ticks": []})
        entry["summary_ticks"].append(int(row["t"]))
        for name, n, active, planned, running, desire in _FUNCTION.findall(line):
            f = entry["functions"].setdefault(name, {"max_n": 0, "active": 0, "planned": 0, "running": 0, "first_running": None, "first_seen": int(row["t"])})
            f["max_n"] = max(f["max_n"], int(n))
            f["active"] += 1 if int(active) else 0
            f["planned"] += 1 if int(planned) else 0
            if int(running):
                f["running"] += 1
                if f["first_running"] is None:
                    f["first_running"] = int(row["t"])
    return out


def _minutes(tick: int | None) -> str:
    return "-" if tick is None else f"{tick} ({tick / TICKS_PER_MINUTE:.1f} min)"


def render(runs: list[logs.Run], deadline: int) -> str:
    lines = ["# Galactic tech-up report", "",
             "Field read: `account.current_tech` and `account.maximum_tech` of the per-player GALACTIC technology account "
             "(`GalacticPlayerEconomy`), logged as `tech` rows (first tick and every change) and as `tech`/`mx` in the 300-tick samples. "
             "Not a tactical value.", ""]
    for run in sorted(runs, key=lambda r: (r.meta.get("profile", ""), int(r.meta["seed"]), r.meta["arm"])):
        m = run.meta
        ticks = int((run.end or {}).get("ticks") or 0)
        full = run.complete and ticks >= deadline
        lines += [f"## {m.get('profile')} {m['arm']} seed {m['seed']} ({m.get('campaign')}), ran {ticks} ticks "
                  f"({ticks / TICKS_PER_MINUTE:.1f} min), wall {(run.end or {}).get('wall_s', 0):.0f} s"
                  + ("" if full else f" - **NOT a full {deadline}-tick run**"), ""]
        rows = tech_rows(run, deadline)
        header = "| player | faction | start | cap (mx) | level at 5 / 10 / 15 min | top | " + " | ".join(f"first L{n}" for n in range(1, 6)) + " | teched up |"
        lines += [header, "|" + "---|" * (header.count("|") - 1)]
        for r in rows:
            if r.get("no_data"):
                lines.append(f"| {r['player']} | {r['faction']} | no tech rows | | | | | | | | | |")
                continue
            marks = " / ".join("-" if r["marks"][k] is None else str(r["marks"][k]) for k in MARKS)
            firsts = " | ".join(_minutes(r["first"].get(n)) if n > r["start"] else ("start" if n == r["start"] else "-") for n in range(1, 6))
            verdict = ("YES at " + _minutes(r["cap_tick"]) if r["teched_up"] else "NO") if full else "NA (short run)"
            if r["starts_at_cap"]:
                verdict += " (starts at its cap)"
            lines.append(f"| {r['player']} | {r['faction']} | {r['start']} | {r['max_cap']} | {marks} | {r['top']} | {firsts} | {verdict} |")
        lines.append("")
        lines += ["Galactic tech level at the end of each minute (minute 1..15):", "",
                  "| player | faction | " + " | ".join(f"m{n}" for n in range(1, 16)) + " |", "|" + "---|" * 17]
        for player in run.ai_players:
            track = tech_track(run, player)
            if track:
                cells = ["-" if level_at(track, n * TICKS_PER_MINUTE) is None else str(level_at(track, n * TICKS_PER_MINUTE)) for n in range(1, 16)]
                lines.append(f"| {player} | {run.faction(player)} | " + " | ".join(cells) + " |")
        lines.append("")
        goals = goal_rows(run)
        lines += ["Tech goals seen by the native AI (tgoal rows every 300 ticks; summary rows every 900 ticks):", "",
                  "| player | faction | goal function | tgoal samples | first seen | active | planned | running | first planned | first running | plans | max desire |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|"]
        empty = []
        for player in run.ai_players:
            entry = goals.get(player, {"details": {}})
            if not entry["details"]:
                empty.append(player)
            for name, d in sorted(entry["details"].items()):
                lines.append(f"| {player} | {run.faction(player)} | {name} | {d['samples']} | {_minutes(d['first'])} | {d['active']} | {d['planned']} | {d['running']} | "
                             f"{_minutes(d['first_planned'])} | {_minutes(d['first_running'])} | {', '.join(sorted(d['plans'])) or 'none'} | {d['max_desire']:g} |")
        for player in empty:
            lines.append(f"| {player} | {run.faction(player)} | NONE: no goal with 'tech' in its function or plan name was ever listed | 0 | - | - | - | - | - | - | - | - |")
        lines.append("")
        lines += ["All goal functions the player held (900-tick summaries; samples with active / planned / running):", ""]
        for player in run.ai_players:
            entry = goals.get(player, {"functions": {}, "summary_ticks": []})
            names = ", ".join(f"{n} (max n={f['max_n']}, act {f['active']}, plan {f['planned']}, run {f['running']})" for n, f in sorted(entry["functions"].items()))
            lines.append(f"- player {player} {run.faction(player)} ({len(entry['summary_ticks'])} summaries): {names or 'none'}")
        lines.append("")
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--deadline", type=int, default=27000)
    args = parser.parse_args(argv)
    runs: list[logs.Run] = []
    for path in args.paths:
        files = sorted(path.glob("*.ndjson*")) if path.is_dir() else [path]
        for file in files:
            runs.extend(logs.read_any(file))
    text = render(runs, args.deadline)
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
