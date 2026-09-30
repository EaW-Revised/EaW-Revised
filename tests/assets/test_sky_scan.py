#!/usr/bin/env python3
"""Black-box contracts for the P1-06 sky_scan surface/shader ledger.

Everything runs against the wholly invented fixture in
fixtures/sky_surface_fixture.py; no installed asset is read.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "fixtures"))
import sky_surface_fixture as fixture  # noqa: E402

CORPUS = HERE.parents[1] / "plan" / "inventories" / "shader-corpus.json"


def run(scanner: pathlib.Path, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run([str(scanner), *arguments], check=False, capture_output=True, text=True)


def strict_load(path: pathlib.Path) -> dict:
    raw = path.read_bytes()
    raw.decode("ascii")  # every non-ASCII byte must be escaped

    def reject_constant(value: str):
        raise AssertionError(f"non-standard JSON constant {value}")

    def unique(pairs):
        keys = [key for key, _ in pairs]
        if len(keys) != len(set(keys)):
            raise AssertionError(f"duplicate JSON keys {keys}")
        return dict(pairs)

    return json.loads(raw, parse_constant=reject_constant, object_pairs_hook=unique)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def surface(report: dict, model: str, mesh: str, submesh: int = 0) -> dict:
    for row in report["models"]:
        if row["logical_path"] == f"data/art/models/{model}":
            for entry in row["surfaces"]:
                if entry["mesh_name"] == mesh and entry["submesh_index"] == submesh:
                    return entry
    raise AssertionError(f"no surface {model}:{mesh}:{submesh}")


def check_rows(report: dict) -> None:
    expect(report["catalog_status"] == "loaded", "catalog must be loaded")
    maps = report["maps"]
    expect((maps["scanned"], maps["land"], maps["space"], maps["failed"]) == (3, 1, 2, 0), f"map counts {maps}")
    expect(maps["sky_references"] == 8, f"declared sky references {maps['sky_references']}")
    expect(maps["reference_rows_by_status"] == {"resolved": 5, "undeclared": 2, "undeclared_model": 1,
                                                "unresolved_model": 1, "missing_catalog_object": 1},
           f"row statuses {maps['reference_rows_by_status']}")
    records = {row["logical_path"]: row for row in report["map_records"]}
    alpha = records["data/art/maps/eawr_sky_space_alpha.ted"]
    got = [(s["environment_ordinal"], s["field"], s["field_id"], s["reference_name"], s["status"], s["model_path"])
           for s in alpha["skies"]]
    expect(got == [
        (0, "primary_sky", 0x19, "EAWR_SKY_A", "resolved", "data/art/models/eawr_sky_a.alo"),
        (0, "secondary_sky", 0x1A, "EAWR_SKY_B", "resolved", "data/art/models/eawr_sky_b.alo"),
        (1, "primary_sky", 0x19, "EAWR_SKY_NOMODEL", "undeclared_model", None),
        (1, "secondary_sky", 0x1A, "EAWR_SKY_MISSING_MODEL", "unresolved_model", None),
        (2, "primary_sky", 0x19, "EAWR_SKY_UNLISTED", "missing_catalog_object", None),
        (2, "secondary_sky", 0x1A, None, "undeclared", None),
    ], f"alpha ordinal rows {got}")
    groups = {(g["environment_ordinal"], g["field"], g["reference_name"], g["status"], g["map_kind"]): g["maps"]
              for g in report["reference_groups"]}
    expect(groups[(0, "primary_sky", "EAWR_SKY_A", "resolved", "space")] == 2, "A primary on two space maps")
    expect(groups[(0, "secondary_sky", "EAWR_SKY_B", "resolved", "land")] == 1, "B secondary grouped by land kind")
    expect(groups[(0, "secondary_sky", "EAWR_SKY_B", "resolved", "space")] == 1, "B secondary grouped by space kind")
    for unresolved in ((1, "primary_sky", "EAWR_SKY_NOMODEL", "undeclared_model", "space"),
                       (1, "secondary_sky", "EAWR_SKY_MISSING_MODEL", "unresolved_model", "space"),
                       (2, "primary_sky", "EAWR_SKY_UNLISTED", "missing_catalog_object", "space")):
        expect(groups.get(unresolved) == 1, f"unresolved row not retained: {unresolved}")
    expect(len(report["models"]) == 3, "only resolved models are probed")
    expect(all(m["status"] == "loaded" for m in report["models"]), "every fixture model loads")
    model_a = next(m for m in report["models"] if m["logical_path"].endswith("eawr_sky_a.alo"))
    data = pathlib.Path(report["_root"]) / "GameData" / "Data" / "Art" / "Models" / "eawr_sky_a.alo"
    expect(model_a["sha256"] == hashlib.sha256(data.read_bytes()).hexdigest(), "model byte SHA-256")
    expect((model_a["layer"], model_a["origin"]) == ("base", "loose"), "model source layer")


def check_surfaces(report: dict) -> None:
    effects = {e["name"].lower(): (e["name"], e["family"]) for e in json.loads(CORPUS.read_text("utf-8"))["effects"]}
    dome = surface(report, "eawr_sky_a.alo", "Dome")
    veil = surface(report, "eawr_sky_a.alo", "Veil")
    sun = surface(report, "eawr_sky_a.alo", "Sun")
    ring = surface(report, "eawr_sky_a.alo", "Ring")
    haze = surface(report, "eawr_sky_b.alo", "Haze")
    shade = surface(report, "eawr_sky_b.alo", "Shade")
    glow = surface(report, "eawr_sky_b.alo", "Glow")
    land0 = surface(report, "eawr_sky_c.alo", "Dome", 0)
    land1 = surface(report, "eawr_sky_c.alo", "Dome", 1)

    expect([(s["mesh_index"], s["submesh_index"]) for s in (dome, veil, sun, ring)] == [(0, 0), (1, 0), (2, 0), (3, 0)],
           "mesh/submesh ordinals")
    expect((land1["mesh_index"], land1["submesh_index"]) == (0, 1), "second submesh ordinal")
    expect([s["chain_class"] for s in (dome, veil, sun, ring, haze, shade, glow)]
           == ["identity", "identity", "billboard:sun", "rigid_static", "identity", "hidden_bone", "non_rigid"],
           "chain classes")
    expect(sun["billboard_mode"] == 7 and sun["bone"] == 2, "billboard 0x206 mode 7 on bone 2")
    expect(dome["billboard_mode"] is None, "non-billboard surfaces carry no mode")
    expect(haze["skinned"] and haze["skin_bones"] == 1 and not haze["mesh_visible"], "skinned hidden mesh")
    expect(veil["vertex_format"] == "alD3dVertNU2C", "vertex format kept as stored")

    # Family mapping must come from the real descriptor JSON, case-insensitively.
    for row in (veil, sun, ring, haze, land0, land1):
        stem = row["shader"][:-3].lower()
        expect((row["descriptor_effect"], row["family"]) == effects[stem], f"family of {row['shader']}")
    expect(dome["descriptor_effect"] is None and dome["family"] == "not_in_descriptor_bundle",
           "an invented shader is not in the bundle and gets no guessed family")
    expect(veil["family"] == "NEBULA" and land1["family"] == "SKYDOME", "explicit Nebula/Skydome families")

    expect(land1["parameters"] == ["BaseTexture:texture", "CloudTexture:texture"], "parameter names:kinds in order")
    textures = [(t["parameter"], t["name"], t["status"], (t["resolved"] or {}).get("logical_path"))
                for t in land1["textures"]]
    expect(textures == [("BaseTexture", "eawr_sky_land_absent.tga", "not_in_vfs", None),
                        ("CloudTexture", "eawr_sky_land_cloud.tga", "resolved",
                         "data/art/textures/eawr_sky_land_cloud.tga")], f"land textures {textures}")
    expect(ring["textures"][0]["resolved"]["logical_path"] == "data/art/textures/eawr_sky_ring.dds",
           "authored .tga resolves by stem to shipped .dds")
    expect((dome["vertices"], dome["triangles"]) == (4, 2), "geometry counts")

    plans = {name: (s["space_primary_plan"] or {}).get("status") for name, s in
             (("dome", dome), ("veil", veil), ("sun", sun), ("ring", ring))}
    expect(plans == {"dome": "accepted", "veil": "shader_not_qualified", "sun": "hierarchy_unsupported",
                     "ring": "shader_not_qualified"}, f"planner verdicts {plans}")
    expect(sun["space_primary_plan"]["causes"] == ["hierarchy_unsupported", "shader_not_qualified"], "sun causes")
    expect(dome["space_primary_plan"]["causes"] == [], "accepted surface has no causes")
    expect(all(s["space_primary_plan"] is None for s in (haze, land0, land1)),
           "only space primary skies are planned")


def check_aggregates(report: dict) -> None:
    aggregates = report["aggregates"]
    shaders = {row["shader"]: row for row in aggregates["shaders"]}
    expect({k: shaders["MeshGloss"][k] for k in ("surfaces", "models", "maps_primary", "maps_secondary",
                                                  "land_maps", "space_maps")}
           == {"surfaces": 2, "models": 2, "maps_primary": 3, "maps_secondary": 0, "land_maps": 1, "space_maps": 2},
           f"MeshGloss tally {shaders['MeshGloss']}")
    expect((shaders["MeshAlpha"]["surfaces"], shaders["MeshAlpha"]["visible_surfaces"],
            shaders["MeshAlpha"]["maps_secondary"]) == (3, 2, 2), "MeshAlpha tally")
    expect(shaders["eawrsyntheticopaquediffuse"]["family"] == "not_in_descriptor_bundle", "invented shader key")
    families = {row["family"]: row for row in aggregates["families"]}
    expect((families["MESH"]["surfaces"], families["MESH"]["models"], families["MESH"]["maps_primary"],
            families["MESH"]["maps_secondary"]) == (6, 3, 3, 2), f"MESH family {families['MESH']}")
    usage = aggregates["family_usage"]
    expect({k: usage[k]["status"] for k in ("SKYDOME", "NEBULA", "PLANET", "MESH", "other",
                                            "not_in_descriptor_bundle")}
           == {"SKYDOME": "used", "NEBULA": "used", "PLANET": "unused_in_corpus", "MESH": "used",
               "other": "unused_in_corpus", "not_in_descriptor_bundle": "used"}, "explicit family usage")
    expect(usage["SKYDOME"]["tally"]["land_maps"] == 1 and usage["SKYDOME"]["tally"]["space_maps"] == 0,
           "Skydome selected only on the land map")
    expect((aggregates["surfaces"], aggregates["billboard_surfaces"], aggregates["skinned_surfaces"],
            aggregates["vertex_colour_format_surfaces"], aggregates["hidden_mesh_surfaces"],
            aggregates["multi_texture_surfaces"]) == (9, 1, 1, 2, 1, 1), "surface counts")
    expect((aggregates["textures_declared"], aggregates["textures_resolved"], aggregates["textures_not_in_vfs"])
           == (10, 9, 1), "texture counts")
    expect(aggregates["land_sky_models_with_multiple_base_textures"] == ["data/art/models/eawr_sky_c.alo"]
           and aggregates["land_maps_with_multiple_base_texture_sky"] == 1, "land multi-BaseTexture sky")
    expect(aggregates["chain_classes"] == {"billboard:sun": 1, "hidden_bone": 1, "identity": 5,
                                           "non_rigid": 1, "rigid_static": 1}, "chain class tally")


def no_host_path(path: pathlib.Path, root: pathlib.Path) -> None:
    text = path.read_text("ascii").lower()
    for needle in {str(root).lower(), root.as_posix().lower(), str(root).lower().replace("\\", "\\\\"),
                   root.name.lower()}:
        expect(needle not in text, f"host path fragment leaked into {path.name}")


def no_parameter_values(path: pathlib.Path) -> None:
    text = path.read_text("ascii")
    for key in ('"value"', '"values"', '"default"'):
        expect(key not in text, f"non-texture parameter value field {key} in {path.name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scanner", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="eawr-sky-scan-") as temporary:
        base = pathlib.Path(temporary)
        root = fixture.write_fixture_root(base / "fixture_install")
        game = root / "GameData"
        out = base / "reports"

        first = run(args.scanner, "--profile", "eaw", "--game-root", str(game),
                    "--report", str(out / "a.json"), "--inventory", str(out / "a-fragment.json"))
        expect(first.returncode == 0, f"scan with unresolved rows must exit 0: {first.stderr}")
        report = strict_load(out / "a.json")
        fragment = strict_load(out / "a-fragment.json")
        report["_root"] = str(root)
        check_rows(report)
        check_surfaces(report)
        check_aggregates(report)
        del report["_root"]

        expect("map_records" not in fragment, "the committed fragment carries no per-map records")
        expect(fragment["aggregates"] == report["aggregates"], "fragment aggregates match the report")
        expect(fragment["reference_groups"] == report["reference_groups"], "fragment groups match the report")
        expect(all("source_id" not in m for m in fragment["models"]), "fragment omits source ids")
        for written in (out / "a.json", out / "a-fragment.json"):
            no_host_path(written, root)
            no_parameter_values(written)

        second = run(args.scanner, "--profile", "eaw", "--game-root", str(game),
                     "--report", str(out / "b.json"), "--inventory", str(out / "b-fragment.json"))
        expect(second.returncode == 0, "rerun failed")
        expect((out / "a.json").read_bytes() == (out / "b.json").read_bytes(), "report is not byte-stable")
        expect((out / "a-fragment.json").read_bytes() == (out / "b-fragment.json").read_bytes(),
               "fragment is not byte-stable")

        stripped = fixture.write_fixture_root(base / "stripped_install", "stripped_catalog")
        result = run(args.scanner, "--profile", "eaw", "--game-root", str(stripped / "GameData"),
                     "--report", str(out / "stripped.json"))
        expect(result.returncode == 0, f"unavailable catalog is a completed scan: {result.stderr}")
        bare = strict_load(out / "stripped.json")
        expect(bare["catalog_status"] == "unavailable", "stripped catalog must be unavailable")
        expect(bare["maps"]["reference_rows_by_status"] == {"missing_catalog_object": 8, "undeclared": 2},
               f"stripped rows retained {bare['maps']['reference_rows_by_status']}")
        expect(bare["models"] == [] and bare["maps"]["sky_references"] == 8, "no model is probed without a catalog")

        bad_arguments = [
            [],
            ["--profile", "eaw", "--game-root", str(game)],
            ["--profile", "xbox", "--game-root", str(game), "--report", str(out / "x.json")],
            ["--profile", "eaw", "--game-root", str(game), "--report", str(out / "x.json"), "--bogus", "1"],
            ["--profile", "eaw", "--game-root", str(game), "--report"],
            ["--profile", "remake", "--game-root", str(game), "--report", str(out / "x.json")],
            ["--profile", "eaw", "--game-root", str(game), "--mod-root", str(game), "--report", str(out / "x.json")],
            ["--profile", "eaw", "--profile", "eaw", "--game-root", str(game), "--report", str(out / "x.json")],
        ]
        for arguments in bad_arguments:
            expect(run(args.scanner, *arguments).returncode == 2, f"bad arguments accepted: {arguments}")
        bad_roots = [
            ["--profile", "eaw", "--game-root", str(base / "absent"), "--report", str(out / "x.json")],
            ["--profile", "eaw", "--game-root", str(out / "a.json"), "--report", str(out / "x.json")],
            ["--profile", "foc", "--game-root", str(game), "--report", str(out / "x.json")],
            ["--profile", "remake", "--game-root", str(root), "--mod-root", str(base / "absent"),
             "--report", str(out / "x.json")],
        ]
        for arguments in bad_roots:
            expect(run(args.scanner, *arguments).returncode != 0, f"bad root accepted: {arguments}")
        expect(not (out / "x.json").exists(), "a failed invocation wrote a report")

    print("sky_scan report contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
