"""Compare the async-AI arm with the synchronous reference arm and write one markdown report.

Input: a directory of decision logs (logs.write_run) or any mix of log files. Every threshold of the design's quality
plan (out/coord/gc/ai-async-design.md section 6, owner decision async-only) is one check with PASS / FAIL / REVIEW /
WARN / NA. Run: python -m gc_quality.compare <logs...> --out report.md   (from tools/).
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path

from . import logs, metrics, stats

SMOKE_TICKS = 3000  # runs shorter than this are host smoke tests, not experiments
PASS, FAIL, REVIEW, WARN, NA = "PASS", "FAIL", "REVIEW", "WARN", "NA"
# Report-level verdicts that are not a pass: an arm that never ran, or a gated check that could not be evaluated.
NOT_RUN, INCOMPLETE = "NOT-RUN", "INCOMPLETE"


@dataclass
class Thresholds:
    identity_fraction: float = 0.85       # early decisions identical in kind and target
    shift_min: int = 25                   # D + j expected 30..45, +-5
    shift_max: int = 50
    latency_added: float = 45.0           # async minus reference at p50 and p95; design maximum D + K - 1 = 44
    latency_floor: int = 30               # never below D
    alpha: float = 0.05
    median_shift: float = 0.10
    median_shift_small: float = 0.15      # for counts below 20
    min_samples: int = 5
    noise_quantile: float = 0.95
    noise_splits: int = 200
    tech_level: int = 3
    tech_deadline: int = metrics.TECH_DEADLINE
    refusal_delta: float = 0.01           # async refusal rate may exceed the reference by one percentage point
    probe_window: int = metrics.PROBE_WINDOW


@dataclass
class Check:
    name: str
    status: str
    value: str
    threshold: str
    note: str = ""
    gated: bool = True  # an NA on a gated check leaves the report INCOMPLETE; informational rows may stay NA


@dataclass
class Report:
    checks: list[Check] = field(default_factory=list)
    sections: list[str] = field(default_factory=list)  # markdown detail blocks
    warnings: list[str] = field(default_factory=list)
    not_run: list[str] = field(default_factory=list)  # groups whose async arm is the reference schedule (stub)
    compared: int = 0                                 # groups that had both arms and were evaluated

    def add(self, name, status, value, threshold, note="", gated=True):
        self.checks.append(Check(name, status, value, threshold, note, gated))

    @property
    def verdict(self) -> str:
        statuses = {c.status for c in self.checks}
        if FAIL in statuses:
            return FAIL
        if self.not_run:
            return NOT_RUN
        if self.compared == 0 or any(c.status == NA and c.gated for c in self.checks):
            return INCOMPLETE
        if REVIEW in statuses:
            return REVIEW
        return PASS


def _fmt(x, digits=3):
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "n/a"
    return f"{x:.{digits}g}" if isinstance(x, float) else str(x)


def _by_seed(runs):
    return {int(r.meta["seed"]): r for r in runs}


# ---- sample families ------------------------------------------------------------------------------------------------

def _window_families(run) -> dict[str, list[float]]:
    out: dict[str, list[float]] = {}
    rows = metrics.window_rows(run)
    for name in (*metrics.COUNT_METRICS, *metrics.STATE_METRICS):
        out[name] = [float(r[name]) for r in rows if r.get(name) is not None]
    for r in rows:  # per faction versions of the order counts
        for name in ("attack_orders", "expand_orders"):
            out.setdefault(f"{name}[{r['faction']}]", []).append(float(r[name]))
    return out


def _player_families(run) -> dict[str, list[float]]:
    """Per (AI player) samples: first attack tick, target-class shares of its moves."""
    out: dict[str, list[float]] = {}
    for player in run.ai_players:
        fac = run.faction(player)
        first = metrics.first_attack(run, player)
        if first is not None:
            out.setdefault("first_attack_tick", []).append(float(first["t"]))
            out.setdefault(f"first_attack_tick[{fac}]", []).append(float(first["t"]))
        moves = [e for e in metrics.decisions(run, player) if e["k"] == "movement_begin"]
        if len(moves) >= 5:
            for tc in ("enemy", "neutral", "own"):
                share = sum(1 for e in moves if e.get("tc") == tc) / len(moves)
                out.setdefault(f"target_share_{tc}", []).append(share)
                out.setdefault(f"target_share_{tc}[{fac}]", []).append(share)
    return out


def families(runs) -> dict[str, dict[int, list[float]]]:
    """name -> seed -> samples, over a list of complete runs."""
    out: dict[str, dict[int, list[float]]] = {}
    for run in runs:
        seed = int(run.meta["seed"])
        for fam in (_window_families(run), _player_families(run)):
            for name, values in fam.items():
                out.setdefault(name, {}).setdefault(seed, []).extend(values)
    return out


def _pool(per_seed: dict[int, list[float]], seeds=None) -> list[float]:
    return [v for s, vals in sorted(per_seed.items()) if seeds is None or s in seeds for v in vals]


def noise_floor(per_seed: dict[int, list[float]], group: int, th: Thresholds):
    """Null distribution of (KS D, |median shift|) between disjoint seed groups of the reference pool."""
    seeds = sorted(per_seed)
    ds, shifts = [], []
    for first, second in stats.balanced_splits(len(seeds), group, th.noise_splits, seed=len(seeds) * 1009 + group):
        a = _pool(per_seed, {seeds[i] for i in first})
        b = _pool(per_seed, {seeds[i] for i in second})
        if len(a) < 2 or len(b) < 2:
            continue
        ds.append(stats.ks_statistic(a, b))
        shifts.append(abs(stats.relative_shift(a, b)))
    if not ds:
        return None
    return {"d": stats.quantile_of(ds, th.noise_quantile), "shift": stats.quantile_of(shifts, th.noise_quantile),
            "splits": len(ds)}


# ---- the checks -------------------------------------------------------------------------------------------------------

def _distribution_checks(report: Report, profile: str, ref_runs, asy_runs, ref_pool_runs, th: Thresholds):
    seeds = sorted(set(_by_seed(asy_runs)) & set(_by_seed(ref_runs)))
    ref_f, asy_f, pool_f = families(ref_runs), families(asy_runs), families(ref_pool_runs)
    names = sorted(set(ref_f) & set(asy_f))
    rows = []
    for name in names:
        a, b = _pool(ref_f[name], set(seeds)), _pool(asy_f[name], set(seeds))
        if len(a) < th.min_samples or len(b) < th.min_samples:
            rows.append((name, None, len(a), len(b)))
            continue
        d, p = stats.ks_test(a, b)
        rows.append((name, (d, p, stats.relative_shift(a, b), stats.median(a), stats.median(b)), len(a), len(b)))
    adjusted = stats.holm([r[1][1] if r[1] else math.nan for r in rows])
    group = max(1, min(len(seeds), len(pool_f.get(names[0], {})) // 2)) if names else 1
    lines = ["| metric | n ref/async | KS D | p (Holm) | median ref -> async | shift | noise D95 / shift95 | status |",
             "|---|---|---|---|---|---|---|---|"]
    worst = PASS
    failed = []
    for (name, res, na, nb), padj in zip(rows, adjusted):
        if res is None:
            lines.append(f"| {name} | {na}/{nb} | | | | | | NA (fewer than {th.min_samples} samples) |")
            continue
        d, p, shift, med_a, med_b = res
        floor = noise_floor(pool_f.get(name, {}), group, th)
        small = max(abs(med_a), abs(med_b)) < 20 and name.split("[")[0] in metrics.COUNT_METRICS
        limit = th.median_shift_small if small else th.median_shift
        shift_limit = max(limit, floor["shift"]) if floor else limit
        d_limit = floor["d"] if floor else 0.0
        significant = padj < th.alpha
        beyond_noise = (d > d_limit) if floor else True
        status = PASS
        if (significant and beyond_noise) or abs(shift) > shift_limit:
            status = FAIL
            failed.append(name)
        elif significant:
            status = PASS  # significant but inside the reference-vs-reference envelope
        if not floor and status == PASS:
            status = WARN
        if status == FAIL:
            worst = FAIL
        elif status == WARN and worst == PASS:
            worst = WARN
        noise = f"{_fmt(floor['d'])} / {_fmt(floor['shift'])} ({floor['splits']} splits)" if floor else "no floor"
        lines.append(f"| {name} | {na}/{nb} | {_fmt(d)} | {_fmt(padj)} | {_fmt(med_a)} -> {_fmt(med_b)} | "
                     f"{_fmt(shift * 100, 3)} % | {noise} | {status} |")
    n_fail = len(failed)
    report.add(f"[{profile}] distributions (KS + median shift, noise floor)",
               worst if rows else NA, f"{n_fail} of {len(rows)} metrics outside noise" if rows else "no metrics",
               f"Holm p >= {th.alpha} or inside noise; |shift| <= {th.median_shift:.0%} ({th.median_shift_small:.0%} small counts) or noise",
               ", ".join(failed[:6]))
    report.sections.append(f"### [{profile}] distribution comparison\n\n" + "\n".join(lines))


def _identity_checks(report: Report, profile: str, ref_runs, asy_runs, th: Thresholds, stub: bool):
    ref, asy = _by_seed(ref_runs), _by_seed(asy_runs)
    matched = total = 0
    shifts: list[int] = []
    per_pair = []
    first_same = first_total = 0
    first_lines = []
    for seed in sorted(set(ref) & set(asy)):
        for player in ref[seed].ai_players:
            a = metrics.decisions(ref[seed], player)[: metrics.EARLY_DECISIONS]
            b = metrics.decisions(asy[seed], player)[: metrics.EARLY_DECISIONS]
            n = min(len(a), len(b))
            same = 0
            for x, y in zip(a, b):
                if metrics.decision_key(x) == metrics.decision_key(y):
                    same += 1
                    shifts.append(y["t"] - x["t"])
            matched += same
            total += max(len(a), len(b))
            per_pair.append((seed, player, same, max(len(a), len(b))))
            fa, fb = metrics.first_attack(ref[seed], player), metrics.first_attack(asy[seed], player)
            if fa is not None or fb is not None:
                first_total += 1
                ok = fa is not None and fb is not None and fa["to"] == fb["to"]
                first_same += ok
                first_lines.append(f"seed {seed} player {player}: ref {fa['to'] if fa else '-'} @ {fa['t'] if fa else '-'}, "
                                   f"async {fb['to'] if fb else '-'} @ {fb['t'] if fb else '-'} -> {'same' if ok else 'DIFFERENT'}")
    if stub:
        # The stub arm runs the reference schedule: its tick hashes must equal the reference run of the same seed. This is
        # the harness's own determinism check (same seed, same build, same digest).
        paired = [(ref[seed].end.get("hash_fnv"), asy[seed].end.get("hash_fnv")) for seed in sorted(set(ref) & set(asy))]
        equal = sum(1 for a, b in paired if a is not None and a == b)
        report.add(f"[{profile}] stub arm tick-hash digest equals the reference", PASS if paired and equal == len(paired) else FAIL,
                   f"{equal}/{len(paired)} seeds", "all", "harness self-check")
    if total == 0:
        report.add(f"[{profile}] early-window identity", NA, "no paired decisions", "")
        return
    fraction = matched / total
    lo, hi = (0, 0) if stub else (th.shift_min, th.shift_max)
    target = 1.0 if stub else th.identity_fraction
    report.add(f"[{profile}] early-window identity (first {metrics.EARLY_DECISIONS} decisions per player)",
               PASS if fraction >= target else FAIL, f"{fraction:.1%} identical ({matched}/{total})", f">= {target:.0%}",
               "harness self-check" if stub else "")
    report.add(f"[{profile}] first attack order, same target",
               NA if first_total == 0 else (PASS if first_same == first_total else FAIL),
               f"{first_same}/{first_total} (seed, player)", "all", gated=False)
    med = stats.median(shifts)
    shift_status = NA if not shifts else (PASS if lo <= med <= hi else FAIL)
    report.add(f"[{profile}] tick shift of matched decisions", shift_status, f"median {_fmt(med)} ticks (n={len(shifts)})",
               f"{lo}..{hi} ticks" + (" (stub arm: identical expected)" if stub else " (D + j = 30..45, +-5)"))
    report.sections.append(f"### [{profile}] early-window identity\n\n| seed | player | identical / compared |\n|---|---|---|\n"
                           + "\n".join(f"| {s} | {p} | {a}/{b} |" for s, p, a, b in per_pair)
                           + "\n\nFirst attack order:\n\n" + "\n".join(f"- {line}" for line in first_lines))


def _latency_checks(report: Report, profile: str, ref_runs, asy_runs, th: Thresholds, stub: bool):
    def pooled(runs):
        values: list[int] = []
        planet_values: list[int] = []
        unanswered = total = 0
        by_kind: dict[str, list[int]] = {}
        for run in runs:
            rows, grouped = metrics.reaction_latencies(run)
            total += len(rows)
            unanswered += sum(1 for _, first, _ in rows if first is None)
            values += [first for _, first, _ in rows if first is not None]
            planet_values += [planet for _, _, planet in rows if planet is not None]
            for kind, vals in grouped.items():
                by_kind.setdefault(kind, []).extend(vals)
        return values, unanswered, total, by_kind, planet_values
    rv, ru, rt, rk, rp = pooled(ref_runs)
    av, au, at, ak, ap = pooled(asy_runs)
    if not av:
        report.add(f"[{profile}] reaction latency", NA, "no answered triggers in the async arm", "")
        return
    p50, p95 = stats.percentile(av, 50), stats.percentile(av, 95)
    rp50, rp95 = stats.percentile(rv, 50), stats.percentile(rv, 95)
    # Reference-relative (owner/coordinator decision): the added latency is what the snapshot delay costs; its design
    # maximum is D + K - 1 = 44 ticks. The absolute values stay in the notes as information.
    report.add(f"[{profile}] reaction latency added at p50", PASS if p50 - rp50 <= th.latency_added else FAIL,
               f"{_fmt(p50 - rp50)} ticks (async {_fmt(p50)}, reference {_fmt(rp50)}, n={len(av)})", f"<= {th.latency_added:g} over the reference")
    report.add(f"[{profile}] reaction latency added at p95", PASS if p95 - rp95 <= th.latency_added else FAIL,
               f"{_fmt(p95 - rp95)} ticks (async {_fmt(p95)}, reference {_fmt(rp95)})", f"<= {th.latency_added:g} over the reference")
    if stub:
        report.add(f"[{profile}] reaction latency floor", NA, f"min {min(av)} ticks", "stub arm: no delay expected", gated=False)
    else:
        report.add(f"[{profile}] reaction latency floor", PASS if min(av) >= th.latency_floor else FAIL,
                   f"min {min(av)} ticks", f">= {th.latency_floor} (never below D)")
    lines = ["| trigger kind | ref n / p50 / p95 | async n / p50 / p95 |", "|---|---|---|"]
    for kind in sorted(set(rk) | set(ak)):
        r, a = rk.get(kind, []), ak.get(kind, [])
        lines.append(f"| {kind} | {len(r)} / {_fmt(stats.percentile(r, 50))} / {_fmt(stats.percentile(r, 95))} | "
                     f"{len(a)} / {_fmt(stats.percentile(a, 50))} / {_fmt(stats.percentile(a, 95))} |")
    lines.append(f"| ALL | {len(rv)} / {_fmt(stats.percentile(rv, 50))} / {_fmt(stats.percentile(rv, 95))} | "
                 f"{len(av)} / {_fmt(p50)} / {_fmt(p95)} |")
    unanswered = (f"\n\nUnanswered inside {metrics.ANSWER_HORIZON} ticks: reference {ru}/{rt}, async {au}/{at}. "
                  "Latency = ticks to the player's first order of any kind after the trigger. Orders touching the trigger planet "
                  f"(informational): reference {len(rp)} answered, p50 {_fmt(stats.percentile(rp, 50))} / p95 {_fmt(stats.percentile(rp, 95))}; "
                  f"async {len(ap)} answered, p50 {_fmt(stats.percentile(ap, 50))} / p95 {_fmt(stats.percentile(ap, 95))}.")
    report.sections.append(f"### [{profile}] reaction latency\n\n" + "\n".join(lines) + unanswered)


def _tech_checks(report: Report, profile: str, ref_runs, asy_runs, th: Thresholds):
    def tally(runs):
        n = ok3 = ok4 = 0
        rows = []
        for run in runs:
            for player, info in sorted(metrics.tech_times(run).items()):
                n += 1
                reached3 = info["tech3"] is not None and info["tech3"] <= th.tech_deadline
                ok3 += reached3
                ok4 += info["tech4"] is not None and info["tech4"] <= th.tech_deadline
                rows.append((run.meta["seed"], info["faction"], info["tech3"], info["tech4"], info["max"]))
        return n, ok3, ok4, rows
    rn, r3, r4, _ = tally(ref_runs)
    an, a3, a4, arows = tally(asy_runs)
    shortest = min((int(r.end["ticks"]) for r in asy_runs), default=0)
    if shortest < th.tech_deadline:
        report.add(f"[{profile}] tech {th.tech_level} within {th.tech_deadline} ticks", NA,
                   f"runs of {shortest} ticks end before the gate", f"needs >= {th.tech_deadline} ticks")
        return
    if an == 0:
        report.add(f"[{profile}] tech {th.tech_level}-4 within 15 min", NA, "no async samples", "")
        return
    lo, hi = stats.wilson(a3, an)
    ref_fraction = r3 / rn if rn else math.nan
    if a3 == an:
        status, note = PASS, ""
    elif rn and hi < ref_fraction:
        status, note = FAIL, "async Wilson upper bound below the reference fraction"
    elif rn and r3 < rn:
        status, note = PASS, "reference misses some players too; async inside its interval"
    else:
        status, note = FAIL, "reference reaches the level everywhere, async does not"
    report.add(f"[{profile}] tech {th.tech_level} within {th.tech_deadline} ticks", status,
               f"async {a3}/{an} (Wilson {lo:.2f}..{hi:.2f}); reference {r3}/{rn}; tech 4: async {a4}/{an}, ref {r4}/{rn}",
               "every AI player", note)
    report.sections.append(f"### [{profile}] tech progression (async arm)\n\n| seed | faction | first tick tech>=3 | first tick tech>=4 | max |\n|---|---|---|---|---|\n"
                           + "\n".join(f"| {s} | {f} | {_fmt(a)} | {_fmt(b)} | {m} |" for s, f, a, b, m in sorted(arows)))


def _refusal_checks(report: Report, profile: str, ref_runs, asy_runs, th: Thresholds):
    def rates(runs):
        refused: dict[str, int] = {}
        orders = rejected = 0
        for run in runs:
            for reason, n in metrics.refusal_totals(run).items():
                refused[reason] = refused.get(reason, 0) + n
            orders += metrics.ai_orders(run)
            rejected += metrics.ai_rejected(run)
        total = metrics.commit_refusals(refused)
        return refused, orders, rejected, (total / (total + orders) if total + orders else math.nan)
    for arm, runs in (("reference", ref_runs), ("async", asy_runs)):
        if not sum(metrics.goal_summaries(r) for r in runs):
            report.add(f"[{profile}] refusal rate", NA, f"no goal summaries in the {arm} arm", "")
            return
    try:
        rr, ro, rj, rrate = rates(ref_runs)
        ar, ao, aj, arate = rates(asy_runs)
    except ValueError as error:
        report.add(f"[{profile}] refusal rate", FAIL, "refusal counts unreadable", "goal summaries carry a refusal list", str(error))
        return
    if ao == 0:
        report.add(f"[{profile}] refusal rate", NA, "no async orders", "")
        return
    reject_rate = aj / (aj + ao) if aj + ao else 0.0
    reject_ref = rj / (rj + ro) if rj + ro else 0.0
    delta_engine = 0.0 if math.isnan(arate) or math.isnan(rrate) else arate - rrate
    delta_reject = reject_rate - reject_ref
    ok = delta_engine <= th.refusal_delta and delta_reject <= th.refusal_delta
    report.add(f"[{profile}] refusal rate delta", PASS if ok else FAIL,
               f"engine refusals outside planning {arate:.2%} vs reference {rrate:.2%}; commit rejections {reject_rate:.2%} vs {reject_ref:.2%}",
               f"async <= reference + {th.refusal_delta:.0%} points")
    lines = ["| reason | reference | async |", "|---|---|---|"]
    planning = {"reference": 0, "async": 0}
    for reason in sorted(set(rr) | set(ar)):
        if reason.startswith(metrics.PLANNING_PREFIXES):
            planning["reference"] += rr.get(reason, 0)
            planning["async"] += ar.get(reason, 0)
        else:
            lines.append(f"| {reason} | {rr.get(reason, 0)} | {ar.get(reason, 0)} |")
    lines.append(f"| (planning allocations, alloc:/goal: reasons, not gated) | {planning['reference']} | {planning['async']} |")
    report.sections.append(f"### [{profile}] refusals\n\nAI orders: reference {ro}, async {ao}; command_rejected events by AI seats: "
                           f"reference {rj}, async {aj}.\n\n" + "\n".join(lines))


def _goal_type_check(report: Report, profile: str, ref_runs, asy_runs):
    def counts(runs):
        out: dict[str, int] = {}
        lines = 0
        for run in runs:
            for g in run.goals:
                lines += 1
                for part in g.get("line", "").split(";")[1:]:
                    name = part.strip().split(" ")[0]
                    if name and "=" not in name and name != "refusals:":
                        out[name] = out.get(name, 0) + 1
        return out, lines
    rc, rl = counts(ref_runs)
    ac, al = counts(asy_runs)
    if not rc or not al:
        report.add(f"[{profile}] goal types persist", NA, "no goal summaries logged", "")
        return
    vanished = []
    for name, n in rc.items():
        expected = n * al / rl
        if ac.get(name, 0) == 0 and math.exp(-expected) < 0.05:
            vanished.append(name)
    report.add(f"[{profile}] goal types persist", FAIL if vanished else PASS,
               f"{len(vanished)} of {len(rc)} reference goal types absent" + (": " + ", ".join(vanished[:5]) if vanished else ""),
               "none disappear (Poisson 95 %)")


def _probe_checks(report: Report, profile: str, all_runs, th: Thresholds):
    probes = [r for r in all_runs if r.meta.get("probe", "none") != "none"]
    if not probes:
        report.add(f"[{profile}] defence probes", NA, "no probe runs", "", gated=False)
        return
    lines = ["| scenario | arm | seeds | defended | lost | no battle | reinforcements (mean) | first response (median) |",
             "|---|---|---|---|---|---|---|---|"]
    for scenario in sorted({r.meta["probe"] for r in probes}):
        results = {}
        for arm in ("reference", "async"):
            outs = [metrics.probe_outcome(r) for r in probes if r.meta["probe"] == scenario and r.meta["arm"] == arm and r.complete]
            outs = [o for o in outs if o]
            results[arm] = outs
            counts = {k: sum(1 for o in outs if o["outcome"] == k) for k in ("defended", "lost", "no_battle")}
            resp = [o["first_response_latency"] for o in outs if o["first_response_latency"] is not None]
            lines.append(f"| {scenario} | {arm} | {len(outs)} | {counts['defended']} | {counts['lost']} | {counts['no_battle']} | "
                         f"{_fmt(stats.mean([o['reinforcements'] for o in outs]))} | {_fmt(stats.median(resp))} |")
        ref, asy = results["reference"], results["async"]
        battles_a = [o for o in asy if o["outcome"] != "no_battle"]
        battles_r = [o for o in ref if o["outcome"] != "no_battle"]
        if not asy or not battles_r:
            report.add(f"[{profile}] defence probe {scenario}", NA, "no battles in the reference runs" if asy else "no async probe runs", "", gated=False)
            continue
        la, lr = sum(o["outcome"] == "lost" for o in battles_a), sum(o["outcome"] == "lost" for o in battles_r)
        lo_a, hi_a = stats.wilson(la, len(battles_a)) if battles_a else (math.nan, math.nan)
        lo_r, hi_r = stats.wilson(lr, len(battles_r))
        worse = battles_a and (la / len(battles_a)) > (lr / len(battles_r))
        disjoint = battles_a and lo_a > hi_r
        report.add(f"[{profile}] defence probe {scenario}", REVIEW if disjoint else PASS,
                   f"AI lost {la}/{len(battles_a)} (async) vs {lr}/{len(battles_r)} (reference)",
                   "inside the reference interval, else the owner sees the numbers",
                   "async loses more than the reference interval allows" if disjoint else ("async worse inside the interval" if worse else ""))
    report.sections.append(f"### [{profile}] defence probes\n\n" + "\n".join(lines))


# ---- driver -----------------------------------------------------------------------------------------------------------

def build_report(all_runs, th: Thresholds | None = None) -> Report:
    th = th or Thresholds()
    report = Report()
    for r in all_runs:
        if not r.complete:
            report.warnings.append(f"[{r.meta.get('profile', 'stock')}] run {r.key} ({r.meta.get('ticks')} ticks) has no end row (cut log): excluded")
    complete = [r for r in all_runs if r.complete]
    # Runs of different lengths are different experiments (a 300-tick smoke is not a 54,000-tick acceptance run).
    groups = sorted({(r.meta.get("profile", "stock"), int(r.meta["ticks"])) for r in complete if r.meta.get("probe", "none") == "none"})
    for profile, ticks in groups:
        label = f"{profile} {ticks}"
        plain = [r for r in complete if r.meta.get("profile", "stock") == profile and int(r.meta["ticks"]) == ticks
                 and r.meta.get("probe", "none") == "none"]
        ref_pool = [r for r in plain if r.meta["arm"] == "reference"]
        asy = [r for r in plain if r.meta["arm"] == "async"]
        seeds_async = {int(r.meta["seed"]) for r in asy}
        ref = [r for r in ref_pool if int(r.meta["seed"]) in seeds_async]
        stub = bool(asy) and all(r.meta.get("stub") for r in asy)
        if stub:
            report.not_run.append(label)
            report.warnings.append(f"[{label}] NOT-RUN: the async arm is the reference schedule (stub), so nothing was measured about "
                                   "the async AI; the rows below are a harness self-check of the pipeline and the digests")
        if not asy or not ref:
            report.add(f"[{label}] comparison", NA, "need reference and async runs for the same seeds", "")
            continue
        report.compared += 1
        _identity_checks(report, label, ref, asy, th, stub)
        if ticks < SMOKE_TICKS:
            report.add(f"[{label}] quality checks", NA, f"smoke run of {ticks} ticks", f"needs >= {SMOKE_TICKS} ticks",
                       "only the identity checks ran")
            report.sections.append(f"### [{label}] smoke run\n\nShorter than {SMOKE_TICKS} ticks: only the identity checks apply.")
            continue
        _latency_checks(report, label, ref, asy, th, stub)
        _distribution_checks(report, label, ref, asy, ref_pool, th)
        _tech_checks(report, label, ref, asy, th)
        _refusal_checks(report, label, ref, asy, th)
        _goal_type_check(report, label, ref, asy)
        extra = sorted({int(r.meta["seed"]) for r in ref_pool} - seeds_async)
        report.sections.append(f"### [{label}] runs\n\nPaired seeds: {sorted(seeds_async)}; extra reference seeds for the noise floor: {extra}.")
    for profile in sorted({r.meta.get("profile", "stock") for r in complete}):
        _probe_checks(report, profile, [r for r in complete if r.meta.get("profile", "stock") == profile], th)
    return report


def render(report: Report, title="Async galactic AI quality report") -> str:
    out = [f"# {title}", "", f"**Verdict: {report.verdict}**", ""]
    for w in report.warnings:
        out.append(f"> WARNING: {w}")
    if report.warnings:
        out.append("")
    out += ["| check | status | value | threshold | note |", "|---|---|---|---|---|"]
    for c in report.checks:
        out.append(f"| {c.name} | **{c.status}** | {c.value} | {c.threshold} | {c.note} |")
    out.append("")
    out += [s + "\n" for s in report.sections]
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="decision-log files, raw test logs, or directories of them")
    parser.add_argument("--out", type=Path, required=True, help="markdown report to write (a .json copy lands beside it)")
    parser.add_argument("--title", default="Async galactic AI quality report")
    args = parser.parse_args(argv)
    runs = []
    for item in args.inputs:
        if item.is_dir():
            runs.extend(logs.load_directory(item))
        else:
            runs.extend(logs.read_any(item))
    report = build_report(runs)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(render(report, args.title), encoding="utf-8", newline="\n")
    args.out.with_suffix(".json").write_text(json.dumps(
        {"verdict": report.verdict, "not_run": report.not_run, "warnings": report.warnings, "checks": [asdict(c) for c in report.checks]}, indent=2) + "\n",
        encoding="utf-8", newline="\n")
    print(f"{report.verdict}: {len(report.checks)} checks -> {args.out}")
    # 0: PASS or REVIEW (the owner reads the numbers); 1: FAIL; 2: nothing was measured (NOT-RUN) or a gated check had no data.
    return {FAIL: 1, NOT_RUN: 2, INCOMPLETE: 2}.get(report.verdict, 0)


if __name__ == "__main__":
    sys.exit(main())
