#!/usr/bin/env python3
"""The XML tag registry (docs/tag-coverage.md): every FoC XML tag has a target in our code, or a known reason it has none.

The universe is every (object class, tag path) of the FoC XML the game loads, taken from the committed tag
inventory (plan/inventories/xml-tags.json) with data-defined names collapsed to `*`. The registry
(docs/tag-coverage/statuses.json, schema_version 2) gives each pair exactly one status. This module loads and
validates it, renders its Markdown view and reports on it; tag_coverage.py joins it with the M2 load trace.

check    the gate without game data: every universe pair has a row, every row has what its status needs,
         every `applied` code location still exists, the JSON is canonical.
check-data
         the same plus the game data: every element or attribute name in the effective FoC XML is in the
         inventory (a stale inventory hides new tags from the gate).
render   writes the report (counts, partial rows, top todo) from statuses.json to out/tag-coverage/statuses.md;
         it is generated on demand and never committed.
format   rewrites statuses.json in its canonical layout.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from common import ascii_fold
else:
    from .common import ascii_fold

SCHEMA_VERSION = 2
# Code the M2 load trace's tags feed: the unit tables, the skirmish start, the simulation and the data layer. An
# `applied` row that cites one of these is for a tag the trace must show read (tag_coverage.py scene).
TRACED_CODE = ("src/units/", "src/skirmish/", "src/sim/", "src/data/")
STATUSES = ("applied", "partial", "todo", "foc-ignores", "land-or-galactic", "multiplayer", "presentation-later",
            "deferred")
# Statuses that say "out of scope for now": each names a ticket or a reason.
OUT_OF_SCOPE = ("land-or-galactic", "multiplayer", "presentation-later", "deferred")
AREAS = ("movement", "combat", "fighters", "ai", "presentation", "economy", "data")
BASES = ("auto", "reviewed")
ROW_FIELDS = {"applied", "area", "basis", "classes", "evidence", "missing_types", "note", "status", "tag", "ticket",
              "trace"}
TRACES = ("unrecorded",)
APPLIED_FIELDS = {"code", "rules", "types"}
RULE_ID = re.compile(r"^[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*-\d+[a-z]?$")


class RegistryError(RuntimeError):
    pass


# --- The universe ---------------------------------------------------------------------------------

def normalise_path(path: str) -> str:
    """The inventory's tag path with the data-defined segments (goal names, dialog control names, planets, terrain
    kinds, equation and enum names) collapsed to `*`, so the universe holds tag names and not instance names."""
    parts = path.split("/")
    head = parts[0]
    if head == "GUIDialogs" and len(parts) > 2 and parts[1] in ("Fonts", "Textures"):
        parts[2] = "*"
    elif head in ("Goals", "FunctionSet", "AITemplates") and len(parts) > 1:
        parts[1] = "*"
    elif head in ("Equations", "EnumDefinition", "MovementClassType") and len(parts) > 1:
        parts[1] = "*"
    elif head == "Galactic_Markup" and len(parts) > 2 and parts[1] == "Planets":
        parts[2] = "*"
    elif head in ("Terrain_Effectiveness", "HintSets") and len(parts) > 1:
        for index in range(1, min(3, len(parts))):
            parts[index] = "*"
    return "/".join(parts)


def split_key(path: str) -> tuple[str, str]:
    """(object class, tag path below it): `SpaceUnit/Abilities/Stun_Ability/Stun_Range` is class `SpaceUnit`."""
    head, _, tail = path.partition("/")
    return head, tail


@dataclass
class UniverseRow:
    object_class: str
    tag: str
    node_kind: str
    usage: int = 0
    files: int = 0
    object_types: set[str] = field(default_factory=set)


def is_attribute(path: str) -> bool:
    return "/@" in path


def load_universe(inventory: dict[str, Any], profile: str = "foc") -> dict[tuple[str, str], UniverseRow]:
    """The (class, tag) pairs of the inventory's profile, keyed by their ASCII-folded spelling: every attribute and
    every element that has no element below it (a container carries no value of its own)."""
    normalised: dict[str, tuple[str, str, int, int, str]] = {}
    for row in inventory["rows"]:
        if row["profile"] != profile:
            continue
        path = normalise_path(row["tag_path"])
        key = ascii_fold(path)
        old = normalised.get(key)
        usage = row["usage_count"] + (old[2] if old else 0)
        files = max(row["file_count"], old[3] if old else 0)
        normalised[key] = (old[0] if old else path, row["node_kind"], usage, files,
                           row["object_type"] if not old else old[4] + "," + row["object_type"])
    containers: set[str] = set()
    for key in normalised:
        if is_attribute(key):
            continue
        parts = key.split("/")
        for index in range(1, len(parts)):
            containers.add("/".join(parts[:index]))
    pairs: dict[tuple[str, str], UniverseRow] = {}
    for key, (path, kind, usage, files, types) in normalised.items():
        object_class, tail = split_key(path)
        if not tail:
            continue  # the object's own element
        if kind == "element" and key in containers:
            continue
        pair = (ascii_fold(object_class), ascii_fold(tail))
        row = pairs.get(pair)
        if row is None:
            row = pairs[pair] = UniverseRow(object_class, tail, kind)
        row.usage += usage
        row.files = max(row.files, files)
        row.object_types.update(types.split(","))
    return pairs


# --- The registry ---------------------------------------------------------------------------------

def load_registry(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema_version") != SCHEMA_VERSION:
        raise RegistryError(f"{path}: not a schema_version {SCHEMA_VERSION} tag registry")
    return value


def pair_key(object_class: str, tag: str) -> tuple[str, str]:
    return ascii_fold(object_class), ascii_fold(tag)


def expand(registry: dict[str, Any]) -> Iterable[tuple[str, str, dict[str, Any]]]:
    """(class, tag, row) for every pair the registry covers."""
    for row in registry["rows"]:
        for object_class in row.get("classes", ()):
            yield object_class, row.get("tag", ""), row


def row_order(row: dict[str, Any]) -> tuple[str, str]:
    classes = row.get("classes") or [""]
    return ascii_fold(row.get("tag", "")), ascii_fold(classes[0])


def resolve_evidence(registry: dict[str, Any], row: dict[str, Any]) -> str:
    text = row.get("evidence", "")
    return registry.get("evidence", {}).get(text, text)


def code_location(location: str) -> tuple[str, str]:
    path, _, identifier = location.partition("#")
    return path, identifier


_RULES: dict[Path, set[str]] = {}


def known_rules(root: Path) -> set[str]:
    """Every rule ID that appears in a docs/behaviour note."""
    if root not in _RULES:
        found: set[str] = set()
        token = re.compile(r"\b([A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*-\d+[a-z]?)\b")
        for path in sorted((root / "docs" / "behaviour").rglob("*.md")):
            found.update(token.findall(path.read_text(encoding="utf-8", errors="replace")))
        _RULES[root] = found
    return _RULES[root]


def registry_problems(registry: dict[str, Any], universe: dict[tuple[str, str], UniverseRow] | None,
                      root: Path | None) -> list[str]:
    """Everything the gate checks. `universe` None skips the coverage check, `root` None the code locations."""
    problems: list[str] = []
    seen: dict[tuple[str, str], str] = {}
    unknown_top = set(registry) - {"schema_version", "evidence", "rows", "note"}
    if unknown_top:
        problems.append(f"registry: unknown top-level fields {sorted(unknown_top)}")
    evidence = registry.get("evidence", {})
    for name, text in evidence.items():
        if not text.strip():
            problems.append(f"evidence {name!r} is empty")
    file_text: dict[str, str | None] = {}

    def text_of(relative: str) -> str | None:
        if relative not in file_text:
            path = root / relative if root else None
            file_text[relative] = path.read_text(encoding="utf-8", errors="replace") if path and path.is_file() else None
        return file_text[relative]

    for row in registry["rows"]:
        tag = row.get("tag", "")
        classes = row.get("classes", [])
        where = f"row {tag!r} {classes}"
        if not tag:
            problems.append(f"{where}: no tag")
        if not classes or not isinstance(classes, list) or classes != sorted(set(classes), key=ascii_fold):
            problems.append(f"{where}: classes must be a non-empty sorted list without duplicates")
            classes = classes if isinstance(classes, list) else []
        unknown = set(row) - ROW_FIELDS
        if unknown:
            problems.append(f"{where}: unknown fields {sorted(unknown)}")
        for object_class in classes:
            key = pair_key(object_class, tag)
            if key in seen:
                problems.append(f"{where}: {object_class}/{tag} is also in the row for {seen[key]}")
            seen[key] = f"{tag!r} {classes[:1]}"
        status = row.get("status")
        if status not in STATUSES:
            problems.append(f"{where}: status {status!r} is not one of {', '.join(STATUSES)}")
            continue
        if row.get("area") not in AREAS:
            problems.append(f"{where}: area {row.get('area')!r} is not one of {', '.join(AREAS)}")
        if row.get("trace") is not None and row["trace"] not in TRACES:
            problems.append(f"{where}: trace {row['trace']!r} is not one of {', '.join(TRACES)}")
        if row.get("basis", "reviewed") not in BASES:
            problems.append(f"{where}: basis {row.get('basis')!r} is not one of {', '.join(BASES)}")
        ticket = row.get("ticket")
        if ticket is not None and (not isinstance(ticket, int) or isinstance(ticket, bool) or ticket <= 0):
            problems.append(f"{where}: ticket must be an issue number")
        text = resolve_evidence(registry, row)
        if row.get("evidence") and row["evidence"] not in evidence and " " not in row["evidence"]:
            problems.append(f"{where}: evidence {row['evidence']!r} is not defined in the evidence table")
        if status in ("applied", "partial"):
            targets = row.get("applied")
            if not isinstance(targets, list) or not targets:
                problems.append(f"{where}: an {status} row lists the code that applies it")
                targets = []
            for target in targets:
                extra = set(target) - APPLIED_FIELDS
                if extra:
                    problems.append(f"{where}: unknown applied fields {sorted(extra)}")
                location = target.get("code", "")
                path, identifier = code_location(location)
                if not path or not identifier:
                    problems.append(f"{where}: code location {location!r} is not path#identifier")
                elif root is not None:
                    body = text_of(path)
                    if body is None:
                        problems.append(f"{where}: code location {location}: {path} does not exist")
                    elif identifier not in body:
                        problems.append(f"{where}: code location {location}: {identifier!r} is not in {path}")
                rules = target.get("rules", [])
                for rule in rules:
                    if not RULE_ID.match(rule):
                        problems.append(f"{where}: {rule!r} is not a rule ID such as WCC-02")
                if not rules and row.get("basis", "reviewed") == "reviewed" and not row.get("note"):
                    problems.append(f"{where}: a reviewed applied target names its rule IDs, or a note says none exists")
                if root is not None:
                    for rule in rules:
                        if rule not in known_rules(root):
                            problems.append(f"{where}: rule {rule} is in no docs/behaviour note")
                types = target.get("types", [])
                if types != sorted(set(types), key=ascii_fold):
                    problems.append(f"{where}: applied types must be a sorted list without duplicates")
            if status == "partial":
                if not row.get("missing_types"):
                    problems.append(f"{where}: a partial row lists the missing types")
                if not any(t.get("types") for t in targets):
                    problems.append(f"{where}: a partial row lists the types it applies to")
                if ticket is None:
                    problems.append(f"{where}: a partial row links the ticket for the missing types")
            elif row.get("missing_types"):
                problems.append(f"{where}: only a partial row has missing_types")
        else:
            if row.get("applied") or row.get("missing_types"):
                problems.append(f"{where}: a {status} row has no applied targets")
            if status == "todo" and ticket is None:
                problems.append(f"{where}: a todo row links its ticket")
            if status == "foc-ignores" and "debug build" not in text:
                problems.append(f"{where}: foc-ignores evidence cites the debug build")
            if status in OUT_OF_SCOPE and ticket is None and not text.strip():
                problems.append(f"{where}: a {status} row names a ticket or gives its reason as evidence")
            if status == "deferred" and ticket is None:
                problems.append(f"{where}: a deferred row names its ticket")
    order = [row_order(row) for row in registry["rows"]]
    if order != sorted(order):
        problems.append("registry rows are not sorted by tag, then first class (ASCII case folded)")
    if universe is not None:
        missing = sorted(set(universe) - set(seen))
        for pair in missing[:40]:
            row = universe[pair]
            problems.append(f"loaded tag {row.object_class}/{row.tag} has no registry row")
        if len(missing) > 40:
            problems.append(f"... and {len(missing) - 40} more loaded tags without a row")
        stale = sorted(set(seen) - set(universe))
        for pair in stale[:40]:
            problems.append(f"registry row {pair[0]}/{pair[1]} is not in the loaded XML; drop it")
        if len(stale) > 40:
            problems.append(f"... and {len(stale) - 40} more stale rows")
    return problems


# --- The canonical layout -------------------------------------------------------------------------

def dumps_registry(registry: dict[str, Any]) -> str:
    """statuses.json's layout: the header keys, then one row per line with sorted keys."""
    lines = ["{", f'  "schema_version": {registry["schema_version"]},']
    if "note" in registry:
        lines.append(f'  "note": {json.dumps(registry["note"], ensure_ascii=False)},')
    evidence = registry.get("evidence", {})
    lines.append('  "evidence": {')
    items = sorted(evidence.items())
    for index, (name, text) in enumerate(items):
        comma = "," if index + 1 < len(items) else ""
        lines.append(f"    {json.dumps(name)}: {json.dumps(text, ensure_ascii=False)}{comma}")
    lines.append("  },")
    lines.append('  "rows": [')
    rows = registry["rows"]
    for index, row in enumerate(rows):
        comma = "," if index + 1 < len(rows) else ""
        lines.append("    " + json.dumps(row, ensure_ascii=False, sort_keys=True, separators=(", ", ": ")) + comma)
    lines += ["  ]", "}"]
    return "\n".join(lines) + "\n"


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


# --- The report -----------------------------------------------------------------------------------

def counts(registry: dict[str, Any]) -> tuple[dict[str, int], dict[tuple[str, str], int]]:
    """Pair counts by status, and by (status, area)."""
    by_status: dict[str, int] = defaultdict(int)
    by_area: dict[tuple[str, str], int] = defaultdict(int)
    for _object_class, _tag, row in expand(registry):
        by_status[row["status"]] += 1
        by_area[(row["status"], row["area"])] += 1
    return dict(by_status), dict(by_area)


def render(registry: dict[str, Any], universe: dict[tuple[str, str], UniverseRow] | None = None,
           top_todo: int = 40) -> str:
    by_status, by_area = counts(registry)
    total = sum(by_status.values())
    lines = [
        "# Tag registry",
        "",
        "Generated from `docs/tag-coverage/statuses.json` by `python tools/inventory/tag_registry.py render`; "
        "edit the JSON, never this file.",
        "What the registry is and how a PR that applies a tag updates its row: docs/tag-coverage.md.",
        "",
        f"{total} (class, tag) pairs in {len(registry['rows'])} rows. Every pair the FoC XML loads has exactly one status.",
        "",
        "| Status | Pairs |",
        "|---|---|",
    ]
    lines += [f"| {status} | {by_status.get(status, 0)} |" for status in STATUSES]
    lines += ["", "| Area | " + " | ".join(STATUSES) + " |", "|---|" + "---|" * len(STATUSES)]
    for area in AREAS:
        cells = [str(by_area.get((status, area), 0)) for status in STATUSES]
        lines.append(f"| {area} | " + " | ".join(cells) + " |")
    unreviewed = sum(1 for _c, _t, row in expand(registry)
                     if row["status"] in ("applied", "partial") and row.get("basis", "reviewed") == "auto")
    if unreviewed:
        lines += ["", f"{unreviewed} applied or partial pairs are `auto`: seeded from a scan of the loaders and the "
                  "uses of the field they fill, not yet reviewed by hand."]
    partial = [row for row in registry["rows"] if row["status"] == "partial"]
    lines += ["", "## Partial: applied for some types, not others", ""]
    if partial:
        lines += ["| Tag | Classes | Applies to | Missing | Ticket |", "|---|---|---|---|---|"]
        for row in partial:
            applies = sorted({t for target in row["applied"] for t in target.get("types", [])}, key=ascii_fold)
            lines.append(f"| `{row['tag']}` | {', '.join(row['classes'])} | {', '.join(applies)} | "
                         f"{', '.join(row['missing_types'])} | #{row['ticket']} |")
    else:
        lines.append("None.")
    todo = [row for row in registry["rows"] if row["status"] == "todo"]
    tickets: dict[int, int] = defaultdict(int)
    for row in todo:
        tickets[row["ticket"]] += len(row["classes"])
    lines += ["", "## Todo by ticket (pairs)", "", "| Ticket | Pairs |", "|---|---|"]
    lines += [f"| #{ticket} | {count} |" for ticket, count in sorted(tickets.items())]
    if universe is not None:
        weighted = []
        for row in todo:
            usage = sum(universe[pair_key(c, row["tag"])].usage for c in row["classes"]
                        if pair_key(c, row["tag"]) in universe)
            weighted.append((usage, row))
        weighted.sort(key=lambda item: (-item[0], ascii_fold(item[1]["tag"])))
        lines += ["", f"## The {top_todo} most-authored todo tags", "",
                  "| Tag | Classes | Authored in | Area | Ticket |", "|---|---|---|---|---|"]
        for usage, row in weighted[:top_todo]:
            classes = ", ".join(row["classes"][:4]) + (f" +{len(row['classes']) - 4}" if len(row["classes"]) > 4 else "")
            lines.append(f"| `{row['tag']}` | {classes} | {usage} | {row['area']} | #{row['ticket']} |")
    return "\n".join(lines) + "\n"


# --- Commands -------------------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_STATUSES = ROOT / "docs" / "tag-coverage" / "statuses.json"
DEFAULT_INVENTORY = ROOT / "plan" / "inventories" / "xml-tags.json"
DEFAULT_REPORT = ROOT / "out" / "tag-coverage" / "statuses.md"


def load_inventory(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def gate_problems(statuses: Path, inventory: Path, root: Path | None) -> tuple[list[str], dict[str, Any], dict]:
    registry = load_registry(statuses)
    universe = load_universe(load_inventory(inventory))
    problems = registry_problems(registry, universe, root)
    if not problems and statuses.read_text(encoding="utf-8") != dumps_registry(registry):
        problems.append(f"{statuses.name} is not in its canonical layout; run format")
    if not problems:
        try:
            render(registry, universe)
        except (KeyError, TypeError) as exc:  # a row shape the checks above do not cover
            problems.append(f"the report cannot be rendered from the rows: {type(exc).__name__} {exc}")
    return problems, registry, universe


def print_problems(problems: Sequence[str], limit: int = 60) -> None:
    for problem in problems[:limit]:
        print(f"tag_registry: {problem}", file=sys.stderr)
    if len(problems) > limit:
        print(f"tag_registry: ... and {len(problems) - limit} more", file=sys.stderr)


def check_command(args: argparse.Namespace) -> int:
    problems, registry, _universe = gate_problems(args.statuses, args.inventory, args.repo)
    print_problems(problems)
    if not problems:
        by_status, _ = counts(registry)
        print("tag_registry: ok " + json.dumps(by_status, sort_keys=True))
    return 1 if problems else 0


def data_names(game_root: Path) -> set[str]:
    """Every element and attribute name of the effective FoC XML (ASCII folded)."""
    if __package__ in (None, ""):
        from corpus import Corpus
        from xml_inventory import _configure_safe_parser
    else:
        from .corpus import Corpus
        from .xml_inventory import _configure_safe_parser
    import xml.parsers.expat

    names: set[str] = set()
    corpus = Corpus(game_root)
    for source in corpus.iter_sources("foc", ".xml", "effective"):
        parser = xml.parsers.expat.ParserCreate()
        _configure_safe_parser(parser, source.logical_path)

        def start(name: str, attributes: dict[str, str]) -> None:
            names.add(ascii_fold(name))
            names.update("@" + ascii_fold(a) for a in attributes)

        parser.StartElementHandler = start
        try:
            parser.Parse(source.data, True)
        except xml.parsers.expat.ExpatError:
            continue  # a file FoC cannot parse either; the inventory records the failure
    return names


def check_data_command(args: argparse.Namespace) -> int:
    problems, registry, universe = gate_problems(args.statuses, args.inventory, args.repo)
    inventory_names: set[str] = set()
    for row in load_inventory(args.inventory)["rows"]:
        if row["profile"] == "foc":
            name = ascii_fold(row["tag_name"])
            inventory_names.add(name if row["node_kind"] == "element" else "@" + name)
    missing = sorted(data_names(args.game_root) - inventory_names)
    for name in missing[:40]:
        problems.append(f"the loaded FoC XML has {name!r}, which the tag inventory lacks; regenerate the inventory")
    if len(missing) > 40:
        problems.append(f"... and {len(missing) - 40} more names the inventory lacks")
    print_problems(problems)
    if not problems:
        print(f"tag_registry: ok, {len(universe)} pairs, the inventory covers the loaded XML")
    return 1 if problems else 0


def render_command(args: argparse.Namespace) -> int:
    registry = load_registry(args.statuses)
    universe = load_universe(load_inventory(args.inventory))
    problems = registry_problems(registry, universe, None)
    if problems:
        print_problems(problems)
        return 1
    write_text(args.out, render(registry, universe))
    print(f"tag_registry: wrote {args.out}")
    return 0


def format_command(args: argparse.Namespace) -> int:
    registry = load_registry(args.statuses)
    registry["rows"].sort(key=row_order)
    write_text(args.statuses, dumps_registry(registry))
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "check-data", "render", "format"):
        sub = commands.add_parser(name)
        sub.add_argument("--statuses", type=Path, default=DEFAULT_STATUSES)
        sub.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
        sub.add_argument("--repo", type=Path, default=ROOT, help="the repository root the applied code locations are under")
        if name == "render":
            sub.add_argument("--out", type=Path, default=DEFAULT_REPORT,
                             help="where the report goes; it is generated, never committed")
        if name == "check-data":
            sub.add_argument("--game-root", required=True, type=Path)
    args = parser.parse_args(list(argv) if argv is not None else None)
    handlers = {"check": check_command, "check-data": check_data_command, "render": render_command,
                "format": format_command}
    try:
        return handlers[args.command](args)
    except (RegistryError, OSError, KeyError, ValueError) as exc:
        detail = f"a row or file is missing the field {exc}" if isinstance(exc, KeyError) else str(exc)
        print(f"tag_registry: {detail}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
