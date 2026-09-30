#!/usr/bin/env python3
"""Private-corpus acceptance checks with independently hand-labelled samples.

The script records only metadata and expected locations; it never writes source bytes.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))
from corpus import Corpus  # noqa: E402
from common import canonical_bytes, manifest_id  # noqa: E402
from lua_calls import PGLUA_HEADER, analyze_pglua, parse_source_calls  # noqa: E402
from schema_index import build_schema_index  # noqa: E402
from xml_inventory import SchemaLookup, parse_xml, registry_type_hints  # noqa: E402


def load(path: Path) -> dict: return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--mod-root", required=True, action="append", type=Path,
                        help="MODPATH root; repeat in leaf-first dependency order")
    parser.add_argument("--schema-root", required=True, type=Path)
    parser.add_argument("--inventory-dir", default=ROOT / "plan" / "inventories", type=Path); args = parser.parse_args()
    lua = load(args.inventory_dir / "lua-manifest.json"); xml = load(args.inventory_dir / "xml-manifest.json")
    lua_api = load(args.inventory_dir / "lua-api.json"); xml_tags = load(args.inventory_dir / "xml-tags.json")
    assert set(lua_api) == {"schema_version", "manifest_id", "rows"} and lua_api["schema_version"] == 2
    assert set(xml_tags) == {"schema_version", "manifest_id", "schema_revision", "rows"} and xml_tags["schema_version"] == 1
    lua_row_keys = {"profile", "symbol", "call_kind", "receiver_type", "classification",
                    "call_count", "example", "reference_status", "reference_locations"}
    assert all(set(row) in ((lua_row_keys,) if row["profile"] == "eaw" else
                            (lua_row_keys, lua_row_keys | {"receiver_types"}))
               and ("receiver_types" not in row or
                    row["receiver_types"] == sorted(set(row["receiver_types"])))
               and set(row["example"]) == {"source_id", "logical_path", "source_kind", "line", "column",
                                            "prototype_id", "pc"} for row in lua_api["rows"])
    assert all(set(row) == {"profile", "object_type", "tag_path", "node_kind", "tag_name", "usage_count",
                            "file_count", "status", "schema_ref", "example"}
               and set(row["example"]) == {"source_id", "logical_path", "line", "column"}
               for row in xml_tags["rows"])
    assert manifest_id(lua) == lua["manifest_id"] and manifest_id(xml) == xml["manifest_id"]
    for name in ("lua-api.json", "lua-unresolved.json", "lua-manifest.json",
                 "xml-tags.json", "xml-unresolved.json", "xml-manifest.json"):
        path = args.inventory_dir / name
        assert path.read_bytes() == canonical_bytes(load(path))
    lua_expected = {"eaw": (520, 260), "foc": (1168, 370), "remake": (2120, 709)}
    xml_expected = {"eaw": (375, 367, 10), "foc": (1036, 639, 4), "remake": (2473, 1740, 18)}
    for profile in lua["profiles"]:
        assert (profile["raw_file_count"], profile["effective_file_count"]) == lua_expected[profile["profile"]]
        assert profile["raw_parse_failures"] == profile["parse_failures"] == 0
    for profile in xml["profiles"]:
        expected = xml_expected[profile["profile"]]
        assert (profile["raw_file_count"], profile["effective_file_count"], profile["parse_failures"]) == expected
    remake_lua = next(p for p in lua["profiles"] if p["profile"] == "remake")
    remake_xml = next(p for p in xml["profiles"] if p["profile"] == "remake")
    assert sum(r["layer_id"] == "mod" and r["origin"] == "loose" for r in remake_lua["raw_file_outcomes"]) == 436
    assert sum(r["layer_id"] == "mod" and r["origin"] == "loose" for r in remake_xml["raw_file_outcomes"]) == 1311
    unsupported = Counter(i["opcode"] for r in remake_lua["raw_file_outcomes"] for i in r.get("unsupported_instructions", []))
    assert unsupported == {"FORLOOP": 3, "SETLISTO": 11}

    corpus = Corpus(args.game_root, args.mod_root)
    eaw_raw = list(corpus.iter_sources("eaw", ".lua", "raw"))
    compiled = next(s for s in eaw_raw if s.logical_path == "data/scripts/story/story_empire_activ_m11_space.lua"
                    and s.data.startswith(PGLUA_HEADER))
    calls, _ = analyze_pglua(compiled.data)
    assert [(c.symbol, c.call_kind, c.prototype_id, c.pc) for c in calls] == [
        ("require", "global", 1, 2), ("tostring", "global", 2, 4), ("DebugMessage", "global", 2, 5)]

    remake_lua_sources = list(corpus.iter_sources("remake", ".lua", "effective"))
    source = next(s for s in remake_lua_sources if s.logical_path ==
                  "data/scripts/evaluators/targetdistancetoplayerspacestationevaluator.lua")
    calls, _helpers = parse_source_calls(source.data)
    assert [(c.symbol, c.call_kind, c.line, c.column) for c in calls] == [
        ("require", "global", 1, 1), ("require", "global", 2, 1),
        ("GetTargetDistanceToPlayerSpaceStation", "global", 8, 12)]

    schema = SchemaLookup(build_schema_index(args.schema_root))
    remake_xml_sources = list(corpus.iter_sources("remake", ".xml", "effective"))
    hints = registry_type_hints(remake_xml_sources, "remake", schema)
    hardpoints = next(s for s in remake_xml_sources if s.logical_path == "data/xml/gamemodes/attrition/hardpoints.xml")
    occurrences = parse_xml(hardpoints, "remake", schema, hints[hardpoints.logical_path])
    assert [(o.object_type, o.tag_path, o.node_kind, o.line) for o in occurrences] == [
        ("HardPoint", "HardPoints", "element", 2),
        ("HardPoint", "HardPoint", "element", 4),
        ("HardPoint", "HardPoint/@Name", "attribute", 4),
        ("HardPoint", "HardPoint/Type", "element", 5),
        ("HardPoint", "HardPoint/Is_Targetable", "element", 6),
        ("HardPoint", "HardPoint/Is_Destroyable", "element", 7),
        ("HardPoint", "HardPoint/Health", "element", 8),
        ("HardPoint", "HardPoint/Attachment_Bone", "element", 9),
        ("HardPoint", "HardPoint/Collision_Mesh", "element", 10),
    ]
    print(json.dumps({"compiled_sample_calls": 3, "lua_counts": lua_expected,
                      "source_sample_calls": 3, "unsupported": unsupported,
                      "xml_counts": xml_expected, "xml_sample_occurrences": 9}, sort_keys=True))
    return 0


if __name__ == "__main__": raise SystemExit(main())
