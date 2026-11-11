#!/usr/bin/env python3
"""Asset-free coverage obligations and explicit, offline corpus refresh.

CI reads the committed manifest; refresh/check-refresh alone open installed data.
Roots come from a local JSON mapping of declared layer IDs to directories, never
from drive discovery. Recovered XML is an authoring candidate, not acceptance evidence.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import xml.parsers.expat

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.inventory import corpus, tag_registry as tr
from tools.inventory.common import ascii_fold
from tools.inventory.engine_tags import EngineTags

DIRECTORY = tr.ROOT / "docs/tag-coverage"
MANIFEST = DIRECTORY / "coverage-universe.json"
PROFILES = DIRECTORY / "coverage-profiles.json"
OWNERS = DIRECTORY / "coverage-parser-owners.json"
HEX = re.compile(r"[0-9a-f]{64}\Z")


class CoverageError(ValueError):
    pass


def canonical(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def fingerprint(value: object) -> str:
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def read(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema_version") != 1:
        raise CoverageError(f"{path.name}: unsupported coverage schema")
    return value


def write(path: Path, value: dict) -> None:
    # One row per line keeps generated changes and additive rebases reviewable.
    if "engine_obligations" in value:
        lines = ["{"]
        for index, (name, item) in enumerate(value.items()):
            comma = "," if index + 1 < len(value) else ""
            if isinstance(item, list):
                lines.append(f"  {json.dumps(name)}: [")
                lines.extend("    " + json.dumps(row, ensure_ascii=False, sort_keys=True)
                             + ("," if i + 1 < len(item) else "") for i, row in enumerate(item))
                lines.append("  ]" + comma)
            else:
                lines.append(f"  {json.dumps(name)}: {json.dumps(item, ensure_ascii=False)}{comma}")
        text = "\n".join(lines + ["}", ""])
    else:
        text = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    path.write_bytes(text.encode())


def declared_profiles(value: dict) -> dict[str, dict]:
    profiles = {}
    for profile in value["profiles"]:
        identity = profile["id"]
        layers = profile["layers_leaf_first"]
        if (not isinstance(identity, str) or not identity or identity in profiles
                or not isinstance(layers, list) or not layers
                or any(not isinstance(layer, str) or not layer for layer in layers)
                or len(layers) != len(set(layers))):
            raise CoverageError("coverage profiles need unique IDs and nonempty ordered layer IDs")
        profiles[identity] = profile
    if "stock" not in profiles or profiles["stock"]["layers_leaf_first"] != ["stock"]:
        raise CoverageError("coverage profiles must include the stock layer")
    return profiles


def project_engine(authored: list[dict], catalog: dict, owners: dict) -> tuple[list[dict], list[dict]]:
    """Project only observed canonical owner/leaf contexts; keep unauthored owners separate."""
    index = defaultdict(set)
    labels = {ascii_fold(name): name for name in owners["owners"].values()}
    for row in authored:
        key = tr.pair_key(row["class"], row["tag"])
        leaf = key[1].rsplit("/", 1)[-1]
        for owner in row["canonical_types"]:
            index[(ascii_fold(owner), leaf)].add(key)
        for segment in row["tag"].split("/")[:-1]:
            name = ascii_fold(segment.replace("_", "").replace("-", ""))
            if name.endswith("ability") and name in labels:
                index[(name, leaf)].add(key)
    obligations, placeholders = [], {}
    for entry in catalog["rows"]:
        owner = owners["owners"].get(entry["table_kind"])
        if not owner:
            raise CoverageError(f"engine owner mapping missing: {entry['table_kind']}/{entry['tag']}")
        keys = sorted(index.get((ascii_fold(owner), ascii_fold(entry["tag"])), ()))
        if not keys:
            key = tr.pair_key(owner, entry["tag"])
            keys = [key]
            placeholders[key] = {"class": owner, "tag": entry["tag"], "canonical_types": [owner],
                                 "sources": [], "parser_owner_only": True}
        obligations.append({"table_kind": entry["table_kind"], "tag": entry["tag"],
                            "value_type": entry["value_type"], "pairs": [list(k) for k in keys]})
    obligations.sort(key=lambda r: (ascii_fold(r["tag"]), r["table_kind"], r["value_type"]))
    return obligations, [placeholders[k] for k in sorted(placeholders)]


def load(manifest_path: Path = MANIFEST, profiles_path: Path = PROFILES,
         owners_path: Path = OWNERS, catalog_path: Path | None = None) -> tuple[dict, dict]:
    """Validate independent declarations, typed obligations and exact source membership."""
    manifest, declarations, owners = read(manifest_path), read(profiles_path), read(owners_path)
    catalog = EngineTags.load(catalog_path or DIRECTORY / "engine-tags.json").value
    declared = declared_profiles(declarations)
    if manifest["profiles_sha256"] != fingerprint(declarations):
        raise CoverageError("coverage profile declarations/chain precedence changed; refresh required")
    if manifest["owners_sha256"] != fingerprint(owners):
        raise CoverageError("coverage parser-owner mappings changed; refresh required")
    profiles = {p["id"]: p for p in manifest["profiles"]}
    if len(profiles) != len(manifest["profiles"]) or profiles.keys() != declared.keys():
        raise CoverageError("coverage manifest lacks a declared target profile")
    for identity, profile in profiles.items():
        if profile["layers_leaf_first"] != declared[identity]["layers_leaf_first"]:
            raise CoverageError(f"{identity}: wrong coverage chain precedence")
        for field in ("content_sha256", "manifest_sha256"):
            if not isinstance(profile.get(field), str) or not HEX.fullmatch(profile[field]):
                raise CoverageError(f"{identity}: missing corpus {field}")
        if not isinstance(profile.get("xml_files"), int) or profile["xml_files"] <= 0:
            raise CoverageError(f"{identity}: unavailable/empty XML corpus")
    pairs, authored = {}, []
    counts = Counter()
    for row in manifest["pairs"]:
        key = tr.pair_key(row["class"], row["tag"])
        if key in pairs or not all(key) or tr.normalise_path(row["class"] + "/" + row["tag"]) != row["class"] + "/" + row["tag"]:
            raise CoverageError(f"coverage pair duplicate or not normalized: {key}")
        if (not isinstance(row["canonical_types"], list) or not row["canonical_types"]
                or any(not isinstance(t, str) or not t for t in row["canonical_types"])):
            raise CoverageError(f"{key}: canonical parser types missing")
        sources = row["sources"]
        if (not isinstance(sources, list) or len({s["profile"] for s in sources}) != len(sources)
                or bool(sources) == bool(row.get("parser_owner_only"))):
            raise CoverageError(f"{key}: authored and parser-owner obligations must be separate")
        for source in sources:
            if source["profile"] not in profiles:
                raise CoverageError(f"{key}: undeclared source profile {source['profile']}")
            if (not isinstance(source.get("recovered"), bool) or source.get("occurrences", 0) <= 0):
                raise CoverageError(f"{key}: source needs occurrence/recovery provenance")
            counts[source["profile"]] += 1
        pairs[key] = row
        if sources:
            authored.append(row)
    for identity, profile in profiles.items():
        if counts[identity] != profile["pairs"]:
            raise CoverageError(f"{identity}: coverage pair count does not match source manifest")
    expected, placeholders = project_engine(authored, catalog, owners)
    actual = manifest["engine_obligations"]
    if actual != expected:
        old = {canonical(e) for e in actual}
        new = next((e for e in expected if canonical(e) not in old), None)
        label = f"{new['table_kind']}/{new['tag']} ({new['value_type']})" if new else "removed/changed catalog entry"
        raise CoverageError(f"engine obligation has no current coverage: {label}; refresh and add rows")
    for row in placeholders:
        if tr.pair_key(row["class"], row["tag"]) not in pairs:
            raise CoverageError(f"engine obligation missing: {row['class']}/{row['tag']}")
    placeholder_keys = {tr.pair_key(r["class"], r["tag"]) for r in placeholders}
    for key, row in pairs.items():
        if row.get("parser_owner_only") and key not in placeholder_keys:
            raise CoverageError(f"stale parser-owner obligation: {key}")
    return manifest, pairs


def safe_root(raw: str, identity: str) -> Path:
    root = Path(raw).resolve()
    if root == Path(root.anchor) or len(root.parts) < 3:
        raise CoverageError(f"{identity}: refuse drive/root-level corpus scan")
    if not root.is_dir():
        raise CoverageError(f"{identity}: corpus unavailable")
    return root


def mount(root: Path, identity: str, stock: bool = False) -> tuple[dict, dict]:
    """Read XML winners only: loose, then reversed active archive order."""
    winners = {}
    data_root = root if stock else root / "Data"
    if not data_root.is_dir() or not data_root.resolve().is_relative_to(root):
        raise CoverageError(f"{identity}: Data directory unavailable")

    def unavailable(error):
        raise CoverageError(f"{identity}: corpus directory unavailable") from error

    for directory, subdirs, filenames in os.walk(data_root, topdown=True, followlinks=False, onerror=unavailable):
        # Inspect directory links before os.walk can descend into one.
        for name in subdirs:
            path = Path(directory) / name
            if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
                raise CoverageError(f"{identity}: corpus links are not permitted")
        subdirs.sort()
        for name in sorted(filenames):
            path = Path(directory) / name
            if path.suffix.lower() != ".xml":
                continue
            if path.is_symlink():
                raise CoverageError(f"{identity}: corpus links are not permitted")
            if not path.resolve().is_relative_to(root):
                raise CoverageError(f"{identity}: XML escapes declared root")
            if path.stat().st_size > corpus.MAX_MEMBER_BYTES:
                raise CoverageError(f"{identity}: XML exceeds 64 MiB")
            logical = path.relative_to(data_root).as_posix().lower()
            if not stock:
                logical = "data/" + logical
            winners[logical] = (path.read_bytes(), identity + ":loose")
    archives, missing = ([], []) if stock else corpus._parse_manifest(corpus.Layer(identity, data_root))
    archive_names = []
    for path, relative, prefix in reversed(archives):
        if not path.resolve().is_relative_to(root):
            raise CoverageError(f"{identity}: archive escapes declared root")
        # Index bounds stay checked; the larger limit is for unrequested assets.
        members, _ = corpus._read_meg(path, prefix, hash_archive=False, member_limit=2**32 - 1)
        archive_names.append(relative)
        with path.open("rb") as stream:
            for member in members:
                if not member.logical_path.endswith(".xml") or member.logical_path in winners:
                    continue
                if member.size > corpus.MAX_MEMBER_BYTES:
                    raise CoverageError(f"{identity}: archive XML exceeds 64 MiB")
                stream.seek(member.offset)
                winners[member.logical_path] = (stream.read(member.size), identity + ":" + relative)
    if not winners:
        raise CoverageError(f"{identity}: unavailable/empty XML corpus")
    # Pin declarations (including unavailable archives) without publishing assets.
    manifest_path = corpus._ci_child(data_root, "MegaFiles.xml")
    return winners, {"active_archives_reversed": archive_names, "missing_archives": missing,
                     "declaration_sha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest()
                     if manifest_path is not None else None}


def scan_xml(data: bytes, logical: str, schema, hints: dict, concrete_types: dict) -> tuple[list, bool]:
    stack, contexts, events = [], [], []
    forced = hints.get(logical, ())
    meta = schema.meta(logical, "foc")

    def start(name, attrs):
        depth = len(stack)
        stack.append(name)
        named = any(ascii_fold(a) in ("name", "id") for a in attrs)
        canonical_type = schema.types.get(ascii_fold(name)) if depth == 0 or named else None
        if named:
            canonical_type = canonical_type or concrete_types.get(ascii_fold(name))
        if canonical_type:
            typ, base = canonical_type, depth
        elif len(forced) == 1 and (depth == 0 or depth == 1 and not contexts[0]["definition"]):
            typ, base = forced[0], depth
        elif depth == 0:
            singleton = [m for m in meta if m["meta_file_type"] == "singleton" and len(m["types"]) == 1]
            typ, base = (singleton[0]["types"][0], depth) if singleton else (name, depth)
        elif depth == 1:
            direct = [m for m in meta if m["meta_file_type"] == "directContent" and len(m["types"]) == 1]
            typ, base = (direct[0]["types"][0], depth) if direct else (contexts[-1]["typ"], contexts[-1]["base"])
        else:
            typ, base = contexts[-1]["typ"], contexts[-1]["base"]
        if contexts:
            contexts[-1]["child"] = True
        path = "/".join(stack[base:])
        contexts.append({"typ": typ, "base": base, "path": path, "child": False,
                         "definition": named and depth == base})
        for attr in attrs:
            events.append((path + "/@" + attr, typ))

    def end(_name):
        if contexts:
            row = contexts.pop()
            if not row["child"] and "/" in row["path"]:
                events.append((row["path"], row["typ"]))
            stack.pop()

    parser = xml.parsers.expat.ParserCreate()
    parser.StartDoctypeDeclHandler = lambda *args: (_ for _ in ()).throw(ValueError("DOCTYPE refused"))
    parser.ExternalEntityRefHandler = lambda *args: 0
    parser.StartElementHandler, parser.EndElementHandler = start, end
    recovered = False
    try:
        parser.Parse(data, True)
    except (xml.parsers.expat.ExpatError, ValueError):
        recovered = True
        stack.clear()
        contexts.clear()
        events.clear()
        text = data.decode("utf-8-sig", errors="replace")
        text = re.sub(r"<!--.*?-->|<!\[CDATA\[.*?\]\]>|<\?.*?\?>", "", text, flags=re.S)
        for match in re.finditer(r"<(/?)([A-Za-z_][\w:.-]*)([^<>]*?)(/?)>", text):
            closing, name, body, self_closing = match.groups()
            if closing:
                if name in stack:
                    while stack[-1] != name:
                        end(stack[-1])
                    end(name)
            else:
                attrs = {m[0]: m[2] for m in re.findall(r"([\w:.-]+)\s*=\s*(['\"])(.*?)\2", body, re.S)}
                start(name, attrs)
                if self_closing:
                    end(name)
        while stack:
            end(stack[-1])
    return events, recovered


def refresh(declarations: dict, owners: dict, roots: dict, schema_value: dict,
            inventory: dict, catalog: dict, prior: dict | None = None) -> dict:
    from tools.inventory.xml_inventory import SchemaLookup, registry_type_hints
    declared = declared_profiles(declarations)
    schema = SchemaLookup(schema_value)
    concrete = {}
    for row in tr.load_universe(inventory).values():
        if len(row.object_types) == 1:
            typ = next(iter(row.object_types))
            if ascii_fold(typ) in schema.types:
                concrete[ascii_fold(row.object_class)] = schema.types[ascii_fold(typ)]
    cache = {}
    for profile in declared.values():
        for identity in profile["layers_leaf_first"]:
            if identity not in cache:
                if identity not in roots:
                    raise CoverageError(f"{identity}: corpus root unavailable")
                cache[identity] = mount(safe_root(roots[identity], identity), identity, identity == "stock")
    base = cache["stock"][0]
    pairs, profiles = {}, []
    previous = {tr.pair_key(r["class"], r["tag"]): r for r in (prior or {}).get("pairs", [])}
    for identity, declaration in declared.items():
        selected = {}
        for layer in declaration["layers_leaf_first"]:
            for name, value in cache[layer][0].items():
                selected.setdefault(name, value)
        effective = {**base, **selected}
        sources = [corpus.Source(name, origin, "loose", identity, data, hashlib.sha256(data).hexdigest())
                   for name, (data, origin) in sorted(effective.items())]
        # Recovering a malformed registry must not erase valid file-type hints.
        try:
            hints = registry_type_hints(sources, "foc", schema)
        except Exception:
            hints = {}
            for source in sources:
                try:
                    hints.update(registry_type_hints([source], "foc", schema))
                except (ValueError, xml.parsers.expat.ExpatError):
                    pass
        profile_pairs = {}
        recoveries = 0
        for logical, (data, _origin) in sorted(selected.items()):
            events, recovered = scan_xml(data, logical, schema, hints, concrete)
            recoveries += int(recovered)
            for path, typ in events:
                cls, tail = tr.split_key(tr.normalise_path(path))
                if not tail:
                    continue
                key = tr.pair_key(cls, tail)
                row = profile_pairs.setdefault(key, {"class": cls, "tag": tail, "types": set(),
                                                       "occurrences": 0, "recovered": False})
                row["types"].add(typ)
                row["occurrences"] += 1
                row["recovered"] |= recovered
        containers = set()
        for cls, tag in profile_pairs:
            if "@" not in tag:
                parts = (cls + "/" + tag).split("/")
                containers.update("/".join(parts[:i]) for i in range(1, len(parts)))
        profile_pairs = {k: r for k, r in profile_pairs.items()
                         if "@" in k[1] or "/".join(k) not in containers}
        for key, source in profile_pairs.items():
            row = pairs.setdefault(key, {"class": source["class"], "tag": source["tag"],
                                          "canonical_types": set(), "sources": []})
            row["canonical_types"].update(source["types"])
            row["sources"].append({"profile": identity, "occurrences": source["occurrences"],
                                    "recovered": source["recovered"]})
        profiles.append({"id": identity, "layers_leaf_first": declaration["layers_leaf_first"],
                         "xml_files": len(selected), "recovered_files": recoveries, "pairs": len(profile_pairs),
                         "content_sha256": fingerprint([[name, origin, hashlib.sha256(data).hexdigest()]
                                                         for name, (data, origin) in sorted(selected.items())]),
                         "manifest_sha256": fingerprint([[layer, cache[layer][1]]
                                                          for layer in declaration["layers_leaf_first"]])})
    for key, row in pairs.items():
        row["canonical_types"] = sorted(row["canonical_types"])
        row["sources"].sort(key=lambda s: s["profile"])
        for field, default in (("area", "data"), ("owning_lane", "MM")):
            row[field] = previous.get(key, {}).get(field, default)
    obligations, placeholders = project_engine(list(pairs.values()), catalog, owners)
    for row in placeholders:
        key = tr.pair_key(row["class"], row["tag"])
        row["area"] = previous.get(key, {}).get("area", "data")
        row["owning_lane"] = previous.get(key, {}).get("owning_lane", "MM")
        pairs.setdefault(key, row)
    return {"schema_version": 1, "profiles_sha256": fingerprint(declarations),
            "owners_sha256": fingerprint(owners), "schema_sha256": fingerprint(schema_value),
            "profiles": profiles, "pairs": [pairs[k] for k in sorted(pairs)],
            "engine_obligations": obligations}


def seed_rows(manifest: dict, statuses: Path) -> dict:
    """Explicit opt-in: only missing pairs become todo, never inferred applications."""
    registry = tr.load_registry(statuses)
    seen = {tr.pair_key(c, r["tag"]) for r in registry["rows"] for c in r["classes"]}
    area, lanes = Counter(), Counter()
    for row in manifest["pairs"]:
        key = tr.pair_key(row["class"], row["tag"])
        if key in seen:
            continue
        registry["rows"].append({"classes": [row["class"]], "tag": row["tag"],
                                  "status": "todo", "area": row["area"], "basis": "auto", "ticket": 6215,
                                  "note": "Coverage backlog; authoring/recognition is not application evidence. "
                                  + "Owning lane: " + row["owning_lane"] + "."})
        area[row["area"]] += 1
        lanes[row["owning_lane"]] += 1
        seen.add(key)
    registry["rows"].sort(key=tr.row_order)
    tr.write_registry(statuses, registry)
    return {"generated_pairs": sum(area.values()), "by_area": dict(sorted(area.items())),
            "by_owning_lane": dict(sorted(lanes.items()))}


def check_refresh(expected: dict, actual: dict) -> None:
    if actual != expected:
        raise CoverageError("installed corpus/profile/fingerprint differs from coverage manifest; refresh required")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("refresh", "check-refresh", "seed"))
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--profiles", type=Path, default=PROFILES)
    parser.add_argument("--owners", type=Path, default=OWNERS)
    parser.add_argument("--roots", type=Path, help="local JSON: declared layer ID -> installed directory")
    parser.add_argument("--schema-root", type=Path)
    parser.add_argument("--statuses", type=Path, default=tr.DEFAULT_STATUSES)
    args = parser.parse_args(argv)
    try:
        if args.command == "seed":
            manifest, _ = load(args.manifest, args.profiles, args.owners)
            print(json.dumps(seed_rows(manifest, args.statuses), sort_keys=True))
            return 0
        if args.roots is None or args.schema_root is None:
            parser.error("offline refresh requires --roots and --schema-root")
        from tools.inventory.schema_index import build_schema_index
        prior = read(args.manifest) if args.manifest.exists() else None
        value = refresh(read(args.profiles), read(args.owners), json.loads(args.roots.read_text(encoding="utf-8")),
                        build_schema_index(safe_root(str(args.schema_root), "schema")),
                        tr.load_inventory(tr.DEFAULT_INVENTORY), EngineTags.load().value, prior)
        if args.command == "check-refresh":
            check_refresh(prior, value)
            print("coverage_universe: exact pairs, profiles, chain precedence and fingerprints match")
        else:
            write(args.manifest, value)
            print(f"coverage_universe: wrote {len(value['pairs'])} pairs; add explicit coverage rows before check")
        return 0
    except (OSError, ValueError, KeyError, tr.RegistryError, corpus.CorpusError) as exc:
        print(f"coverage_universe: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
