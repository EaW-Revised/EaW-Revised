#!/usr/bin/env python3
"""Generate the public XML tag report using only statuses.json; --check detects stale pages."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import html
import json
from pathlib import Path
import tempfile

if __package__ in (None, ""):
    from tag_registry import load_registry
else:
    from .tag_registry import load_registry

ROOT = Path(__file__).resolve().parents[2]
STATUSES = {
    "applied": "The value reaches our simulation or presentation; parsing alone does not count.",
    "partial": "Applied for the listed kinds, with missing kinds still to implement.",
    "todo": "A behaviour remains to implement; the ticket tracks the work.",
    "presentation-later": "Presentation work outside the current space battle scope.",
    "foc-ignores": "The registry records evidence that FoC ignores this value.",
    "deferred": "Explicitly postponed; see the note or tracking ticket.",
    "by-design": "Our implementation intentionally replaces this mechanism; the evidence cites the rule that says so.",
    "land-or-galactic": "Land battle or galactic campaign work outside space skirmish.",
    "multiplayer": "Multiplayer work outside the current scope.",
}
EXCLUDED = {"land-or-galactic", "multiplayer", "presentation-later"}


def cell(value: object) -> str:
    """Literal table text: registry notes must not create links or break Markdown."""
    text = html.escape(str(value), quote=False).replace("#", "&#35;")
    return (text.replace("|", "&#124;").replace("`", "&#96;")
            .replace("[", "&#91;").replace("]", "&#93;")
            .replace("\r\n", "<br>").replace("\n", "<br>").replace("\r", "<br>"))


def ordered(values) -> list:
    return sorted(set(values), key=lambda value: (value.casefold(), value))


def counts(rows: list[dict]) -> Counter:
    result = Counter()
    for row in rows:
        result[row["status"]] += len(set(row["classes"]))
    return result


def totals(rows: list[dict]) -> list[str]:
    all_counts = counts(rows)
    space = counts([row for row in rows if row["status"] not in EXCLUDED])
    return ["| Status | Space skirmish | Everything |", "|---|---:|---:|"] + [
        f"| {status} | {space[status]} | {all_counts[status]} |" for status in STATUSES
    ] + [f"| **Total** | **{sum(space.values())}** | **{sum(all_counts.values())}** |"]


def render(registry: dict) -> dict[str, str]:
    rows = registry["rows"]
    areas = ordered(row["area"] for row in rows)
    if any(not area.replace("-", "").isalnum() for area in areas):
        raise ValueError("area must be a filename-safe name")
    intro = ["# XML tag coverage", "", "Generated from [statuses.json](../statuses.json). For how coverage is assessed, see the",
             "[registry guide](../../tag-coverage.md). These are registry claims, not a runtime test of every tag.", "",
             "Counts are **(object class, tag) pairs**, not unique tag names or JSON rows. A row shared by",
             "three classes contributes three pairs. Space skirmish excludes `land-or-galactic`,",
             "`multiplayer` and `presentation-later` rows; it includes `deferred` rows so postponed battle",
             "work remains visible. Mixed classes retain the registry's classification. This is a scope",
             "view, not a census of tags used in a particular battle.", "", "## Status legend", "",
             "| Status | Meaning |", "|---|---|"]
    intro += [f"| {status} | {meaning} |" for status, meaning in STATUSES.items()]
    intro += ["", "## Totals by status", ""] + totals(rows)
    intro += ["", "## Totals by area", "", "| Area | Space skirmish | Everything |", "|---|---:|---:|"]
    result = {}
    for area in areas:
        selected = [row for row in rows if row["area"] == area]
        everything = sum(counts(selected).values())
        space = sum(counts([row for row in selected if row["status"] not in EXCLUDED]).values())
        intro.append(f"| [{cell(area)}]({area}.md) | {space} | {everything} |")
        lines = [f"# {cell(area)} tag coverage", "", "[All areas and legend](README.md)", ""] + totals(selected)
        lines += ["", "Tables group the exact object class families listed together in the registry. Object kinds",
                  "are station, ship, squadron or craft when specified; an empty kind list means the whole",
                  "listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:",
                  "the registry does not supply public link targets. Tickets refer to the private tracker",
                  "and are plain numbers. Code locations are repository paths without identifier anchors.", ""]
        families = defaultdict(list)
        for row in selected:
            families[tuple(ordered(row["classes"]))].append(row)
        for family in sorted(families, key=lambda names: tuple((name.casefold(), name) for name in names)):
            lines += ["## " + ", ".join(cell(name) for name in family), "",
                      "| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |",
                      "|---|---|---|---|---|---|---|"]
            for row in sorted(families[family], key=lambda row: (row["tag"].casefold(), row["tag"], json.dumps(row, sort_keys=True))):
                applications = sorted(row.get("applied", []), key=lambda item: json.dumps(item, sort_keys=True))
                kinds = []
                rules = []
                paths = []
                for application in applications:
                    path = application["code"].split("#", 1)[0]
                    kind = ", ".join(ordered(application.get("types", []))) or "whole class"
                    kinds.append(f"{kind} ({path})")
                    rules.append(", ".join(ordered(application.get("rules", []))) or "none recorded")
                    paths.append(path)
                if row.get("missing_types"):
                    kinds.append("missing: " + ", ".join(ordered(row["missing_types"])))
                notes = [row[key] for key in ("note", "evidence") if row.get(key)]
                for exclusion in sorted(row.get("no_targets", []), key=lambda item: json.dumps(item, sort_keys=True)):
                    kind = ", ".join(ordered(exclusion["types"]))
                    kinds.append(f"{kind}: {exclusion['status']}")
                    source = exclusion["evidence"]
                    notes.append(f"{kind}: {registry.get('evidence', {}).get(source, source)}")
                if row.get("basis"):
                    notes.append("basis: " + row["basis"])
                if row.get("trace"):
                    notes.append("trace: " + row["trace"])
                values = [row["tag"], row["status"], "; ".join(kinds) or "application not recorded",
                          "; ".join(rules) or "—", row.get("ticket", "—"), "; ".join(ordered(paths)) or "—", "; ".join(notes) or "—"]
                lines.append("| " + " | ".join(cell(value) for value in values) + " |")
            lines.append("")
        result[f"{area}.md"] = "\n".join(lines).rstrip() + "\n"
    intro += ["", "Regenerate with `python tools/inventory/tag_report.py`. Do not edit generated pages by hand.", ""]
    result["README.md"] = "\n".join(intro)
    return result


def write_report(pages: dict[str, str], destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for stale in destination.glob("*.md"):
        if stale.name not in pages:
            stale.unlink()
    for name, content in pages.items():
        (destination / name).write_bytes(content.encode("utf-8"))


def check_report(pages: dict[str, str], destination: Path) -> list[str]:
    # Exercise the same writer as regeneration, including byte-for-byte LF output.
    with tempfile.TemporaryDirectory(prefix="tag-report-") as temporary:
        generated = Path(temporary)
        write_report(pages, generated)
        actual = {path.name for path in destination.glob("*.md")}
        return sorted(name for name in actual | set(pages)
                      if name not in pages or name not in actual
                      or (destination / name).read_bytes() != (generated / name).read_bytes())


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--statuses", type=Path, default=ROOT / "docs/tag-coverage/statuses.json")
    parser.add_argument("--out", type=Path, default=ROOT / "docs/tag-coverage/report")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    pages = render(load_registry(args.statuses))
    if args.check:
        different = check_report(pages, args.out)
        if different:
            print("XML tag report is stale: " + ", ".join(different))
            print("Run python tools/inventory/tag_report.py and commit docs/tag-coverage/report/.")
            return 1
        print(f"XML tag report: {len(pages)} pages match.")
    else:
        write_report(pages, args.out)
        print(f"XML tag report: wrote {len(pages)} pages.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
