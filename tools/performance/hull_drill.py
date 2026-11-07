#!/usr/bin/env python3
"""Separate measured pass counters from capture-time retained hull incidence."""
from __future__ import annotations

import argparse
import collections
import json
import math
from pathlib import Path
import statistics


def spread(values):
    return dict(n=len(values), median=statistics.median(values), minimum=min(values), maximum=max(values)) if values else None


def pass_rows(document):
    """Do not align independently changing frames or subtract marginal medians."""
    if not document.get("valid") or document["meta"].get("exit_code") != 0:
        raise ValueError("successful native benchmark receipt required")
    result = []
    for row in document["scenes"]:
        if row.get("drawtype_ablate") not in ("none", "hull", "shadows", "hull-census"):
            continue
        if row.get("render_thread_observed") is not True or row.get("measurement_error"):
            raise ValueError("valid Separate measurements required")
        samples = row.get("producer_samples", [])
        passes = {key: spread([s["viewport_draws"][key] for s in samples if key in s.get("viewport_draws", {})])
                  for key in ("visible", "shadow", "canvas")}
        result.append(dict(scene=row["source_scene"], treatment=row["drawtype_ablate"], repeat=row["repeat"],
                           total_draws=row.get("draws"), viewport_lists=passes, engine_build=row.get("engine_build"),
                           scope=row.get("producer_scope"), render_cpu_ms=row.get("render_cpu_ms")))
    return dict(provenance=document["meta"], rows=result)


def ship_class(piece):
    """Diagnostic buckets from authored tags; retain unmatched/ambiguous content."""
    tokens = set(piece.get("categories", "").upper().replace(",", " ").split())
    layer = piece.get("layer", "").upper()
    if piece.get("xml_type", "").upper() == "STARBASE" or "STRUCTURE" in tokens or layer == "STATICOBJECT":
        return "structure"
    if tokens & {"FIGHTER", "BOMBER"}:
        return "fighter"
    if layer == "CORVETTE" or "CORVETTE" in tokens:
        return "corvette"
    if layer in {"CAPITAL", "SUPERCAPITAL"} or tokens & {"CAPITAL", "SUPERCAPITAL"}:
        return "capital"
    if layer == "FRIGATE" or "FRIGATE" in tokens:
        return "frigate"
    return "unknown"


def visibility(bounds, camera):
    """Conservative world-AABB sphere tests; occlusion/engine culling stay unknown."""
    if not bounds or not camera:
        return dict(offscreen=None, tiny=None, coverage=None)
    values = list(bounds) + list(camera["eye"]) + list(camera["target"]) + list(camera["up"])
    if not all(math.isfinite(x) for x in values) or any(x < 0 for x in bounds[3:]):
        return dict(offscreen=None, tiny=None, coverage=None)
    def dot(a, b): return sum(x*y for x, y in zip(a, b))
    def sub(a, b): return [x-y for x, y in zip(a, b)]
    def norm(v):
        length = math.sqrt(dot(v, v))
        return [x / length for x in v] if length > 0 else None
    def cross(a, b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
    forward = norm(sub(camera["target"], camera["eye"]))
    right = norm(cross(forward, camera["up"])) if forward else None
    if not right or camera["height"] <= 0 or camera["width"] <= 0 or not 0 < camera["fov"] < 180:
        return dict(offscreen=None, tiny=None, coverage=None)
    up = cross(right, forward)
    center = [bounds[i]+bounds[i+3]/2 for i in range(3)]
    offset = sub(center, camera["eye"])
    radius = math.sqrt(sum(x*x for x in bounds[3:])) / 2
    depth = dot(offset, forward)
    vertical = math.tan(math.radians(camera["fov"]) / 2)
    horizontal = vertical * camera["width"] / camera["height"]
    offscreen = (depth+radius < camera["near"] or depth-radius > camera["far"] or
                 abs(dot(offset, right))-horizontal*depth > radius*math.sqrt(1+horizontal*horizontal) or
                 abs(dot(offset, up))-vertical*depth > radius*math.sqrt(1+vertical*vertical))
    lateral = math.sqrt(max(0, dot(offset, offset)-depth*depth))
    coverage = radius*(1+lateral/depth)/((depth-radius)*vertical) if radius > 0 and depth-radius > camera["near"] else None
    return dict(offscreen=offscreen, tiny=coverage < .005 if coverage is not None else None, coverage=coverage)


def census(document):
    source = document["hull_census"]
    classes, models, roles = {}, {}, collections.Counter()
    pieces = source["pieces"]
    for piece in pieces:
        role = piece["role"]
        surfaces = piece["surfaces"]
        roles[role] += surfaces
        classification = ship_class(piece) if piece.get("unit") else "unowned"
        model = piece.get("source_model") or piece.get("model", "unknown")
        for groups, key in ((classes, classification), (models, (classification, piece.get("object", "unknown"), model))):
            group = groups.setdefault(key, dict(units=set(), assets=set(), meshes=set(), materials=set(), shaders=set(), programs=set(),
                                                surfaces=0, hull_surfaces=0, hidden_surfaces=0, offscreen_bound_surfaces=0,
                                                tiny_bound_surfaces=0, unknown_bound_surfaces=0, roles=collections.Counter(), hull_parts=collections.Counter(),
                                                kinds=collections.Counter(), original_model_totals=set()))
            if piece.get("unit"): group["units"].add(piece["unit"])
            group["assets"].add(piece["asset"])
            if "source_mesh" in piece:
                group["meshes"].add((model, piece["source_mesh"]))
            else:
                group["meshes"].update((model, name) for name in piece.get("mesh_names", []))
            group["materials"].add(piece["material"])
            group["shaders"].add(piece["shader"])
            group["programs"].add((piece.get("program"), piece.get("technique"), piece.get("pass_name")))
            group["surfaces"] += surfaces
            group["roles"][role] += surfaces
            group["kinds"][piece.get("kind", "unknown")] += surfaces
            if piece.get("model_meshes"):
                group["original_model_totals"].add((model, piece["model_meshes"], piece["model_submeshes"]))
            if role == "hull":
                group["hull_surfaces"] += surfaces
                group["hull_parts"][{0: "unknown", 1: "body", 2: "declared_turret"}[piece.get("hull_part", 0)]] += surfaces
            if not piece["requested_visible"]:
                group["hidden_surfaces"] += surfaces
            view = visibility(piece.get("world_bounds"), source.get("camera"))
            if view["offscreen"] is None: group["unknown_bound_surfaces"] += surfaces
            if view["offscreen"]: group["offscreen_bound_surfaces"] += surfaces
            if view["tiny"]: group["tiny_bound_surfaces"] += surfaces
    def finish(group):
        result = {k: (sorted(v) if k == "original_model_totals" else len(v) if isinstance(v, set) else dict(v) if isinstance(v, collections.Counter) else v) for k, v in group.items()}
        result["retained_surfaces_per_ship"] = result["surfaces"] / result["units"] if result["units"] else None
        return result
    rigid = [r for r in document.get("cohorts", []) if r.get("rigid") and r["count"] > 1]
    return dict(scope=source["scope"], roles=dict(roles), classes={k: finish(v) for k, v in sorted(classes.items())},
                top10=[dict(unit_class=k[0], object=k[1], model=k[2], **finish(v))
                       for k, v in sorted(models.items(), key=lambda row: (-row[1]["hull_surfaces"], row[0]))[:10]],
                rigid_compatible_cohorts=rigid, rigid_retained_candidate_saving=sum(r["count"]-1 for r in rigid),
                duplicate_scope="requested-visible rigid cohorts; exact asset/material/colour/opacity/light/pass/caster state; excludes skin and billboards; visibility and pass savings not established")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    results = []
    for path in args.inputs:
        doc = json.loads(path.read_text(encoding="utf-8-sig"))
        results.append(dict(input=str(path), **(census(doc) if "hull_census" in doc else pass_rows(doc))))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
