#!/usr/bin/env python3
"""The tag perturbation check (docs/tag-applied-check.md).

The tag registry says, per (class, tag), whether our code applies the value and for which object
types. This check proves it: for every `applied` or `partial` row and every type, the M2 battle
runs with that value changed in memory for the scene's objects of that type (foc_tag_perturb) and
is compared with the unchanged battle. A type the row says is applied must change the battle; a
type a `partial` row says is missing must not.

    tag_applied_check.py plan   --registry <json> --out <dir>
    tag_applied_check.py run    --registry <json> --binary <foc_tag_perturb> --out <dir>
                                [--game-root <install>] [--ticks N] [--jobs N] [--nice]
    tag_applied_check.py report --out <dir>
    tag_applied_check.py issues --out <dir> [--file]

`run` writes plan.json, plan.tsv, results.jsonl, report.json, report.md and one issue draft per
subsystem (area) with findings under issue-drafts/. `issues --file` files the drafts through the
nightly soak's issue client (create or comment only; EAWR_SOAK_REPO and its token file), searching
for an open issue of the same area first. Standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable

# The object types foc_tag_perturb can change, and the classes whose objects are not units.
TYPES = ("station", "ship", "squadron", "craft", "hardpoint", "projectile", "faction", "constants")
UNIT_TYPES = ("station", "ship", "squadron", "craft")
CLASS_TYPES = {"gameconstants": ("constants",), "faction": ("faction",), "hardpoint": ("hardpoint",),
               "projectile": ("projectile",)}
CHECKED = ("applied", "partial")
PRESENTATION_AREAS = ("presentation", "ui", "audio")
# The subsystem tracking issues of the tag registry (docs/tag-coverage.md).
AREA_TICKETS = {"movement": 649, "combat": 650, "fighters": 651, "ai": 652, "presentation": 653, "economy": 654}
# The [Walk] rule-list issues of each subsystem and the data-loading logic review (#791).
AREA_WALKS = {"movement": (721,), "combat": (707, 715, 760), "fighters": (695,), "ai": (737,), "economy": (728,)}
DATA_REVIEW = 791
TITLE_PREFIX = "Tag applied-check:"
NOT_IN_SCENE = "not exercised: the scene has no"
DEFAULT_HINTS = Path(__file__).resolve().parents[2] / "docs" / "tag-coverage" / "perturb-hints.json"


def fold(text: str) -> str:
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in text)


# ---- The registry -------------------------------------------------------------------------

def registry_rows(value: Any) -> list[dict[str, Any]]:
    """The rows of a registry file: a list, or an object with a `rows` list."""
    rows = value.get("rows") if isinstance(value, dict) else value
    if not isinstance(rows, list):
        raise ValueError("the registry has no rows list")
    return [row for row in rows if isinstance(row, dict)]


def listed(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [part.strip() for part in value.split(",") if part.strip()]
    return [str(part) for part in value]


def normalise(row: dict[str, Any], hints: dict[str, dict[str, Any]] | None = None) -> list[dict[str, Any]]:
    """The fields the check reads from a registry row, one per object class, with their defaults.

    Two shapes are read: the registry's (schema 2, docs/tag-coverage.md: `tag` below the class,
    `classes`, `applied` targets with their `types`, `missing_types`) and a flat one (`tag` from the
    class, optional `class`, `types`). The change hint comes from the row's `check` object, else
    from the hints file (docs/tag-coverage/perturb-hints.json) under "Class/tag"."""
    tag = str(row.get("tag", ""))
    classes = listed(row.get("classes")) or [str(row.get("class") or tag.split("/")[0])]
    types = listed(row.get("types"))
    code: list[str] = listed(row.get("code"))
    targets = row.get("applied")
    if isinstance(targets, list):
        for target in targets:
            if isinstance(target, dict):
                types += [t for t in listed(target.get("types")) if t not in types]
                code += [str(target["code"])] if target.get("code") else []
    result = []
    for element in classes:
        path = tag if fold(tag).startswith(fold(element) + "/") else f"{element}/{tag}"
        check = row.get("check") if isinstance(row.get("check"), dict) else None
        if check is None and hints:
            check = hints.get(fold(path))
        check = check or {}
        result.append({
            "class": element,
            "tag": path,
            "status": str(row.get("status", "")),
            "types": [fold(t) for t in types],
            "missing_types": [fold(t) for t in listed(row.get("missing_types"))],
            "area": str(row.get("area", "")),
            "ticket": row.get("ticket"),
            "change": str(check.get("perturb") or "auto"),
            "skip": str(check.get("skip") or ""),
            "code": code,
        })
    return result


def load_hints(path: Path | None) -> dict[str, dict[str, Any]]:
    """The perturbation hints, keyed by the folded "Class/tag" path."""
    if path is None or not path.is_file():
        return {}
    value = json.loads(path.read_text(encoding="utf-8"))
    return {fold(key): hint for key, hint in value.get("hints", {}).items() if isinstance(hint, dict)}


def candidate_types(element: str) -> tuple[str, ...]:
    return CLASS_TYPES.get(fold(element), UNIT_TYPES)


def expand(rows: Iterable[dict[str, Any]], hints: dict[str, dict[str, Any]] | None = None) -> list[dict[str, Any]]:
    """One check per (applied or partial row, class, type). A row that names its types expects
    each to change the battle and each missing type not to; a row that names none is tried on
    every type its class can have (a type without such objects in the scene drops out of the
    report)."""
    checks: list[dict[str, Any]] = []
    for row in (item for raw in rows for item in normalise(raw, hints)):
        if row["status"] not in CHECKED or not row["tag"]:
            continue
        key = {"class": row["class"], "tag": row["tag"], "status": row["status"], "area": row["area"],
               "ticket": row["ticket"], "code": row["code"]}
        preset = ""
        presentation_code = any(code.replace("\\", "/").startswith(
            ("apps/viewer/", "src/presentation/", "src/scene/")) for code in row["code"])
        if row["skip"]:
            preset = row["skip"]
        elif presentation_code:
            preset = "registry target is presentation or scene code: not checkable headless"
        elif row["area"] in PRESENTATION_AREAS:
            preset = f"{row['area']}: not checkable headless (the viewer draws it; no headless viewer report yet)"
        wanted = [(t, "changes", True) for t in row["types"]]
        wanted += [(t, "no change", True) for t in row["missing_types"] if t not in row["types"]]
        if not wanted:
            wanted = [(t, "changes", False) for t in candidate_types(row["class"])]
        for kind, expect, explicit in wanted:
            check = dict(key, type=kind, expect=expect, explicit=explicit, change=row["change"],
                         id=f"c{len(checks) + 1:04d}")
            if preset:
                check["preset"] = preset
            elif kind not in TYPES:
                check["preset"] = f"type {kind}: the check cannot change it headless"
            checks.append(check)
    return checks


def plan_tsv(checks: list[dict[str, Any]]) -> str:
    lines = ["id\tclass\ttag\ttype\tchange"]
    for check in checks:
        if "preset" in check:
            continue
        lines.append("\t".join((check["id"], check["class"], check["tag"], check["type"], check["change"])))
    return "\n".join(lines) + "\n"


# ---- Results ------------------------------------------------------------------------------

def read_results(path: Path) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    baseline: dict[str, Any] = {}
    results: dict[str, dict[str, Any]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        value = json.loads(line)
        if "baseline" in value:
            baseline = value["baseline"]
        else:
            results[value["id"]] = value
    return baseline, results


def outcome(check: dict[str, Any], result: dict[str, Any] | None) -> dict[str, Any]:
    """The check's verdict against its expectation. `finding` is set for a registry claim the
    battle contradicts."""
    if "preset" in check:
        return {"verdict": "not checkable", "reason": check["preset"], "finding": None, "in_scene": True}
    if result is None:
        return {"verdict": "not run", "reason": "no result (the run stopped early)", "finding": None, "in_scene": True}
    verdict, reason = result["verdict"], result["reason"]
    in_scene = not reason.startswith(NOT_IN_SCENE)
    finding = None
    if verdict == "no change" and check["expect"] == "changes" and in_scene and not reason.startswith("not exercised"):
        finding = "no change on an applied type"
    elif verdict == "changes" and check["expect"] == "no change":
        finding = "changes on a type the registry says is missing"
    if result.get("workers_check", "") and str(result["workers_check"]).startswith("diverged"):
        finding = "worker divergence"
    return {"verdict": verdict, "reason": reason, "finding": finding, "in_scene": in_scene}


def evidence(done: dict[str, Any], result: dict[str, Any] | None) -> str:
    if done["finding"] == "worker divergence":
        return "contradicted"
    if done["verdict"] == "changes":
        return "contradicted" if done["finding"] else "proven"
    objects = (result or {}).get("objects") or []
    if (done["finding"] and done["verdict"] == "no change" and objects
            and all(o.get("from") and o.get("read") is True for o in objects)
            and (result or {}).get("tables_changed") is False):
        return "contradicted"
    return "unproven"


def build_report(checks: list[dict[str, Any]], baseline: dict[str, Any], results: dict[str, dict[str, Any]],
                 meta: dict[str, Any]) -> dict[str, Any]:
    rows = []
    counts: dict[str, int] = {}
    findings = []
    for check in checks:
        result = results.get(check["id"])
        done = outcome(check, result)
        if not done["in_scene"] and not check["explicit"]:
            continue  # a type the class does not have in the M2 scene
        entry = {k: check.get(k) for k in ("id", "class", "tag", "type", "status", "area", "ticket", "expect",
                                           "explicit", "change", "code")}
        entry.update(done)
        entry["evidence"] = evidence(done, result)
        if result:
            for field in ("objects", "without_tag", "tables_changed", "first_tick", "kinds", "ticks_run",
                          "workers_check", "seconds"):
                entry[field] = result.get(field)
        rows.append(entry)
        label = done["verdict"]
        if label == "no change" and done["reason"].startswith("not exercised"):
            label = "not exercised"
        counts[label] = counts.get(label, 0) + 1
        if done["finding"]:
            findings.append(entry)
    # A row none of whose types the scene has (its class is not in the M2 scene, or it is a
    # document the check cannot change) still gets a line.
    kept = {(entry["class"], entry["tag"]) for entry in rows}
    for check in checks:
        key = (check["class"], check["tag"])
        if key in kept:
            continue
        kept.add(key)
        entry = {k: check[k] for k in ("id", "class", "tag", "status", "area", "ticket", "change")}
        entry.update(type="-", expect="changes", explicit=False, verdict="not in scene", finding=None, in_scene=False,
                     evidence="unproven", reason=f"the M2 scene has no object of class {check['class']} the check can change")
        rows.append(entry)
        counts["not in scene"] = counts.get("not in scene", 0) + 1
    proof_counts = {label: sum(entry["evidence"] == label for entry in rows)
                    for label in ("proven", "contradicted", "unproven")}
    return {"meta": meta, "baseline": baseline, "counts": dict(sorted(counts.items())),
            "proof_counts": proof_counts, "rows": rows,
            "findings": findings}


def cell(text: Any) -> str:
    return str(text if text is not None else "").replace("|", "\\|").replace("\n", " ")


def objects_text(entry: dict[str, Any], limit: int = 3) -> str:
    objects = entry.get("objects") or []
    def short(values: list[str]) -> str:
        text = " | ".join(" ".join(v.split()) for v in values) or "(none)"
        return text if len(text) <= 60 else text[:57] + "..."

    parts = [f"{o['id']} {short(o['from'])} -> {short(o['to'])}" for o in objects[:limit]]
    if len(objects) > limit:
        parts.append(f"{len(objects) - limit} more")
    return "; ".join(parts)


def render_report(report: dict[str, Any]) -> str:
    meta, baseline = report["meta"], report["baseline"]
    lines = ["# Tag perturbation check", ""]
    lines.append(f"Commit `{meta.get('commit', '')}`, {meta.get('date', '')}; registry `{meta.get('registry', '')}`.")
    lines.append(f"The M2 battle (the AI on both sides, seed {baseline.get('seed', '?')}), {baseline.get('ticks', '?')} "
                 f"ticks at 1 worker; baseline deterministic at {baseline.get('check_workers', '?')} workers: "
                 f"{'yes' if baseline.get('deterministic') else 'NO'}.")
    lines += ["", "| verdict | checks |", "|---|---|"]
    for verdict, count in report["counts"].items():
        lines.append(f"| {verdict} | {count} |")
    lines += ["", "Evidence: " + ", ".join(f"{count} {label}" for label, count in report["proof_counts"].items()) + "."]
    lines += ["", f"## Findings ({len(report['findings'])})", ""]
    if report["findings"]:
        lines += ["| area | class | tag | type | finding | why | values |", "|---|---|---|---|---|---|---|"]
        for entry in sorted(report["findings"], key=lambda e: (e["area"], e["tag"], e["type"])):
            lines.append(f"| {cell(entry['area'])} | {cell(entry['class'])} | `{cell(entry['tag'])}` | {entry['type']} | "
                         f"{cell(entry['finding'])} | {cell(entry['reason'])} | {cell(objects_text(entry))} |")
    else:
        lines.append("None: no contradiction or unproven applied claim was flagged; see the evidence column for coverage.")
    lines += ["", "## Every check", "",
              "| class | tag | type | status | expect | verdict | evidence | first tick | why |", "|---|---|---|---|---|---|---|---|---|"]
    for entry in report["rows"]:
        lines.append(f"| {cell(entry['class'])} | `{cell(entry['tag'])}` | {entry['type']} | {entry['status']} | "
                     f"{entry['expect']} | {entry['verdict']} | {entry['evidence']} | {cell(entry.get('first_tick'))} | {cell(entry['reason'])} |")
    return "\n".join(lines) + "\n"


def issue_drafts(report: dict[str, Any]) -> dict[str, tuple[str, str, str]]:
    """One draft per area with findings: (key, title, body). The key goes into the body so a later
    run finds the open issue and comments instead of filing again."""
    groups: dict[str, list[dict[str, Any]]] = {}
    for entry in report["findings"]:
        groups.setdefault(entry["area"] or "unassigned", []).append(entry)
    drafts = {}
    meta = report["meta"]
    # The types of the same row the battle does change with: a finding beside them is the #666
    # and #843 class (applied for one kind only), a partial the registry does not know.
    changes_on: dict[tuple[str, str], list[str]] = {}
    for row in report["rows"]:
        if row["verdict"] == "changes":
            changes_on.setdefault((row["class"], row["tag"]), []).append(row["type"])
    for area, entries in sorted(groups.items()):
        key = f"area {area}"
        tracking = AREA_TICKETS.get(area)
        strong = [e for e in entries if e["evidence"] == "contradicted"]
        weak = [e for e in entries if e["evidence"] == "unproven"]
        title = f"{TITLE_PREFIX} {area}: {len(strong)} contradicted, {len(weak)} unproven claims"
        fingerprint = finding_fingerprint(entries)
        body = [f"<!-- tag-applied-check {key} -->",
                f"<!-- tag-applied-findings {fingerprint} -->",
                f"The tag perturbation check (docs/tag-applied-check.md) changed each value below in memory for "
                f"the M2 scene's objects of one type and ran the AI-vs-AI M2 battle against the unchanged one "
                f"(commit `{str(meta.get('commit', ''))[:10]}`, {meta.get('date', '')}).",
                ""]
        links = ([f"#{tracking}"] if tracking else []) + [f"#{n}" for n in AREA_WALKS.get(area, ())] + [f"#{DATA_REVIEW}"]
        body.append("Tracking: " + ", ".join(links) + ".")
        tickets = sorted({e["ticket"] for e in entries if e.get("ticket")})
        if tickets:
            body.append("Registry tickets: " + ", ".join(f"#{t}" for t in tickets) + ".")
        for heading, group in (("Contradicted: read and dropped, missing-type changes, or worker divergence", strong),
                               (f"Unproven: the scenario may not reach the consumer in "
                                f"{report['baseline'].get('ticks', '?')} ticks", weak)):
            if not group:
                continue
            body += ["", f"### {heading} ({len(group)})", "",
                     "| class | tag | type | changes on | registry target | why | values |",
                     "|---|---|---|---|---|---|---|"]
            for entry in sorted(group, key=lambda e: (e["tag"], e["type"])):
                other = ", ".join(changes_on.get((entry["class"], entry["tag"]), [])) or "-"
                body.append(f"| {cell(entry['class'])} | `{cell(entry['tag'])}` | {entry['type']} | {other} | "
                            f"{cell(', '.join(f'`{c}`' for c in entry.get('code') or []))} | {cell(entry['reason'])} | "
                            f"{cell(objects_text(entry))} |")
        body += ["", "A read-and-dropped contradiction concerns the full unit tables in this headless load. "
                 "An unproven claim needs a focused scenario or consumer review; an unchanged battle alone "
                 "does not establish that a tag is not applied. Changes on another type establish proof only "
                 "for that tested type and window (docs/tag-coverage.md)."]
        drafts[area] = (key, title, "\n".join(body) + "\n")
    return drafts


# ---- Commands -----------------------------------------------------------------------------

def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, check=True,
                              cwd=Path(__file__).resolve().parent).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def write_plan(args: argparse.Namespace) -> list[dict[str, Any]]:
    registry = json.loads(Path(args.registry).read_text(encoding="utf-8"))
    checks = expand(registry_rows(registry), load_hints(args.hints))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "plan.json").write_text(json.dumps(
        {"registry": str(args.registry), "checks": checks}, indent=2) + "\n", encoding="utf-8")
    (args.out / "plan.tsv").write_text(plan_tsv(checks), encoding="utf-8", newline="\n")
    runnable = sum(1 for check in checks if "preset" not in check)
    print(f"plan: {len(checks)} check(s), {runnable} to run, in {args.out}")
    return checks


def plan_command(args: argparse.Namespace) -> int:
    write_plan(args)
    return 0


def run_command(args: argparse.Namespace) -> int:
    checks = write_plan(args)
    game_root = args.game_root or os.environ.get("EAWR_EAW_GAME_ROOT", "")
    if not game_root:
        print("tag_applied_check: --game-root or EAWR_EAW_GAME_ROOT is required", file=sys.stderr)
        return 2
    command = [str(args.binary), "--plan", str(args.out / "plan.tsv"), "--out", str(args.out / "results.jsonl"),
               "--game-root", game_root, "--ticks", str(args.ticks), "--jobs", str(args.jobs),
               "--seed", str(args.seed), "--check-workers", str(args.check_workers),
               "--check-every", str(args.check_every)]
    if args.nice and os.name == "posix":
        command = ["nice", "-n", "19", *command]
    started = datetime.datetime.now(datetime.timezone.utc)
    with (args.out / "perturb.log").open("w", encoding="utf-8") as log:
        code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False).returncode
    meta = {"commit": git_commit(), "date": started.strftime("%Y-%m-%dT%H:%M:%SZ"), "registry": str(args.registry),
            "exit": code, "seconds": round((datetime.datetime.now(datetime.timezone.utc) - started).total_seconds(), 1)}
    (args.out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    if code not in (0, 1) or not (args.out / "results.jsonl").is_file():
        print(f"tag_applied_check: foc_tag_perturb exit {code}; see {args.out / 'perturb.log'}", file=sys.stderr)
        return 2
    return report_from(args.out, checks, meta)


def report_from(out: Path, checks: list[dict[str, Any]], meta: dict[str, Any]) -> int:
    baseline, results = read_results(out / "results.jsonl")
    report = build_report(checks, baseline, results, meta)
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (out / "report.md").write_text(render_report(report), encoding="utf-8", newline="\n")
    drafts = out / "issue-drafts"
    drafts.mkdir(exist_ok=True)
    for stale in drafts.glob("*.md"):
        stale.unlink()
    for area, (_key, title, body) in issue_drafts(report).items():
        (drafts / f"{re.sub(r'[^A-Za-z0-9_.-]+', '-', area)}.md").write_text(f"{title}\n\n{body}", encoding="utf-8")
    print(f"report: {report['counts']}; {len(report['findings'])} finding(s); {out / 'report.md'}")
    return 0 if baseline.get("deterministic") and not any(
        entry["finding"] == "worker divergence" for entry in report["findings"]) else 1


def report_command(args: argparse.Namespace) -> int:
    plan = json.loads((args.out / "plan.json").read_text(encoding="utf-8"))
    meta_path = args.out / "meta.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.is_file() else {"registry": plan["registry"]}
    return report_from(args.out, plan["checks"], meta)


def finding_fingerprint(entries: list[dict[str, Any]]) -> str:
    facts = sorted((e["class"], e["tag"], e["type"], e["finding"], e["evidence"]) for e in entries)
    return hashlib.sha256(json.dumps(facts, separators=(",", ":")).encode()).hexdigest()


def latest_fingerprint(client: Any, number: int, body: str) -> str | None:
    """Read persisted issue/comment markers, including repeats across hosts or restarts."""
    marker = re.compile(r"<!-- tag-applied-findings ([0-9a-f]{64}) -->")
    found = marker.search(body)
    latest = found[1] if found else None
    page = 1
    while True:
        comments = client._request("GET", f"/repos/{client.repo}/issues/{number}/comments?per_page=100&page={page}") or []
        for comment in comments:
            found = marker.search(comment.get("body") or "")
            if found:
                latest = found[1]
        if len(comments) < 100:
            return latest
        page += 1


def issues_command(args: argparse.Namespace) -> int:
    report = json.loads((args.out / "report.json").read_text(encoding="utf-8"))
    drafts = issue_drafts(report)
    if not args.file:
        for area, (key, title, _body) in drafts.items():
            print(f"{key}: draft {title}")
        return 0
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "soak"))
    import soak_issues  # noqa: E402  the nightly soak's create-and-comment-only client

    client = soak_issues.client_from_environment()
    if client is None:
        print("tag_applied_check: no issue client (EAWR_SOAK_REPO and its token file); drafts only", file=sys.stderr)
        return 1
    for area, (key, title, body) in drafts.items():
        item = find_open_item(client, key)
        number = int(item["number"]) if item else None
        if number is None:
            print(f"{key}: created issue #{client.create(title, body)}")
        elif latest_fingerprint(client, number, item.get("body") or "") == re.search(
                r"<!-- tag-applied-findings ([0-9a-f]{64}) -->", body)[1]:
            print(f"{key}: unchanged findings on issue #{number}; no comment")
        else:
            client.comment(number, body)
            print(f"{key}: commented on issue #{number}")
    return 0


def find_open_item(client: Any, key: str) -> dict[str, Any] | None:
    """The open issue of the check that carries `key`, searched by the title prefix."""
    import urllib.parse

    query = urllib.parse.urlencode({"q": f'repo:{client.repo} is:issue is:open in:title "{TITLE_PREFIX}"',
                                    "per_page": 100})
    found = client._request("GET", f"/search/issues?{query}") or {}
    for item in found.get("items", []):
        if f"<!-- tag-applied-check {key} -->" in (item.get("body") or ""):
            return item
    return None


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    plan = commands.add_parser("plan")
    plan.add_argument("--registry", type=Path, required=True)
    plan.add_argument("--out", type=Path, required=True)
    plan.add_argument("--hints", type=Path, default=DEFAULT_HINTS)
    plan.set_defaults(handler=plan_command)
    run = commands.add_parser("run")
    run.add_argument("--registry", type=Path, required=True)
    run.add_argument("--binary", type=Path, required=True)
    run.add_argument("--out", type=Path, required=True)
    run.add_argument("--hints", type=Path, default=DEFAULT_HINTS)
    run.add_argument("--game-root", default="")
    run.add_argument("--ticks", type=int, default=3600)
    run.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    run.add_argument("--seed", type=int, default=1)
    run.add_argument("--check-workers", type=int, default=4)
    run.add_argument("--check-every", type=int, default=10)
    run.add_argument("--nice", action="store_true", help="run the driver at nice 19 (POSIX)")
    run.set_defaults(handler=run_command)
    report = commands.add_parser("report")
    report.add_argument("--out", type=Path, required=True)
    report.set_defaults(handler=report_command)
    issues = commands.add_parser("issues")
    issues.add_argument("--out", type=Path, required=True)
    issues.add_argument("--file", action="store_true", help="file or comment through the soak's issue client")
    issues.set_defaults(handler=issues_command)
    args = parser.parse_args(list(argv) if argv is not None else None)
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
