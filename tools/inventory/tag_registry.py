#!/usr/bin/env python3
"""The XML tag registry (docs/tag-coverage.md): every FoC XML tag has a target in our code, or a known reason it has none.

The universe is every (object class, tag path) of the FoC XML the game loads, taken from the committed tag
inventory (plan/inventories/xml-tags.json) with data-defined names collapsed to `*`. The registry
(docs/tag-coverage/statuses.json and its shards, schema_version 2) gives each pair exactly one status. This module loads and
validates it, renders its Markdown view and reports on it; tag_coverage.py joins it with the M2 load trace.

check    the gate without game data: every universe pair has a row, every row has what its status needs,
         every `applied` code location still exists, the JSON is canonical.
check-data
         the same plus the game data: every element or attribute name in the effective FoC XML is in the
         inventory (a stale inventory hides new tags from the gate).
render   writes the report (counts, partial rows, top todo) from statuses.json to out/tag-coverage/statuses.md;
         it is generated on demand and never committed.
format   rewrites statuses.json in its canonical layout.
relocate --map moves.json
         rewrites the `path#identifier` code anchors (and the region anchors of data/census/mechanic-review.json)
         after code moved, so a file split or rename keeps its evidence; see docs/tag-coverage.md, "Moving code".
"""
from __future__ import annotations

import argparse
import json
import os
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
            "deferred", "by-design")
# Statuses that say "out of scope for now": each names a ticket or a reason.
OUT_OF_SCOPE = ("land-or-galactic", "multiplayer", "presentation-later", "deferred")
# Classes whose objects fight in a tactical battle, so a name-based scope word on them (Hyperspace, Land_, Piracy) says
# what the tag is called, not what the battle does with it, and the tag stays todo until someone has read it. Shared by
# the seed's scope rule (tag_registry_seed.py) and the gate on auto-resolve-only citations.
TACTICAL_CLASSES = frozenset({"spaceunit", "uniqueunit", "transportunit", "container", "squadron", "starbase",
                              "spacebuildable", "spacestructure", "specialstructure", "secondarystructure",
                              "herounit", "genericherounit", "mobile_defense_unit"})
# Galactic auto-resolve code: src/campaign/auto_resolve_*.cpp and include/eawr/campaign/auto_resolve_*.hpp. Applying a
# value there is not the battle applying it (tag audit 2026-10-16), so an applied or partial row of a tactical class
# cites at least one other location.
AUTO_RESOLVE_CODE = re.compile(r"^(?:src|include/eawr)/campaign/auto_resolve[^/]*$")
RULE_IN_TEXT = re.compile(r"(?<![A-Za-z0-9-])[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*-\d+[a-z]?(?![A-Za-z0-9-])")
AREAS = ("movement", "combat", "fighters", "ai", "presentation", "economy", "data")
BASES = ("auto", "reviewed")
ROW_FIELDS = {"applied", "area", "basis", "classes", "evidence", "missing_types", "note", "status", "tag", "ticket",
              "trace", "engine_default", "no_targets", "scene"}
NO_TARGET_FIELDS = {"types", "status", "evidence"}
UNIT_KINDS = {"station", "ship", "squadron", "craft"}
TRACES = ("unrecorded",)
APPLIED_FIELDS = {"code", "rules", "types", "trace_scope"}
TRACE_SCOPE_FIELDS = {"ability_types", "exclude_behaviors", "exclude_space_behaviors", "object_types"}
RULE_ID = re.compile(r"^[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*-\d+[a-z]?$")
GENERIC_IDENTIFIERS = {"own", "id", "name", "list", "node", "box", "rules", "curve", "result", "config", "buffer", "number", "params"}
MIN_IDENTIFIER_LENGTH = 6
# Remaining documentation gaps tracked in #2566; lower this as notes are resolved.
TODO_NOTE_CEILING = 0
TODO_MARKER = re.compile(r"\bTODO(?: #([1-9][0-9]*))?:")

# Keep literals intact so their comment markers do not consume subsequent code.
CPP_TOKEN = re.compile(
    r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|\b[0-9][\w.\']*'
    r'|"(?:\\.|[^"\\\r\n])*"|\'(?:\\.|[^\'\\\r\n])*\''
    r'|(?P<comment>//[^\n]*|/\*.*?(?:\*/|\Z))', re.DOTALL)


def uncomment_cpp(body: str) -> str:
    """Remove C/C++ comments without joining tokens or mistaking literals for comments."""
    # Translation-phase line splicing precedes recognition of // comments.
    body = re.sub(r"\\\r?\n", "", body)
    return CPP_TOKEN.sub(lambda match: " " if match.group("comment") else match.group(), body)


def identifier_pattern(identifier: str) -> re.Pattern[str]:
    return re.compile(r"(?<!\w)" + re.escape(identifier) + r"(?!\w)")


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

def load_shard(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema_version") != SCHEMA_VERSION:
        raise RegistryError(f"{path}: not a schema_version {SCHEMA_VERSION} tag registry")
    return value


def registry_shards(paths: Path | Sequence[Path]) -> list[tuple[Path, dict[str, Any]]]:
    """Read explicit shards and relative shard lists; reject cycles and repeated files."""
    result: list[tuple[Path, dict[str, Any]]] = []
    seen: set[Path] = set()

    def visit(path: Path) -> None:
        path = path.resolve()
        if path in seen:
            raise RegistryError(f"{path}: repeated registry shard or cycle")
        seen.add(path)
        shard = load_shard(path)
        result.append((path, shard))
        children = shard.get("shards", [])
        if not isinstance(children, list) or any(not isinstance(p, str) or not p for p in children):
            raise RegistryError(f"{path}: shards must be a list of relative file names")
        for child in children:
            relative = Path(child)
            if relative.is_absolute() or ".." in relative.parts:
                raise RegistryError(f"{path}: shard paths must stay below the registry directory")
            visit(path.parent / relative)

    for path in [paths] if isinstance(paths, Path) else paths:
        visit(path)
    return result


def load_registry(paths: Path | Sequence[Path]) -> dict[str, Any]:
    """Combine shards for all registry consumers, preserving every pair and its status."""
    merged: dict[str, Any] = {"schema_version": SCHEMA_VERSION, "evidence": {}, "rows": []}
    for path, shard in registry_shards(paths):
        unknown = set(shard) - {"schema_version", "evidence", "rows", "note", "shards"}
        if unknown:
            raise RegistryError(f"{path}: unknown top-level fields {sorted(unknown)}")
        for name, text in shard.get("evidence", {}).items():
            if name in merged["evidence"] and merged["evidence"][name] != text:
                raise RegistryError(f"{path}: conflicting evidence {name!r}")
            merged["evidence"][name] = text
        merged["rows"].extend(shard["rows"])
    merged["rows"].sort(key=row_order)
    return merged


def write_registry(path: Path, registry: dict[str, Any]) -> None:
    """Write area rows to registered area shards, retaining other shards' pair ownership."""
    if not path.exists():
        write_text(path, dumps_registry(registry))
        return
    shards = registry_shards(path)
    distribute_rows(shards, registry)
    for target, shard in shards:
        text = dumps_registry(shard)
        if target.read_text(encoding="utf-8") != text:
            write_text(target, text)


def distribute_rows(shards: Sequence[tuple[Path, dict[str, Any]]], registry: dict[str, Any]) -> None:
    """Route root/new rows and changed areas; domain shards such as galactic keep their pairs."""
    root = shards[0][0]
    areas = {area: index for index, (target, _shard) in enumerate(shards)
             for area in AREAS if target == root.with_name(f"statuses-{area}.json")}
    area_layout = bool(areas) or root.resolve() == DEFAULT_STATUSES.resolve()
    evidence = registry.get("evidence", {})
    existing = {name for _target, shard in shards for name in shard.get("evidence", {})}
    for _target, shard in shards:
        for name in shard.get("evidence", {}):
            if name in evidence:
                shard["evidence"][name] = evidence[name]
    for name in evidence.keys() - existing:
        shards[0][1].setdefault("evidence", {})[name] = evidence[name]
    owners = {pair_key(c, tag): index for index, (_path, shard) in enumerate(shards)
              for c, tag, _row in expand(shard)}
    for _path, shard in shards:
        shard["rows"] = []
    for row in registry["rows"]:
        groups: dict[int, list[str]] = defaultdict(list)
        for object_class in row["classes"]:
            owner = owners.get(pair_key(object_class, row["tag"]), 0)
            if owner == 0 or owner in areas.values():
                if area_layout and row.get("area") not in areas:
                    raise RegistryError(f"area {row.get('area')!r} has no listed registry shard")
                owner = areas.get(row.get("area"), owner)
            groups[owner].append(object_class)
        for index, classes in groups.items():
            shards[index][1]["rows"].append(dict(row, classes=classes))
    for _target, shard in shards:
        shard["rows"].sort(key=row_order)


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
    """Leading rule declarations in headings, table cells, list items or bold paragraphs."""
    if root not in _RULES:
        found: set[str] = set()
        token = re.compile(r"^\s*(?:\*\*|__|`)?([A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*-\d+[a-z]?)(?=$|[\s:.(\-\u2014|*`_])")
        for path in sorted((root / "docs" / "behaviour").rglob("*.md")):
            fenced = False
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.lstrip().startswith(("```", "~~~")):
                    fenced = not fenced
                    continue
                if fenced:
                    continue
                heading = re.match(r"^ {0,3}#{1,6}\s+(.+)", line)
                cell = re.match(r"^\s*\|\s*([^|]+)\|", line)
                bullet = re.match(r"^\s*(?:[-*+]|\d+[.)])\s+(.+)", line)
                bold = re.match(r"^\s*((?:\*\*|__).+)", line)
                declaration = next((match[1] for match in (heading, cell, bullet, bold) if match), None)
                if declaration:
                    match = token.match(declaration)
                    if match:
                        found.add(match[1])
        _RULES[root] = found
    return _RULES[root]


def reviewed_campaign_scene(row: dict[str, Any]) -> bool:
    """A reviewed campaign-only application, with a sourced rule for every consumer."""
    targets = row.get("applied")
    note = row.get("note")
    return (row.get("scene") == "campaign" and row.get("status") == "applied"
            and row.get("basis") == "reviewed" and isinstance(note, str) and bool(note.strip())
            and isinstance(targets, list) and bool(targets)
            and all(isinstance(target, dict) and isinstance(target.get("rules"), list)
                    and bool(target["rules"]) and all(isinstance(rule, str) and RULE_ID.fullmatch(rule)
                                                    for rule in target["rules"])
                    for target in targets))


def registry_problems(registry: dict[str, Any], universe: dict[tuple[str, str], UniverseRow] | None,
                      root: Path | None) -> list[str]:
    """Everything the gate checks. `universe` None skips the coverage check, `root` None the code locations."""
    problems: list[str] = []
    todo_rows = [row for row in registry["rows"] if "TODO" in row.get("note", "")]
    if len(todo_rows) > TODO_NOTE_CEILING:
        problems.append(f"TODO note count {len(todo_rows)} exceeds ceiling {TODO_NOTE_CEILING}")
    for row in todo_rows:
        markers = list(TODO_MARKER.finditer(row["note"]))
        if not markers or any(not marker[1] for marker in markers):
            problems.append(f"row {row.get('tag')!r}: TODO note needs a tracking ticket (TODO #issue:)")
    seen: dict[tuple[str, str], str] = {}
    engine_defaults: set[tuple[str, str]] = set()
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
            file_text[relative] = uncomment_cpp(path.read_text(encoding="utf-8", errors="replace")) if path and path.is_file() else None
        return file_text[relative]

    moved: dict[str, list[str]] = {}

    def other_files(identifier: str, relative: str) -> str:
        if identifier not in moved:
            pattern = identifier_pattern(identifier)
            moved[identifier] = []
            for directory in ("src", "include", "apps"):
                for candidate in sorted((root / directory).rglob("*")):
                    if candidate.suffix in (".cpp", ".hpp", ".h", ".cc", ".c", ".cxx", ".hxx"):
                        name = candidate.relative_to(root).as_posix()
                        if pattern.search(text_of(name) or ""):
                            moved[identifier].append(name)
        matches = [name for name in moved[identifier] if name != relative]
        return "; also found in: " + ", ".join(matches) if matches else "; not found in other source files"

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
        if "scene" in row and not reviewed_campaign_scene(row):
            problems.append(f"{where}: scene must be campaign with an applied, explicitly reviewed row, "
                            "a scope note and rule IDs for every consumer")
        ticket = row.get("ticket")
        if ticket is not None and (not isinstance(ticket, int) or isinstance(ticket, bool) or ticket <= 0):
            problems.append(f"{where}: ticket must be an issue number")
        text = resolve_evidence(registry, row)
        if "engine_default" in row:
            # Unauthored type defaults still need a recognized parser owner and sourced consumer.
            try:
                from .engine_tags import EngineTags
            except ImportError:
                from engine_tags import EngineTags
            object_type = row["engine_default"]
            catalog_path = (root / "docs/tag-coverage/engine-tags.json") if root else Path(__file__).resolve().parents[2] / "docs/tag-coverage/engine-tags.json"
            try:
                catalog = EngineTags.load(catalog_path)
                valid = (object_type == "GameObjectType" and status == "applied"
                         and catalog.recognizes("foc", object_type, tag)
                         and any(entry["table_kind"] == "object-definition"
                                 and entry["evidence"] == row.get("evidence")
                                 for entry in catalog.tags.get(ascii_fold(tag), [])))
            except (OSError, ValueError, KeyError):
                valid = False
            if not valid:
                problems.append(f"{where}: engine default needs an applied object-definition tag and matching catalog evidence")
            else:
                engine_defaults.update(pair_key(c, tag) for c in classes)
        if row.get("evidence") and row["evidence"] not in evidence and " " not in row["evidence"]:
            problems.append(f"{where}: evidence {row['evidence']!r} is not defined in the evidence table")
        if "no_targets" in row:
            exclusions = row["no_targets"]
            if status not in ("applied", "partial"):
                problems.append(f"{where}: only an applied or partial row has no_targets")
            if not isinstance(exclusions, list) or not exclusions:
                problems.append(f"{where}: no_targets must be a nonempty array")
                exclusions = []
            excluded = set()
            applications = row.get("applied", [])
            if not isinstance(applications, list):
                applications = []
            applied_types = {kind for target in applications for kind in target.get("types", [])}
            for exclusion in exclusions:
                if not isinstance(exclusion, dict) or set(exclusion) != NO_TARGET_FIELDS:
                    problems.append(f"{where}: no_targets entry needs types, status and evidence only")
                    continue
                kinds = exclusion["types"]
                if (not isinstance(kinds, list) or not kinds
                        or any(not isinstance(kind, str) or kind not in UNIT_KINDS for kind in kinds)
                        or kinds != sorted(set(kinds), key=ascii_fold)):
                    problems.append(f"{where}: no_targets types must be sorted distinct unit kinds")
                    continue
                if (excluded.intersection(kinds) or applied_types.intersection(kinds)
                        or set(row.get("missing_types", [])).intersection(kinds) or any(
                        not target.get("types") for target in applications)):
                    problems.append(f"{where}: no_targets kinds overlap an applied target, missing kind or another exclusion")
                excluded.update(kinds)
                if exclusion["status"] != "foc-ignores":
                    problems.append(f"{where}: no_targets status must be foc-ignores")
                source = exclusion["evidence"]
                if not isinstance(source, str) or not source.strip():
                    problems.append(f"{where}: no_targets evidence must be nonempty")
                elif source not in evidence and " " not in source:
                    problems.append(f"{where}: no_targets evidence {source!r} is not defined in the evidence table")
                else:
                    resolved = evidence.get(source, source)
                    if not isinstance(resolved, str) or "debug build" not in resolved:
                        problems.append(f"{where}: no_targets evidence must cite the debug build")
        if status in ("applied", "partial"):
            targets = row.get("applied")
            if not isinstance(targets, list) or not targets:
                problems.append(f"{where}: an {status} row lists the code that applies it")
                targets = []
            for target in targets:
                extra = set(target) - APPLIED_FIELDS
                if extra:
                    problems.append(f"{where}: unknown applied fields {sorted(extra)}")
                if "trace_scope" in target:
                    scope = target["trace_scope"]
                    if not isinstance(scope, dict) or not scope or set(scope) - TRACE_SCOPE_FIELDS:
                        problems.append(f"{where}: trace_scope needs explicit XML selectors {sorted(TRACE_SCOPE_FIELDS)}")
                    else:
                        for selector, values in scope.items():
                            if (not isinstance(values, list) or not values
                                    or any(not isinstance(value, str) or not value.strip() for value in values)
                                    or values != sorted(set(values), key=ascii_fold)):
                                problems.append(f"{where}: trace_scope {selector} must be a sorted nonempty list of names")
                location = target.get("code", "")
                path, identifier = code_location(location)
                if not path or not identifier:
                    problems.append(f"{where}: code location {location!r} is not path#identifier")
                elif len(identifier) < MIN_IDENTIFIER_LENGTH or identifier.lower() in GENERIC_IDENTIFIERS:
                    problems.append(f"{where}: TODO code location {location}: generic or too-short identifier; "
                                    "cite a specific identifier or enclosing function")
                elif root is not None:
                    body = text_of(path)
                    if body is None:
                        problems.append(f"{where}: code location {location}: {path} does not exist" + other_files(identifier, path))
                    elif not identifier_pattern(identifier).search(body):
                        problems.append(f"{where}: TODO code location {location}: {identifier!r} is not in {path} "
                                        "as a whole word outside comments" + other_files(identifier, path))
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
            tactical = sorted(c for c in row.get("classes", []) if ascii_fold(c) in TACTICAL_CLASSES)
            if tactical and targets and all(
                    AUTO_RESOLVE_CODE.match(code_location(str(t.get("code", "")))[0] or "") for t in targets):
                problems.append(f"{where}: an {status} row of the tactical class {', '.join(tactical)} cites only "
                                "auto-resolve code; cite the tactical code that applies it, or make the row todo")
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
            if status == "by-design":
                resolved = evidence.get(row.get("evidence"), row.get("evidence")) or ""
                cited = RULE_IN_TEXT.findall(resolved) if isinstance(resolved, str) else []
                if not cited:
                    problems.append(f"{where}: a by-design row cites the rule ID that says our code replaces it")
                elif root is not None:
                    for rule in cited:
                        if rule not in known_rules(root):
                            problems.append(f"{where}: rule {rule} is in no docs/behaviour note")
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
        stale = sorted(set(seen) - set(universe) - engine_defaults)
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
    if "shards" in registry:
        lines.append(f'  "shards": {json.dumps(registry["shards"], ensure_ascii=False)},')
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
        "Generated from `docs/tag-coverage/statuses.json` and its shards by `python tools/inventory/tag_registry.py render`; "
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


def gate_problems(statuses: Path | Sequence[Path], inventory: Path, root: Path | None) -> tuple[list[str], dict[str, Any], dict]:
    registry = load_registry(statuses)
    universe = load_universe(load_inventory(inventory))
    problems = registry_problems(registry, universe, root)
    shards = registry_shards(statuses)
    roots = [statuses] if isinstance(statuses, Path) else statuses
    for registry_root in roots:
        tree = registry_shards(registry_root)
        listed = {path for path, _shard in tree}
        expected = {registry_root.resolve().with_name(f"statuses-{area}.json"): area for area in AREAS}
        is_area_shard = registry_root.resolve() in expected
        if not is_area_shard and (registry_root.resolve() == DEFAULT_STATUSES.resolve() or listed.intersection(expected)):
            for missing in expected.keys() - listed:
                problems.append(f"{registry_root.name}: area {expected[missing]!r} has no listed registry shard")
            for path, shard in tree:
                if path == registry_root.resolve() and shard["rows"]:
                    problems.append(f"{path.name}: root rows must be empty; route rows with write_registry or relocate")
        for path, shard in tree:
            if path in expected:
                for row in shard["rows"]:
                    if row.get("area") != expected[path]:
                        problems.append(f"{path.name}: row {row.get('tag')!r} belongs to area {row.get('area')!r}")
    for path, shard in shards:
        if shard["rows"] != sorted(shard["rows"], key=row_order):
            problems.append(f"{path.name}: rows are not sorted; run format")
        if path.read_text(encoding="utf-8") != dumps_registry(shard):
            problems.append(f"{path.name} is not in its canonical layout; run format")
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
    if registry is not None:
        for row in registry["rows"]:
            note = row.get("note", "")
            marker = TODO_MARKER.search(note)
            if marker:
                print(f"tag_registry: TODO row {row['tag']!r} {row['classes']}: " + note[marker.end():].strip())
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
    for path, shard in registry_shards(args.statuses):
        shard["rows"].sort(key=row_order)
        write_text(path, dumps_registry(shard))
    return 0


# --- Moving code ----------------------------------------------------------------------------------

DEFAULT_CENSUS_REVIEW = ROOT / "data" / "census" / "mechanic-review.json"


@dataclass(frozen=True)
class CodeMove:
    old: str
    new: str
    tag: str | None = None

    @property
    def label(self) -> str:
        return self.old if self.tag is None else f"{self.old} [tag={self.tag}]"


def load_moves(path: Path) -> list[CodeMove]:
    """A list of {old, new} moves, optionally scoped to an exact registry `tag` path.

    File moves keep each anchor's symbol. Tag-scoped moves take precedence over unscoped moves and never
    apply to census regions; within either scope, a symbol move wins over a file move.
    """
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, list):
        raise RegistryError(f"{path}: moves must be a list of {{old, new}} objects")
    moves: list[CodeMove] = []
    seen: set[tuple[str, str | None]] = set()
    for index, move in enumerate(value):
        if (not isinstance(move, dict) or set(move) not in ({"old", "new"}, {"old", "new", "tag"})
                or not all(isinstance(move[key], str) and move[key] for key in move)):
            raise RegistryError(f'{path}: move {index} must be {{"old": ..., "new": ...}} with optional "tag" '
                                'and non-empty strings')
        old, new = move["old"], move["new"]
        tag = move.get("tag")
        if ("#" in old) != ("#" in new):
            raise RegistryError(f"{path}: move {index} {old!r} -> {new!r}: both sides are path#symbol or both are a path")
        if "#" in old and not (old.partition("#")[2] and new.partition("#")[2]):
            raise RegistryError(f"{path}: move {index} {old!r} -> {new!r}: empty symbol")
        if not old.partition("#")[0] or not new.partition("#")[0]:
            raise RegistryError(f"{path}: move {index} {old!r} -> {new!r}: empty path (a move names a file)")
        if old == new:
            raise RegistryError(f"{path}: move {index} moves {old!r} onto itself")
        if (old, tag) in seen:
            raise RegistryError(f"{path}: {CodeMove(old, new, tag).label!r} is moved twice")
        seen.add((old, tag))
        moves.append(CodeMove(old, new, tag))
    return moves


class Relocator:
    """Follows each anchor through the moves to its final location, so A -> B and B -> C end at C (a cycle is an
    error); remembers which moves matched something."""

    def __init__(self, moves: Sequence[CodeMove]) -> None:
        self.moves = {(move.tag, move.old): move for move in moves}
        self.used: set[CodeMove] = set()
        # Validate each effective chain, including cycles crossing scoped and unscoped moves.
        for tag in [None, *sorted({move.tag for move in moves if move.tag is not None})]:
            for move in moves:
                self(move.old, tag)
        self.used.clear()

    def step(self, location: str, tag: str | None) -> tuple[CodeMove, str] | None:
        path, hash_mark, identifier = location.partition("#")
        for scope in (tag, None) if tag is not None else (None,):
            if move := self.moves.get((scope, location)):
                return move, move.new
            if move := self.moves.get((scope, path)):
                return move, move.new + hash_mark + identifier
        return None

    def __call__(self, location: str, tag: str | None = None) -> str | None:
        current = location
        seen = {location}
        while (step := self.step(current, tag)) is not None:
            self.used.add(step[0])
            current = step[1]
            if current in seen:
                raise RegistryError(f"the moves form a cycle through {current!r}")
            seen.add(current)
        return current if current != location else None

    def unused(self, moves: Sequence[CodeMove]) -> list[str]:
        return [move.label for move in moves if move not in self.used]


def relocate_registry(shards: Sequence[tuple[Path, dict[str, Any]]], relocate: Relocator,
                      finals: set[str] | None = None) -> tuple[list[Path], int]:
    """Rewrite the applied code anchors of every shard in place; (changed shard paths, anchors moved).
    `finals` collects the new location of every moved anchor."""
    changed: list[Path] = []
    count = 0
    for path, shard in shards:
        before = count
        for row in shard["rows"]:
            codes = [str(target.get("code", "")) for target in row.get("applied", ())]
            for target in row.get("applied", ()):
                new = relocate(str(target.get("code", "")), tag=row.get("tag"))
                if new is not None:
                    target["code"] = new
                    count += 1
                    if finals is not None:
                        finals.add(new)
            after = [str(target.get("code", "")) for target in row.get("applied", ())]
            if len(set(after)) < len(set(codes)):
                raise RegistryError(f"{path.name}: row {row.get('tag')!r}: the moves leave two applied entries on the "
                                    f"same anchor {sorted(c for c in set(after) if after.count(c) > codes.count(c))}")
        if count != before:
            changed.append(path)
    return changed, count


def missing_anchors(codes: Iterable[str], repo: Path) -> list[str]:
    """Relocated registry anchors whose final file is not in the tree or does not mention the identifier."""
    problems = []
    texts: dict[str, str | None] = {}
    for code in sorted(set(codes)):
        path, _hash, identifier = code.partition("#")
        if path not in texts:
            file = repo / path
            texts[path] = uncomment_cpp(file.read_text(encoding="utf-8", errors="replace")) if file.is_file() else None
        if texts[path] is None:
            problems.append(f"{code!r}: {path} is not a file under {repo}")
        elif identifier and not identifier_pattern(identifier).search(texts[path]):
            problems.append(f"{code!r}: {path} does not mention {identifier}")
    return problems


def with_eol(original: bytes | None, text: str) -> bytes:
    """`text` as bytes, in the line ending the file had (the writers emit LF)."""
    data = text.encode("utf-8")
    return data.replace(b"\n", b"\r\n") if original and b"\r\n" in original else data


def write_all(files: dict[Path, bytes]) -> None:
    """Write every file or none: temp siblings first, then replace each, restoring the replaced ones on failure."""
    originals = {path: path.read_bytes() for path in files}
    temps: dict[Path, Path] = {}
    replaced: list[Path] = []
    try:
        for path, data in files.items():
            temp = path.with_name(path.name + ".relocate-tmp")
            temp.write_bytes(data)
            temps[path] = temp
        for path, temp in temps.items():
            os.replace(temp, path)
            replaced.append(path)
    except BaseException:
        for path in replaced:
            path.write_bytes(originals[path])
        raise
    finally:
        for temp in temps.values():
            temp.unlink(missing_ok=True)


def relocate_command(args: argparse.Namespace) -> int:
    if __package__ in (None, ""):
        sys.path.insert(0, str(ROOT))  # census_review imports tools.inventory.*
        from census_review import relocate_regions
    else:
        from .census_review import relocate_regions
    moves = load_moves(args.map)
    relocate = Relocator(moves)
    shards = registry_shards(args.statuses)
    finals: set[str] = set()
    changed, count = relocate_registry(shards, relocate, finals)
    # only the final location of each moved registry anchor is checked; census keys are logical names (text-checked)
    problems = missing_anchors(finals, args.repo)
    merged = {"rows": [row for _path, shard in shards for row in shard["rows"]]}
    distribute_rows(shards, merged)
    outputs: dict[Path, bytes] = {}
    for path, shard in shards:
        original = path.read_bytes()
        updated = with_eol(original, dumps_registry(shard))
        if updated != original:
            outputs[path] = updated
    census_moved: list[tuple[str, str]] = []
    if args.census.is_file():
        original = args.census.read_bytes()
        review, census_moved = relocate_regions(json.loads(original.decode("utf-8")), relocate)
        if census_moved:
            outputs[args.census] = with_eol(original, json.dumps(review, indent=2, ensure_ascii=False) + "\n")
    if not args.allow_unmatched:
        problems += [f"{old!r} matches no registry or census anchor" for old in relocate.unused(moves)]
    if problems:
        for problem in problems:
            print(f"tag_registry: relocate: {problem}", file=sys.stderr)
        print("tag_registry: relocate: nothing written", file=sys.stderr)
        return 2
    for old in relocate.unused(moves):
        print(f"tag_registry: relocate: {old!r} matches no registry or census anchor", file=sys.stderr)
    if not args.dry_run:
        write_all(outputs)
    verb = "would move" if args.dry_run else "moved"
    print(f"tag_registry: relocate: {verb} {count} registry anchors in {len(changed)} shards "
          f"and {len(census_moved)} census regions")
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "check-data", "render", "format", "relocate"):
        sub = commands.add_parser(name)
        sub.add_argument("--statuses", type=Path, action="append",
                         help="registry shard or root with a relative shards list; repeat for multiple roots")
        sub.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
        sub.add_argument("--repo", type=Path, default=ROOT, help="the repository root the applied code locations are under")
        if name == "render":
            sub.add_argument("--out", type=Path, default=DEFAULT_REPORT,
                             help="where the report goes; it is generated, never committed")
        if name == "check-data":
            sub.add_argument("--game-root", required=True, type=Path)
        if name == "relocate":
            sub.add_argument("--map", required=True, type=Path,
                             help="moves.json: [{old: path#symbol, new: path#symbol, tag?: exact registry tag path}]; "
                                  "tag-scoped entries never move census regions")
            sub.add_argument("--census", type=Path, default=DEFAULT_CENSUS_REVIEW,
                             help="the census review whose region anchors move too (skipped when the file is absent)")
            sub.add_argument("--dry-run", action="store_true", help="count what would move, write nothing")
            sub.add_argument("--allow-unmatched", action="store_true",
                             help="report, instead of failing on, a move that matches no anchor (default: exit 2, nothing written)")
    args = parser.parse_args(list(argv) if argv is not None else None)
    args.statuses = args.statuses or [DEFAULT_STATUSES]
    handlers = {"check": check_command, "check-data": check_data_command, "render": render_command,
                "format": format_command, "relocate": relocate_command}
    try:
        return handlers[args.command](args)
    except (RegistryError, OSError, KeyError, ValueError) as exc:
        detail = f"a row or file is missing the field {exc}" if isinstance(exc, KeyError) else str(exc)
        print(f"tag_registry: {detail}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
