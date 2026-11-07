#!/usr/bin/env python3
"""Summarize repeated remake_bench draw-type diagnostics; output JSON, Markdown and SVG.

Inputs are results.json files from the SAME host/build/Separate mode. Producer surface
incidence is an estimate; ablation draw/CPU deltas are measurements conditional on the
bench timing queries. They are non-additive and can be negative. No per-draw CPU/GPU
cost or material-switch count is inferred. Unit class remains unknown.
"""
from __future__ import annotations
import argparse
import html
import json
import pathlib
import statistics
import textwrap

TYPES = ("hull", "hardpoint", "effect", "particle", "debris", "wreck", "shadows")
EXPERIMENTS = tuple(f"{role}-band-{cutoff}" for role in ("hardpoint", "particle", "shadows") for cutoff in ("005", "010", "020")) + ("groups", "integrated-turret", "other-hull")
UNSUPPORTED = ("projectile", "terrain/env", "HUD/selection", "other")
METRICS = ("draws", "primitives", "frame_ms", "render_cpu_ms", "gpu_ms")


def spread(values):
    return dict(n=len(values), median=statistics.median(values), minimum=min(values), maximum=max(values)) if values else None


def producer_stats(controls, role_names, shadow_only=False):
    records, shares = [], []
    keys = ("retained", "retained_surfaces", "retained_triangles", "material_incidence", "potential_shadow_surfaces")
    for control in controls:
        for sample in control.get("producer_samples", []):
            roles = sample.get("roles", {})
            present = list(roles.values()) if shadow_only else [roles[name] for name in role_names if name in roles]
            if not present:
                continue
            combined = {key: sum(role[key] for role in present) for key in keys if all(key in role for role in present)}
            if "retained_surfaces" in combined and "potential_shadow_surfaces" in combined:
                estimate = combined["potential_shadow_surfaces"] if shadow_only else combined["retained_surfaces"] + combined["potential_shadow_surfaces"]
                combined["draw_estimate_one_colour_one_shadow"] = estimate
                total = sum(role.get("retained_surfaces", 0) + role.get("potential_shadow_surfaces", 0) for role in roles.values())
                if total:
                    shares.append(100 * estimate / total)
            if shadow_only:
                # Shadow-pass incidence overlaps the provenance rows. Geometry
                # and material counts per actual shadow pass are unavailable.
                shadow = {}
                if all("eligible_casters" in role for role in present):
                    shadow["retained"] = sum(role["eligible_casters"] for role in present)
                if "potential_shadow_surfaces" in combined:
                    shadow["draw_estimate_one_colour_one_shadow"] = combined["potential_shadow_surfaces"]
                combined = shadow
            records.append(combined)
    return ({key: spread([record[key] for record in records if key in record])
             for key in (*keys, "draw_estimate_one_colour_one_shadow")}, spread(shares))


def summarize(documents):
    groups = {}
    identity = None
    engine_build = None
    seen = set()
    red_runs = []
    for doc in documents:
        if not doc.get("valid"):
            raise ValueError("invalid benchmark receipt")
        meta = doc["meta"]
        if meta.get("exit_code") != 0:
            raise ValueError("successful rig controller receipt required")
        if meta.get("pacing") != "free":
            raise ValueError("uncapped free-pacing diagnostic receipts required")
        key = (meta.get("machine"), meta.get("viewer"), meta.get("viewer_dll_sha256"), meta.get("engine_gui_sha256"))
        if not all(key) or (identity is not None and identity != key):
            raise ValueError("same-host exact viewer/engine SHA-256 provenance required")
        identity = key
        for row in doc["scenes"]:
            if not row.get("drawtype_ablate"):
                continue
            if row.get("render_thread_observed") is not True or row.get("measurement_error"):
                raise ValueError("Separate mode and valid measurements required")
            if not row.get("engine_build") or (engine_build is not None and engine_build != row["engine_build"]):
                raise ValueError("same engine build required")
            engine_build = row["engine_build"]
            run_key = (row["source_scene"], row["drawtype_ablate"], row["repeat"])
            if run_key in seen:
                raise ValueError("duplicate scene/category/repeat")
            seen.add(run_key)
            errors = row.get("error_markers_total") or 0
            if errors:
                red_runs.append(dict(run=row.get("scene", row["source_scene"]), errors=errors))
            groups.setdefault(row["source_scene"], {}).setdefault(row["drawtype_ablate"], []).append(row)
    result = dict(host=identity[0] if identity else None, viewer=identity[1] if identity else None,
                  dll_sha256=identity[2] if identity else None, mode="Separate", engine_build=engine_build, engine_gui_sha256=identity[3] if identity else None, diagnostic_status="RED diagnostic" if red_runs else "diagnostic", render_error_runs=red_runs, scenes={})
    for scene, variants in groups.items():
        controls = {r["repeat"]: r for r in variants.get("none", [])}
        if len(controls) < 2:
            raise ValueError("at least two matched control repeats required")
        table = []
        for category in (*TYPES, *(name for name in EXPERIMENTS if name in variants)):
            runs = variants.get(category, [])
            if runs and {r["repeat"] for r in runs} != set(controls):
                raise ValueError("every measured category needs all matched control repeats")
            row = dict(type=category, unit_class="unknown", status="measured" if runs else "not run")
            deltas = {k: [] for k in METRICS}
            shares = []
            for run in runs:
                control = controls.get(run["repeat"])
                if not control:
                    raise ValueError("missing paired control")
                for metric in METRICS:
                    before = control.get(metric, {}).get("median")
                    after = run.get(metric, {}).get("median")
                    if metric == "gpu_ms" and before == 0 and after == 0:
                        continue  # No measurable GPU timer value; retain the raw bench receipt.
                    if before is not None and after is not None:
                        deltas[metric].append(before - after)
                before = control.get("draws", {}).get("median")
                after = run.get("draws", {}).get("median")
                if before and after is not None:
                    shares.append(100 * (before - after) / before)
            row["delta"] = {k: spread(v) for k, v in deltas.items()}
            row["draw_delta_percent"] = spread(shares)
            producer_role = category.split("-band-", 1)[0]
            row["producer"], row["producer_estimate_share_percent"] = producer_stats(controls.values(), (producer_role,), shadow_only=producer_role == "shadows")
            if category in ("integrated-turret", "other-hull"):
                field = "declared_turret_surfaces" if category == "integrated-turret" else "other_hull_surfaces"
                row["producer"]["retained_surfaces"] = spread([s["diagnostic"][field] for r in controls.values()
                    for s in r.get("producer_samples", []) if field in s.get("diagnostic", {})])
            table.append(row)
        for category in UNSUPPORTED:
            roles = {"terrain/env": ("map", "sky"), "other": ("unknown",)}.get(category, ())
            producer, share = producer_stats(controls.values(), roles)
            table.append(dict(type=category, status="not separable", unit_class="unknown",
                              producer=producer, producer_estimate_share_percent=share))
        def ranking(metric):
            eligible = [r for r in table if r.get("delta", {}).get(metric)]
            return [r["type"] for r in sorted(eligible, key=lambda r: r["delta"][metric]["median"], reverse=True)[:3]]
        diagnostic_keys = ("eligible_units", "compatible_groups", "grouped_units", "potential_colour_saving", "declared_turret_surfaces", "other_hull_surfaces", "unclassified_hull_surfaces", "named_hull_surfaces")
        retained = {key: spread([s["diagnostic"][key] for r in controls.values() for s in r.get("producer_samples", [])
                                if key in s.get("diagnostic", {})]) for key in diagnostic_keys}
        result["scenes"][scene] = dict(table=table, retained_eligible_diagnostics=retained, producer_scope=sorted({row.get("producer_scope", "not recorded") for row in controls.values()}), top3_by_draw_delta=ranking("draws"), top3_by_render_cpu_delta=ranking("render_cpu_ms"),
            top3_by_measured_draw_share=[r["type"] for r in sorted(
                (r for r in table if r.get("draw_delta_percent")),
                key=lambda r: r["draw_delta_percent"]["median"], reverse=True)[:3]],
            top3_by_producer_estimated_share=[r["type"] for r in sorted(
                (r for r in table if r.get("producer_estimate_share_percent")),
                key=lambda r: r["producer_estimate_share_percent"]["median"], reverse=True)[:3]])
    if not result["scenes"]:
        raise ValueError("no draw-type diagnostics")
    return result


def write_outputs(result, out):
    out.mkdir(parents=True, exist_ok=True)
    (out / "drawtype.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    provenance = f"{result['diagnostic_status']}; Host {result['host']}; viewer {result['viewer']}; DLL {result['dll_sha256']}; engine {result['engine_build']}; engine GUI SHA256 {result['engine_gui_sha256']}; Separate; uncapped, vsync off."
    lines = [f"# {result['diagnostic_status']} draw-type profile", "", provenance, "", "Deltas are control minus hidden, paired by repeat; median [min, max]. Timing-query overhead is present in every control. Deltas are non-additive, may be negative, and are not per-draw costs. Retained producer geometry includes hidden/off-camera resources. One-colour plus one eligible-shadow-pass surfaces are estimates; actual cascades/culling/passes and material switches are unknown. Other includes unknown provenance, including unseparated projectile model slots; terrain/env combines map and sky. The shadows producer estimate overlaps role estimates and includes only one eligible shadow pass; its instances count eligible casters. Unit class is unknown. Producer scope is recorded per scene. Producer min/max spans sampled frames; timing-delta min/max spans paired run medians.", ""]
    cells = []
    for scene, data in result["scenes"].items():
        lines += [f"## {scene}", "", "Producer scope: " + "; ".join(data["producer_scope"]), "", f"Top 3 measured draw shares: {', '.join(data['top3_by_measured_draw_share'])}. Top 3 producer estimated shares: {', '.join(data['top3_by_producer_estimated_share'])}. Top 3 render CPU deltas: {', '.join(data['top3_by_render_cpu_delta'])}.", "", "| Type | N | Draw delta | Draw delta % | Frame ms delta | Render CPU ms delta | GPU ms delta | Primitives delta |", "|---|---:|---:|---:|---:|---:|---:|---:|"]
        for row in data["table"]:
            def fmt(metric):
                return "unknown" if not metric else f"{metric['median']:.2f} [{metric['minimum']:.2f}, {metric['maximum']:.2f}]"
            delta = row.get("delta", {})
            lines.append(f"| {row['type']} ({row['status']}) | {delta.get('draws', {}).get('n', 0) if delta.get('draws') else 0} | {fmt(delta.get('draws'))} | {fmt(row.get('draw_delta_percent'))} | {fmt(delta.get('frame_ms'))} | {fmt(delta.get('render_cpu_ms'))} | {fmt(delta.get('gpu_ms'))} | {fmt(delta.get('primitives'))} |")
        lines += ["", "| Type | Estimated share % | Retained instances | Surface incidence | Triangle incidence/capacity | Material incidence | Draw estimate (colour + one eligible shadow pass) |", "|---|---:|---:|---:|---:|---:|---:|"]
        for row in data["table"]:
            producer = row.get("producer", {})
            values = [fmt(row.get("producer_estimate_share_percent"))] + [fmt(producer.get(k)) for k in ("retained", "retained_surfaces", "retained_triangles", "material_incidence", "draw_estimate_one_colour_one_shadow")]
            lines.append("| " + row["type"] + " | " + " | ".join(values) + " |")
        if any(data.get("retained_eligible_diagnostics", {}).values()):
            lines += ["", "Retained/eligible identical-unit candidates (whole scene, squadron membership unavailable; no engine visibility claim):", ""]
            for key, value in data["retained_eligible_diagnostics"].items():
                lines.append(f"- {key}: {fmt(value)}")
            lines += ["", "Potential colour saving counts compatible retained surfaces only. The groups row measures hide-all-but-one, including shadow/visibility interactions; it is not an instanced implementation. Hull names are retained at upload. Declared turret/body incidence uses XML-resolved bone ownership; mixed or unresolved surfaces stay unclassified. The integrated-turret and other-hull rows measure their independent hide-one deltas.", ""]
        cells.append((scene, data["table"]))
    (out / "drawtype.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    # Standard-library SVG keeps the artifact standalone and requires no plotting dependency.
    columns = tuple(dict.fromkeys(row["type"] for _, rows in cells for row in rows))
    width, height = 290 + len(columns) * 150, 180 + len(cells) * 150
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}"><rect width="100%" height="100%" fill="white"/>', '<g font-family="sans-serif" font-size="12">']
    for index, line in enumerate(textwrap.wrap(provenance, 155)):
        svg.append(f'<text x="15" y="{20 + index*17}">{html.escape(line)}</text>')
    svg.append('<text x="15" y="92">Draw delta % / render CPU delta ms / producer estimated share %. Conditional, non-additive; producer passes estimated.</text>')
    svg.append('<text x="15" y="110">Red = positive; blue = negative. Full colour = 100% or 10 ms. Unknown = no attributed producer record.</text>')
    for col, category in enumerate(columns):
        svg.append(f'<text x="{250 + col*150}" y="140">{html.escape(category)}</text>')
    for index, (scene, rows) in enumerate(cells):
        y = 160 + index * 150
        svg.append(f'<text x="15" y="{y+20}">{html.escape(scene)}</text>')
        svg += [f'<text x="125" y="{y+20}">draw delta %</text>', f'<text x="125" y="{y+60}">render CPU ms</text>', f'<text x="125" y="{y+100}">producer est. %</text>']
        by_type = {row["type"]: row for row in rows}
        for col, category in enumerate(columns):
            row = by_type.get(category, {})
            for offset, metric, scale in ((0, row.get("draw_delta_percent"), 100), (40, row.get("delta", {}).get("render_cpu_ms"), 10), (80, row.get("producer_estimate_share_percent"), 100)):
                value = metric["median"] if metric else None
                shade = min(abs(value or 0) / scale, 1)
                colour = f'rgb(255,{int(255*(1-shade))},{int(255*(1-shade))})' if (value or 0) >= 0 else f'rgb({int(255*(1-shade))},{int(255*(1-shade))},255)'
                x = 245 + col * 150
                svg += [f'<rect x="{x}" y="{y+offset}" width="125" height="35" fill="{colour}" stroke="#ccc"/>', f'<text x="{x+8}" y="{y+offset+23}">{value:.2f}</text>' if value is not None else f'<text x="{x+8}" y="{y+offset+23}">unknown</text>']
    svg.append('</g></svg>')
    (out / "drawtype-heatmap.svg").write_text("\n".join(svg), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", nargs="+", type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()
    write_outputs(summarize([json.loads(p.read_text(encoding="utf-8-sig")) for p in args.results]), args.out)


if __name__ == "__main__":
    main()
