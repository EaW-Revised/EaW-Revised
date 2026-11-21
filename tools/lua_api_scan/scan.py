"""Join the extracted Lua calls of every mod profile with what each host binds (the coverage matrix)."""
from __future__ import annotations

import dataclasses
import json
import re
from collections import defaultdict
from pathlib import Path

from . import bindings as bindings_mod
from . import collect, extract, objects

TOOL_VERSION = 1
SEVERITY_ORDER = {"critical": 4, "high": 3, "medium": 2, "low": 1, "parked": 0}
MAX_SITES = 12


def load_config(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def declared_api(repo: Path) -> tuple[set[str], set[str]]:
    rows = json.loads((repo / "plan/inventories/foc-lua-registrations.json").read_text(encoding="utf-8"))["entries"]
    return ({r["exposed_name"] for r in rows if r["call_kind"] == "global"},
            {r["exposed_name"] for r in rows if r["call_kind"] != "global"})


def _timing(function: str | None, config: dict, reachable: set[str] | None = None) -> str:
    timing = config["timing"]
    if function is None:
        return timing["top_level"]
    if reachable is not None and function in reachable:
        return "start"
    for rule in timing["rules"]:
        if re.search(rule["function"], function):
            return rule["timing"]
    return timing["default"]


_THREAD = re.compile(r"""Create_Thread\s*\(\s*["']([A-Za-z_][\w.]*)["']""")


def start_reachable(members: list[str], parsed: dict, texts: dict[str, str], config: dict) -> set[str]:
    """Function names reachable at start: from the top level of any script of the closure and from the start
    functions, through direct calls and Create_Thread("Name") literals."""
    known = {c.function for m in members for c in parsed[m].calls if c.function}
    edges: dict[str | None, set[str]] = defaultdict(set)
    for m in members:
        threads: dict[int, list[str]] = defaultdict(list)
        for number, line in enumerate(texts[m].splitlines(), 1):
            for match in _THREAD.finditer(line):
                threads[number].append(match.group(1))
        for call in parsed[m].calls:
            if call.symbol in known and call.kind in {"global", "dotted"}:
                edges[call.function].add(call.symbol)
            if call.symbol == "Create_Thread":
                edges[call.function].update(threads.get(call.line, []))
    start = {None} | set(config["timing"].get("start_functions", []))
    seen, todo = set(), list(start)
    while todo:
        node = todo.pop()
        if node in seen:
            continue
        seen.add(node)
        todo.extend(edges.get(node, ()))
    return {n for n in seen if n}


def _classify_script(logical: str, config: dict) -> dict | None:
    for rule in config["script_classes"]:
        if re.search(rule["pattern"], logical):
            return rule
    return None


@dataclasses.dataclass
class Site:
    layer: str
    logical: str
    line: int
    function: str | None
    timing: str
    root: str


def _defined_names(parsed: extract.Parsed, text_tables: set[str]) -> set[str]:
    return set(parsed.helpers) | text_tables


_TABLE = re.compile(r"^([A-Za-z_]\w*)\s*=\s*\{", re.M)
_NS_FUNC = re.compile(r"^\s*function\s+([A-Za-z_]\w*)[.:]", re.M)


def classify_call(call: extract.Call, defined: set[str], declared_globals: set[str], declared_members: set[str],
                  wrappers: set[str]) -> tuple[str, str, str | None]:
    """(class, kind, name): class in std | script | engine | unresolved; kind global | member."""
    symbol, kind = call.symbol, call.kind
    if extract.is_standard(symbol, kind):
        return "std", "global", symbol
    if kind == "global":
        if symbol in defined:
            return "script", "global", symbol
        if symbol in declared_globals:
            return "engine", "global", symbol
        return "unresolved", "global", symbol
    if kind == "method":
        return ("engine" if symbol in declared_members else "unresolved"), "member", symbol
    first, _, rest = symbol.partition(".")
    last = symbol.rsplit(".", 1)[-1]
    if first in defined:
        return "script", "global", symbol
    if first in wrappers:
        return ("engine" if last in declared_members else "unresolved"), "member", last
    if first in declared_globals:
        return "engine", "global", f"{first}.{rest.split('.')[0]}"
    return ("engine" if last in declared_members else "unresolved"), "member", last


def run_scan(repo: Path, config: dict, roots: dict[str, Path], profile_ids: list[str] | None = None,
             progress=None) -> dict:
    declared_globals, declared_members = declared_api(repo)
    wrappers = set(config["wrapper_variables"])
    ground_names = set(config["ground_api_names"])
    hosts_cfg = {h["id"]: h for h in config["hosts"]}
    host_bind = bindings_mod.read_all(repo, config["hosts"], config.get("follow"))
    rows_cfg = collect.load_profiles(repo, config)
    if profile_ids:
        rows_cfg = [r for r in rows_cfg if r["id"] in profile_ids]
    cache = collect.LayerCache(roots)
    parse_cache: dict[str, extract.Parsed] = {}
    text_cache: dict[str, set[str]] = {}

    def parsed_of(sf: collect.ScriptFile) -> extract.Parsed:
        key = str(sf.path)
        if key not in parse_cache:
            data = sf.read()
            parse_cache[key] = extract.parse_script(data)
            text = extract._decode(data)
            text_cache[key] = set(_TABLE.findall(text)) | set(_NS_FUNC.findall(text))
        return parse_cache[key]

    profiles_out = []
    # (profile, host, kind, name) -> {"sites": {...}, "roots": set, ...}
    demand: dict[tuple, dict] = {}
    unresolved: dict[tuple, dict] = {}
    for row in rows_cfg:
        profile = collect.build_profile(row, cache)
        if not profile.scripts:
            profiles_out.append({"id": row["id"], "title": row["title"], "scripts": 0, "available": False,
                                 "missing_layers": profile.missing_layers})
            continue
        if progress:
            progress(f"{row['id']}: {len(profile.scripts)} scripts")
        files = profile.scripts
        parsed = {logical: parsed_of(sf) for logical, sf in files.items()}
        by_stem: dict[str, list[str]] = defaultdict(list)
        for logical in files:
            by_stem[Path(logical).stem].append(logical)
        xml_refs = objects.references(objects.xml_roots_of(roots, [la for la in profile.layers_leaf_first] + ["stock"]),
                                      config["object_modes"])
        xml_dirs = objects.xml_roots_of(roots, [la for la in profile.layers_leaf_first] + ["stock"])
        gc_story = objects.galactic_story_scripts(xml_dirs)
        roots_by_host: dict[str, list[tuple[str, str]]] = defaultdict(list)
        unreferenced_objects = 0
        unreferenced_story = 0
        for logical in sorted(files):
            rule = _classify_script(logical, config)
            if rule is None or rule["host"] == "library":
                continue
            targets = [rule["host"]] + list(rule.get("also", []))
            if rule.get("needs") == "gc_story":
                if Path(logical).stem not in gc_story:
                    unreferenced_story += 1
                    targets = list(rule.get("also", []))
            if rule["host"] == "object":
                ref = xml_refs.get(Path(logical).stem)
                if ref is None:
                    unreferenced_objects += 1
                    continue
                # Upper bound: the runtime attaches only the types that exist as galactic fleets or planets.
                targets = ["galactic_object", "tactical_object"]
            for host in targets:
                roots_by_host[host].append((logical, rule["label"]))
        parse_errors = sorted(p for p, v in parsed.items() if v.parse_error)
        ground_hosts = {h["id"] for h in config["hosts"] if h.get("mode") == "ground"}
        for host, entries in roots_by_host.items():
            for logical, label in entries:
                ground_script = host in ground_hosts
                members = extract.closure(logical, parsed, by_stem, [
                    "data/scripts/library/", "data/scripts/" + ("ai/" if "galactic_ai" in host or "plan" in host else "gameobject/"),
                    "data/scripts/"])
                defined: set[str] = set()
                for m in members:
                    defined |= _defined_names(parsed[m], text_cache[str(files[m].path)])
                reachable = start_reachable(members, parsed, {m: extract._decode(files[m].read()) for m in members}, config)
                for m in members:
                    for call in parsed[m].calls:
                        klass, kind, name = classify_call(call, defined, declared_globals, declared_members, wrappers)
                        if klass in {"std", "script"} or name is None:
                            continue
                        timing = _timing(call.function, config, reachable)
                        site = Site(files[m].layer, m, call.line, call.function, timing, logical)
                        target = demand if klass == "engine" else unresolved
                        key = (row["id"], host, kind, name)
                        slot = target.setdefault(key, {"sites": {}, "roots": set(), "ground_script": False})
                        slot["sites"].setdefault((m, call.line), site)
                        slot["roots"].add(logical)
                        if m == logical:
                            slot.setdefault("direct", set()).add(logical)
                        slot["ground_script"] = slot["ground_script"] or ground_script
        profiles_out.append({
            "id": row["id"], "title": collect.layer_title(roots.get(row["layers_leaf_first"][0])) or row["title"],
            "configured_title": row["title"], "owner_stack": row["owner_stack"], "available": True,
            "scripts": len(files), "script_roots": {h: len(v) for h, v in sorted(roots_by_host.items())},
            "unreferenced_object_scripts": unreferenced_objects, "story_scripts_not_in_a_campaign_plot": unreferenced_story, "parse_errors": len(parse_errors),
            "approximate_parses": parse_errors[:20], "missing_layers": profile.missing_layers,
            "layers_leaf_first": row["layers_leaf_first"]})
    return build_report(config, hosts_cfg, host_bind, profiles_out, demand, unresolved, ground_names)


def _severity(sites: list[Site], ground: bool, config: dict) -> tuple[str, str]:
    if ground:
        return "parked", "ground-battle only"
    timings = sorted({s.timing for s in sites}, key=lambda t: -SEVERITY_ORDER[config["severity"][t]])
    best = timings[0]
    return config["severity"][best], best


def build_report(config, hosts_cfg, host_bind, profiles_out, demand, unresolved, ground_names) -> dict:
    rows: dict[tuple, dict] = {}
    pending_demand: dict[tuple, dict] = {}
    forbidden = set(config.get("intentionally_unbound", []))
    for (profile, host, kind, name), slot in demand.items():
        hb = host_bind.get(host)
        host_cfg = hosts_cfg.get(host, {})
        if hb is None:
            # a host whose reader belongs to another lane: record the demand only
            pending_demand[(host, kind, name)] = pending_demand.get((host, kind, name), {"profiles": {}})
            pending_demand[(host, kind, name)]["profiles"][profile] = len(slot["sites"])
            continue
        if name in forbidden:
            continue  # deliberately unbound (sandbox rule), not a gap
        bound = hb.knows_global(name) if kind == "global" else hb.knows_member(name)
        stub = kind == "global" and name in hb.stubs
        if bound and not stub:
            continue
        sites = list(slot["sites"].values())
        ground = name in ground_names or slot["ground_script"]
        severity, timing = _severity(sites, ground, config)
        row = rows.setdefault((host, kind, name), {
            "host": host, "lane": host_cfg.get("lane"), "name": name, "kind": kind,
            "status": "stub-raises-unbound" if stub else "not-registered",
            "ground_only": ground, "per_profile": {}, "severity": severity, "timing": timing})
        if SEVERITY_ORDER[severity] > SEVERITY_ORDER[row["severity"]]:
            row["severity"], row["timing"] = severity, timing
        row["ground_only"] = row["ground_only"] and ground
        stock_sites = sum(1 for s in sites if s.layer == "stock")
        cell_severity, cell_timing = _severity(sites, ground, config)
        row["per_profile"][profile] = {
            "severity": cell_severity,
            "call_sites": len(sites), "scripts_blocked": len(slot["roots"]), "scripts_blocked_directly": len(slot.get("direct", ())), "stock_sites": stock_sites,
            "mod_sites": len(sites) - stock_sites, "timing": cell_timing,
            "example_sites": [f"{s.layer}:{s.logical}:{s.line}" + (f" in {s.function}" if s.function else " top-level")
                              for s in sorted(sites, key=lambda s: (SEVERITY_ORDER[config["severity"][s.timing]] * -1, s.logical, s.line))[:MAX_SITES]],
            "example_roots": sorted(slot["roots"])[:MAX_SITES]}
    result_rows = []
    for row in rows.values():
        mods = sorted(p for p in row["per_profile"] if p != "stock")
        row["mods_blocked"] = mods
        row["mods_blocked_count"] = len(mods)
        row["total_call_sites"] = sum(v["call_sites"] for v in row["per_profile"].values())
        row["total_scripts_blocked"] = sum(v["scripts_blocked"] for v in row["per_profile"].values())
        row["total_scripts_directly"] = sum(v["scripts_blocked_directly"] for v in row["per_profile"].values())
        result_rows.append(row)
    result_rows.sort(key=lambda r: (r["ground_only"], -SEVERITY_ORDER[r["severity"]], -r["mods_blocked_count"],
                                    -r["total_scripts_directly"], -r["total_call_sites"], r["host"], r["name"]))
    unresolved_rows = defaultdict(lambda: {"per_profile": {}})
    for (profile, host, kind, name), slot in unresolved.items():
        row = unresolved_rows[(host, kind, name)]
        row["host"], row["kind"], row["name"] = host, kind, name
        row["per_profile"][profile] = len(slot["sites"])
    unresolved_list = sorted(unresolved_rows.values(), key=lambda r: (-sum(r["per_profile"].values()), r["host"], r["name"]))
    host_summary = {}
    for host_id, hb in host_bind.items():
        host_summary[host_id] = {"bound_globals": len(hb.globals), "bound_members": len(hb.members),
                                 "stubbed_names": len(hb.stubs), "infrastructure": len(hb.infrastructure),
                                 "problems": hb.problems, "title": hosts_cfg[host_id].get("title")}
    pending = [{"host": h, "kind": k, "name": n, "per_profile": v["profiles"]}
               for (h, k, n), v in sorted(pending_demand.items(), key=lambda it: (it[0][0], -sum(it[1]["profiles"].values()), it[0][2]))]
    report = {"schema_version": 1, "tool_version": TOOL_VERSION, "profiles": profiles_out, "hosts": host_summary,
              "rows": result_rows, "unresolved": unresolved_list, "demand_for_unread_hosts": pending}
    report["runtime_crosscheck"] = crosscheck(config, report, host_bind)
    return report


def crosscheck(config: dict, report: dict, host_bind: dict) -> list[dict]:
    out = []
    for entry in config["runtime_unbound"]["entries"]:
        name, kind = entry["name"], entry["kind"]
        hits = [r for r in report["rows"] if r["name"] == name and r["kind"] == kind]
        row = {"name": name, "kind": kind, "host_hint": entry.get("host_hint")}
        if hits:
            row["static"] = "found"
            row["hosts"] = sorted({r["host"] for r in hits})
            row["hint_host_flagged"] = any(r["host"] == entry.get("host_hint") for r in hits)
            if entry.get("host_hint") and not row["hint_host_flagged"]:
                hb = host_bind.get(entry["host_hint"])
                row["note"] = "flagged in other hosts only; the hint host binds it" if hb else "hint host has no reader"
        elif entry.get("explain"):
            row["static"] = "explained"
        else:
            row["static"] = "MISSING"
        if entry.get("explain"):
            row["explain"] = entry["explain"]
        out.append(row)
    return out
