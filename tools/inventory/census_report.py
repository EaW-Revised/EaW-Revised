#!/usr/bin/env python3
"""Refresh the FoC space-skirmish roster and its mechanic evidence (UC-01..06)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict, deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
from tools.inventory.corpus import Corpus, CorpusError

from tools.inventory.census_catalog import (
    FACTIONS,
    value,
)

def encode(data):
    # One record per line keeps generated diffs reviewable and avoids megabytes
    # of indentation around the nested evidence and per-unit validation needs.
    lines = ["{"]
    items = list(data.items())
    for index, (key, val) in enumerate(items):
        tail = "," if index + 1 < len(items) else ""
        if isinstance(val, list) and val and isinstance(val[0], dict):
            lines.append("  " + json.dumps(key) + ": [")
            lines.extend("    " + json.dumps(row, ensure_ascii=True, sort_keys=True) +
                         ("," if n + 1 < len(val) else "") for n, row in enumerate(val))
            lines.append("  ]" + tail)
        elif key == "weapon_definitions":
            lines.append('  "weapon_definitions": {')
            for category, records in val.items():
                lines.append("    " + json.dumps(category) + ": {")
                rows = list(records.items())
                lines.extend("      " + json.dumps(name) + ": " + json.dumps(record, ensure_ascii=True, sort_keys=True) +
                             ("," if n + 1 < len(rows) else "") for n, (name, record) in enumerate(rows))
                lines.append("    }" + ("," if category == "hardpoints" else ""))
            lines.append("  }" + tail)
        else:
            lines.append("  " + json.dumps(key) + ": " + json.dumps(val, ensure_ascii=True, sort_keys=True) + tail)
    lines.append("}")
    return "\n".join(lines) + "\n"


def write_text(path, value):
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(value)


def doc_tables(data):
    mechanics = {m["id"]: m for m in data["mechanics"]}
    lines = ["", f"The resolved roster contains **{len(data['units'])} object types** and **{len(data['obtainable_roots'])} acquisition roots**. "
             "Counts include station/pad/upgrade types and ability projectiles; craft variants count separately. "
             "A missing fidelity need does not by itself prove that a unit cannot spawn or fight.", ""]
    for faction in FACTIONS:
        lines += [f"### {faction}", "", "Menu levels are the actual station lists. `pad` means a capture producer or its construction menu; "
                  "`producer` means a completed mine's upgrade menu. `linked` objects are obtained through the listed dependencies. "
                  "Authored galactic tech/base restrictions are retained in JSON and do not substitute for WPR-33 menu admission.", "",
                  "| Object / category | Station levels / route | Cost | Locomotor / weapons / abilities | Fidelity needs | Status |",
                  "|---|---|---:|---|---|---|"]
        for unit in data["units"]:
            if faction not in unit["factions"]:
                continue
            levels = sorted({a["level"] for a in unit["acquisition"] if a["route"] in ("station", "station_menu")
                             and ("producer" not in a or faction.lower() in a["producer"].lower())})
            routes = set(a["route"] for a in unit["acquisition"])
            route = ",".join(map(str, levels)) if levels else "pad" if routes & {"pad_menu", "map_capture", "constructed"} else "producer" if "producer_menu" in routes else "linked"
            if "starting_force" in routes:
                route += "; start"
            weapons = unit["weapons"]
            ability_names = sorted({a["type"] for a in unit["unit_abilities"]})
            locomotor = ", ".join(n.replace("_LOCOMOTOR", "") for n in unit["locomotors"]) or "stationary/implicit"
            hp = str(len(weapons["hardpoints"])) + " HP" if weapons["hardpoints"] else "own: " + ", ".join(weapons["object_projectiles"]) if weapons["object_projectiles"] else "no weapon"
            needs = [f"[{mechanics[k]['display_id']}](#{mechanics[k]['display_id'].lower()})" for k, s in unit["need_status_including_dependencies"].items() if s != "implemented"]
            category = ", ".join(unit["category"]) or unit["class"]
            cell = locomotor + "; " + hp + ("; " + ", ".join(ability_names) if ability_names else "")
            lines.append(f"| `{unit['id']}` / {category} | {route} | {unit['cost'] or '—'} | {cell} | {', '.join(needs) or 'covered consumers'} | {unit['status']} |")
        lines.append("")
    lines += ["### Mechanics in fix order", "", "Blocked objects include each direct user and any acquisition root or container "
              "that depends on it; each object is counted once per mechanic. Roots are a separate count because a carrier, its squadron "
              "and its craft should not be confused with three distinct purchases. Counts describe fidelity coverage, and overlapping "
              "mechanics must not be summed. All applied consumers are listed in JSON, including per-class and per-kind registry evidence.", "",
              "| ID / mechanic | Blocked objects / roots | Status / missing work | Rules / evidence |",
              "|---|---:|---|---|"]
    for m in data["mechanics"]:
        tags = sorted({e["tag"] for e in m["registry_evidence"] if e["status"] in ("todo", "partial")})
        missing = "; inputs: " + ", ".join("`" + tag + "`" for tag in tags) if tags else ""
        code = "; ".join("`" + c + "`" for c in m["code"])
        walk = sorted({g["source"] for g in m["walk_gaps"]})
        rule_ids = ", ".join(m["rules"]) or "UC-03 (registry; see JSON)"
        evidence = rule_ids + ("; " + code if code else "; " + ", ".join(walk) if walk else "; data / dispatch audit")
        ticket = f" (legacy #{m['legacy_ticket']})" if m["legacy_ticket"] else ""
        lines.append(f"| <a id=\"{m['display_id'].lower()}\"></a>{m['display_id']} {m['title']}{ticket} | {m['units_blocked']} / {len(m['affected_obtainable_roots'])} | **{m['status']}**: {m['reason']}{missing} | {evidence} |")
    return "\n".join(lines) + "\n"


def refresh_doc(path, data, check):
    original = path.read_text(encoding="utf-8")
    before, rest = original.split("<!-- census tables begin -->", 1)
    _, after = rest.split("<!-- census tables end -->", 1)
    updated = before + "<!-- census tables begin -->\n" + doc_tables(data) + "<!-- census tables end -->" + after
    if check:
        return original == updated
    write_text(path, updated)
    return True


def proposed_issues(data):
    lines = ["# Proposed census follow-ups", "", "Coordinator-owned proposals only; no issues were created. "
             "Reuse the listed tracking work where it already owns the mechanic; the unit list refines its scope. "
             "Counts overlap, and unknown mechanics need evidence rather than speculative implementation.", ""]
    for m in data["mechanics"]:
        if m["status"] not in ("partial", "missing"):
            continue
        lines += ["## " + m["title"], "", "Title: Complete " + m["title"].lower(),
                  f"Status: {m['status']}; rough size: {m['rough_size']}",
                  "Rules: " + (", ".join(m["rules"]) or "UC-03; registry_evidence in JSON"),
                  "Work: " + m["reason"],
                  "Units affected (" + str(m["units_blocked"]) + "): " + ", ".join(m["affected_units"]),
                  "Acquisition roots (" + str(len(m["affected_obtainable_roots"])) + "): " + ", ".join(m["affected_obtainable_roots"])]
        if m["legacy_ticket"]:
            lines.append(f"Existing ownership: {m['title']} (legacy #{m['legacy_ticket']}); extend this work rather than file a duplicate.")
        lines.append("")
    return "\n".join(lines)
