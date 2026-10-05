#!/usr/bin/env python3
"""Generate the skirmish ship gate from the census (RG-01..05)."""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CENSUS = ROOT / "data/census/space-units.json"
REVIEW = ROOT / "data/census/roster-review.json"
OUTPUT = ROOT / "data/skirmish/roster-gate.json"
SHIP_CLASSES = {"SpaceUnit", "UniqueUnit", "TransportUnit", "Squadron", "HeroCompany", "Container"}


def fields(rows):
    return {r["tag"].lower(): r["value"] for r in rows}


def tooltip(reasons):
    joined = " ".join(reasons)
    if "Hero-company" in joined:
        return "Hero ship deployment is not supported yet."
    if "Mass-driver" in joined:
        return "Mass-driver weapons are not supported yet."
    if "blast-area" in joined:
        return "Special weapons and area damage are not supported yet." if "WEAPON_SPECIAL" in joined else "Weapons with area damage are not supported yet."
    if "dependency" in joined:
        return "A required fighter or spawned unit is not supported yet."
    if "locomotor" in joined:
        return "This ship's movement is not supported yet."
    return "This ship's weapons are not supported yet."


def generate(census, review):
    """Only primary weapons, movement, deployment and active dependencies gate ships."""
    units = {u["id"]: u for u in census["units"]}
    weapons = census["weapon_definitions"]
    disabled = {}
    abilities = []
    infrastructure = []
    supported = set(review["supported_capabilities"])
    hp_types = set(review["weapon_hardpoints"])
    locomotors = set(review["locomotors"])
    for name, u in sorted(units.items()):
        reasons = set()
        for a in u["unit_abilities"]:
            modifiers = [v.split(",")[0].strip() for v in a["fields"].get("Mod_Multiplier", [])]
            if a["type"] not in review["abilities"] or any(m not in review["ability_modifiers"] for m in modifiers):
                abilities.append({"unit": name, "ability": a["type"],
                                  "reason": "Ability handler or authored modifiers are unsupported."})
        # Stations/pads/upgrades are infrastructure. Their lifecycle has its own gates.
        if "DUMMY_STAR_BASE" in u["behaviours"]:
            gaps = sorted({weapons["hardpoints"][hp]["type"] for hp in u["weapons"]["hardpoints"]
                           if (weapons["hardpoints"][hp]["type"] or "").startswith("HARD_POINT_WEAPON_")
                           and weapons["hardpoints"][hp]["type"] not in hp_types
                           and not (weapons["hardpoints"][hp]["type"] == "HARD_POINT_WEAPON_MASS_DRIVER" and "mass-driver" in supported)})
            if gaps:
                infrastructure.append({"unit": name, "reason": "Required station infrastructure is retained; unsupported weapons: " + ", ".join(gaps) + "."})
            continue
        if u["class"] not in SHIP_CLASSES:
            continue
        if u["class"] == "HeroCompany" and "hero-deployment" not in supported:
            reasons.add("Hero-company purchase and space deployment are unsupported.")
        for loco in u["locomotors"]:
            if loco not in locomotors:
                reasons.add("Unsupported locomotor: " + loco + ".")
        primary = set()
        own = fields(u["special_tags"])
        primary.update(re.findall(r"[^,\s|]+", own.get("projectile_types", own.get("fire_projectile_type", ""))))
        for hp in u["weapons"]["hardpoints"]:
            h = weapons["hardpoints"][hp]
            kind = h["type"] or ""
            if hp in review["ability_only_hardpoints"]:
                continue
            if not kind.startswith("HARD_POINT_WEAPON_"):
                continue
            if kind not in hp_types:
                if kind == "HARD_POINT_WEAPON_MASS_DRIVER":
                    if "mass-driver" not in supported:
                        reasons.add("Mass-driver weapons are unsupported (legacy #1046).")
                else:
                    reasons.add("Unsupported weapon hardpoint: " + kind + ".")
            if h["projectile"]:
                primary.add(h["projectile"])
            else:
                reasons.add("Weapon has no projectile: " + hp + ".")
        for projectile in sorted(primary):
            p = weapons["projectiles"].get(projectile)
            if p is None:
                raise ValueError("Census missing primary projectile: " + projectile)
            f = fields(p["fields"])
            if float(f.get("max_speed", "0")) <= 0:
                reasons.add("Primary projectile cannot fly: " + projectile + ".")
            if float(f.get("projectile_blast_area_range", "0")) > 0 and "blast-damage" not in supported:
                reasons.add("Projectile blast-area damage is unsupported: " + projectile + ".")
            if "mass_driver" in f.get("damage_type", "").lower() and "mass-driver" not in supported:
                reasons.add("Mass-driver weapons are unsupported (legacy #1046).")
        if reasons:
            disabled[name] = reasons
    # Monotone closure terminates even for container/craft cycles. Disabled optional
    # abilities do not launch their projectiles, so ability_spawn is excluded (RG-03).
    changed = True
    while changed:
        changed = False
        for name, u in sorted(units.items()):
            if u["class"] not in SHIP_CLASSES or "DUMMY_STAR_BASE" in u["behaviours"]:
                continue
            for e in sorted(u["dependencies"], key=lambda e: e["object"]):
                if e["relation"] not in ("craft", "team_container", "launched", "spawned", "hero_deployment_candidate"):
                    continue
                child = e["object"]
                if child not in units:
                    raise ValueError("Census missing dependency: " + child)
                if child in disabled and name not in disabled:
                    disabled[name] = {"Required deployment/launch dependency is unsupported: " + child + "."}
                    changed = True
    return {"schema_version": 1, "rules": ["RG-01", "RG-02", "RG-03", "RG-04", "RG-05"],
            "disabled_units": [{"unit": n, "reason": " ".join(sorted(r)), "tooltip": tooltip(sorted(r)), "factions": units[n]["factions"]}
                               for n, r in sorted(disabled.items(), key=lambda item: item[0].lower())],
            "disabled_abilities": sorted(abilities, key=lambda a: (a["unit"].lower(), a["ability"])),
            "infrastructure_gaps": sorted(infrastructure, key=lambda u: u["unit"].lower())}


def encode(data):
    return json.dumps(data, indent=2, ensure_ascii=True) + "\n"


def header(data):
    def table(name, rows, keys):
        kind = "DisabledUnit" if name == "disabled_units" else "DisabledAbility"
        values = ["    " + kind + "{" + ", ".join(json.dumps(row[k].lower() if k == "unit" else row[k]) for k in keys) + "}," for row in rows]
        return f"inline constexpr std::array<{kind}, {len(rows)}> {name}{{{{\n" + "\n".join(values) + "\n}};\n"
    return ("// Generated from data/skirmish/roster-gate.json; do not edit.\n" +
            table("disabled_units", data["disabled_units"], ["unit", "reason", "tooltip"]) +
            table("disabled_abilities", data["disabled_abilities"], ["unit", "ability", "reason"]))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--check", action="store_true")
    p.add_argument("--game-data", action="store_true")
    p.add_argument("--header", type=Path)
    p.add_argument("--output", type=Path, default=OUTPUT)
    args = p.parse_args()
    if args.header:
        args.header.parent.mkdir(parents=True, exist_ok=True)
        args.header.write_text(header(json.loads(args.output.read_text(encoding="utf-8"))), encoding="utf-8", newline="\n")
        return 0
    if args.game_data:
        game_root = os.environ.get("EAWR_EAW_GAME_ROOT")
        if not game_root:
            print("SKIPPED: EAWR_EAW_GAME_ROOT is not set")
            return 77
        sys.path.insert(0, str(ROOT))
        from tools.inventory.unit_census import census as refresh
        census = refresh(game_root)
    else:
        census = json.loads(CENSUS.read_text(encoding="utf-8"))
    data = generate(census, json.loads(REVIEW.read_text(encoding="utf-8")))
    encoded = encode(data)
    if args.check:
        if args.output.read_bytes() != encoded.encode("utf-8"):
            print("Roster gate is stale; regenerate tools/inventory/roster_gate.py", file=sys.stderr)
            return 1
        print("Roster gate in sync")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8", newline="\n")
    for faction in ("Rebel", "Empire", "Underworld"):
        roots = {u["id"] for u in census["units"] if faction in u["factions"] and u["class"] != "UpgradeObject"
                 and any(a["route"] in ("station_menu", "starting_force") for a in u["acquisition"])}
        blocked = [u for u in data["disabled_units"] if u["unit"] in roots]
        print(f"{faction}: {len(roots) - len(blocked)} enabled / {len(blocked)} disabled")
        for u in blocked:
            print("  " + u["unit"] + ": " + u["reason"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
