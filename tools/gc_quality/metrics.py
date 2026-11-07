"""Metrics derived from one decision log (see logs.py). Pure functions of a Run, identical for every arm."""
from __future__ import annotations

import re
from dataclasses import dataclass

from .logs import Run

WINDOW_TICKS = 18000  # 10 game minutes at 30 ticks per second
ANSWER_HORIZON = 1800  # a trigger counts as unanswered after 60 s
TRIGGER_DEBOUNCE = 90  # repeated triggers on one (player, planet, kind) inside 3 s are one trigger
EARLY_DECISIONS = 40
TECH_DEADLINE = 27000  # 15 minutes

# Window-count metrics (per AI player and 10-minute window) and the end-of-window state samples.
COUNT_METRICS = ("attack_orders", "expand_orders", "reinforce_orders", "builds_started", "builds_completed",
                 "planets_captured", "planets_lost", "fleets_lost")
STATE_METRICS = ("planets_owned", "credits", "tech")


def _ai(run: Run) -> set[int]:
    return set(run.ai_players)


def decisions(run: Run, player: int) -> list[dict]:
    """The player's orders in log order: moves (movement_begin) and builds (production_started)."""
    out = []
    for e in run.events:
        if e.get("o") != player:
            continue
        if e["k"] == "movement_begin" or e["k"] == "production_started":
            out.append(e)
    return out


def decision_key(e: dict) -> tuple:
    """What identifies a decision across arms: the kind and its target, never its tick."""
    if e["k"] == "movement_begin":
        return ("move", e["to"])
    return ("build", e["s"], e["ty"])


def first_attack(run: Run, player: int) -> dict | None:
    for e in run.events:
        if e["k"] == "movement_begin" and e.get("o") == player and e.get("tc") == "enemy":
            return e
    return None


def window_rows(run: Run) -> list[dict]:
    """One row per (AI player, complete 10-minute window): counts of orders/events plus end-of-window state."""
    ticks = int((run.end or {}).get("ticks") or run.meta.get("ticks") or 0)
    windows = ticks // WINDOW_TICKS
    ai = _ai(run)
    rows: dict[tuple[int, int], dict] = {}
    for player in ai:
        for w in range(windows):
            rows[(player, w)] = {"player": player, "window": w, "faction": run.faction(player),
                                 **{name: 0 for name in COUNT_METRICS}}

    def bump(player, tick, name):
        w = (tick - 1) // WINDOW_TICKS
        row = rows.get((player, w))
        if row is not None:
            row[name] += 1

    for e in run.events:
        k, t = e["k"], e["t"]
        o = e.get("o")
        if k == "movement_begin" and o in ai:
            bump(o, t, {"enemy": "attack_orders", "neutral": "expand_orders", "own": "reinforce_orders"}.get(e.get("tc"), "reinforce_orders"))
        elif k == "production_started" and o in ai:
            bump(o, t, "builds_started")
        elif k == "production_completed" and o in ai:
            bump(o, t, "builds_completed")
        elif k == "fleet_destroyed" and o in ai:
            bump(o, t, "fleets_lost")
        elif k == "planet_owner_changed":
            if e.get("v") in ai and e.get("prev") != e.get("v"):
                bump(e["v"], t, "planets_captured")
            if e.get("prev") in ai and e.get("prev") != e.get("v"):
                bump(e["prev"], t, "planets_lost")
    # End-of-window state: the last sample at or before the window end.
    last: dict[tuple[int, int], dict] = {}
    for s in run.samples:
        w = (s["t"] - 1) // WINDOW_TICKS
        if s["p"] in ai and (s["p"], w) in rows:
            last[(s["p"], w)] = s  # samples are in tick order
    for key, row in rows.items():
        s = last.get(key)
        row["planets_owned"] = s["pl"] if s else None
        row["credits"] = s["cr"] if s else None
        row["tech"] = s["tech"] if s else None
    return list(rows.values())


@dataclass
class Trigger:
    tick: int
    player: int
    planet: int
    kind: str


def triggers(run: Run) -> list[Trigger]:
    """World events an AI player should answer: an enemy fleet arriving at its planet, losing a fleet or a planet,
    capturing a planet. Debounced per (player, planet, kind)."""
    ai = _ai(run)
    raw: list[Trigger] = []
    for e in run.events:
        k, t = e["k"], e["t"]
        if k == "movement_finished":
            owner = e.get("po")
            if owner in ai and e.get("o") not in (0, owner, None):
                raw.append(Trigger(t, owner, e["at"], "enemy_arrival"))
        elif k == "fleet_destroyed" and e.get("o") in ai:
            raw.append(Trigger(t, e["o"], e.get("at", 0), "fleet_lost"))
        elif k == "planet_owner_changed":
            if e.get("prev") in ai and e.get("prev") != e.get("v"):
                raw.append(Trigger(t, e["prev"], e["s"], "planet_lost"))
            if e.get("v") in ai and e.get("prev") != e.get("v"):
                raw.append(Trigger(t, e["v"], e["s"], "planet_captured"))
    out: list[Trigger] = []
    seen: dict[tuple[int, int, str], int] = {}
    for trig in sorted(raw, key=lambda x: x.tick):
        key = (trig.player, trig.planet, trig.kind)
        if key in seen and trig.tick - seen[key] <= TRIGGER_DEBOUNCE:
            seen[key] = trig.tick
            continue
        seen[key] = trig.tick
        out.append(trig)
    return out


def _touches(e: dict, planet: int) -> bool:
    if e["k"] == "movement_begin":
        return planet in (e.get("from"), e.get("to"))
    return e.get("s") == planet


def reaction_latencies(run: Run) -> tuple[list[tuple[Trigger, int | None, int | None]], dict[str, list[int]]]:
    """Per trigger: ticks to the player's first order of any kind after the trigger (None: none inside the horizon),
    and ticks to its first order touching the trigger planet (informational). Also the first latencies grouped by
    trigger kind. The first number is the gated one: it measures how soon the AI acts again, which is what the
    snapshot delay changes."""
    by_player: dict[int, list[dict]] = {}
    for player in _ai(run):
        by_player[player] = decisions(run, player)
    rows: list[tuple[Trigger, int | None, int | None]] = []
    grouped: dict[str, list[int]] = {}
    for trig in triggers(run):
        first = planet = None
        for e in by_player.get(trig.player, []):
            if e["t"] < trig.tick:
                continue
            if e["t"] - trig.tick > ANSWER_HORIZON:
                break
            if first is None:
                first = e["t"] - trig.tick
            if planet is None and _touches(e, trig.planet):
                planet = e["t"] - trig.tick
                break
        rows.append((trig, first, planet))
        if first is not None:
            grouped.setdefault(trig.kind, []).append(first)
    return rows, grouped


def tech_times(run: Run) -> dict[int, dict]:
    """Per AI player: first tick at tech >= 3 and >= 4 and the maximum tech seen."""
    out: dict[int, dict] = {}
    for player in _ai(run):
        out[player] = {"tech3": None, "tech4": None, "max": 0, "faction": run.faction(player)}
    for s in run.samples:
        row = out.get(s["p"])
        if row is None:
            continue
        row["max"] = max(row["max"], s["tech"])
        if s["tech"] >= 3 and row["tech3"] is None:
            row["tech3"] = s["t"]
        if s["tech"] >= 4 and row["tech4"] is None:
            row["tech4"] = s["t"]
    return out


_SUMMARY = re.compile(r"^player (\d+) .+?: \d+ goals, \d+ movement rows")  # per-player summary; per-goal detail lines differ
_REFUSALS = re.compile(r"^player (\d+) .*?; refusals: (.*)$")


def goal_summaries(run: Run) -> int:
    """Number of per-player goal summary lines in the run."""
    return sum(1 for g in run.goals if _SUMMARY.match(g.get("line", "")))


def refusal_totals(run: Run) -> dict[str, int]:
    """Cumulative engine refusals by reason at the last goal summary of each AI player.

    A summary line without a readable refusal list is an error (ValueError), never an empty count: a changed line format
    must not read as "no refusals" and pass the refusal gate.
    """
    last: dict[int, dict[str, int]] = {}
    for g in run.goals:
        line = g.get("line", "")
        if not _SUMMARY.match(line):
            continue
        match = _REFUSALS.match(line)
        if not match:
            raise ValueError(f"goal summary without a refusal list: {line!r}")
        counts: dict[str, int] = {}
        text = match.group(2).strip()
        if text != "none":
            for part in text.split(","):
                reason, _, number = part.strip().rpartition("=")
                if not reason or not number.strip().isdigit():
                    raise ValueError(f"unreadable refusal entry {part.strip()!r} in goal summary: {line!r}")
                counts[reason] = int(number)
        last[int(match.group(1))] = counts
    total: dict[str, int] = {}
    for counts in last.values():
        for reason, number in counts.items():
            total[reason] = total.get(reason, 0) + number
    return total


PLANNING_PREFIXES = ("alloc:", "goal:")


def commit_refusals(totals: dict[str, int]) -> int:
    """Engine refusals that are not planning outcomes. "alloc:*" and "goal:*" count goals that found no budget, units
    or plan; they are normal AI behaviour and large in the reference."""
    return sum(n for reason, n in totals.items() if not reason.startswith(PLANNING_PREFIXES))


def ai_orders(run: Run) -> int:
    ai = _ai(run)
    return sum(1 for e in run.events if e["k"] in ("movement_begin", "production_started") and e.get("o") in ai)


def ai_rejected(run: Run) -> int:
    ai = _ai(run)
    return sum(1 for e in run.events if e["k"] == "command_rejected" and e.get("o") in ai)


PROBE_WINDOW = 3600


def probe_outcome(run: Run) -> dict | None:
    """Result of the scripted human probe in a probe run, or None when the run has none / it was not issued."""
    probe = next((p for p in run.probes if p.get("issued")), None)
    if probe is None:
        return None
    t0, target, human = int(probe["t"]), int(probe["target"]), int(run.meta.get("human", 0))
    ai = _ai(run)
    outcome = "no_battle"
    arrival = None
    reinforcements = 0
    first_response = None
    for e in run.events:
        if e["t"] < t0 or e["t"] > t0 + PROBE_WINDOW:
            continue
        k = e["k"]
        if k == "movement_finished" and e.get("o") == human and e.get("at") == target and arrival is None:
            arrival = e["t"]
        elif k == "movement_begin" and e.get("o") in ai and e.get("to") == target:
            reinforcements += 1
            if first_response is None:
                first_response = e["t"]
        elif k in ("auto_resolve_won", "auto_resolve_lost") and e.get("s") == target and outcome == "no_battle":
            winner_is_human = (e.get("v") == human) == (k == "auto_resolve_won")
            outcome = "lost" if winner_is_human else "defended"
        elif k == "planet_owner_changed" and e.get("s") == target and e.get("v") == human:
            outcome = "lost"
    latency = None if first_response is None else first_response - (arrival if arrival is not None else t0)
    return {"scenario": run.meta.get("probe"), "outcome": outcome, "reinforcements": reinforcements,
            "arrival": arrival, "first_response_latency": latency}
