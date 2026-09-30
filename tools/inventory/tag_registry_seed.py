#!/usr/bin/env python3
"""The seeding evidence of the tag registry (docs/tag-coverage.md): what the debug build knows, the scope families, the
rule IDs the behaviour notes attach to a tag. tag_registry_build.py turns it into statuses.json.

Inputs that live outside the repository (ignored `out/`): a `tag-tables` run of `Invoke-GhidraQuery.ps1`
(present.tsv, templates.txt: which tag names the debug build holds at all). The gate (tag_registry.py) never
imports this module.
"""
from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from common import ascii_fold
    from tag_coverage import guess_area, tag_words
else:
    from .common import ascii_fold
    from .tag_coverage import guess_area, tag_words

ROOT = Path(__file__).resolve().parents[2]

EVIDENCE = {
    "DG-24": "debug build (#607): the tag is parsed only into hardpoint data; an object's own weapon scatters by "
             "Targeting_Fire_Inaccuracy (docs/behaviour/space-damage.md DG-24)",
    "DB-NOTAG": "debug build: no tag table row, string or format template carries this tag name (whole-image scan "
                "of 2026-09-30, `tag-tables` query of tools/rig/ghidra/EawrQuery.java); FoC never parses it",
    "SCOPE-LAND": "ground classes are the land game, outside M2's space skirmish (plan/phase-2/README.md)",
    "SCOPE-GALACTIC": "campaign, planet, trade-route and story data, and the galactic-only unit abilities (piracy, "
                      "sabotage, slice), are the galactic game, phase 3 (plan/phase-2/README.md)",
    "SCOPE-CINEMATIC": "cinematic objects and movies serve story-mode scenes, not the skirmish "
                       "(plan/phase-2/README.md)",
    "SCOPE-SETTINGS": "graphics-settings and hardware-profile tables and the intro crawl: presentation after M2's "
                      "look-and-feel goals (plan/phase-2/README.md)",
}

# Area tickets (#655's convention): a todo row links the tracking issue of its subsystem.
AREA_TICKET = {"movement": 649, "combat": 650, "fighters": 651, "ai": 652, "presentation": 653, "economy": 654,
               "data": 791}

# Classes out of M2's scope, by folded name or prefix: (status, area, evidence id).
SCOPE_EXACT: dict[str, tuple[str, str]] = {}
SCOPE_PREFIX: list[tuple[str, str, str, str]] = [
    # (prefix, status, area, evidence)
    ("ground", "land-or-galactic", "combat", "SCOPE-LAND"),
    ("props_", "land-or-galactic", "presentation", "SCOPE-LAND"),
    ("prop_", "land-or-galactic", "presentation", "SCOPE-LAND"),
    ("land", "land-or-galactic", "presentation", "SCOPE-LAND"),
    ("herocompan", "land-or-galactic", "combat", "SCOPE-LAND"),
    ("slavecompan", "land-or-galactic", "combat", "SCOPE-LAND"),
    ("slave_unit", "land-or-galactic", "combat", "SCOPE-LAND"),
    ("indigenous", "land-or-galactic", "combat", "SCOPE-LAND"),
    ("terrain_effectiveness", "land-or-galactic", "movement", "SCOPE-LAND"),
    ("cin_", "presentation-later", "presentation", "SCOPE-CINEMATIC"),
    ("mov_cinematic", "presentation-later", "presentation", "SCOPE-CINEMATIC"),
    ("movie", "presentation-later", "presentation", "SCOPE-CINEMATIC"),
    ("planet", "land-or-galactic", "economy", "SCOPE-GALACTIC"),
    ("campaign", "land-or-galactic", "economy", "SCOPE-GALACTIC"),
    ("galactic_markup", "land-or-galactic", "economy", "SCOPE-GALACTIC"),
    ("traderoute", "land-or-galactic", "economy", "SCOPE-GALACTIC"),
    ("trade_route", "land-or-galactic", "economy", "SCOPE-GALACTIC"),
    ("blackmarket", "land-or-galactic", "economy", "SCOPE-GALACTIC"),
    ("story", "land-or-galactic", "economy", "SCOPE-GALACTIC"),
    ("weather", "land-or-galactic", "presentation", "SCOPE-LAND"),
    ("terraindecal", "land-or-galactic", "presentation", "SCOPE-LAND"),
    ("surfaceeffects", "land-or-galactic", "presentation", "SCOPE-LAND"),
    ("dynamictrack", "land-or-galactic", "presentation", "SCOPE-LAND"),
    ("graphicdetailsettings", "presentation-later", "presentation", "SCOPE-SETTINGS"),
    ("starwars3dtextscroll", "presentation-later", "presentation", "SCOPE-SETTINGS"),
]


def scope_of(object_class: str) -> tuple[str, str, str] | None:
    folded = ascii_fold(object_class)
    for prefix, status, area, evidence in SCOPE_PREFIX:
        if folded.startswith(prefix):
            return status, area, evidence
    return None


# Words of a tag name that put a tag the loaders do not read outside M2's scope: (words, status, area, evidence).
NAME_SCOPE: list[tuple[frozenset[str], str, str, str]] = [
    (frozenset({"land", "ground", "infantry", "garrison"}), "land-or-galactic", "combat", "SCOPE-LAND"),
    (frozenset({"galactic", "planet", "planets", "campaign", "story", "corruption", "trade", "conquest", "hyperspace"}),
     "land-or-galactic", "economy", "SCOPE-GALACTIC"),
    (frozenset({"encyclopedia", "credits", "advisor", "tutorial", "movie", "intro", "finale", "hint", "hints"}),
     "presentation-later", "presentation", "SCOPE-MENU"),
]
EVIDENCE["SCOPE-MENU"] = ("menu, encyclopedia, end-roll credits, advisor and tutorial presentation comes after M2's "
                          "battle goals (plan/phase-2/README.md)")

# Words that make a `Credits` tag the end-roll (menu presentation) rather than money.
END_ROLL_WORDS = {"logo", "font", "color", "margin", "scroll", "spacing", "movie", "header", "top", "bottom", "size",
                  "display"}


def named_todo(tag: str) -> tuple[str, str, int, str] | None:
    """Tags whose names read like menu scope but that the skirmish uses: (status, area, ticket, note).

    Money (unit prices, skirmish starting credits, income factors) is #530's purchasing; the space battle intro speech
    is battle audio."""
    words = set(tag_words(tag))
    if words & {"credits", "credit"} and not words & END_ROLL_WORDS:
        return "todo", "economy", 530, "money the skirmish purchasing reads or should read (#530, #556)"
    if {"intro", "space"} <= words and "tactical" in words:
        return "todo", "presentation", AREA_TICKET["presentation"], "space battle intro speech, not menu audio"
    return None


# Galactic-only unit abilities; the per-ability tags below `Abilities/` stay with the abilities walk (#760).
GALACTIC_ABILITY_WORDS = frozenset({"piracy", "sabotage", "slice", "siphon"})


def scope_of_name(tag: str, path: str = "") -> tuple[str, str, str] | None:
    words = set(tag_words(tag))
    if words & GALACTIC_ABILITY_WORDS and not path.startswith(("Abilities/", "Unit_Abilities_Data/")):
        return "land-or-galactic", "economy", "SCOPE-GALACTIC"
    for names, status, area, evidence in NAME_SCOPE:
        if words & names:
            return status, area, evidence
    return None


# --- Inputs ---------------------------------------------------------------------------------------

class Knowledge:
    """What the debug build knows: the identifier tokens of its data, and the printf templates among them."""

    def __init__(self, directory: Path):
        self.present: dict[str, bool] = {}
        for line in (directory / "present.tsv").read_text(encoding="utf-8").splitlines()[1:]:
            name, _, flag = line.partition("\t")
            self.present[ascii_fold(name)] = flag == "1"
        self.templates: list[re.Pattern[str]] = []
        for template in (directory / "templates.txt").read_text(encoding="utf-8").splitlines():
            if len(re.sub(r"%\d*[a-z]|[^a-z]", "", template)) < 4:
                continue  # a bare `%s%s` would match every name
            pattern = "".join(".+" if m == "%s" else r"\d+" if re.fullmatch(r"%\d*[diu]", m) else re.escape(m)
                              for m in re.split(r"(%\d*[dsiu])", template) if m)
            self.templates.append(re.compile(pattern + "$"))

    def knows(self, name: str) -> bool:
        folded = ascii_fold(name)
        if self.present.get(folded, True):
            return True
        return any(t.match(folded) for t in self.templates)


def leaf_of(tag: str) -> str:
    return tag.rsplit("/", 1)[-1]


RULE_TOKEN = re.compile(r"\b([A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*-\d+[a-z]?)\b")


class Docs:
    """Rule IDs of docs/behaviour that mention a tag: those on the same line, else on the paragraph's earlier lines."""

    def __init__(self, root: Path):
        self.lines: list[tuple[str, list[str]]] = []
        self.known: set[str] = set()
        for path in sorted((root / "docs" / "behaviour").rglob("*.md")):
            self.lines.append((path.name, path.read_text(encoding="utf-8", errors="replace").splitlines()))
        for _name, lines in self.lines:
            for line in lines:
                self.known.update(RULE_TOKEN.findall(line))
        self.cache: dict[str, list[str]] = {}

    def rules(self, tag: str) -> list[str]:
        if tag in self.cache:
            return self.cache[tag]
        pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(tag) + r"(?![A-Za-z0-9_])", re.IGNORECASE)
        found: dict[str, int] = defaultdict(int)
        for _name, lines in self.lines:
            for index, line in enumerate(lines):
                if not pattern.search(line):
                    continue
                ids = RULE_TOKEN.findall(line)
                if not ids:
                    for back in range(index - 1, max(index - 7, -1), -1):
                        if not lines[back].strip():
                            break
                        ids = RULE_TOKEN.findall(lines[back])
                        if ids:
                            break
                for rule in ids:
                    found[rule] += 1
        result = [rule for rule, _ in sorted(found.items(), key=lambda item: (-item[1], item[0]))][:5]
        self.cache[tag] = result
        return result


