#!/usr/bin/env python3
"""Reclassify tag registry rows that only ground or galactic-mode objects carry (owner 2026-10-06).

A registry row covers (object class, tag path) pairs. A space hero and a ground hero share the class HeroUnit, so a
`todo` row for an ability tag that only ground heroes author is not a space-skirmish target. For each pair of a
`todo` or `presentation-later` row this script lists the effective-XML objects that carry the tag (following
Variant_Of_Existing_Type) and classifies each object as space, ground, galactic or unknown from its own data:

  space    class is a space class, or its CategoryMask has a space token (SpaceHero, Capital, Fighter, ...), or it has a
           Space_Layer other than Land/None, or a Space_Model_Name
  galactic CategoryMask is only NonCombatHero (galactic-mode agents) and the tag is one of a galactic-only ability
           (spy, siphon, slice, sabotage, piracy, neutralize, bribe); any other tag of such an object is `unknown`,
           because these heroes attach to a flagship in space battles
  ground   Space_Layer Land, a Land_Model_Name, or a ground CategoryMask (LandHero, Infantry, Vehicle, ...)
  dual-form hero  a hero whose only space evidence is the SpaceHero CategoryMask token: Space_Layer Land (or none), a
           Land_Model_Name, no Space_Model_Name and no SpaceBehavior (Boba_Fett, Bossk, Vader, Han_Solo, ...). In a space
           skirmish the hero is never spawned as itself: the company's Company_Transport_Unit (a separate ship such as
           Slave_I) is, and the hero member is only a hidden, collision-free limbo rider on it (WHE-07, WHE-49), so an
           ability authored on the ground-model object never runs in space. Such a hero counts as ground.
  unknown  none of the above (kept: it may be a space object)

A pair whose carriers are all ground/galactic becomes `land-or-galactic` with the carrier list as evidence. Pairs with
carriers of both kinds keep their status and the row gets a note naming the space carriers. Applied and partial rows,
classes this classifier cannot judge (GameConstants, Faction, ...) and pairs without a carrier are not touched.

  python tools/inventory/tag_registry_reclassify_ground.py --game-xml <foc effective Data/XML> [--write]
"""
from __future__ import annotations

import argparse
import collections
import copy
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from common import ascii_fold
    from tag_registry import dumps_registry, normalise_path, row_order
else:
    from .common import ascii_fold
    from .tag_registry import dumps_registry, normalise_path, row_order

REPO = Path(__file__).resolve().parents[2]
STATUSES_PATH = REPO / "docs" / "tag-coverage" / "statuses.json"
CANDIDATE_STATUSES = ("todo", "presentation-later")
ALWAYS_SPACE = {"spaceunit", "squadron", "starbase", "spacebuildable", "spacestructure", "spaceprop"}
# Classes whose objects the classifier can judge from their own data.
JUDGED = ALWAYS_SPACE | {"herounit", "genericherounit", "uniqueunit", "transportunit", "container", "specialstructure",
                         "secondarystructure", "miscobject", "upgradeobject", "projectile", "particle", "marker",
                         "mobile_defense_unit"}
SPACE_TOKENS = {"spacehero", "capital", "corvette", "frigate", "fighter", "bomber", "transport", "anticapital",
                "antifrigate", "anticorvette", "antifighter", "antibomber", "antisuper"}
GROUND_TOKENS = {"landhero", "infantry", "vehicle", "air", "antivehicle", "antiinfantry", "antiair", "antistructure",
                 "wall"}
GALACTIC_ABILITIES = ("spy", "siphon", "slice", "sabotage", "piracy", "neutralize", "bribe")
SHOWN = 8


class Obj:
    def __init__(self, cls: str, name: str, tags: dict[str, str], paths: set[str]):
        self.cls, self.name, self.tags, self.paths = cls, name, tags, paths


def element_paths(element: ET.Element) -> set[str]:
    """Every tag path below the object, as the inventory writes them (`Abilities/X/@Name`, `@Name` on the object)."""
    found: set[str] = set()

    def walk(node: ET.Element, prefix: str) -> None:
        for attribute in node.attrib:
            found.add(f"{prefix}/@{attribute}" if prefix else f"@{attribute}")
        for child in node:
            if not isinstance(child.tag, str):
                continue
            path = f"{prefix}/{child.tag}" if prefix else child.tag
            found.add(path)
            walk(child, path)

    walk(element, "")
    return found


def load_objects(xml_root: Path) -> tuple[dict[str, Obj], int]:
    objects: dict[str, Obj] = {}
    failed = 0
    for path in sorted(xml_root.rglob("*.xml")):
        try:
            tree = ET.parse(path)
        except ET.ParseError:
            failed += 1
            continue
        for element in tree.getroot().iter():
            if not isinstance(element.tag, str) or not element.get("Name"):
                continue
            cls = ascii_fold(element.tag)
            if cls not in JUDGED:
                continue
            tags = {ascii_fold(child.tag): (child.text or "").strip() for child in element
                    if isinstance(child.tag, str)}
            objects[ascii_fold(element.get("Name"))] = Obj(element.tag, element.get("Name"), tags, element_paths(element))
    return objects, failed


def merged(obj: Obj, objects: dict[str, Obj]) -> tuple[dict[str, str], set[str]]:
    chain = [obj]
    seen = {ascii_fold(obj.name)}
    while True:
        base = ascii_fold(chain[-1].tags.get("variant_of_existing_type", ""))
        if not base or base in seen or base not in objects:
            break
        seen.add(base)
        chain.append(objects[base])
    tags: dict[str, str] = {}
    paths: set[str] = set()
    for link in reversed(chain):
        tags.update(link.tags)
        paths |= link.paths
    return tags, paths


def dual_form_hero(obj: Obj, tags: dict[str, str], mask: set[str], layer: str) -> bool:
    """A named/generic hero that only the SpaceHero mask token ties to space (see the module docstring)."""
    return (ascii_fold(obj.cls) in ("herounit", "genericherounit") and "land_model_name" in tags
            and "space_model_name" not in tags and "spacebehavior" not in tags
            and layer in ("land", "none", "") and not (mask & (SPACE_TOKENS - {"spacehero"})))


def kind_of(obj: Obj, tags: dict[str, str]) -> str:
    if ascii_fold(obj.cls) in ALWAYS_SPACE:
        return "space"
    mask = {token.strip().lower() for token in tags.get("categorymask", "").split("|") if token.strip()}
    layer = tags.get("space_layer", "").strip().lower()
    if dual_form_hero(obj, tags, mask, layer):
        return "ground"
    if mask & SPACE_TOKENS or (layer and layer not in ("land", "none")) or "space_model_name" in tags:
        return "space"
    if mask == {"noncombathero"}:
        return "agent"
    if layer == "land" or "land_model_name" in tags or mask & GROUND_TOKENS:
        return "ground"
    return "unknown"


def listing(names: list[str]) -> str:
    names = sorted(names, key=str.lower)
    shown = ", ".join(names[:SHOWN])
    return shown + (f" (+{len(names) - SHOWN} more)" if len(names) > SHOWN else "")


def classify(objects: dict[str, Obj]) -> dict[tuple[str, str], dict[str, list[str]]]:
    """(folded class, folded tag path) -> kind -> carrier names."""
    carriers: dict[tuple[str, str], dict[str, list[str]]] = collections.defaultdict(lambda: collections.defaultdict(list))
    for obj in objects.values():
        tags, paths = merged(obj, objects)
        kind = kind_of(obj, tags)
        for path in paths:
            if kind == "agent":
                head = path.split("/")
                galactic = head[0].lower() == "abilities" and len(head) > 1 and any(
                    hint in head[1].lower() for hint in GALACTIC_ABILITIES)
                tag_kind = "galactic" if galactic else "unknown"
            else:
                tag_kind = kind
            key = (ascii_fold(obj.cls), ascii_fold(normalise_path(obj.cls + "/" + path).partition("/")[2]))
            carriers[key][tag_kind].append(obj.name)
    return carriers


def evidence_text(kinds: dict[str, list[str]]) -> str:
    parts = []
    if kinds.get("ground"):
        parts.append(f"SCOPE-LAND: only ground objects author it in the effective FoC XML ({listing(kinds['ground'])})")
    if kinds.get("galactic"):
        parts.append(f"SCOPE-GALACTIC: only galactic-mode agents (CategoryMask NonCombatHero) author this galactic ability "
                     f"({listing(kinds['galactic'])})")
    return "; ".join(parts)


def reclassify(registry: dict[str, Any], carriers: dict[tuple[str, str], dict[str, list[str]]]
               ) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, int]]:
    out = copy.deepcopy(registry)
    changed: list[dict[str, Any]] = []
    stats = collections.Counter()
    new_rows: list[dict[str, Any]] = []
    for row in out["rows"]:
        if row["status"] not in CANDIDATE_STATUSES:
            new_rows.append(row)
            continue
        keep: list[str] = []
        moved: dict[str, list[str]] = collections.defaultdict(list)
        mixed: dict[str, list[str]] = {}
        for cls in row["classes"]:
            kinds = carriers.get((ascii_fold(cls), ascii_fold(row["tag"])))
            if ascii_fold(cls) not in JUDGED or not kinds:
                keep.append(cls)
                stats["not judged or no carrier"] += 1
                continue
            if kinds.get("space") or kinds.get("unknown"):
                keep.append(cls)
                if (kinds.get("ground") or kinds.get("galactic")) and kinds.get("space"):
                    mixed[cls] = kinds["space"]
                    stats["mixed (kept)"] += 1
                else:
                    stats["space carriers (kept)"] += 1
                continue
            moved[evidence_text(kinds)].append(cls)
            stats["reclassified pairs"] += 1
        if keep and mixed:
            note = " ".join(f"Mixed ground/space, space carriers on {cls}: {listing(names)}." for cls, names in
                            sorted(mixed.items()))
            row["note"] = (row["note"] + " " + note) if row.get("note") else note
        if not moved:
            new_rows.append(row)
            continue
        if keep:
            row["classes"] = keep
            new_rows.append(row)
        for evidence, classes in moved.items():
            fresh = {key: value for key, value in row.items() if key not in ("ticket", "note", "classes")}
            fresh.update(classes=sorted(classes, key=ascii_fold), status="land-or-galactic", evidence=evidence)
            new_rows.append(fresh)
            for cls in classes:
                changed.append({"class": cls, "tag": row["tag"], "area": row["area"], "was": row["status"],
                                "evidence": evidence})
    new_rows.sort(key=row_order)
    out["rows"] = new_rows
    return out, changed, dict(stats)


def table(registry: dict[str, Any], classes: set[str] | None = None) -> dict[tuple[str, str, str], int]:
    counts: dict[tuple[str, str, str], int] = collections.Counter()
    for row in registry["rows"]:
        for cls in row["classes"]:
            if classes is None or cls in classes:
                counts[(cls, row["status"], row["area"])] += 1
    return counts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game-xml", required=True, type=Path, help="effective FoC Data/XML directory")
    parser.add_argument("--statuses", type=Path, default=STATUSES_PATH)
    parser.add_argument("--report", type=Path, help="write the before/after report (Markdown) here")
    parser.add_argument("--write", action="store_true", help="rewrite statuses.json")
    args = parser.parse_args(argv)
    registry = json.loads(args.statuses.read_text(encoding="utf-8"))
    objects, failed = load_objects(args.game_xml)
    result, changed, stats = reclassify(registry, classify(objects))
    print(f"{len(objects)} objects ({failed} XML files failed to parse); {stats}")
    if args.report:
        before, after = table(registry), table(result)
        lines = ["# Reclassified rows", "", f"{len(changed)} (class, tag) pairs moved to land-or-galactic.", "",
                 f"Classifier stats: {stats}", "", "## Before/after by class, status, area", "",
                 "| class | status | area | before | after |", "|---|---|---|---|---|"]
        for key in sorted(set(before) | set(after)):
            if before.get(key, 0) != after.get(key, 0):
                lines.append(f"| {key[0]} | {key[1]} | {key[2]} | {before.get(key, 0)} | {after.get(key, 0)} |")
        lines += ["", "## All reclassified pairs", "", "| class | tag | was | area | evidence |", "|---|---|---|---|---|"]
        lines += [f"| {c['class']} | `{c['tag']}` | {c['was']} | {c['area']} | {c['evidence']} |" for c in changed]
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    if args.write:
        args.statuses.write_text(dumps_registry(result), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
