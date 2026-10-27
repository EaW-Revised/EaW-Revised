"""Audit checkpoint Fields against declarations; never rewrite released wire order.

Adapted from the child-state field inventory generator. The inventory follows nested
member types as well as the registered roots, so adding a member or a nested state
struct requires serialization, an explicit alternate codec, or a documented
derived-state exclusion.
"""
import argparse
from pathlib import Path
import re
import sys

# The shared source-reader map: a logical path reads all of its physical members.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "presentation/renderer"))
from viewer_mode_sources import SOURCE_BUNDLES, source_text  # noqa: E402

EXCLUDED = {
    "eawr::sim::tactical::QueueEntry": {
        "controller_seat": "serialized and identity-validated by the v4 co-op controller tail in session_state.cpp; disabled policy requires the default value",
    },
    "eawr::sim::tactical::PlayerEconomy": {
        "pool_controllers": "serialized and token/owner-validated by the v4 co-op controller tail in session_state.cpp; disabled policy requires an empty map",
    },
    "eawr::sim::tactical::RespawnState": {
        "controller_seat": "serialized and owner-validated by the v4 co-op controller tail in session_state.cpp; disabled policy requires the default value",
    },
    "eawr::sim::tactical::SpawnerState": {
        "controller_output_ordinal": "serialized by spawner identity in the v4 co-op controller tail in session_state.cpp; disabled policy requires zero",
    },
    "eawr::sim::tactical::AbilityState": {
        "pending_buzz_controller": "serialized with live identity and validated against its pending request in the v4 co-op controller tail in session_state.cpp; disabled policy requires the default value",
    },
    "eawr::sim::tactical::UnitState": {
        "control": "serialized and validated by the tagged controller tail in session_state.cpp save_checkpoint_state/load_checkpoint_state; disabled policy requires the default value",
        "controller_seat": "serialized and validated by the tagged controller tail in session_state.cpp save_checkpoint_state/load_checkpoint_state; disabled policy requires the default value",
    },
    "eawr::units::LandCompanyMember": {
        "stay_in_transport": "immutable unit-table creation admission; transport members are refused before runtime land state exists",
    },
    "eawr::sim::tactical::session_detail::LiveUnit": {
        "placement_cache": "derived transform cache rebuilt from position and rotation",
        "banked_placement_cache": "derived banked transform cache rebuilt from heading and roll",
        "matrix_builds": "per-tick diagnostic counter",
        "banked_matrix_builds": "per-tick diagnostic counter",
        "engines_recovered": "per-tick movement replan scratch",
        "cause_damage_mode": "per-tick ability service scratch",
        "take_damage_mode": "per-tick ability service scratch",
        "in_tractor_beam": "derived from live source/category entries each tick",
    },
    "eawr::sim::tactical::TacticalSession::ReinforcementSearchResult": {
        "nanoseconds": "host timing; not deterministic state"},
}
# Every owner omission is explicit, so newly added state still fails the gate.
OWNER_EXCLUSIONS = {
    "eawr::sim::galactic::GalacticSession::Impl": {
        "immutable setup encoded by the recording codec": "setup",
        "registry objects encoded by the world object sections": "registry planets routes fleets",
        "runtime callbacks rebound by the caller": "ability_consumption_handler ground_ability_story_handler conflict_tail control_handler corruption_completion_handler corruption_mission_handler corruption_result_handler credit_notification_handler hasher movement_access_provider native_actor_changes_handler native_movement_listener native_production_handler native_production_outcome_handler native_replayers planet_ruins_story_handler technology_notice_handler timeline_service",
        "synchronous consumption guard, zero at every checkpoint barrier": "consuming_spy",
        "derived service inputs and indexes rebuilt on load": "ability_input_generation ability_work conflict_requests_enabled conflict_tail_outer_timer control_stealth_generation control_stealth_inputs economy_input_generation gathered_income_inputs graph income_control_generation income_edges income_forecast_cache income_gather_generation income_input_generation income_modifier_generation income_modifiers income_source_candidates movement_access_generation native_invasion_transport_count native_invasions_enabled observer_input_generation observer_neighbours    published_actor_records   published_economy_input_generation   published_production published_production_generation  published_production_originals published_sale_types snapshot technology_price_expiry victory_tail_deferred  production_generation",
        "match-local diagnostic collector; never authoritative, hashed or checkpointed": "load_note_collector load_note_count",
        "derived hash caches and host diagnostics": "cached_hash cached_hash_present digests dirty production_offer_hashes discovery_records discovery_targets income_forecast_rebuilds income_modifier_rebuilds income_sources_gathered observer_projection_records population_objects population_producers production_entries production_offer_rebuilds production_producers verify_cached_hash",
        "local per-player publication caches rebuilt on demand, never serialized": "view_publications",
        "immutable snapshot section copies and revision counters rebuilt on load": "published_sections section_generation",
        "immutable native setup schemas supplied with the session setup": "native_fiscal_schema native_income_schema native_population_schema",
        "native AI barrier publication state rebuilt after load": " native_barrier_active native_commit_publication native_commit_publications ",
        "runtime callbacks rebound by the caller (feedback)": "feedback_notice_handler",
        "admitted History service owner rebound by the scripted adapter": "history_service_deferred",
        "unhealthy or in-flight state cannot reach a save barrier": "failed native_applying",
        "transient nested surface-launch admission guard, active only while native_applying forbids saves": "launch_leg",
        "system pause hold derived from conflict, summary and child ownership and rebound on load": "system_hold",
        "applied native journal history is carried by the recording section; the owner keeps only its length (#4058)": "native_effects",
        "prebattle autosave admission window derived by the conflict owner each step, never saved (#2524)": "prebattle_autosave_window",
    },
    "eawr::campaign::GameSession::Impl": {
        "serialized separately by world, script, child and return owner codecs": "galaxy scripts child staged return_state live_hero_returns native_ai story_engine story_goals",
        "runtime callbacks rebound by the caller": "auto_progress auto_resolver auto_state_reader auto_state_writer auto_withdraw child_tech conflict_gate control_observer corruption_mission_service hasher hero_placement hero_resolver hero_structures layers outcome_services pending_alert story_handler story_rewards technology_handler vote_progress",
        "immutable content lookup tables rebound on load": "story_factions story_planets story_scripts",
        "transient outcome and publication history": "latest_auto_outcome latest_context latest_result outcome_history outcome_record_index entry_notes story_notes story_notices story_reported story_triggers story_presentation",
        "host-side diagnostic oracle trace session id": "native_oracle_session",
        "history rebase scheduled by a save or load and applied at the next step; a recapture before it sees the saved window (#4058)": "scheduled_base",
        "unhealthy or reconstruction-only state cannot reach a save barrier": "failure reconstructing_native_notices restoring_child",
        "the running step's executor, set only inside a step (#683)": "executor",
    },
    "eawr::campaign::GameSession::Impl::TimelinePublication": {
        "process-local publication slots rebuilt from saved timelines": "child_slot historical_slot",
    },
}
for owner, groups in OWNER_EXCLUSIONS.items():
    EXCLUDED[owner] = {field: reason for reason, fields in groups.items() for field in fields.split()}

FIELD_SCOPE = "eawr::sim::tactical::checkpoint_detail"
FIELD_SOURCES = {
    "src/sim/tactical/checkpoint_fields_internal.hpp": FIELD_SCOPE,
    "src/sim/tactical/checkpoint_access_internal.hpp": FIELD_SCOPE,
    "src/sim/tactical/land_checkpoint_fields_internal.hpp": FIELD_SCOPE,
    "src/sim/galactic/checkpoint_fields_internal.hpp": "eawr::sim::galactic::checkpoint_detail",
    "src/sim/galactic/checkpoint_commands_internal.hpp": "eawr::sim::galactic::checkpoint_detail",
    "src/campaign/checkpoint_fields_internal.hpp": "eawr::sim::galactic::checkpoint_detail",
    "src/campaign/checkpoint_owner_internal.hpp": "eawr::sim::galactic::checkpoint_detail",
}
OWNERS = {
    "src/sim/galactic/session.cpp": ("checkpoint_owner", "eawr::sim::galactic::GalacticSession::Impl"),
    "src/campaign/game_session_lifecycle.cpp": ("checkpoint_game_owner", "eawr::campaign::GameSession::Impl"),
}
# These Fields specializations delegate to independently checked replay codecs.
CUSTOM_CODECS = {
    "eawr::sim::tactical::TacticalReplay", "eawr::sim::tactical::TacticalSetup",
    "std::monostate",
}

# State owners can inherit private subsystem stores. Keep their lexical scope
# and resolve inherited nested types just as qualified C++ lookup does.
DECLARATION = re.compile(
    r"\b(struct|class)\s+(\w+(?:::\w+)*)\s*(?:final\s*)?"
    r"(?:\s*:\s*([^{};]+))?\s*\{")


def scopes(source, header):
    """Track lexical namespace/class scopes, including out-of-class definitions."""
    openings = {}
    for match in re.finditer(
        r"\b(?:namespace|struct|class)\s+(\w+(?:::\w+)*)\s*(?:final\s*)?"
        r"(?:\s*:\s*[^{};]+)?\s*\{", source
    ):
        openings[match.end() - 1] = match[1]
    # Anonymous helper declarations in another header must not be mistaken
    # for public state declarations in the surrounding named namespace.
    for match in re.finditer(r"\bnamespace\s*\{", source):
        openings[match.end() - 1] = f"(anonymous in {header})"
    stack = [""]
    positions = []
    for index, char in enumerate(source):
        positions.append(stack[-1])
        if char == "{":
            name = openings.get(index, "")
            stack.append("::".join(filter(None, (stack[-1], name))))
        elif char == "}":
            stack.pop()
    return positions


def resolve(name, scope, types, aliases, seen=None):
    """Use C++ lexical lookup rather than matching a type's final component."""
    seen = set() if seen is None else seen
    prefixes = scope.split("::") if scope else []
    if name.startswith("::"):
        prefixes = []
        name = name[2:]
    while True:
        candidate = "::".join([*prefixes, name])
        if candidate in types:
            return candidate
        # A qualified name can start with an alias, e.g. State::CommandSource.
        components = candidate.split("::")
        for length in range(len(components), 0, -1):
            alias = "::".join(components[:length])
            if alias in aliases and alias not in seen:
                target, declaring_scope = aliases[alias]
                resolved = resolve(target, declaring_scope, types, aliases, seen | {alias})
                if resolved:
                    expanded = "::".join([resolved, *components[length:]])
                    if expanded in types:
                        return expanded
                    if expanded in aliases and expanded not in seen:
                        inherited = resolve("::" + expanded, "", types, aliases, seen | {alias})
                        if inherited:
                            return inherited
        if not prefixes:
            return None
        prefixes.pop()


def inventory(overrides):
    root = Path(__file__).resolve().parents[3]
    types = {}
    # State types may move across simulation headers; discover the whole tree.
    files = sorted((root / "include/eawr/sim").rglob("*.hpp"))
    files += sorted((root / "src/sim").rglob("*.hpp"))
    files += [root / "include/eawr/units/land_companies.hpp"]
    for tree in ("include/eawr/campaign", "src/campaign", "include/eawr/script/authoritative", "include/eawr/script/numeric"):
        files += sorted((root / tree).rglob("*.hpp"))
    files += [root / member for path in OWNERS for member in SOURCE_BUNDLES.get(path, (path,))]
    files = sorted(set(files) | {root / path for path in overrides
        if path.endswith(".hpp") and path.startswith(("include/eawr/", "src/"))})
    aliases = {}
    inheritance = {}
    for file in files:
        path = file.relative_to(root).as_posix()
        source = overrides[path] if path in overrides else file.read_text(encoding="utf-8")
        source = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
        source = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', source)
        lexical = scopes(source, path)
        for match in re.finditer(r"\busing\s+(\w+)\s*=\s*(\w+(?:::\w+)*)\s*;", source):
            scope = lexical[match.start()]
            aliases["::".join(filter(None, (scope, match[1])))] = (match[2], scope)
        for match in DECLARATION.finditer(source):
            kind, local_name, bases = match.groups()
            scope = lexical[match.start()]
            name = "::".join(filter(None, (scope, local_name)))
            if bases:
                inheritance[name] = (scope, [re.sub(r"\b(public|protected|private|virtual)\b", "", base).strip()
                    for base in bases.split(",")])
            if kind == "class" and local_name not in ("GameSession::Impl", "GalacticSession::Impl"):
                name = "::".join(filter(None, (lexical[match.start()], local_name)))
                types.setdefault(name, (None, None))
                continue
            start = end = match.end()
            level = 1
            while level and end < len(source):
                if source[end] == "{":
                    level += 1
                elif source[end] == "}":
                    level -= 1
                end += 1
            body = source[start:end - 1]
            declarations = []
            depth = offset = 0
            for index, char in enumerate(body):
                if char in "{(":
                    depth += 1
                elif char in "})":
                    depth -= 1
                    if char == "}" and depth == 0:
                        prefix = body[offset:index].split("{", 1)[0]
                        if "(" in re.split(r"[={]", prefix)[0] or re.search(r"\b(?:struct|class|enum)\b", prefix):
                            offset = index + 1
                elif char == ";" and depth == 0:
                    declarations.append(body[offset:index].strip())
                    offset = index + 1
            fields, member_types = [], []
            for declaration in declarations:
                declaration = re.sub(r"\[\[.*?\]\]", "", declaration).strip()
                declaration = re.sub(r"^(?:public|private|protected):\s*", "", declaration)
                if not declaration or declaration.startswith((
                    "friend", "using", "static", "template", "struct", "enum", "class", "operator",
                )) or re.search(r"\boperator\b", declaration) or "(" in re.split(r"[={]", declaration)[0]:
                    continue
                # Multiple declarations such as id{}, host{} retain the first type.
                base = re.split(r"[={]", declaration)[0].strip()
                parts, part_start, nesting = [], 0, 0
                for index, char in enumerate(declaration):
                    if char in "<{([":
                        nesting += 1
                    elif char in ">})]":
                        nesting -= 1
                    elif char == "," and nesting == 0:
                        parts.append(declaration[part_start:index].strip())
                        part_start = index + 1
                parts.append(declaration[part_start:].strip())
                for index, part in enumerate(parts):
                    part = re.split(r"[={]", part)[0].strip()
                    member = re.search(r"(\w+)\s*(?:\[[^]]*\])?$", part)
                    if not member:
                        raise ValueError(f"Unrecognized declaration in {file}: {declaration}")
                    fields.append(member[1])
                    member_types.append(part[:member.start()] if index == 0 else base[:base.rfind(" ")])
            name = "::".join(filter(None, (lexical[match.start()], local_name)))
            if name in types:
                raise ValueError(f"Duplicate declaration of {name} in {file}")
            types[name] = (fields, member_types)
        # Classes also establish lookup scopes and can be alias targets, although
        # only struct field lists are audited here.
        for match in DECLARATION.finditer(source):
            if match[1] != "class":
                continue
            name = "::".join(filter(None, (lexical[match.start()], match[2])))
            types.setdefault(name, (None, None))
    # Files are discovered lexically, so all declarations must be collected
    # before exporting base-class nested types into the derived lookup scope.
    for _ in range(len(inheritance)):
        additions = {}
        for owner, (scope, bases) in inheritance.items():
            for base in bases:
                resolved = resolve(base, scope, types, aliases)
                if not resolved:
                    continue
                for nested in types.keys() | aliases.keys():
                    if nested.startswith(resolved + "::"):
                        inherited = owner + nested[len(resolved):]
                        if inherited not in types and inherited not in aliases:
                            additions[inherited] = (nested, "")
        if not additions:
            break
        aliases.update(additions)
    return root, types, aliases


def check(overrides=None):
    overrides = overrides or {}
    root, types, aliases = inventory(overrides)
    registered = {}
    for path, scope in FIELD_SOURCES.items():
        source = overrides.get(path, (root / path).read_text(encoding="utf-8"))
        for match in re.finditer(r"template <> struct Fields<([^>]+)> \{(.*?)\n\};", source, re.S):
            qualified, body = match.groups()
            if qualified in ("detail::CollectionTree", "detail::CollectionTrees"):
                continue
            name = resolve(qualified, scope, types, aliases) or qualified.lstrip(":")
            if name in CUSTOM_CODECS:
                continue
            # An array written slot by slot (LiveUnit::upgrade_bonuses keeps its
            # version-gated seventh slot) is one field; writing the same subscript
            # twice, or a plain field twice, is still a duplicate.
            fields, subscripts = [], set()
            for field, subscript in re.findall(r"\bv\.(\w+)(\[[^\]]*\])?", body):
                key = (field, re.sub(r"\s", "", subscript))
                if not subscript or key in subscripts or field not in {name for name, _ in subscripts}:
                    fields.append(field)
                if subscript:
                    subscripts.add(key)
            # Command keys expose nested members and the payload is referenced in
            # both read/write branches of its existing payload codec.
            registered[name] = list(dict.fromkeys(fields)) if name == "eawr::sim::galactic::GameCommand" else fields
    for path, (function, name) in OWNERS.items():
        source = overrides.get(path, source_text(path))
        match = re.search(r"void " + function + r"\(A& a, T& v\) \{(.*?)\n\}", source, re.S)
        if not match:
            raise ValueError(f"Owner field list {function} not found in {path}")
        # Nested timeline fields have a separate Fields list; audit that owner too.
        registered[name] = re.findall(r"\bv\.(\w+)(?!\w*->)", match[1])
        if "v.timelines->" in match[1]:
            registered[name].append("timelines")
            registered[name + "::TimelinePublication"] = re.findall(r"\bv\.timelines->(\w+)", match[1])
    errors = []
    for name, fields in registered.items():
        if name not in types:
            errors.append(f"{name}: declaration not found")
            continue
        declared = set(types[name][0])
        excluded = EXCLUDED.get(name, {})
        missing = declared - set(fields) - set(excluded)
        extra = set(fields) - declared
        stale = set(excluded) - declared
        duplicate = len(fields) != len(set(fields))
        if missing or extra or stale or duplicate:
            errors.append(f"{name}: missing={sorted(missing)}, unknown={sorted(extra)}, "
                          f"stale exclusions={sorted(stale)}, duplicate={duplicate}")
        for member_name, member_type in zip(*types[name]):
            if member_name in excluded:
                continue
            for token in re.findall(r"\b\w+(?:::\w+)*\b", member_type):
                child = resolve(token, name, types, aliases)
                if child and types[child][0] is not None and child not in registered and child not in CUSTOM_CODECS:
                    errors.append(f"{name}: nested struct {child} has no Fields list")
    return errors, len(registered)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check without modifying wire fields")
    parser.parse_args()
    errors, count = check()
    for error in errors:
        print(error)
    if errors:
        raise SystemExit(1)
    print(f"checkpoint field completeness: {count} structs covered")
