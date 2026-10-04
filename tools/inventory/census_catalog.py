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


FACTIONS = ("Rebel", "Empire", "Underworld")


PADS = ("Defense_Satellite_Laser_Pad", "Defense_Satellite_Missile_Pad",
        "Mineral_Extractor_Pad", "Skirmish_Merchant_Dock", "N_Orbital_Construction_Pod",
        "Skirmish_Hutt_Asteroid_Base")


REGISTRIES = ("gameobjectfiles.xml", "hardpointdatafiles.xml", "factionfiles.xml")


REGENERATE = "python tools/inventory/unit_census.py --docs docs/behaviour/unit-census.md"


RUNTIME = re.compile(
    r"behavio[u]?r|locomotor|weapon|projectile|fire_|targeting|hardpoint|damage|shield|energy|"
    r"cloak|stealth|tractor|gravity|repair|spawn|squadron|team|garrison|capture|income|"
    r"health|tactical_|build_limit|upgrade|population|victory|speed|thrust|accel|decel|"
    r"braking|rate_of|turn|bank|layer|collision|footprint|obstacle|reveal|nebula|"
    r"stun|invulner|self_destruct|transport|unique_space|named_hero", re.I)


PRESENTATION = re.compile(r"sfx|sound|particle|model|animat|icon|text_|gui_|tooltip|radar_icon", re.I)


def tokens(value):
    return [v for v in re.split(r"[,\s|]+", value.strip()) if v]


def text(node):
    return " ".join((node.text or "").split())


def fields(node, prefix=""):
    """Keep repeated leaf occurrences; do not silently replace nested timers."""
    result = defaultdict(list)
    for child in node:
        path = prefix + child.tag
        if len(child):
            for key, values in fields(child, path + "/").items():
                result[key].extend(values)
        else:
            result[path].append(text(child))
    return dict(sorted(result.items()))


class Catalog:
    """Registry winners + nearest-authored lists, matching our XML/unit loaders."""
    def __init__(self, game_root):
        self.corpus = Corpus(game_root)
        self.defs = {}
        self.nested = {}
        self.hashes = {}
        self.cache = {}
        self.duplicates = []
        order = 0
        for registry in REGISTRIES:
            root, _ = self.read("data/xml/" + registry)
            for entry in root:
                if entry.tag.lower() != "file":
                    continue
                path = text(entry).replace("\\", "/").lower()
                if not path.startswith("data/"):
                    path = "data/xml/" + path
                tree, source = self.read(path)
                for index, node in enumerate(tree):
                    self.add(node, path, source.layer_id, order, index)
                    # The runtime catalog also indexes named, nested special abilities.
                    for container in node:
                        if container.tag.lower() == "abilities":
                            for ability in container:
                                self.nested.setdefault(ability.get("Name", "").lower(), []).append(ability)
                order += 1

    def read(self, path):
        source = self.corpus.read_effective("foc", path)
        if source is None:
            raise ValueError("missing active registry/include: " + path)
        if b"<!DOCTYPE" in source.data.upper():
            raise ValueError("forbidden XML doctype: " + path)
        self.hashes[path] = source.sha256
        return ET.fromstring(source.data), source

    def add(self, node, path, layer, order, index):
        name = next((v for k, v in node.attrib.items() if k.lower() == "name"), None)
        if not name:
            return
        rank = (layer == "expansion", order, index)
        row = {"id": name, "class": node.tag, "source": path, "node": node, "rank": rank}
        key = name.lower()
        if key in self.defs:
            self.duplicates.append(name)
        if key not in self.defs or rank >= self.defs[key]["rank"]:
            self.defs[key] = row

    def resolve(self, name, visiting=()):
        key = name.lower()
        if key in visiting:
            raise ValueError("variant cycle: " + " -> ".join((*visiting, key)))
        if key in self.cache:
            return self.cache[key]
        if key not in self.defs:
            raise ValueError("unresolved object: " + name)
        definition = self.defs[key]
        authored = defaultdict(list)
        for child in definition["node"]:
            authored[child.tag.lower()].append((child, definition["class"], definition["source"]))
        values = {}
        chain = [definition["id"]]
        parent = authored.get("variant_of_existing_type")
        if parent and text(parent[-1][0]):
            base = self.resolve(text(parent[-1][0]), (*visiting, key))
            values = dict(base["values"])
            chain += base["variant_chain"]
        # Scalar consumers use the last occurrence; list consumers use all occurrences
        # in the closest authored layer (src/units/unit_support.cpp Object::list).
        values.update(authored)
        result = {**definition, "values": values, "variant_chain": chain}
        self.cache[key] = result
        return result


def value(obj, tag):
    rows = obj["values"].get(tag.lower(), [])
    return text(rows[-1][0]) if rows else None


def menu(obj):
    faction = None
    result = []
    for item in tokens(value(obj, "Tactical_Buildable_Objects_Multiplayer") or ""):
        if item.lower() in {f.lower() for f in FACTIONS}:
            faction = next(f for f in FACTIONS if f.lower() == item.lower())
        elif faction:
            result.append((faction, item))
        else:
            raise ValueError("menu type before faction: " + item)
    return result


def abilities(obj):
    unit = []
    nested = []
    # HeroCompany Unit_Abilities_Data is ground data (WHE-05); deployment is separate.
    if obj["class"].lower() != "herocompany":
        for tag in ("unit_abilities_data", "abilities"):
            rows = obj["values"].get(tag, [])
            for container, cls, source in rows[-1:]:
                for node in container:
                    row = {"handler": node.tag, "name": node.get("Name"),
                           "fields": fields(node), "registry_class": cls, "source": source}
                    if tag == "unit_abilities_data":
                        row["type"] = next(iter(row["fields"].get("Type", ["UNKNOWN"]))).upper()
                        unit.append(row)
                    else:
                        nested.append(row)
    return unit, nested


def dependencies(catalog, obj, unit_abilities, special):
    edges = []
    def add(name, relation, tag, count=None):
        if name.lower() not in catalog.defs:
            raise ValueError(obj["id"] + ": missing " + relation + " " + name)
        edges.append({"object": catalog.defs[name.lower()]["id"], "relation": relation,
                      "tag": tag, "count": count})
    if obj["class"].lower() == "herocompany":
        members = tokens(value(obj, "Company_Units") or "")
        unique = [value(catalog.resolve(n), "Unique_Space_Unit") for n in members]
        unique = [n for n in unique if n]
        for n in unique:
            add(n, "hero_deployment_candidate", "Unique_Space_Unit")
        for n in tokens(value(obj, "Company_Transport_Unit") or ""):
            add(n, "hero_deployment_candidate", "Company_Transport_Unit")
    for tag, rows in obj["values"].items():
        relation = ("launched" if re.fullmatch(r"(?:starting|reserve)_spawned_units_tech_\d+", tag)
                    else "craft" if tag == "squadron_units"
                    else "team_container" if tag == "create_team_type"
                    else "constructed" if tag == "tactical_buildable_constructed"
                    else "spawned" if tag in ("spawned_object_type", "self_destruct_projectile",
                                               "retreat_self_destruct_explosion") else None)
        if not relation:
            continue
        if relation == "launched" and "SPAWN_SQUADRON" not in tokens(value(obj, "SpaceBehavior") or ""):
            continue  # Lists without the service opt-in remain authored metadata.
        for node, _, _ in rows:
            parts = tokens(text(node))
            if not parts:
                continue
            if relation == "launched":
                add(parts[0], relation, node.tag, parts[1] if len(parts) > 1 else None)
            else:
                for part in parts:
                    if part.isdigit():
                        continue
                    add(part, relation, node.tag)
    for ability in unit_abilities + special:
        for tag, vals in ability["fields"].items():
            if tag.lower().split("/")[-1] in ("spawned_object_type", "bomb_type", "projectile_type", "object_type"):
                for val in vals:
                    if val:
                        add(val, "ability_spawn", tag)
    craft_counts = Counter((e["object"], e["tag"]) for e in edges if e["relation"] == "craft")
    edges = [e for e in edges if e["relation"] != "craft"]
    edges.extend({"object": name, "relation": "craft", "tag": tag, "count": str(count)}
                 for (name, tag), count in craft_counts.items())
    return sorted(edges, key=lambda e: (e["relation"], e["object"], e["tag"], e["count"] or ""))


def selected_fields(obj, all_fields=False):
    result = []
    for key, rows in sorted(obj["values"].items()):
        if key in ("abilities", "unit_abilities_data"):
            continue
        if not all_fields and (not RUNTIME.search(key) or PRESENTATION.search(key)):
            continue
        for node, cls, source in rows:
            if len(node):
                continue
            result.append({"tag": node.tag, "value": text(node), "registry_class": cls, "source": source})
    return result


def weapon(catalog, obj):
    own = value(obj, "Projectile_Types") or value(obj, "Fire_Projectile_Type")
    hardpoints = []
    projectiles = set(tokens(own or ""))
    for ability in abilities(obj)[0]:
        for authored in ability["fields"].get("Projectile_Types_Override", []):
            projectiles.update(tokens(authored))
    for node, _, _ in obj["values"].get("hardpoints", []):
        for name in tokens(text(node)):
            hp = catalog.resolve(name)
            projectile = value(hp, "Fire_Projectile_Type")
            # EWW-03, WHE-35/WHE-36: charged shots replace the ordinary projectile.
            # Keep the authored reference alongside the ordinary profile, including
            # inherited overrides; both profiles contribute mechanic requirements.
            for tag in ("Fire_Projectile_Type", "Blast_Ability_Fire_Projectile_Type"):
                projectiles.update(tokens(value(hp, tag) or ""))
            hardpoints.append({"id": hp["id"], "type": value(hp, "Type"),
                               "projectile": projectile, "fields": selected_fields(hp, True)})
    projectile_rows = []
    queue = deque(sorted(projectiles))
    seen = set()
    while queue:
        name = queue.popleft()
        if name.lower() in seen:
            continue
        seen.add(name.lower())
        p = catalog.resolve(name)
        projectile_rows.append({"id": p["id"], "class": p["class"], "fields": selected_fields(p)})
        for key, rows in p["values"].items():
            if any(s in key for s in ("projectile", "spawned_object")):
                for node, _, _ in rows:
                    for token in tokens(text(node)):
                        if token.lower() in catalog.defs and token.lower() not in seen:
                            queue.append(token)
    return {"object_projectiles": sorted(projectiles), "hardpoints": hardpoints,
            "projectiles": sorted(projectile_rows, key=lambda p: p["id"])}
