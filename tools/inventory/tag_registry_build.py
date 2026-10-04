#!/usr/bin/env python3
"""Generates docs/tag-coverage/statuses.json (schema_version 2) from the seeding evidence; see tag_registry_seed.py.

    python tools/inventory/tag_registry_build.py --game-root <install> --scan out/tag-coverage/apply-scan.json \
        --scene out/tag-coverage/scene.json --tag-tables out/research/tag-tables/all3/tag-tables \
        --previous <old statuses.json> --out docs/tag-coverage/statuses.json

Run by hand when the registry needs regenerating in bulk. It keeps nothing from a previous v2 registry: hand
decisions live in the OVERRIDES table below, so the result is reproducible from the evidence.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import xml.parsers.expat
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from common import ascii_fold
    from corpus import Corpus
    from xml_inventory import _configure_safe_parser
    import tag_registry as tr
    import tag_registry_seed as seed
else:
    from .common import ascii_fold
    from .corpus import Corpus
    from .xml_inventory import _configure_safe_parser
    from . import tag_registry as tr
    from . import tag_registry_seed as seed

ROOT = Path(__file__).resolve().parents[2]
# The load trace (data/tag_trace.hpp) covers the headless M2 start: the unit tables and the skirmish start.
TRACED_PARSE = ("src/units/", "src/skirmish/")
LOADER_FILES = ("src/units/unit_tables", "src/data/", "src/vfs/")

# The unit tables loader (src/units/unit_tables.cpp) reads one object per definition and fills these records; a
# sink's first identifier says which classes a parse site serves.
UNIT_CLASSES = {"spaceunit", "squadron", "starbase", "spacestructure", "spacebuildable", "container",
                "uniqueunit", "herounit", "genericherounit", "transportunit", "specialstructure", "secondarystructure",
                "upgradeobject", "miscobject", "marker", "mobile_defense_unit", "techbuilding"}
SINK_ROOT_CLASSES = {"hardpoint": {"hardpoint"}, "weapon": {"hardpoint"} | UNIT_CLASSES, "projectile": {"projectile"},
                     "unit": UNIT_CLASSES, "movement": UNIT_CLASSES, "spawner": UNIT_CLASSES, "ability": UNIT_CLASSES,
                     "obstacle": {"spaceprop"}, "footprint": UNIT_CLASSES | {"spaceprop"}}

# Unit kinds (include/eawr/units/unit_tables.hpp UnitKind) the sim applies a tag to, where the sim splits by kind:
# the profile builders of src/units/unit_motion.cpp fill ship profiles and craft profiles from different tags.
KIND_APPLIED: dict[str, set[str]] = {
    "min_speed": {"craft"},
    "max_lift": {"craft"},
    "max_thrust": {"craft"},
    "overrideacceleration": {"ship"},
    "overridedeceleration": {"ship"},
    "strafe_distance": {"craft"},
    "out_of_combat_defense_adjustment": {"craft"},
    "minimum_follow_distance": {"craft"},
    "spin_away_on_death": {"craft"},
    "spin_away_on_death_chance": {"craft"},
    "spin_away_on_death_time": {"craft"},
    "guard_chase_range": {"squadron"},
    "idle_chase_range": {"squadron"},
    "attack_move_response_range": {"squadron"},
}
def applied(code: str, *, types: list[str] | None = None, rules: list[str] | None = None,
            note: str = "") -> dict[str, Any]:
    entry: dict[str, Any] = {"status": "applied", "code": code, "types": types or [], "rules": rules or []}
    if note:
        entry["note"] = note
    return entry


def todo(note: str, ticket: int | None = None, area: str | None = None) -> dict[str, Any]:
    entry: dict[str, Any] = {"status": "todo", "note": note}
    if ticket:
        entry["ticket"] = ticket
    if area:
        entry["area"] = area
    return entry


# Hand decisions from reading the code, by (folded class or "*", folded leaf tag). An applied entry names the code
# that applies the value; a todo entry says why it is not applied.
OVERRIDES: dict[tuple[str, str], dict[str, Any]] = {
    ("container", "layer_z_adjust"): {
        "status": "land-or-galactic", "evidence": "SCOPE-GALACTIC", "area": "movement",
        "note": "the only Container that authors it is Galactic_Fleet"},
    **{(space_class, "layer_z_adjust"): applied(
        "src/skirmish/start.cpp#layer_position", rules=["LZ-01", "LZ-02"],
        note="#718: every start object is created at its placement point raised by its type's height")
       for space_class in ("spaceunit", "uniqueunit", "transportunit", "starbase", "spacestructure",
                           "secondarystructure", "specialstructure", "miscobject")},
    ("spaceprop", "layer_z_adjust"): todo(
        "SpaceProps are drawn by the view, not simulated: the start does not place them at their height (LZ-01)", 649),
    ("radarmapsettings", "passability_color_settings/color"): todo(
        "nothing reads the passability colours; the minimap draws its own", 653),
    ("radarmapsettings", "color"): applied("src/presentation/ui/minimap.cpp#background", rules=["MM-14"],
                                           note="the entry named space fills the background layer"),
    ("*", "lua_script"): todo(
        "only the PowerToShields object script is compared by name; every other object script is ignored", 652, "ai"),
    ("squadron", "squadron_offsets"): applied("src/units/unit_motion.cpp#offsets"),
    ("spaceprop", "scale_factor"): {**applied("src/units/unit_motion.cpp#scale_factor",
                                              note="the map-object footprint loader reads it under an unrecorded trace"),
                                    "trace": "unrecorded"},
    ("*", "projectile_types_override"): applied("src/units/unit_tables_profiles.cpp#Projectile_Types_Override",
                                                note="a team ability's projectile list, read by load_team"),
    ("*", "sfxevent_gui_unit_ability_activated"): applied("apps/viewer/src/battle_audio_prepare.cpp#ability_voices"),
    ("*", "sfxevent_gui_unit_ability_deactivated"): applied("apps/viewer/src/battle_audio_prepare.cpp#ability_voices"),
    ("*", "variant_of_existing_type"): applied("src/data/xml_merge.cpp#Variant_Of_Existing_Type",
                                               rules=["R-08", "R-11"]),
    ("*", "music_event_list_battle"): applied("apps/viewer/src/battle_audio_prepare.cpp#battle"),
    ("*", "sfxevent_gui_enemy_toggle_non_hero_ability_off"): applied("apps/viewer/src/battle_audio_events.cpp#Toggle"),
    ("*", "sfxevent_gui_enemy_toggle_non_hero_ability_on"): applied("apps/viewer/src/battle_audio_events.cpp#Toggle"),
    ("*", "sfxevent_gui_toggle_non_hero_ability_off"): applied("apps/viewer/src/battle_audio_events.cpp#Toggle"),
    ("*", "space_skirmish_ai_default_forces"): applied("src/skirmish/inputs.cpp#Space_Skirmish_AI_Default_Forces"),
    ("*", "marker_for_specific_object_type"): applied("src/skirmish/inputs.cpp#Marker_For_Specific_Object_Type"),
    ("functionset", "function"): applied("src/script/foc/ai_data.cpp#GoalFunction"),
    ("functionset", "goal"): applied("src/script/foc/ai_data.cpp#GoalFunction"),
    ("aiplayertype", "goalproposalfunctionsets"): applied("src/script/foc/ai_data.cpp#function_sets"),
    ("aiplayertype", "space"): applied("src/script/foc/ai_data.cpp#space_templates"),
    ("aiplayertype", "normal"): applied("src/script/foc/ai_service.cpp#difficulty"),
    ("aiplayertype", "easy"): todo("only the Normal difficulty adjustments are read (walk 6 G-07)", 735, "ai"),
    ("aiplayertype", "hard"): todo("only the Normal difficulty adjustments are read (walk 6 G-07)", 735, "ai"),
    ("goals", "aigoalapplicationflags"): applied("src/script/foc/ai_data.cpp#AIGoalApplicationFlags"),
    ("gameconstants", "damage_to_armor_mod"): applied("src/units/unit_support.cpp#damage_to_armor"),
    ("gameconstants", "auto_rotate_for_space_targeting"): todo(
        "loaded into the combat constants, never read; the sim hard-codes FoC's false (src/sim/tactical/combat_algorithms.hpp)", 844),
    ("gameconstants", "bombing_run_reduction_per_squadron_percent"): todo(
        "loaded into the combat constants, never read; 0 in the shipped data", 844),
    ("gameconstants", "hardpoint_recharge_cutoff_for_opportunity_fire"): todo(
        "loaded into the combat constants, never read (logic review L2 R-01)", 743),
    ("gameconstants", "space_elevated_vulnerability_duration"): todo("loaded into the combat constants, never read", 844),
    ("gameconstants", "space_elevated_vulnerability_factor"): todo("loaded into the combat constants, never read", 844),
    ("gameconstants", "battle_pending_message_color"): applied("src/presentation/ui/battle_messages.cpp#looks"),
    ("gameconstants", "win_message_color"): applied("src/presentation/ui/battle_messages.cpp#looks"),
    ("gameconstants", "lose_message_color"): applied("src/presentation/ui/battle_messages.cpp#looks"),
    ("gameconstants", "win_lose_message_font_size"): applied("src/presentation/ui/battle_messages.cpp#fallback"),
    ("tacticalcamera", "space_tactical_camera_locked"): todo(
        "the camera constants loader rejects an override of it; nothing applies the value", 653),
    ("*", "death_fade_time"): applied("apps/viewer/src/live_session_death.cpp#seconds_tag"),
    ("*", "death_persistence_duration"): applied("apps/viewer/src/live_session_death.cpp#seconds_tag"),
    ("*", "specific_death_anim_type"): applied("apps/viewer/src/live_session_death.cpp#death_anim_type"),
    ("hardpoint", "death_explosion_particles"): applied("apps/viewer/src/battle_effects_prepare.cpp#hardpoint_explosions"),
    ("hardpoint", "fire_inaccuracy_distance"): applied("src/units/unit_tables_decode.cpp#inaccuracy"),
    **{(unit_class, "fire_inaccuracy_distance"): {"status": "foc-ignores", "evidence": "DG-24", "area": "combat"}
       for unit_class in ("spacebuildable", "spaceunit", "specialstructure", "transportunit", "uniqueunit")},
    ("lensflares", "radius"): todo("no loader reads it (the scan's match is an unrelated particle property)", 653),
    ("lightsource", "radius"): todo("no loader reads it (the scan's match is an unrelated particle property)", 653),
    ("lightningeffect", "radius"): todo("no loader reads it (the scan's match is an unrelated particle property)", 653),
    ("projectile", "categorymask"): todo("the projectile loader does not read it", 650),
    **{(unit_class, "collidable_by_projectile_living"): {
        "status": "partial", "basis": "reviewed", "area": "combat", "ticket": 649,
        "applied": [{"code": "src/sim/tactical/blast.cpp#living_projectile_collision",
                     "rules": ["WAD-14"], "types": ["craft", "ship", "station"]}],
        "missing_types": ["objects outside the simulated unit closure"],
        "note": "Every loaded tactical unit reads and applies the flag; unsimulated map props and projectile objects remain outside unit combat profiles"}
        for unit_class in ("genericherounit", "herounit", "marker", "miscobject", "mobile_defense_unit",
            "projectile", "spaceprop", "spaceunit", "starbase", "transportunit", "uniqueunit",
            "secondarystructure", "spacebuildable", "specialstructure")},
    ("spacestructure", "collidable_by_projectile_living"): todo(
        "WAD-14: map-only space structures are outside the simulated tactical unit closure", 649),
    **{("projectile", tag): applied("src/sim/tactical/blast.cpp#" + location,
        rules=rules, note="WAD blast service with a temporary secondary hull route; positive distance delay is U-04 project policy")
        for tag, location, rules in (
            ("projectile_blast_area_damage", "primary_damage", ["WAD-01", "WAD-02"]),
            ("projectile_blast_area_range", "prepare_blast", ["WAD-01", "WAD-10", "WAD-12"]),
            ("projectile_blast_area_dropoff", "blast_factor", ["WAD-15", "WAD-16"]),
            ("projectile_blast_area_dropoff_tiers", "blast_factor", ["WAD-16"]),
            ("projectile_blast_area_immune_faction", "immune_faction", ["WAD-09"]),
            ("max_secs_for_ae_delayed_damage", "max_delay", ["WAD-17"]))},
    ("*", "projectile_sfxevent_detonate"): applied("apps/viewer/src/battle_audio_prepare.cpp#detonate"),
    ("*", "projectile_sfxevent_detonate_reduced_by_armor"): applied("apps/viewer/src/battle_audio_prepare.cpp#detonate_armor"),
    ("sfxevent", "use_preset"): applied("src/presentation/audio/sfx.cpp#Use_Preset"),
    ("*", "fires_forward"): applied("src/units/unit_tables_profiles.cpp#cone_width_degrees"),
    ("enumdefinition", "*"): applied("src/units/unit_support.cpp#enum_number"),
    ("priority_set", "attack_priorities"): applied("src/units/unit_priority.cpp#attack_priorities"),
    ("priority_set", "unit_exclusions"): applied("src/units/unit_priority.cpp#unit_exclusions"),
    ("priority_set", "category_exclusions"): applied("src/units/unit_priority.cpp#category_exclusion_bits"),
    ("priority_set", "property_exclusions"): applied("src/units/unit_priority.cpp#property_exclusion_bits"),
    ("priority_set", "hard_point_priorities"): todo(
        "loaded into the priority set; the ship-level hardpoint choice does not use it (#702)", 702, "combat"),
    ("*", "special_ability_name"): todo("loaded into the hardpoint, never used (#797)", 715, "combat"),
    ("*", "file"): applied("src/data/xml.cpp#File", note="a registry file entry, read by the catalog's file lists"),
}

# Tickets that own a tag's gap, by folded leaf name; every other todo row links its area's tracking issue.
PARTIAL_TICKET = 843  # the issue for tags applied for one unit kind only
TICKET_BY_TAG = {
    "layer_z_adjust": 649,
    "targeting_stickiness_time_threshold": 701,
    "reserve_spawned_units_tech_0": 651,
}
AI_CLASSES = {"goals", "functionset", "aitemplates", "aiplayertype", "equations", "difficulty_adjustment"}
KIND_CLASSES = {"spaceunit": ("ship", "craft"), "uniqueunit": ("ship", "craft"), "herounit": ("ship", "craft"),
                "genericherounit": ("ship", "craft"), "transportunit": ("ship", "craft"), "starbase": ("station",),
                "squadron": ("squadron",), "spacestructure": ("station",)}
OBJECT_CLASSES = {"spaceunit", "starbase", "squadron", "uniqueunit", "herounit", "genericherounit", "transportunit",
                  "spacestructure"}


def authored_kinds(game_root: Path) -> dict[tuple[str, str], Counter]:
    """(folded class, folded tag) -> how many space objects of each unit kind author the tag, variants merged."""
    corpus = Corpus(game_root)
    objects: dict[str, tuple[str, dict[str, str]]] = {}
    for source in corpus.iter_sources("foc", ".xml", "effective"):
        parser = xml.parsers.expat.ParserCreate()
        _configure_safe_parser(parser, source.logical_path)
        parser.buffer_text = True
        state = {"depth": 0, "object_depth": -1}
        current: list[Any] = []  # [class, name, tags, text of the tag being read]

        def start(name: str, attributes: dict[str, str]) -> None:
            state["depth"] += 1
            folded = ascii_fold(name)
            if state["object_depth"] < 0 and folded in OBJECT_CLASSES and attributes.get("Name"):
                state["object_depth"] = state["depth"]
                current[:] = [folded, attributes["Name"], {}, None]
            elif state["object_depth"] >= 0 and state["depth"] == state["object_depth"] + 1:
                current[3] = [ascii_fold(name), ""]

        def end(name: str) -> None:
            if state["object_depth"] >= 0 and state["depth"] == state["object_depth"] + 1 and current[3]:
                current[2][current[3][0]] = current[3][1].strip()
                current[3] = None
            if state["depth"] == state["object_depth"]:
                objects[ascii_fold(current[1])] = (current[0], current[2])
                state["object_depth"] = -1
            state["depth"] -= 1

        def text(value: str) -> None:
            if state["object_depth"] >= 0 and current and current[3] and state["depth"] == state["object_depth"] + 1:
                current[3][1] += value

        parser.StartElementHandler = start
        parser.EndElementHandler = end
        parser.CharacterDataHandler = text
        try:
            parser.Parse(source.data, True)
        except xml.parsers.expat.ExpatError:
            continue

    def merged(name: str, seen: set[str] | None = None) -> dict[str, str]:
        seen = seen or set()
        cls, tags = objects[name]
        result: dict[str, str] = {}
        base = ascii_fold(tags.get("variant_of_existing_type", ""))
        if base and base in objects and base not in seen:
            result.update(merged(base, seen | {base}))
        result.update(tags)
        return result

    kinds: dict[tuple[str, str], Counter] = defaultdict(Counter)
    for name, (cls, _tags) in objects.items():
        tags = merged(name)
        mask = tags.get("categorymask", "").lower()
        layer = tags.get("space_layer", "").lower()
        if cls == "starbase" or cls == "spacestructure":
            kind = "station"
        elif cls == "squadron":
            kind = "squadron"
        elif "fighter" in mask or "bomber" in mask or layer in ("fighter", "bomber"):
            kind = "craft"
        else:
            kind = "ship"
        for tag in tags:
            kinds[(cls, tag)][kind] += 1
    return kinds


# --- Classification -------------------------------------------------------------------------------

def leaf_of(tag: str) -> str:
    return tag.rsplit("/", 1)[-1]


def snake(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_").lower()


def site_applies(site: dict[str, Any]) -> bool | None:
    """True when the site's value provably reaches other code, False when it provably stays in the loader, None when
    the scan could not follow it."""
    if not site.get("sink"):
        return None
    if site.get("local"):
        if site["file"].startswith(LOADER_FILES):
            return False if site.get("local_uses", 0) == 0 else None
        return site.get("local_uses", 0) > 0
    return site.get("use_lines", 0) > 0


# Loaders that serve one XML family only: a tag name they mention is theirs alone (`Scale` in the command bar
# table is not a SpaceProp tag). The empty set marks files whose literals are not XML tags at all.
FILE_CLASSES: dict[str, set[str]] = {
    "src/data/ui/command_bar.cpp": {"commandbarcomponent"},
    "src/data/ui/": {"guidialogs", "commandbarcomponent"},
    "src/presentation/ui/theme.cpp": {"guidialogs"},
    "src/presentation/godot/ui/kit.cpp": {"guidialogs"},
    "src/presentation/camera/": {"tacticalcamera"},
    "src/presentation/audio/sfx.cpp": {"sfxevent", "musicevent", "speechevent", "audio"},
    "src/presentation/particles/alo_particles.cpp": set(),
    "src/presentation/space/fog_field.cpp": {"gameconstants"},
    "src/presentation/ui/battle_messages.cpp": {"gameconstants"},
    "src/script/foc/ai_data.cpp": {"aiplayertype", "goals", "functionset", "aitemplates", "equations", "enumdefinition",
                                   "difficulty_adjustment", "gameconstants"},
    "src/script/foc/ai_perception.cpp": set(),
    "src/script/foc/tactical_ai.cpp": set(),
    "src/script/foc/tactical_ai_bindings.cpp": set(),
    "src/script/foc/tactical_ai_orders.cpp": set(),
    "src/script/foc/tactical_ai_loading.cpp": set(),
    "apps/viewer/src/ui_gallery_mode.cpp": set(),
    "apps/viewer/src/ui_gallery_controls.cpp": set(),
    "apps/viewer/src/ui_gallery_report.cpp": set(),
    "apps/viewer/src/ui_gallery_internal.hpp": set(),
    "apps/viewer/src/camera_binding_config.cpp": set(),
    "apps/viewer/src/camera_binding_tokens.cpp": set(),
    "apps/viewer/src/camera_binding_values.cpp": set(),
    "apps/viewer/src/camera_binding_table.cpp": set(),
    "apps/viewer/src/camera_binding_config_internal.hpp": set(),
}


# Families with a loader of their own: their tags come from the files FILE_CLASSES lists for them, never from a
# file that happens to spell the same word (`Icon_Name` of the unit cards is not a weather modifier's).
ISOLATED = {"goals", "functionset", "aitemplates", "aiplayertype", "difficulty_adjustment", "equations",
            "enumdefinition", "guidialogs", "commandbarcomponent", "tacticalcamera", "sfxevent", "musicevent",
            "speechevent", "audio", "radarmapsettings", "radarmapevents", "mousepointers", "hero_clash", "movie",
            "material", "lightningeffect", "lensflares", "lightsource", "decal", "decals", "hintsets",
            "surfaceeffects", "dynamictrack", "ambientmapsounds", "weathermodifies", "weather_scenario",
            "specialeffect", "spaceprimaryskydome", "spacesecondaryskydome", "priority_set"}


# Files that serve a family of their own and every object class besides (the minimap reads the radar settings and each
# object type's radar look).
SHARED_FILES = {"src/presentation/ui/minimap.cpp": {"radarmapsettings", "radarmapevents", "gameconstants"}}


def site_serves(site: dict[str, Any], object_class: str) -> bool:
    for prefix, classes in SHARED_FILES.items():
        if site["file"].startswith(prefix) and ascii_fold(object_class) in classes:
            return True
    for prefix, classes in FILE_CLASSES.items():
        if site["file"].startswith(prefix):
            return ascii_fold(object_class) in classes
    if ascii_fold(object_class) in ISOLATED:
        return False
    if not site["file"].startswith("src/units/unit_tables") or not site.get("sink"):
        return True
    root = re.split(r"\.|->", site["sink"])[0]
    classes = SINK_ROOT_CLASSES.get(root)
    return classes is None or ascii_fold(object_class) in classes


USE_PRIORITY = ("src/sim/", "src/skirmish/", "src/script/", "src/units/", "src/presentation/", "apps/viewer/", "src/")


def domain(path: str) -> str:
    return "/".join(path.split("/")[:2] if path.startswith(("src/", "apps/")) else path.split("/")[:1])


def first_use(files: list[str], parse_file: str) -> str:
    """The use to cite: the parse site's own subsystem first (a viewer loader is applied by viewer code), then the
    simulation before the presentation."""
    if parse_file in files and not parse_file.startswith(LOADER_FILES):
        return parse_file  # a presentation loader applies the value where it reads it
    own = [name for name in files if domain(name) == domain(parse_file) and name != parse_file]
    for pool in (own, files):
        for prefix in USE_PRIORITY:
            for name in pool:
                if name.startswith(prefix):
                    return name
    return files[0]


def best_use(site: dict[str, Any]) -> str:
    if site.get("local") or not site.get("uses"):
        return f"{site['file']}#{site['sink']}"
    return f"{first_use(site['uses'], site['file'])}#{site['field']}"


class Builder:
    def __init__(self, universe, previous, knowledge, scan, consumed, docs, kinds, overrides):
        self.universe = universe
        self.previous = previous
        self.knowledge = knowledge
        self.scan = scan
        self.consumed = consumed
        self.docs = docs
        self.kinds = kinds
        self.overrides = {**OVERRIDES, **overrides}
        self.notes: list[str] = []
        self.unknown: list[str] = []
        self.scene_classes: set[str] = set()

    def ticket(self, leaf: str, area: str, tag: str = "", object_class: str = "") -> int:
        """The issue that owns the gap: a tag's own ticket, else its mechanic's walk (abilities, tactical AI), else its
        area's tracking issue."""
        if ascii_fold(leaf) in TICKET_BY_TAG:
            return TICKET_BY_TAG[ascii_fold(leaf)]
        if tag.startswith(("Abilities/", "Unit_Abilities_Data/")):
            return 760
        if ascii_fold(object_class) in AI_CLASSES:
            return 737
        return seed.AREA_TICKET[area]

    def applied_entry(self, code: str, leaf: str) -> dict[str, Any]:
        return {"status": "applied", "basis": "auto", "applied": [{"code": code, "rules": self.docs.rules(leaf)}]}

    def decide(self, row: tr.UniverseRow) -> dict[str, Any]:
        object_class, tag = row.object_class, row.tag
        folded_class = ascii_fold(object_class)
        leaf = leaf_of(tag)
        folded = ascii_fold(leaf.lstrip("@"))
        area = seed.guess_area(f"{object_class}/{tag}")
        pair = tr.pair_key(object_class, tag)
        consumed = ascii_fold(f"{object_class}/{tag}") in self.consumed
        old = self.previous.get(pair)
        attribute = leaf.startswith("@")
        entry: dict[str, Any] = {"area": area}

        override = self.overrides.get((folded_class, ascii_fold(tag))) or self.overrides.get((folded_class, folded))
        if override is None and folded_class == "gameconstants" and folded.startswith("mp_color_"):
            override = applied("src/skirmish/inputs.cpp#colour_prefix", rules=["SK-12"])
        if override is None and not (seed.scope_of(object_class) and not consumed):
            override = self.overrides.get(("*", folded))
        if override:
            entry.update({k: v for k, v in override.items() if k not in ("code", "types", "rules")})
            if entry["status"] == "applied":
                rules = override.get("rules") or self.docs.rules(leaf)
                entry["applied"] = [{"code": override["code"], "rules": rules, "types": override.get("types", [])}]
                entry["basis"] = "reviewed"
                if not rules:
                    entry["note"] = (entry.get("note", "") + "; " if entry.get("note") else "") \
                        + "no behaviour-note rule mentions this tag yet"
            elif "ticket" not in entry and entry["status"] != "foc-ignores":
                entry["ticket"] = self.ticket(leaf, entry["area"], tag, object_class)
            return entry
        if old and old["status"] in ("deferred", "land-or-galactic") and not consumed:
            entry.update(status=old["status"], area=old["area"])
            for key in ("ticket", "evidence"):
                if key in old:
                    entry[key] = old[key]
            return entry
        if attribute and folded == "name" and tag == "@Name":
            # The catalog keys every object definition by its Name attribute.
            return {"area": "data", "status": "applied", "basis": "reviewed",
                    "applied": [{"code": "src/data/xml_registry.cpp#attribute(\"Name\")", "rules": ["R-08", "R-11"]}]}
        if not attribute and not self.knowledge.knows(leaf):
            entry.update(status="foc-ignores", evidence="DB-NOTAG")
            if self.scan.get(folded):
                self.notes.append(f"we parse {object_class}/{tag} but the debug build does not know the name")
            return entry
        scope = seed.scope_of(object_class)
        parsed_constants = folded_class == "gameconstants"
        if scope and not consumed and not parsed_constants:
            entry.update(status=scope[0], area=scope[1], evidence=scope[2])
            return entry
        sites = [s for s in self.scan.get(folded, {}).get("sites", []) if site_serves(s, object_class)] if not attribute else []
        verdicts = [(site, site_applies(site)) for site in sites]
        applying = [s for s, v in verdicts if v]
        if applying and folded_class in self.scene_classes and folded_class != "spaceprop" and not consumed and all(
                s["file"].startswith(TRACED_PARSE) for s in applying):
            # The loaders the load trace covers read this tag for other classes, and never for this class' objects
            # of the M2 scene (Container's Max_Speed: the unit loader reads it for units, not for a team).
            entry.update(status="todo", ticket=self.ticket(leaf, area, tag, object_class),
                         note="the loader reads it for other classes; the M2 scene's objects of this class never "
                              "have it read")
            return entry
        if applying:
            best = max(applying, key=lambda s: (bool(s.get("uses")), s.get("use_lines", s.get("local_uses", 0))))
            entry.update(self.applied_entry(best_use(best), leaf))
            entry["applied"][0]["types"] = []
            if folded_class == "spaceprop" and best["file"].startswith("src/units/unit_tables"):
                entry["trace"] = "unrecorded"  # map objects load through the obstacle loader, off the trace
            return entry
        if sites and any(v is None for _s, v in verdicts):
            proxy = self.proxy(leaf, {s["file"] for s in sites})
            if proxy:
                entry.update(self.applied_entry(proxy, leaf))
                entry["applied"][0]["types"] = []
                entry["note"] = "matched by the field's name; the loader is table-driven"
                return entry
            unknown = [s for s, v in verdicts if v is None]
            presentation = [s for s in unknown if s["file"].startswith(("src/presentation/", "apps/viewer/"))]
            if presentation:
                spelled = re.search(r'"(' + re.escape(leaf) + r')"', presentation[0]["text"], re.IGNORECASE)
                entry.update(self.applied_entry(f"{presentation[0]['file']}#{spelled.group(1) if spelled else leaf}", leaf))
                entry["applied"][0]["types"] = []
                entry["note"] = "a presentation parse site that uses the value where it reads it"
                return entry
            self.unknown.append(f"{object_class}/{tag}: {sites[0]['file']}:{sites[0]['line']}")
            entry.update(status="todo", ticket=self.ticket(leaf, area, tag, object_class),
                         note="a data loader reads it; where the value goes is not traced")
            return entry
        if sites:
            entry.update(status="todo", ticket=self.ticket(leaf, area, tag, object_class),
                         note="read by a loader, but nothing applies the value")
            return entry
        named = seed.scope_of_name(leaf, tag)
        money = seed.named_todo(leaf)
        if money and not (named and named[2] in ("SCOPE-LAND", "SCOPE-GALACTIC")):
            entry.update(status=money[0], area=money[1], ticket=money[2], note=money[3])
            return entry
        if named:
            entry.update(status=named[0], area=named[1], evidence=named[2])
            return entry
        entry.update(status="todo", ticket=self.ticket(leaf, area, tag, object_class))
        return entry

    def proxy(self, leaf: str, parse_files: set[str]) -> str | None:
        """A table-driven loader stores each tag under a member named like it; a use of that name elsewhere."""
        name = snake(leaf)
        if "_" not in name:
            return None
        pattern = self.identifier(name)
        for path in self.source_index:
            if path in parse_files or path.startswith(("tests/", "include/eawr/data/")) or "identity" in path:
                continue
            if pattern.search(self.source_index[path]):
                return f"{path}#{name}"
        return None

    source_index: dict[str, str] = {}

    @staticmethod
    def identifier(name: str) -> re.Pattern[str]:
        return re.compile(rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])")


def apply_kinds(builder: Builder, decisions: dict[tuple[str, str], dict[str, Any]]) -> None:
    """Splits the rows whose tag the sim applies for some unit kinds only into applied, partial and todo."""
    for pair, entry in decisions.items():
        row = builder.universe[pair]
        folded = ascii_fold(leaf_of(row.tag))
        if folded not in KIND_APPLIED or "/" in row.tag:
            continue
        if entry["status"] != "applied":
            continue
        classes = KIND_CLASSES.get(pair[0])
        if classes is None:
            continue
        authored = {kind for kind, _n in builder.kinds.get((pair[0], folded), Counter()).items()} or set(classes)
        applied = KIND_APPLIED[folded]
        target = entry["applied"][0]
        if authored <= applied:
            target["types"] = sorted(authored & applied)
        elif not (authored & applied):
            entry.update(status="todo", ticket=builder.ticket(leaf_of(row.tag), entry["area"], row.tag, row.object_class),
                         note=f"the sim applies it to {', '.join(sorted(applied))} only; authored on "
                              f"{', '.join(sorted(authored))}")
            entry.pop("applied", None)
            entry.pop("basis", None)
        else:
            target["types"] = sorted(authored & applied)
            entry.update(status="partial", ticket=TICKET_BY_TAG.get(folded, PARTIAL_TICKET),
                         missing_types=sorted(authored - applied),
                         note="the sim applies it to " + ", ".join(sorted(applied))
                              + " only; FoC's per-type read is unverified in the debug build")


def group_rows(universe, decisions) -> list[dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = {}
    spelling: dict[str, Counter] = defaultdict(Counter)
    for pair, entry in decisions.items():
        spelling[pair[1]][universe[pair].tag] += 1
        signature = json.dumps([pair[1], entry], sort_keys=True)
        group = groups.setdefault(signature, {"entry": entry, "tag": pair[1], "classes": []})
        group["classes"].append(universe[pair].object_class)
    rows = []
    for group in groups.values():
        row = dict(group["entry"])
        row["tag"] = spelling[group["tag"]].most_common(1)[0][0]
        row["classes"] = sorted(set(group["classes"]), key=ascii_fold)
        rows.append(row)
    rows.sort(key=tr.row_order)
    return rows


def build(args: argparse.Namespace) -> dict[str, Any]:
    inventory = tr.load_inventory(args.inventory)
    universe = tr.load_universe(inventory)
    knowledge = seed.Knowledge(args.tag_tables)
    scan = json.loads(args.scan.read_text(encoding="utf-8"))
    scene = json.loads(args.scene.read_text(encoding="utf-8"))
    consumed = {ascii_fold(x) for x in scene["consumed"]}
    scene_classes = {ascii_fold(x.split("/")[0]) for x in scene["scene"]}
    docs = seed.Docs(ROOT)
    previous: dict[tuple[str, str], dict[str, Any]] = {}
    if args.previous:
        old = json.loads(args.previous.read_text(encoding="utf-8"))
        for row in old["rows"]:
            object_class, _, tail = row["tag"].partition("/")
            previous[tr.pair_key(object_class, tail)] = row
    kinds = authored_kinds(args.game_root)
    overrides = load_overrides(args.overrides) if args.overrides else {}
    builder = Builder(universe, previous, knowledge, scan, consumed, docs, kinds, overrides)
    builder.scene_classes = scene_classes
    builder.source_index = {
        path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8", errors="replace")
        for base in ("src", "include", "apps/viewer/src") for path in sorted((ROOT / base).rglob("*"))
        if path.suffix in (".cpp", ".hpp", ".h") and path.is_file()}
    decisions = {pair: builder.decide(row) for pair, row in sorted(universe.items())}
    apply_kinds(builder, decisions)
    rows = group_rows(universe, decisions)
    registry = {"schema_version": tr.SCHEMA_VERSION, "evidence": dict(seed.EVIDENCE), "rows": rows}
    for note in builder.notes:
        print("note:", note)
    for unknown in builder.unknown:
        print("unresolved:", unknown)
    return registry


def load_overrides(path: Path) -> dict[tuple[str, str], dict[str, Any]]:
    """Hand decisions: [{"class": "*"|"SpaceUnit", "tag": "Leaf", ...row fields}]."""
    result = {}
    for item in json.loads(path.read_text(encoding="utf-8")):
        entry = {k: v for k, v in item.items() if k not in ("class", "tag")}
        result[(ascii_fold(item.get("class", "*")), ascii_fold(item["tag"]))] = entry
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--inventory", type=Path, default=tr.DEFAULT_INVENTORY)
    parser.add_argument("--scan", required=True, type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--tag-tables", required=True, type=Path)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--overrides", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    registry = build(args)
    tr.write_text(args.out, tr.dumps_registry(registry))
    by_status, _ = tr.counts(registry)
    print(json.dumps(by_status, sort_keys=True), f"{len(registry['rows'])} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
