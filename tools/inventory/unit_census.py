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
sys.path.insert(0, str(ROOT))
from tools.inventory.corpus import Corpus, CorpusError


from tools.inventory.census_catalog import (
    FACTIONS,
    PADS,
    REGISTRIES,
    REGENERATE,
    RUNTIME,
    PRESENTATION,
    tokens,
    text,
    fields,
    Catalog,
    value,
    menu,
    abilities,
    dependencies,
    selected_fields,
    weapon,
)
from tools.inventory.census_review import (
    rule_index,
    region_digest,
    changed_fingerprints,
)
from tools.inventory.census_mechanics import (
    tag_family,
)
from tools.inventory.census_report import (
    encode,
    write_text,
    doc_tables,
    refresh_doc,
    proposed_issues,
)

tokens.__module__ = __name__
text.__module__ = __name__
fields.__module__ = __name__
Catalog.__module__ = __name__
value.__module__ = __name__
menu.__module__ = __name__
abilities.__module__ = __name__
dependencies.__module__ = __name__
selected_fields.__module__ = __name__
weapon.__module__ = __name__
rule_index.__module__ = __name__
tag_family.__module__ = __name__
region_digest.__module__ = __name__
changed_fingerprints.__module__ = __name__
encode.__module__ = __name__
write_text.__module__ = __name__
doc_tables.__module__ = __name__
refresh_doc.__module__ = __name__
proposed_issues.__module__ = __name__


def census(game_root):
    review = json.loads((ROOT / "data/census/mechanic-review.json").read_text(encoding="utf-8"))
    review_changes = changed_fingerprints(review)
    if review_changes:
        raise ValueError("Reviewed fingerprints moved: " + ", ".join(review_changes) +
                         ". Re-review these consumers and update data/census/mechanic-review.json; "
                         "then regenerate with EAWR_EAW_GAME_ROOT set: " + REGENERATE)
    catalog = Catalog(game_root)
    registry = json.loads((ROOT / "docs/tag-coverage/statuses.json").read_text(encoding="utf-8"))
    pairs = {(cls.lower(), row["tag"].lower()): row for row in registry["rows"] for cls in row["classes"]}
    rules, titles, gaps, tag_rules = rule_index()
    roster = {}
    roots = []
    stations = []
    queue = deque()
    def obtain(name, faction, route):
        obj = catalog.resolve(name)
        key = obj["id"]
        if key not in roster:
            roster[key] = {"id": key, "class": obj["class"], "source": obj["source"],
                           "variant_chain": obj["variant_chain"], "factions": [], "acquisition": []}
            queue.append(key)
        row = roster[key]
        if faction not in row["factions"]:
            row["factions"].append(faction)
        if route not in row["acquisition"]:
            row["acquisition"].append(route)
    for faction in FACTIONS:
        for level in range(1, 6):
            station = catalog.resolve(f"Skirmish_{faction}_Star_Base_{level}")
            entries = menu(station)
            stations.append({"id": station["id"], "faction": faction, "level": level,
                             "menu": [n for f, n in entries if f == faction]})
            obtain(station["id"], faction, {"route": "station", "level": level})
            roots.append(station["id"])
            for f, name in entries:
                obtain(name, f, {"route": "station_menu", "producer": station["id"], "level": level})
                roots.append(catalog.resolve(name)["id"])
        faction_data = catalog.resolve(faction)
        for name in tokens(value(faction_data, "Space_Skirmish_AI_Default_Forces") or ""):
            obtain(name, faction, {"route": "starting_force", "tag": "Space_Skirmish_AI_Default_Forces"})
            roots.append(catalog.resolve(name)["id"])
    for name in PADS:
        pad = catalog.resolve(name)
        for faction in FACTIONS:
            obtain(name, faction, {"route": "map_capture"})
        roots.append(pad["id"])
        for faction, name in menu(pad):
            obtain(name, faction, {"route": "pad_menu", "producer": pad["id"]})
            roots.append(catalog.resolve(name)["id"])
    while queue:
        name = queue.popleft()
        obj = catalog.resolve(name)
        row = roster[name]
        unit, special = abilities(obj)
        edges = dependencies(catalog, obj, unit, special)
        row.update({"category": tokens(value(obj, "CategoryMask") or ""),
                    "authored_affiliation": tokens(value(obj, "Affiliation") or ""),
                    "catalog_namespace_collision": name.lower() in catalog.nested,
                    "authored_tech_level": value(obj, "Tech_Level"),
                    "authored_required_station_level": value(obj, "Required_Star_Base_Level"),
                    "cost": value(obj, "Tactical_Build_Cost_Multiplayer"),
                    "build_seconds": value(obj, "Tactical_Build_Time_Seconds"),
                    "production_queue": value(obj, "Tactical_Production_Queue"),
                    "population": value(obj, "Population_Value"),
                    "behaviours": sorted(set(tokens(value(obj, "Behavior") or "") + tokens(value(obj, "SpaceBehavior") or ""))),
                    "locomotors": sorted({b for b in tokens(value(obj, "SpaceBehavior") or "") + tokens(value(obj, "Behavior") or "") if "LOCOMOTOR" in b}),
                    "weapons": weapon(catalog, obj), "unit_abilities": unit,
                    "special_abilities": special, "dependencies": edges, "special_tags": selected_fields(obj)})
        if obj["id"] not in {s["id"] for s in stations} and obj["id"] not in PADS:
            for faction, produced in menu(obj):
                obtain(produced, faction, {"route": "producer_menu", "producer": obj["id"]})
                roots.append(catalog.resolve(produced)["id"])
        row["menu"] = [{"faction": f, "object": n} for f, n in menu(obj)]
        for edge in edges:
            for faction in list(row["factions"]):
                obtain(edge["object"], faction, {"route": edge["relation"], "parent": name,
                                                "tag": edge["tag"], "count": edge["count"]})
    # Propagate faction reachability even when a shared dependency was first expanded
    # before a second faction's merchant menu reached it.
    changed = True
    while changed:
        changed = False
        for row in list(roster.values()):
            for edge in row["dependencies"]:
                for faction in list(row["factions"]):
                    target = roster[edge["object"]]
                    if faction not in target["factions"]:
                        obtain(edge["object"], faction, {"route": edge["relation"], "parent": row["id"], "tag": edge["tag"], "count": edge["count"]})
                        changed = True
    mechanics = {}
    def mechanic(key, title, status, reason, evidence=(), rule_ids=(), ticket=None, size="M"):
        if key not in mechanics:
            mechanics[key] = {"id": key, "title": title, "status": status, "reason": reason,
                              "code": sorted(set(evidence)), "rules": sorted(set(rule_ids)),
                              "legacy_ticket": ticket, "rough_size": size,
                              "direct_units": [], "affected_units": [], "registry_evidence": [],
                              "walk_gaps": gaps.get(ticket, [])}
        elif key.startswith("special-handler:"):
            current = mechanics[key]
            current["code"] = sorted(set(current["code"]) | set(evidence))
            current["rules"] = sorted(set(current["rules"]) | set(rule_ids))
            if reason not in current["reason"]:
                current["reason"] += " " + reason
        return key
    priority = {"implemented": 0, "unknown": 1, "partial": 2, "missing": 3}
    def need(row, key, status=None):
        row.setdefault("needs", [])
        row.setdefault("need_status", {})
        status = status or mechanics[key]["status"]
        old = row["need_status"].get(key, "implemented")
        row["need_status"][key] = max((old, status), key=priority.get)
        if key not in row["needs"]:
            row["needs"].append(key)
            mechanics[key]["direct_units"].append(row["id"])
    for row in roster.values():
        observations = list(row["special_tags"])
        for hp in row["weapons"]["hardpoints"]:
            observations.extend(hp["fields"])
        for p in row["weapons"]["projectiles"]:
            observations.extend(p["fields"])
        for ability in row["unit_abilities"] + row["special_abilities"]:
            base = "Unit_Abilities_Data/Unit_Ability/" if ability in row["unit_abilities"] else "Abilities/" + ability["handler"] + "/"
            observations.extend({"tag": base + tag, "registry_class": ability["registry_class"], "value": v}
                                for tag, vals in ability["fields"].items() if not PRESENTATION.search(tag) for v in vals)
        for obs in observations:
            if not obs["value"]:
                continue
            pair = (obs["registry_class"].lower(), obs["tag"].lower())
            entry = pairs.get(pair)
            if entry is None:
                key = mechanic("unregistered:" + "/".join(pair), "Unclassified " + obs["tag"], "unknown",
                                "No exact class/tag registry row; data presence does not prove a consumer.")
                need(row, key)
                continue
            status = entry["status"]
            if status in ("foc-ignores", "land-or-galactic", "presentation-later", "multiplayer", "deferred"):
                row.setdefault("excluded_tags", []).append({"class": obs["registry_class"], "tag": obs["tag"],
                    "registry_status": status, "reason": entry.get("note", entry.get("evidence", "registry scope decision"))})
                continue
            ticket = entry.get("ticket")
            area = entry.get("area", "unclassified")
            family = tag_family(obs["tag"])
            key = "registry:" + area + ":" + (str(ticket) if ticket else "applied") + ":" + family.replace(" ", "-")
            code = [e["code"] for e in entry.get("applied", [])]
            ids = [r for e in entry.get("applied", []) for r in e.get("rules", [])]
            ids += sorted(tag_rules.get(obs["tag"].lower().split("/")[-1], set()))
            mapped = "implemented" if status == "applied" and code else "partial" if status == "partial" else "missing"
            if status == "todo" and not ids:
                mapped = "unknown"
            kind = ("squadron" if row["class"] == "Squadron" else "craft" if "FIGHTER_LOCOMOTOR" in row["behaviours"]
                    else "station" if "DUMMY_STAR_BASE" in row["behaviours"] else "ship")
            if status == "partial" and entry.get("missing_types") and kind not in entry["missing_types"]:
                mapped = "implemented"
            title = family.capitalize() + " — " + titles.get(ticket, area + " application")
            reason = "Registry application evidence, scoped to listed class/tag pairs; missing/partial fields are listed in registry_evidence. This is not a per-unit runtime verdict."
            mechanic(key, title, mapped, reason, code, ids, ticket)
            m = mechanics[key]
            m["code"] = sorted(set(m["code"] + code))
            m["rules"] = sorted(set(m["rules"] + ids))
            if mapped == "missing" and m["status"] == "implemented" or mapped == "implemented" and m["status"] == "missing":
                m["status"] = "partial"
            elif mapped == "partial":
                m["status"] = "partial"
            evidence = {"class": obs["registry_class"], "tag": obs["tag"], "status": status,
                        "census_status": mapped,
                        "basis": entry.get("basis"), "missing_types": entry.get("missing_types", []),
                        "note": entry.get("note"), "applied": entry.get("applied", [])}
            if evidence not in m["registry_evidence"]:
                m["registry_evidence"].append(evidence)
            need(row, key, mapped)
        handler_needs(row, mechanic, need)
    # A purchase needs the mechanics of its deployed objects, containers and spawns.
    for row in roster.values():
        closure = dict(row.get("need_status", {}))
        visited = set()
        todo = [row["id"]]
        while todo:
            name = todo.pop()
            if name in visited:
                continue
            visited.add(name)
            child = roster[name]
            for key, status in child.get("need_status", {}).items():
                closure[key] = max((closure.get(key, "implemented"), status), key=priority.get)
            todo.extend(e["object"] for e in child["dependencies"])
        row["needs_including_dependencies"] = sorted(closure)
        row["need_status_including_dependencies"] = dict(sorted(closure.items()))
        for key, status in closure.items():
            if status in ("missing", "partial"):
                mechanics[key]["affected_units"].append(row["id"])
        row["status"] = max(closure.values(), key=priority.get, default="unknown")
        row["needs"] = sorted(row.get("needs", []))
        row["factions"].sort()
        row["acquisition"].sort(key=lambda a: json.dumps(a, sort_keys=True))
        row["excluded_tags"] = sorted({json.dumps(t, sort_keys=True) for t in row.get("excluded_tags", [])})
        row["excluded_tags"] = [json.loads(t) for t in row["excluded_tags"]]
    for m in mechanics.values():
        m["direct_units"] = sorted(set(m["direct_units"]))
        m["status"] = max((roster[n]["need_status"][m["id"]] for n in m["direct_units"]),
                          key=priority.get, default="unknown")
        if any(e.get("census_status") == "unknown" for e in m["registry_evidence"]):
            m["reason"] += " Some todo inputs have no space-consumption rule in the walks: their relevance stays unknown, and they do not count as confirmed blockers."
        m["affected_units"] = sorted(set(m["affected_units"]))
        m["affected_obtainable_roots"] = sorted(set(m["affected_units"]) & set(roots))
        m["units_blocked"] = len(m["affected_units"]) if m["status"] in ("missing", "partial") else 0
        m["registry_evidence"].sort(key=lambda e: (e["class"], e["tag"]))
    for index, key in enumerate(sorted(mechanics), 1):
        mechanics[key]["display_id"] = f"M{index:03}"
    ordered = sorted(mechanics.values(), key=lambda m: (-m["units_blocked"], m["title"], m["id"]))
    weapon_defs = {"hardpoints": {}, "projectiles": {}}
    for row in roster.values():
        for category in weapon_defs:
            records = row["weapons"][category]
            for record in records:
                weapon_defs[category][record["id"]] = record
            row["weapons"][category] = [r["id"] for r in records]
    return {"schema_version": 1, "profile": "foc", "scope_rules": ["UC-01", "UC-02", "UC-03", "UC-04", "UC-05", "UC-06"],
            "mechanic_review": {"changed_code": review_changes, "source": "data/census/mechanic-review.json"},
            "catalog_namespace_collisions": sorted(catalog.defs[k]["id"] for k in catalog.nested if k in catalog.defs),
            "inputs": [{"logical_path": p, "sha256": h} for p, h in sorted(catalog.hashes.items())],
            "stations": stations, "obtainable_roots": sorted(set(roots)),
            "weapon_definitions": {k: dict(sorted(v.items())) for k, v in weapon_defs.items()},
            "units": sorted(roster.values(), key=lambda r: r["id"].lower()), "mechanics": ordered}


def handler_needs(row, mechanic, need):
    def add(key, title, status, reason, code=(), rules=(), ticket=None, size="M"):
        need(row, mechanic(key, title, status, reason, code, rules, ticket, size), status)
    generic = {"DEFEND", "TURBO", "POWER_TO_WEAPONS", "SPOILER_LOCK", "ION_CANNON_SHOT", "INVULNERABILITY", "BARRAGE"}
    special_rules = {
        "Combat_Bonus_Ability": (["WPR-51", "WHE-11", "WHE-20"], 937),
        "Starbase_Upgrade_Ability": (["WPR-52"], 540),
        "Income_Stream_Ability": (["WPR-11", "WBP-22", "WBP-26"], 541),
        "Income_Stream_Mod_Ability": (["WBP-24", "WBP-25"], 996),
        "Concentrate_Fire_Attack_Ability": (["WHE-24", "WHE-25"], 939),
        "Energy_Weapon_Attack_Ability": (["WHE-26"], 940),
        "Tractor_Beam_Attack_Ability": (["WHE-27"], 940),
        "Sensor_Jamming_Ability": (["WHE-31", "WHE-32"], 943),
        "Corrupt_Systems_Ability": (["WHE-33", "WHE-34"], 945),
        "Blast_Ability": (["WHE-35", "WHE-36"], 946),
        "Force_Healing_Ability": (["WBP-21"], 760),
    }
    type_rules = {
        "HUNT": (["WAB-50", "WAB-56"], None),
        "INVULNERABILITY": (["WHE-22"], 938),
        "CONCENTRATE_FIRE": (["WHE-24", "WHE-25"], 939),
        "ENERGY_WEAPON": (["WHE-26"], 940),
        "TRACTOR_BEAM": (["WHE-27"], 940),
        "WEAKEN_ENEMY": (["WHE-28"], 941),
        "HARMONIC_BOMB": (["WHE-29"], 941),
        "REPLENISH_WINGMEN": (["WHE-30"], 942),
        "SENSOR_JAMMING": (["WHE-31", "WHE-32"], 943),
        "CORRUPT_SYSTEMS": (["WHE-33", "WHE-34"], 945),
        "BLAST": (["WHE-35", "WHE-36"], 946),
        "STEALTH": (["WHE-37"], 946),
    }
    for ability in row["unit_abilities"]:
        kind = ability["type"]
        ids, ticket = type_rules.get(kind, (["UC-03"], 760))
        if kind in generic:
            if kind == "BARRAGE":
                add("unit-ability:" + kind, kind + " ability", "partial",
                    "Point activation, enemy target proxy and ordinary projectile/rate/accuracy overrides exist. Roster gates, targeting presentation and U-07 retail captures remain separate acceptance work.",
                    ("src/units/unit_abilities.cpp#ability_table", "src/sim/tactical/session_step_commands.cpp#apply_area_ability",
                     "src/sim/tactical/combat_fire.cpp#attempt"), ("WAD-38",), 1074)
            else:
                add("unit-ability:" + kind, kind + " ability", "implemented",
                    "The supported kind dispatches through the ability table and simulation service; other modifiers and handler-specific paths are separate needs.",
                    ("src/units/unit_abilities.cpp#ability_table", "src/sim/tactical/abilities.cpp#kind_names"), ("AB-04", "WAB-01"))
            damage_modes = [v for v in ability["fields"].get("Mod_Multiplier", [])
                            if tokens(v) and tokens(v)[0] in {"CAUSE_DAMAGE_MULTIPLIER", "TAKE_DAMAGE_MULTIPLIER", "SCATTER_RADIUS_MULTIPLIER", "FIRE_RATE_MULTIPLIER"}]
            unsupported = [v for v in ability["fields"].get("Mod_Multiplier", [])
                           if tokens(v) and tokens(v)[0] not in {"WEAPON_DELAY_MULTIPLIER", "SHIELD_REGEN_MULTIPLIER",
                               "SHIELD_REGEN_INTERVAL_MULTIPLIER", "ENERGY_REGEN_MULTIPLIER", "ENERGY_REGEN_INTERVAL_MULTIPLIER", "SPEED_MULTIPLIER",
                               "CAUSE_DAMAGE_MULTIPLIER", "TAKE_DAMAGE_MULTIPLIER", "SCATTER_RADIUS_MULTIPLIER", "FIRE_RATE_MULTIPLIER"}]
            if damage_modes and not unsupported:
                add("ability-damage-modifiers", "Cause/take damage ability modifiers", "implemented",
                    "Current active modes feed ordinary damage before diminishing firepower, defense and armor; the Falcon zero mode is independent of arrival protection.",
                    ("src/units/unit_abilities.cpp#ability_table", "src/sim/tactical/damage.cpp#apply_hit",
                     "src/sim/tactical/session_abilities.cpp#refresh_damage_modes"), ("WHE-22", "WHE-51", "WHE-52"))
            if unsupported:
                add("ability-other-modifiers", "Unsupported ability modifiers", "partial",
                    "The loader omits the whole ability when any authored modifier has no consumer rather than exposing a partially supported button.",
                    ("src/units/unit_abilities.cpp#ability_table",), ("WHE-22",), 938)
            add("ability-plan-events", "Ability ready/finished plan events", "partial",
                "Countdown services exist; the ability walk identifies missing AI plan notifications.",
                ("src/sim/tactical/abilities.cpp#expire_abilities", "src/script/foc/tactical_ai_bindings.cpp#Unit_Ability_Ready"), ("WAB-34",), None, "S")
        else:
            add("unit-ability:" + kind, kind + " ability", "missing",
                "Not in the supported simulation dispatch; the loader skips unsupported kinds. UC-03 records data-only handlers whose complete original service remains unverified.",
                ("src/sim/tactical/abilities.cpp#kind_names", "src/units/unit_abilities.cpp#ability_table"), ids, ticket)
    for ability in row["special_abilities"]:
        styles = ability["fields"].get("Activation_Style", [])
        if styles and all(s.lower().startswith(("galactic", "land", "ground")) for s in styles):
            ability["scope"] = "land-or-galactic"
            continue
        handler = ability["handler"]
        ids, ticket = special_rules.get(handler, (["UC-03"], 760))
        implemented = handler == "Ion_Cannon_Shot_Attack_Ability"
        partial = handler == "Income_Stream_Ability"
        reason = ("Team ion-shot state and projectile override are applied." if implemented else
                  "Station additive income exists; completed mine live ownership and allied recipient lifecycle are absent." if partial else
                  "The special handler has no complete tactical service for this object. Handler parameters alone do not implement it; UC-03 marks data-only service semantics unverified.")
        code = ["src/units/unit_tables_profiles.cpp#load_abilities", "src/units/unit_abilities.cpp#ability_table"]
        upgrade = row["class"].lower() == "upgradeobject"
        if upgrade and handler == "Starbase_Upgrade_Ability" and styles == ["Skirmish_Automatic"]:
            implemented = True
            reason = "Completed station upgrades replace the logical station, carry held objects, queues and references, raise allied tech and open the next authored menu."
            code += ["src/skirmish/economy.cpp#economy_rules", "src/sim/tactical/session_step_economy.cpp#income_and_launch"]
        elif upgrade and handler == "Income_Stream_Mod_Ability" and (not styles or styles == ["Space_Automatic"]):
            targets = {v.strip().lower() for v in ability["fields"].get("Target_Stream_Source", [])}
            # WBP-25: the six validated stock levels target these loaded space streams.
            # Underworld's second authored list is a ground source, not a missing space effect.
            space_sources = {"empire_mineral_extractor", "rebel_mineral_extractor", "underworld_mineral_extractor"}
            if targets == {"skirmish_ground_mining_facility_u"}:
                ability["scope"] = "land-or-galactic"
                continue
            if len(targets) == 1 and targets <= space_sources:
                supported = {"Activation_Style", "Target_Stream_Source", "Affects_All_Allied_Sources",
                             "Stacking_Category", "Income_Multiplier", "Income_Additive_Value",
                             "Interval_Multiplier", "Reverse_Application_Logic"}
                unsupported = sorted(set(ability["fields"]) - supported)
                implemented, partial = not unsupported, bool(unsupported)
                ids = ["WBP-24", "WBP-25", "WBP-44", "WBP-45", "WBP-46"]
                reason = ("Held mine upgrades discover matching live space streams and apply independent signed category "
                          "winners with immediate/randomized-periodic discovery, no expiry and source-specific withdrawal. "
                          "Mine-host deletion remains U-BP-6; ground and unloaded source targets are outside this consumer.")
                if unsupported:
                    reason += " Authored effects remain unsupported: " + ", ".join(unsupported) + "."
                code += ["src/skirmish/economy.cpp#economy_rules", "src/sim/tactical/session_economy.cpp#service_income_modifiers",
                         "src/sim/tactical/economy.cpp#modified_income_per_frame"]
        elif upgrade and handler == "Combat_Bonus_Ability" and styles == ["Space_Automatic"]:
            supported = {"Activation_Style", "Applicable_Unit_Types", "Applicable_Unit_Categories", "Stacking_Category",
                         "Health_Bonus_Percentage", "Damage_Bonus_Percentage", "Energy_Pool_Bonus_Percentage",
                         "Shield_Bonus_Percentage", "Defense_Bonus_Percentage", "Movement_Speed_Bonus_Percentage"}
            unsupported = sorted(set(ability["fields"]) - supported)
            implemented, partial = not unsupported, bool(unsupported)
            reason = "Held upgrade profiles apply allied health, damage, energy, shield, defense and movement bonuses with stacking categories."
            if unsupported:
                reason += " Authored effects remain unsupported: " + ", ".join(unsupported) + ". Purchase admission does not prove these effects."
            code += ["src/skirmish/economy.cpp#economy_rules", "src/sim/tactical/session_abilities.cpp#ability_helpers",
                     "src/sim/tactical/session_step_economy.cpp#upgrade_bonus_phase"]
        add("special-handler:" + handler, handler.replace("_", " "),
            "implemented" if implemented else "partial" if partial else "missing", reason, tuple(code), ids, ticket)
    behaviours = set(row["behaviours"])
    if row["class"].lower() == "herocompany" or "SpaceHero" in row["category"] or any("hero_deployment_candidate" == a["route"] for a in row["acquisition"]):
        add("hero-lifecycle", "Hero purchase, deployment and carried identity", "missing",
            "Company relationships and visible identity require a dedicated lifecycle; WHE U-02 leaves company-to-space deployment policy unverified. Candidate links are a conservative census closure.",
            ("src/skirmish/start.cpp#build_start",), ("WHE-01", "WHE-02", "WHE-05", "WHE-07"), 934, "L")
    if row.get("catalog_namespace_collision"):
        add("catalog-ability-name-collision", "Separate object and ability name lookup", "implemented",
            "Production, menus and presentation resolve the game-object namespace; nested ability occurrences remain scoped to their owning object's closest authored layer. Legacy global lookup is retained for inventory tooling.",
            ("src/data/xml_registry.cpp#index_winners", "src/data/xml.cpp#find_game_object", "src/data/xml_merge.cpp#resolve", "src/units/unit_support.cpp#resolve", "src/units/unit_tables_profiles.cpp#load_abilities"), ("UC-02", "WPR-22"), None, "S")
    levels = [a["level"] for a in row["acquisition"] if a["route"] in ("station_menu", "station")]
    if levels and min(levels) > 1:
        add("station-unlocks", "Station level-up and menu unlocks", "implemented",
            "WPR-52 station replacement and allied tech progression open these authored menus; their deployed units' combat and lifecycle needs remain separate.",
            ("src/skirmish/economy.cpp#economy_rules", "src/sim/tactical/session_step_economy.cpp#income_and_launch"), ("WPR-50", "WPR-52"), 540, "L")
    if behaviours & {"CAPTURE_POINT", "TACTICAL_BUILD_OBJECTS", "TACTICAL_UNDER_CONSTRUCTION"} or any(a["route"] in ("pad_menu", "constructed") for a in row["acquisition"]):
        add("pad-lifecycle", "Captured pad construction and completed combat objects", "missing",
            "Damageable construction, replacement/parent links and capture ownership are absent; satellites require admission as ordinary combat units.",
            ("src/skirmish/start.cpp#build_start",), ("WBP-01", "WBP-09", "WBP-18", "WBP-21"), 541, "L")
    if "TACTICAL_SELL" in behaviours:
        add("satellite-sale", "Satellite sale and empty-pad preservation", "missing",
            "The exact-owner sell/refund/parent-link path is a build-pad walk gap.", (), ("WBP-30", "WBP-32"), 998)
    if "SPAWN_SQUADRON" in behaviours:
        higher_tech = any(re.fullmatch(r"(?:Starting|Reserve)_Spawned_Units_Tech_[1-9]", t["tag"]) for t in row["special_tags"])
        add("carrier-service", "Carrier launch and reserve replacement", "partial" if higher_tech else "implemented",
            "Tech_0 launch/reserve service exists. Additional authored tech lists are ignored by load_spawner; per-unit need status identifies only objects authoring these lists.",
            ("src/units/unit_tables_profiles.cpp#load_spawner", "src/sim/tactical/fighters_spawner.cpp#service_spawner"), ("FL-03", "UC-04"), 651)
    if "ASTEROID_FIELD_DAMAGE" in behaviours:
        add("asteroid-damage", "Asteroid contact damage service", "implemented",
            "Opted-in frigate/capital layers service center-XY contact every frame with independent field rolls and ordinary hardpoint damage; failed layer/locomotor/scalar gates retain cached contact.",
            ("src/units/unit_motion.cpp#motion_table", "src/sim/tactical/session_step_systems.cpp#systems", "src/sim/tactical/damage.cpp#apply_hit"),
            ("WHZ-10", "WHZ-11", "WHZ-12", "WHZ-13", "WHZ-14"), 924)
    if "NEBULA" in behaviours:
        add("nebula-contact", "Nebula contact, ability and weapon gates", "missing",
            "The hazard walk identifies missing contact windows and weapon/ability cancellation adapters.",
            (), ("WHZ-20", "WHZ-21", "WHZ-24"), 925)
    for behaviour in behaviours & {"LURE", "SELF_DESTRUCT", "DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED", "BOMB", "PROXIMITY_MINE"}:
        add("behaviour:" + behaviour, behaviour.replace("_", " ") + " service", "unknown",
            "Data opts into this behaviour, but the walks do not establish the complete handler service for this roster. Requires a targeted debug-build read before claiming fidelity.", (), ("UC-03",))
    for locomotor in row["locomotors"]:
        code = "src/units/unit_motion.cpp" if locomotor == "SIMPLE_SPACE_LOCOMOTOR" else ("src/sim/tactical/fighters_motion.cpp" if locomotor == "FIGHTER_LOCOMOTOR" else "src/sim/tactical/fighters_formation.cpp")
        add("locomotor:" + locomotor, locomotor.replace("_", " "), "implemented" if locomotor in {"SIMPLE_SPACE_LOCOMOTOR", "FIGHTER_LOCOMOTOR", "TEAM_LOCOMOTOR"} else "unknown",
            "Movement profiles and simulation service exist; remaining tag-specific gaps are separate needs.", (code,), ("UC-03",))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-root", default=os.environ.get("EAWR_EAW_GAME_ROOT"))
    parser.add_argument("--output", type=Path, default=ROOT / "data/census/space-units.json")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--docs", type=Path, help="refresh/check the marked census tables in this Markdown file")
    parser.add_argument("--issues-out", type=Path, help="write uncommitted coordinator issue proposals")
    args = parser.parse_args()
    if not args.game_root:
        if args.check:
            print("SKIPPED: EAWR_EAW_GAME_ROOT is not set")
            return 77
        parser.error("--game-root or EAWR_EAW_GAME_ROOT is required")
    try:
        data = census(args.game_root)
        encoded = encode(data)
        if args.check:
            if not args.output.is_file() or args.output.read_text(encoding="utf-8") != encoded:
                print("Census data/evidence is stale; re-review changed registry/walk evidence, then "
                      "regenerate with EAWR_EAW_GAME_ROOT set: " + REGENERATE, file=sys.stderr)
                return 1
            print(f"Census in sync: {len(data['units'])} objects, {len(data['mechanics'])} mechanics")
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            write_text(args.output, encoded)
            print(f"Wrote {len(data['units'])} objects, {len(data['mechanics'])} mechanics")
        if args.docs and not refresh_doc(args.docs, data, args.check):
            print("Census Markdown tables are stale; regenerate with EAWR_EAW_GAME_ROOT set: " +
                  REGENERATE, file=sys.stderr)
            return 1
        if args.issues_out and not args.check:
            args.issues_out.parent.mkdir(parents=True, exist_ok=True)
            write_text(args.issues_out, proposed_issues(data))
        return 0
    except (CorpusError, ValueError, ET.ParseError, OSError) as exc:
        print("Census failed: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
