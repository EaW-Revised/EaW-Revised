#!/usr/bin/env python3
"""#628: the M2 scene against the tag registry (docs/tag-coverage.md).

scene   joins the loaders' load-time tag trace (sim_headless --tag-trace-out) with the effective
        FoC files it names, giving the scene set (every tag of the objects and documents the M2 start
        loads) and the consumed set (the tags a loader looked up). Fails when a scene tag has no registry
        row, or when a row that says a tag is applied is for a tag the scene never reads. The registry
        itself (tag_registry.py) is checked without game data.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import xml.parsers.expat
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from common import ascii_fold, canonical_bytes
    from corpus import Corpus, CorpusError
    from xml_inventory import XmlInventoryError, _configure_safe_parser
    import tag_registry
else:
    from .common import ascii_fold, canonical_bytes
    from .corpus import Corpus, CorpusError
    from .xml_inventory import XmlInventoryError, _configure_safe_parser
    from . import tag_registry

TRACE_KINDS = ("used", "attribute", "object", "document")


class CoverageError(RuntimeError):
    pass


@dataclass
class Node:
    """One element of a parsed file, with its attributes."""
    name: str
    line: int
    column: int
    depth: int
    parent: int | None
    text: str = ""
    attributes: list[str] = field(default_factory=list)
    children: int = 0


def parse_nodes(data: bytes, context: str) -> list[Node]:
    """Every element in document order (the inventory tooling's safe Expat set-up)."""
    parser = xml.parsers.expat.ParserCreate()
    _configure_safe_parser(parser, context)
    parser.buffer_text = True
    nodes: list[Node] = []
    stack: list[int] = []

    def start(name: str, attributes: dict[str, str]) -> None:
        parent = stack[-1] if stack else None
        if parent is not None:
            nodes[parent].children += 1
        nodes.append(Node(name, parser.CurrentLineNumber, parser.CurrentColumnNumber + 1, len(stack), parent,
                          attributes=[a for a, v in attributes.items() if v.strip()]))
        stack.append(len(nodes) - 1)

    def end(_name: str) -> None:
        stack.pop()

    def text(value: str) -> None:
        if stack:
            nodes[stack[-1]].text += value

    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.CharacterDataHandler = text
    try:
        parser.Parse(data, True)
    except xml.parsers.expat.ExpatError as exc:
        raise CoverageError(f"{context}: XML parse error: {exc}") from exc
    return nodes


def subtree(nodes: list[Node], index: int) -> range:
    end = index + 1
    while end < len(nodes) and nodes[end].depth > nodes[index].depth:
        end += 1
    return range(index, end)


def tag_path(nodes: list[Node], root: int, index: int) -> str:
    names: list[str] = []
    at: int | None = index
    while at is not None:
        names.append(nodes[at].name)
        if at == root:
            break
        at = nodes[at].parent
    return "/".join(reversed(names))


def has_value(node: Node) -> bool:
    return node.children == 0 and bool(node.text.strip())


def load_trace(paths: Sequence[Path]) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for path in paths:
        value = json.loads(path.read_text(encoding="utf-8"))
        if value.get("schema_version") != 1:
            raise CoverageError(f"{path}: not a schema_version 1 tag trace")
        for entry in value["entries"]:
            if entry["kind"] not in TRACE_KINDS:
                raise CoverageError(f"{path}: unknown entry kind {entry['kind']}")
            entries.append(entry)
    return entries


class File:
    """One effective file the trace names, with its elements indexed by (line, name)."""

    def __init__(self, logical_path: str, data: bytes, source_id: str):
        self.logical_path = logical_path
        self.source_id = source_id
        self.nodes = parse_nodes(data, logical_path)
        self.by_line: dict[tuple[int, str], list[int]] = defaultdict(list)
        for index, node in enumerate(self.nodes):
            self.by_line[(node.line, ascii_fold(node.name))].append(index)

    def locate(self, line: int, column: int, name: str) -> int | None:
        """The element a trace entry names. The loaders count columns from the name, one past the
        inventory tooling's `<`, so the nearest candidate on the line is taken."""
        candidates = self.by_line.get((line, ascii_fold(name)), [])
        if not candidates:
            return None
        return min(candidates, key=lambda index: abs(self.nodes[index].column + 1 - column))


@dataclass
class Row:
    tag: str
    occurrences: int = 0
    files: set[str] = field(default_factory=set)
    owners: set[str] = field(default_factory=set)
    consumed: bool = False


def build_sets(entries: list[dict[str, Any]], read_file) -> tuple[dict[str, Row], list[str]]:
    """The scene rows keyed by tag path, each marked consumed or not, and join problems."""
    files: dict[str, File] = {}
    problems: list[str] = []

    def file_of(logical_path: str) -> File | None:
        key = ascii_fold(logical_path)
        if key not in files:
            loaded = read_file(key)
            if loaded is None:
                problems.append(f"{logical_path}: not in the effective FoC layer")
                files[key] = None  # type: ignore[assignment]
            else:
                files[key] = File(key, *loaded)
        return files[key]

    # Scene roots: each touched object definition and each loaded document.
    roots: dict[tuple[str, int], str] = {}
    reads: set[tuple[str, int]] = set()
    attribute_reads: set[tuple[str, int, str]] = set()
    for entry in entries:
        found = file_of(entry["logical_path"])
        if found is None:
            continue
        index = found.locate(entry["line"], entry["column"], entry["element"])
        if index is None:
            problems.append(f"{entry['logical_path']}:{entry['line']}:{entry['column']}: no <{entry['element']}> "
                            f"for a {entry['kind']} entry")
            continue
        key = (found.logical_path, index)
        if entry["kind"] == "object":
            roots.setdefault(key, entry["name"])
        elif entry["kind"] == "document":
            roots.setdefault(key, found.logical_path)
        elif entry["kind"] == "used":
            reads.add(key)
        else:
            attribute_reads.add((found.logical_path, index, ascii_fold(entry["name"])))

    rows: dict[str, Row] = {}
    covered: set[tuple[str, int]] = set()
    # Objects first, so a definition inside a loaded document keys its tags from its own element.
    for (logical_path, root), owner in sorted(roots.items(), key=lambda item: (item[1] == item[0][0], item[0])):
        found = files[logical_path]
        is_object = owner != logical_path
        for index in subtree(found.nodes, root):
            node = found.nodes[index]
            path = tag_path(found.nodes, root, index)
            if (logical_path, index) in covered:
                continue
            covered.add((logical_path, index))
            values: list[tuple[str, bool]] = []
            if index != root and has_value(node):
                values.append((path, (logical_path, index) in reads))
            for attribute in node.attributes:
                # The catalog keys each object by its root's Name: touching the object reads it.
                read = ((logical_path, index, ascii_fold(attribute)) in attribute_reads
                        or (is_object and index == root and ascii_fold(attribute) == "name"))
                values.append((f"{path}/@{attribute}", read))
            for key, read in values:
                row = rows.setdefault(key, Row(key))
                row.occurrences += 1
                row.files.add(logical_path)
                row.owners.add(owner)
                row.consumed = row.consumed or read
    # Rows differing only in ASCII case are one tag, as FoC matches names.
    merged: dict[str, Row] = {}
    for key in sorted(rows, key=lambda k: (ascii_fold(k), k)):
        row = rows[key]
        target = merged.get(ascii_fold(key))
        if target is None:
            merged[ascii_fold(key)] = row
            continue
        target.occurrences += row.occurrences
        target.files |= row.files
        target.owners |= row.owners
        target.consumed = target.consumed or row.consumed
    return {row.tag: row for row in merged.values()}, problems


# --- Area guess for new rows -------------------------------------------------------------------
# A guess from the tag's words, for rows --update-table adds; the table's area is what counts.
# Each word is a token prefix: `anim` matches Idle_Anim_Rate and SFXEventAnimated.

AREA_RULES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("ai", ("ai", "autoresolve", "threat", "perception", "perceptual", "budget", "goal", "difficulty",
            "evaluator", "contrast", "freestore", "alignment", "bribed", "lua")),
    ("fighters", ("squadron", "fighter", "spawn", "bomber", "bombing", "hangar", "reserve", "landing", "launch",
                  "dogfight", "squad", "escort")),
    ("economy", ("cost", "credit", "build", "tech", "population", "income", "refund", "purchase", "price", "buy",
                 "sell", "upgrade", "research", "unlock", "market", "slice", "corrupt", "tax", "mining", "mineral",
                 "resource", "bounty", "fiscal", "political", "politic", "coin", "stack", "capture", "level",
                 "size", "cap", "overrun", "resolve", "story", "mp", "pay", "timeline", "planet", "planets",
                 "structures", "community", "ownership", "production", "queue", "required", "reward",
                 "maximum", "interdictor", "ranking", "rankings", "garrison", "salvage", "sabotage")),
    ("presentation", ("model", "sfx", "sfxevent", "icon", "text", "encyclopedia", "particle", "texture", "color",
                      "colour", "sound", "speech", "anim", "radar", "font", "camera", "gui", "hud", "tooltip",
                      "lightning", "blob", "shadow", "highlight", "select", "music", "movie", "effect", "fx",
                      "flash", "glow", "light", "decal", "trail", "visual", "display", "portrait", "cursor",
                      "scale", "bink", "cinematic", "name", "clone", "explosion", "debris", "billboard", "flyby",
                      "fog", "fow", "hint", "reticle", "bone", "attach", "sprite", "lod", "mouse", "tint", "mesh",
                      "image", "localized", "splash", "crawl", "zoom", "gmc", "message", "saliency", "telekinesis",
                      "earthquake", "shake", "water", "blend", "idle", "japanese", "demo", "preview", "url", "debug",
                      "sort", "fade", "hides", "visible", "show", "menu", "movie", "screen", "viewport", "ambient",
                      "letterbox", "hotkey", "hot", "beacon", "shield_mesh", "drawn", "draw", "render", "wobble",
                      "win", "lose", "banner", "voice", "tutorial", "load")),
    ("movement", ("speed", "turn", "locomotor", "path", "pathfind", "pathing", "formation", "accel", "decel", "move",
                  "moving", "obstacle", "collision", "collidable", "avoid", "avoidance", "bank", "hover", "layer",
                  "rotation", "rotations", "rotate", "footprint", "extent", "occupation", "waypoint", "waypoints",
                  "wait", "destination", "tracking", "strafe", "roll", "pitch", "yaw", "lift", "orbit",
                  "velocity", "hyperspace", "tractor", "chase", "guard", "stop", "facing", "drift", "mass",
                  "spacing", "repush", "lookahead", "look", "grid", "formup", "drag", "retreat", "expansion",
                  "expansions", "spread", "crush", "crusher", "crushers", "behavior", "locomotion")),
    ("combat", ("damage", "fire", "projectile", "shield", "armor", "weapon", "target", "targeting", "hardpoint",
                "hard", "health", "death", "attack", "range", "lucky", "ion", "laser", "torpedo", "missile",
                "energy", "repair", "regen", "vulnerab", "combat", "kill", "hit", "accuracy", "inaccuracy", "reveal",
                "sensor", "stealth", "cloak", "ability", "abilities", "bombard", "bombardment", "dummy",
                "allies", "enemies", "enemy", "defense", "category", "property", "team", "shot", "salvo")),
)


def tag_words(text: str) -> list[str]:
    """The words of a tag name: split at `_`, `/`, `@` and case changes, lower case."""
    words: list[str] = []
    for part in re.split(r"[^A-Za-z0-9]+", text):
        words += [w.lower() for w in re.findall(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|[0-9]+", part)]
    return words


def guess_area(tag: str) -> str:
    words = tag_words("/".join(tag.split("/")[1:]) or tag)
    for area, prefixes in AREA_RULES:
        if any(word.startswith(prefix) for word in words for prefix in prefixes):
            return area
    element = ascii_fold(tag.split("/")[0])
    return "fighters" if element == "squadron" else "combat"


# --- scene -------------------------------------------------------------------------------------

def scene_report(rows: dict[str, Row], registry: dict[str, Any], problems: list[str]) -> tuple[dict[str, Any], list[str]]:
    """The scene against the registry: rows keyed by (class, tag) after the registry's `*` collapsing."""
    statuses: dict[tuple[str, str], dict[str, Any]] = {}
    for object_class, tag, row in tag_registry.expand(registry):
        statuses[tag_registry.pair_key(object_class, tag)] = row
    scene: dict[tuple[str, str], bool] = {}
    spelled: dict[tuple[str, str], str] = {}
    for row in rows.values():
        object_class, tail = tag_registry.split_key(tag_registry.normalise_path(row.tag))
        key = tag_registry.pair_key(object_class, tail)
        scene[key] = scene.get(key, False) or row.consumed
        spelled.setdefault(key, f"{object_class}/{tail}")
    failures: list[str] = []
    by_status: dict[str, int] = defaultdict(int)
    read_but: list[str] = []
    for key, consumed in sorted(scene.items()):
        row = statuses.get(key)
        if row is None:
            if key[1]:  # the object's own element carries no value
                failures.append(f"scene tag {spelled[key]} has no registry row")
            continue
        by_status[row["status"]] += 1
        applied_in_traced = row["status"] in ("applied", "partial") and any(
            target["code"].startswith(tag_registry.TRACED_CODE) for target in row.get("applied", []))
        if applied_in_traced and not consumed and row.get("trace") != "unrecorded":
            failures.append(f"{spelled[key]} is {row['status']} in the registry, but no loader of the M2 scene reads it")
        if consumed and row["status"] in ("foc-ignores", "land-or-galactic", "multiplayer", "presentation-later"):
            read_but.append(f"{spelled[key]} ({row['status']})")
    value = {
        "consumed": sum(1 for consumed in scene.values() if consumed),
        "join_problems": problems,
        "read_but_out_of_scope": read_but,
        "scene": len(scene),
        "scene_by_status": dict(sorted(by_status.items())),
        "schema_version": 2,
    }
    return value, failures


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


def scene_command(args: argparse.Namespace) -> int:
    corpus = Corpus(args.game_root)

    def read_file(logical_path: str) -> tuple[bytes, str] | None:
        source = corpus.read_effective("foc", logical_path)
        return None if source is None else (source.data, source.source_id)

    entries = load_trace(args.trace)
    rows, problems = build_sets(entries, read_file)
    registry = tag_registry.load_registry(args.statuses)
    value, failures = scene_report(rows, registry, problems)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "tag-coverage.json").write_bytes(canonical_bytes(value))
    print(json.dumps({k: value[k] for k in ("consumed", "scene", "scene_by_status")}, sort_keys=True))
    if problems:
        print(f"tag_coverage: {len(problems)} trace entries did not join", file=sys.stderr)
        for problem in problems[:20]:
            print(f"tag_coverage: {problem}", file=sys.stderr)
    for failure in failures[:50]:
        print(f"tag_coverage: {failure}", file=sys.stderr)
    if len(failures) > 50:
        print(f"tag_coverage: ... and {len(failures) - 50} more", file=sys.stderr)
    if failures:
        print("tag_coverage: update docs/tag-coverage/statuses.json (docs/tag-coverage.md, 'Changing a row')",
              file=sys.stderr)
    return 1 if failures or problems else 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    scene = commands.add_parser("scene")
    scene.add_argument("--game-root", required=True, type=Path)
    scene.add_argument("--trace", required=True, type=Path, action="append")
    scene.add_argument("--statuses", required=True, type=Path)
    scene.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(list(argv) if argv is not None else None)
    try:
        return {"scene": scene_command}[args.command](args)
    except (CoverageError, CorpusError, XmlInventoryError, tag_registry.RegistryError, OSError, KeyError, ValueError) as exc:
        print(f"tag_coverage: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
