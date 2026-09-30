"""Isolated Godot exercise for the production fog-stub-v1 renderer binding.

The probe GDExtension (tests/presentation/fog/renderer) instantiates the
production GodotRenderer, uploads invented meshes and textures, submits
invented RenderSnapshots and compares every rendered pixel against
same-pipeline controls. Each run copies the synthetic project into a
temporary directory, so neither the source tree nor any live editor project is
touched. The graphical run needs:

  EAWR_GODOT_FOG_RENDERER_RUNTIME_TEST=1
  EAWR_GODOT_EXECUTABLE=<pinned Godot 4.7.2 console binary>
  EAWR_FOG_RENDERER_PROBE_LIBRARY=<built libeawr_fog_renderer_probe.* library>
  EAWR_FOG_RENDERER_PROBE_OUTPUT=<optional directory for report and log>

The report verifier's negative controls always run.
"""

import copy
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
PROJECT = pathlib.Path(__file__).resolve().parent / "renderer" / "project"
HARNESS_PATH = ROOT / "apps/viewer/tools/qualify_package_runtime.py"
HARNESS_SPEC = importlib.util.spec_from_file_location("qualify_package_runtime", HARNESS_PATH)
assert HARNESS_SPEC is not None and HARNESS_SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(HARNESS_SPEC)
HARNESS_SPEC.loader.exec_module(HARNESS)

TIMEOUT_SECONDS = 300
ALLOWED_WARNINGS = tuple(
    line.strip() for line in os.environ.get("EAWR_ALLOW_ENGINE_WARNINGS", "").splitlines()
    if line.strip())

COUNTERS = ("uploads", "upload_bytes", "creates", "recreates", "updates", "destroys", "binds", "unbinds",
            "metadata_only", "reselects", "rejected", "live_textures", "attached", "declared")


def stage(readiness, counters, **extra):
    values = dict(zip(COUNTERS, counters))
    values["readiness"] = readiness
    values.update(extra)
    return values


# Status the renderer reports after each stage. Counters are cumulative for
# one enable_fog() lifetime: uploads, bytes, creates, recreates, updates,
# destroys, binds, unbinds, metadata_only, reselects, rejected, live, attached,
# declared.
EXPECTED_STAGES = {
    "setup": stage("disabled", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2), unsupported=10),
    "disabled-never": stage("disabled", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2)),
    "disabled-explicit": stage("disabled", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2)),
    "enabled-awaiting": stage("rejected", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 2),
                              rejection="EAWR-FOG-0001", binding_retained=False, bound_revision=None),
    "first-grid": stage("ready", (1, 6, 1, 0, 0, 0, 2, 0, 0, 0, 1, 1, 2, 2),
                        last_action="created", bound_revision=1, team=0, stream=1),
    "camera-transform-identical": stage("ready", (1, 6, 1, 0, 0, 0, 2, 0, 0, 0, 1, 1, 2, 2),
                                        last_action="unchanged", bound_revision=1),
    "late-consumer": stage("ready", (1, 6, 1, 0, 0, 0, 3, 0, 0, 0, 1, 1, 3, 3), last_action="unchanged"),
    "metadata-origin": stage("ready", (1, 6, 1, 0, 0, 0, 6, 0, 1, 0, 1, 1, 3, 3),
                             last_action="metadata_only", bound_revision=2),
    "cell-change": stage("ready", (2, 12, 1, 0, 1, 0, 9, 0, 2, 0, 1, 1, 3, 3),
                         last_action="updated", bound_revision=4),
    "rollback-conflict": stage("rejected", (2, 12, 1, 0, 1, 0, 9, 0, 2, 0, 3, 1, 3, 3),
                               rejection="EAWR-FOG-0003", binding_retained=True, bound_revision=4),
    "missing-team": stage("rejected", (2, 12, 1, 0, 1, 0, 9, 3, 2, 0, 4, 1, 3, 3),
                          rejection="EAWR-FOG-0001", binding_retained=False, bound_revision=None, team=0),
    "reselect": stage("ready", (2, 12, 1, 0, 1, 0, 12, 3, 2, 1, 4, 1, 3, 3), last_action="reselected"),
    "backend-failure": stage("rejected", (2, 12, 1, 0, 1, 0, 12, 3, 2, 1, 5, 1, 3, 3),
                             rejection="EAWR-FOG-0004", binding_retained=True, bound_revision=4),
    "recreate": stage("ready", (3, 24, 2, 1, 1, 1, 15, 3, 2, 1, 5, 1, 3, 3),
                      last_action="recreated", bound_revision=6),
    "team-7": stage("ready", (4, 30, 3, 1, 1, 1, 18, 3, 2, 1, 5, 2, 3, 3),
                    last_action="created", team=7, bound_revision=1),
    "team-0-again": stage("ready", (4, 30, 3, 1, 1, 1, 24, 6, 2, 3, 6, 2, 3, 3),
                          last_action="reselected", team=0, bound_revision=6),
    "reset-stream": stage("awaiting_grid", (4, 30, 3, 1, 1, 3, 24, 9, 2, 3, 6, 0, 3, 3),
                          stream=2, bound_revision=None),
    "new-stream": stage("ready", (5, 36, 4, 1, 1, 3, 27, 9, 2, 3, 6, 1, 3, 3),
                        last_action="created", stream=2, bound_revision=1),
    "release-consumer": stage("ready", (5, 36, 4, 1, 1, 3, 27, 10, 2, 3, 6, 1, 2, 2)),
    "disabled-again": stage("disabled", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2)),
    # Synthetic controls (stream 5): bound-by-default shader and pre-existing
    # overrides, both declared next to the terrain and unit consumers.
    "synthetic-kept": stage("disabled", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4)),
    "bound-default-awaiting": stage("rejected", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 4, 4),
                                    rejection="EAWR-FOG-0001", binding_retained=False, bound_revision=None,
                                    stream=5),
    "bound-default-grid": stage("ready", (1, 6, 1, 0, 0, 0, 4, 0, 0, 0, 1, 1, 4, 4),
                                last_action="created", bound_revision=1, stream=5),
    "bound-default-missing-team": stage("rejected", (1, 6, 1, 0, 0, 0, 4, 4, 0, 0, 2, 1, 4, 4),
                                        rejection="EAWR-FOG-0001", binding_retained=False, bound_revision=None),
    "overrides-grid": stage("ready", (1, 6, 1, 0, 0, 0, 8, 4, 0, 1, 2, 1, 4, 4),
                            last_action="reselected", bound_revision=1),
    "overrides-missing-team": stage("rejected", (1, 6, 1, 0, 0, 0, 8, 8, 0, 1, 3, 1, 4, 4),
                                    rejection="EAWR-FOG-0001", binding_retained=False, bound_revision=None),
    "synthetic-detached": stage("disabled", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4)),
    "synthetic-release": stage("disabled", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2)),
    "lit-fog": stage("ready", (1, 6, 1, 0, 0, 0, 2, 0, 0, 0, 0, 1, 2, 2), stream=4, last_action="created"),
    "before-destruction": stage("ready", (1, 6, 1, 0, 0, 0, 2, 0, 0, 0, 0, 1, 2, 2), stream=3),
}
EXPECTED_RENDERS = {
    "disabled-explicit": "baseline-equality",
    "enabled-awaiting": "dark",
    "first-grid": "team 0 revision 1",
    "camera-transform-identical": "team 0 revision 1",
    "late-consumer": "team 0 revision 1",
    "metadata-origin": "team 0 revision 2",
    "cell-change": "team 0 revision 4",
    "rollback-conflict": "team 0 revision 4",
    "missing-team": "dark",
    "backend-failure": "team 0 revision 4",
    "recreate": "team 0 revision 6",
    "team-7": "team 7 revision 1",
    "team-0-again": "team 0 revision 6",
    "reset-stream": "dark",
    "new-stream": "team 0 revision 1",
    "disabled-again": "baseline-equality",
    "synthetic-kept": "override-visible",
    "bound-default-awaiting": "dark",
    "bound-default-grid": "team 0 revision 1",
    "bound-default-missing-team": "dark",
    "overrides-grid": "team 0 revision 1",
    "overrides-missing-team": "dark",
    "synthetic-detached": "pre-fog-equality",
    "lit-fog": "team 0 revision 1",
}
UNSUPPORTED_REASONS = {
    4: "MeshGloss.fx", 5: "eawr_fog_texture", 6: "eawr_fog_bound",
    # eawr_fog_texture that is not one plain sampler2D; the other four uniforms
    # are correct.
    9: "not a sampler2D (Texture2D)", 10: "not a sampler2D (Texture2D)", 11: "not a sampler2D (Texture2D)",
    12: "declared as usampler2D", 13: "declared as isampler2D", 14: "eawr_fog_texture as Array",
    15: "declared as usampler2D",
}
SYNTHETIC_ASSETS = (7, 8)
# The explicit unbound state, as RenderingServer reports the overrides.
FORCED_UNBOUND = {"eawr_fog_texture": "Nil:<null>", "eawr_fog_origin": "Vector2:(0.0, 0.0)",
                  "eawr_fog_extent": "Vector2:(1.0, 1.0)", "eawr_fog_size": "Vector2:(1.0, 1.0)",
                  "eawr_fog_bound": "bool:false"}


def verify_parameters(report, failures):
    """The bound-by-default and override controls were forced unbound while
    attached and got exactly their pre-fog parameters back after disable."""
    entries = {}
    for entry in report.get("fog_parameters", []):
        entries.setdefault((entry.get("label"), entry.get("asset")), entry.get("parameters", {}))
    for asset in SYNTHETIC_ASSETS:
        before = entries.get(("before-fog", asset))
        after = entries.get(("after-disable", asset))
        if not before or len(before) != 5:
            failures.append(f"asset {asset}: pre-fog parameters missing")
        elif after != before:
            failures.append(f"asset {asset}: parameters after disable {after!r} != before {before!r}")
        for label in ("attached-awaiting", "active-unbind"):
            forced = entries.get((label, asset), {})
            if forced != FORCED_UNBOUND:
                failures.append(f"asset {asset}: {label} is not forced unbound: {forced!r}")
    if any(v != "Nil:<null>" for v in entries.get(("before-fog", 7), {}).values()):
        failures.append("bound-by-default control had overrides before fog")
    if entries.get(("before-fog", 8), {}).get("eawr_fog_bound") != "int:1":
        failures.append("override control lost its pre-existing eawr_fog_bound override before fog")


def verify_attenuation(name, render, failures, require_coverage):
    """Every visible fogged region is darker than its unattenuated control and
    byte 255 equals it; with require_coverage, both paths show a byte-255 cell
    and at least three attenuated ones."""
    for surface in ("terrain", "unit"):
        means = [m for m in render.get("means", []) if m.get("surface") == surface and m.get("pixels", 0) >= 50]
        full = [m for m in means if m.get("byte") == 255]
        darker = [m for m in means if 0 < m.get("byte", 0) < 255
                  and all(f < u or u < 1 for f, u in zip(m.get("rgb", []), m.get("unattenuated_rgb", [])))
                  and sum(u - f for f, u in zip(m.get("rgb", []), m.get("unattenuated_rgb", []))) >= 6]
        attenuated = [m for m in means if 0 < m.get("byte", 0) < 255]
        if any(abs(f - u) > 1 for m in full for f, u in zip(m.get("rgb", []), m.get("unattenuated_rgb", []))):
            failures.append(f"render {name}: {surface} byte 255 is not unattenuated")
        if len(darker) != len(attenuated):
            failures.append(f"render {name}: {surface} attenuation is not visible on every cell")
        if require_coverage and (not full or len(attenuated) < 3):
            failures.append(f"render {name}: {surface} lacks a byte-255 cell or three attenuated cells")


def verify_report(report, early_exit=False):
    """Return a list of failures for a probe report."""
    failures = []
    if report.get("schema") != "eawr-fog-renderer-probe-v1":
        failures.append("schema")
    if report.get("rendering_method") in (None, "", "headless", "dummy"):
        failures.append("not a graphical renderer")
    if report.get("variant_is_default_plus_fog_blocks") is not True:
        failures.append("fog variant is not the default adapter plus the fog blocks")
    if report.get("destroyed_with_fog_enabled") is not True:
        failures.append("renderer was not destroyed with fog enabled")
    memory = report.get("texture_memory", {})
    if memory.get("after_destroy") != memory.get("baseline"):
        failures.append("texture memory did not return to the pre-renderer baseline")
    if early_exit:
        if report.get("status") != "fog_renderer_probe_failed" or report.get("failures") != ["early exit requested"]:
            failures.append(f"early exit status {report.get('status')!r} {report.get('failures')!r}")
        if not report.get("steps_completed", 0) < report.get("steps_total", 0):
            failures.append("early exit completed every step")
        stages = {s.get("stage"): s for s in report.get("stages", [])}
        if stages.get("first-grid", {}).get("readiness") != "ready":
            failures.append("early exit left before fog was ready")
        return failures

    if report.get("status") != "fog_renderer_probe_passed" or report.get("failures"):
        failures.append(f"status {report.get('status')!r} failures {report.get('failures')!r}")
    if report.get("steps_completed") != report.get("steps_total") or not report.get("steps_total"):
        failures.append("not every step completed")
    if not memory.get("with_assets", 0) > memory.get("baseline", 0):
        failures.append("asset textures not observed in texture memory")
    if not memory.get("with_fog", 0) > memory.get("before_enable", 0):
        failures.append("fog texture not observed in texture memory")
    if memory.get("after_disable") != memory.get("before_enable"):
        failures.append("texture memory did not return to its pre-enable value after disable")
    unsupported = {entry.get("asset"): entry.get("reason", "") for entry in report.get("unsupported", [])}
    if set(unsupported) != set(UNSUPPORTED_REASONS) \
            or any(word not in unsupported[asset] for asset, word in UNSUPPORTED_REASONS.items()):
        failures.append(f"unsupported consumers {unsupported!r}")
    stages = {s.get("stage"): s for s in report.get("stages", [])}
    for name, expected in EXPECTED_STAGES.items():
        actual = stages.get(name)
        if actual is None:
            failures.append(f"missing stage {name}")
            continue
        for key, value in expected.items():
            if actual.get(key) != value:
                failures.append(f"stage {name}.{key}: {actual.get(key)!r} != {value!r}")
    verify_parameters(report, failures)
    renders = {r.get("stage"): r for r in report.get("renders", [])}
    for name, expected in EXPECTED_RENDERS.items():
        render = renders.get(name)
        if render is None:
            failures.append(f"missing render {name}")
            continue
        if expected in ("baseline-equality", "pre-fog-equality"):
            if render.get("kind") != expected or render.get("differing_bytes") != 0 \
                    or render.get("compared_bytes") != 400 * 200 * 4:
                failures.append(f"render {name} differs from its {expected} reference")
            continue
        if expected == "override-visible":
            if render.get("kind") != expected or not render.get("differing_bytes", 0) > 10000:
                failures.append(f"render {name}: pre-existing overrides are not visible")
            continue
        if render.get("expected") != expected:
            failures.append(f"render {name} showed {render.get('expected')!r}, not {expected!r}")
        if render.get("mismatches") != 0 or render.get("max_error", 99) > 1:
            failures.append(f"render {name}: {render.get('mismatches')!r} mismatches")
        if not render.get("compared_terrain", 0) > 20000 or not render.get("compared_unit", 0) > 3000:
            failures.append(f"render {name}: too few compared pixels")
        if expected != "dark":
            if not render.get("compared_outside_grid", 0) > 1000:
                failures.append(f"render {name}: dark outside the grid was not compared")
            verify_attenuation(name, render, failures, require_coverage=name in ("first-grid", "lit-fog"))
    # Scene lighting reached the fog variant: the unattenuated unit colour
    # changed between the P0 rig and the lit scene, and both matched controls.
    def unit_full(name):
        return [m.get("rgb") for m in renders.get(name, {}).get("means", [])
                if m.get("surface") == "unit" and m.get("byte") == 255]
    if not unit_full("first-grid") or not unit_full("lit-fog") or unit_full("first-grid") == unit_full("lit-fog"):
        failures.append("scene lighting did not change the fogged unit")
    return failures


def verify_alpha_report(report):
    """Check the isolated alpha variant's rendered same-pipeline references."""
    failures = []
    if report.get("status") != "fog_renderer_probe_passed" or report.get("failures"):
        failures.append(f"alpha probe status {report.get('status')!r}: {report.get('failures')!r}")
    if report.get("alpha_only") is not True or report.get("alpha_variant_is_default_plus_fog_blocks") is not True:
        failures.append("alpha variant was not the exact baseline plus fog blocks")
    if report.get("rendering_method") in (None, "", "headless", "dummy"):
        failures.append("alpha probe did not use a graphical renderer")
    if report.get("destroyed_with_fog_enabled") is not True:
        failures.append("alpha renderer was not destroyed with fog bound")
    if report.get("shadow_alpha_default_variant") is not True \
            or report.get("shadow_alpha_fog_variant") is not True:
        failures.append("shadow-receiving alpha fog material was not observed")
    if report.get("steps_completed") != report.get("steps_total") or not report.get("steps_total"):
        failures.append("alpha probe did not finish")
    stages = {s.get("stage"): s for s in report.get("stages", [])}
    for name, readiness, uploads, attached in (
            ("alpha-setup", "disabled", 0, 0),
            ("alpha-awaiting", "rejected", 0, 1),
            ("alpha-fog-0", "ready", 1, 1),
            ("alpha-fog-110", "ready", 2, 1),
            ("alpha-fog-255", "ready", 3, 1),
            ("alpha-asymmetric", "ready", 4, 1),
            ("alpha-team7", "ready", 5, 1),
            ("alpha-team0", "ready", 5, 1),
            ("alpha-transform-only", "ready", 5, 1),
            ("alpha-reset", "rejected", 5, 1),
            ("alpha-after-reset", "ready", 6, 1),
            ("alpha-disabled", "disabled", 0, 0),
            ("alpha-reenabled", "ready", 1, 1),
            ("shadow-alpha-setup", "disabled", 0, 0),
            ("shadow-alpha-enabled", "awaiting_grid", 0, 1),
            ("shadow-alpha-fog-0", "ready", 1, 1),
            ("shadow-alpha-fog-110", "ready", 2, 1),
            ("shadow-alpha-fog-255", "ready", 3, 1)):
        actual = stages.get(name, {})
        if (actual.get("readiness") != readiness or actual.get("uploads") != uploads
                or actual.get("attached") != attached):
            failures.append(f"stage {name} has wrong fog lifecycle: {actual!r}")
    renders = {r.get("stage"): r for r in report.get("renders", [])}
    if stages.get("alpha-team7", {}).get("team") != 7 \
            or stages.get("alpha-team0", {}).get("team") != 0 \
            or stages.get("alpha-team0", {}).get("binds") != 3:
        failures.append("alpha selected-team cache did not rebind both teams")
    for name, byte in (("alpha-awaiting", 0), ("alpha-fog-0", 0),
                       ("alpha-fog-110", 110), ("alpha-fog-255", 255),
                       ("alpha-team7", 110), ("alpha-reset", 0),
                       ("alpha-disabled", 255), ("shadow-alpha-fog-0", 0),
                       ("shadow-alpha-fog-110", 110), ("shadow-alpha-fog-255", 255)):
        render = renders.get(name, {})
        if render.get("kind") != "alpha-control-comparison" or render.get("byte") != byte \
                or render.get("compared") != 81 or render.get("max_error", 99) > 2:
            failures.append(f"alpha render {name} did not match its reference: {render!r}")
    expected_samples = [(0.0, 0.0, 0), (0.0, 0.6, 60), (2.0, 0.6, 110),
                        (0.0, 1.4, 210), (2.0, 1.4, 255), (0.0, 1.95, 0)]
    for name in ("alpha-asymmetric", "alpha-team0", "alpha-after-reset", "alpha-reenabled"):
        render = renders.get(name, {})
        samples = [(s.get("x"), s.get("y"), s.get("byte")) for s in render.get("samples", [])]
        if render.get("kind") != "alpha-spatial-comparison" or render.get("max_error", 99) > 2 \
                or samples != expected_samples:
            failures.append(f"alpha spatial render {name} did not match asymmetric controls: {render!r}")
    dark = renders.get("alpha-fog-0", {})
    middle = renders.get("alpha-fog-110", {})
    full = renders.get("alpha-fog-255", {})
    if not dark.get("rgb") or not middle.get("rgb") or not full.get("rgb") \
            or not sum(dark["rgb"]) < sum(middle["rgb"]) < sum(full["rgb"]):
        failures.append("alpha visibility does not increase RGB monotonically")
    shadow_dark = renders.get("shadow-alpha-fog-0", {})
    shadow_middle = renders.get("shadow-alpha-fog-110", {})
    shadow_full = renders.get("shadow-alpha-fog-255", {})
    if not shadow_dark.get("rgb") or not shadow_middle.get("rgb") or not shadow_full.get("rgb") \
            or not sum(shadow_dark["rgb"]) < sum(shadow_middle["rgb"]) < sum(shadow_full["rgb"]):
        failures.append("shadow-receiving alpha visibility does not increase RGB monotonically")
    return failures


def unexpected_engine_lines(text):
    unexpected = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line in ALLOWED_WARNINGS:
            continue
        if HARNESS.is_fatal_diagnostic(line) or line.startswith(HARNESS.ENGINE_DIAGNOSTIC_PREFIXES):
            unexpected.append(line)
    return unexpected


def sample_report():
    stages = []
    for name, values in EXPECTED_STAGES.items():
        stages.append(dict({"stage": name}, **values))
    means = []
    for surface in ("terrain", "unit"):
        means.append({"surface": surface, "byte": 255, "pixels": 500, "rgb": [200, 150, 100],
                      "unattenuated_rgb": [200, 150, 100]})
        for byte in (60, 110, 160):
            means.append({"surface": surface, "byte": byte, "pixels": 500, "rgb": [byte // 2, byte // 3, byte // 4],
                          "unattenuated_rgb": [200, 150, 100]})
    unset = {name: "Nil:<null>" for name in ("eawr_fog_texture", "eawr_fog_origin", "eawr_fog_extent",
                                             "eawr_fog_size", "eawr_fog_bound")}
    overridden = {"eawr_fog_texture": "RID:RID(12)", "eawr_fog_origin": "Vector3:(-1.5, 0.5, 0)",
                  "eawr_fog_extent": "Vector3:(4, 1.5, 0)", "eawr_fog_size": "Vector3:(2, 2, 0)",
                  "eawr_fog_bound": "int:1"}
    forced = FORCED_UNBOUND
    parameters = []
    for asset, own in ((7, unset), (8, overridden)):
        parameters += [{"label": "before-fog", "asset": asset, "parameters": dict(own)},
                       {"label": "attached-awaiting", "asset": asset, "parameters": dict(forced)},
                       {"label": "active-unbind", "asset": asset, "parameters": dict(forced)},
                       {"label": "after-disable", "asset": asset, "parameters": dict(own)}]
    renders = []
    for name, expected in EXPECTED_RENDERS.items():
        if expected in ("baseline-equality", "pre-fog-equality"):
            renders.append({"stage": name, "kind": expected, "differing_bytes": 0, "compared_bytes": 320000})
        elif expected == "override-visible":
            renders.append({"stage": name, "kind": expected, "differing_bytes": 40000, "compared_bytes": 320000})
        else:
            render_means = copy.deepcopy(means)
            if name == "lit-fog":
                for m in render_means:
                    m["rgb"] = [c // 2 for c in m["rgb"]]
                    m["unattenuated_rgb"] = [c // 2 for c in m["unattenuated_rgb"]]
            renders.append({"stage": name, "kind": "control-comparison", "expected": expected, "mismatches": 0,
                            "max_error": 0, "compared_terrain": 30000, "compared_unit": 6000,
                            "compared_outside_grid": 0 if expected == "dark" else 30000,
                            "means": render_means})
    return {"schema": "eawr-fog-renderer-probe-v1", "status": "fog_renderer_probe_passed", "failures": [],
            "rendering_method": "forward_plus", "variant_is_default_plus_fog_blocks": True,
            "destroyed_with_fog_enabled": True, "steps_completed": 30, "steps_total": 30,
            "texture_memory": {"baseline": 100, "with_assets": 200, "before_enable": 300, "with_fog": 307,
                               "after_disable": 300, "after_destroy": 100},
            "unsupported": [{"asset": 4, "reason": "legacy MeshGloss.fx"},
                            {"asset": 5, "reason": "fog uniform eawr_fog_texture"},
                            {"asset": 6, "reason": "fog uniform eawr_fog_bound"}]
            + [{"asset": asset, "reason": "fog uniform eawr_fog_texture " + word}
               for asset, word in UNSUPPORTED_REASONS.items() if asset >= 9],
            "fog_parameters": parameters, "stages": stages, "renders": renders}


def render_named(report, name):
    return next(r for r in report["renders"] if r["stage"] == name)


def stage_named(report, name):
    return next(s for s in report["stages"] if s["stage"] == name)


def parameters_named(report, label, asset):
    return next(e["parameters"] for e in report["fog_parameters"] if e["label"] == label and e["asset"] == asset)


class ReportVerifier(unittest.TestCase):
    def test_sample_passes(self):
        self.assertEqual(verify_report(sample_report()), [])

    def test_negative_controls(self):
        mutations = [
            lambda r: render_named(r, "first-grid").__setitem__("mismatches", 1),
            lambda r: render_named(r, "missing-team").__setitem__("expected", "team 7 revision 1"),
            lambda r: render_named(r, "disabled-again").__setitem__("differing_bytes", 12),
            lambda r: stage_named(r, "camera-transform-identical").__setitem__("uploads", 2),
            lambda r: stage_named(r, "late-consumer").__setitem__("binds", 2),
            lambda r: stage_named(r, "metadata-origin").__setitem__("upload_bytes", 12),
            lambda r: stage_named(r, "missing-team").__setitem__("readiness", "ready"),
            lambda r: stage_named(r, "missing-team").__setitem__("binding_retained", True),
            lambda r: stage_named(r, "team-0-again").__setitem__("uploads", 5),
            lambda r: stage_named(r, "release-consumer").__setitem__("unbinds", 9),
            lambda r: stage_named(r, "disabled-again").__setitem__("live_textures", 1),
            lambda r: r["texture_memory"].__setitem__("after_destroy", 107),
            lambda r: r["texture_memory"].__setitem__("after_disable", 307),
            lambda r: r.__setitem__("variant_is_default_plus_fog_blocks", False),
            lambda r: r.__setitem__("destroyed_with_fog_enabled", False),
            lambda r: r["unsupported"].pop(),
            lambda r: render_named(r, "first-grid")["means"][1].__setitem__("rgb", [200, 150, 100]),
            lambda r: render_named(r, "recreate")["means"][2].__setitem__("rgb", [201, 150, 100]),
            lambda r: render_named(r, "first-grid").__setitem__(
                "means", [m for m in render_named(r, "first-grid")["means"] if m["byte"] != 255]),
            lambda r: render_named(r, "team-7").__setitem__("compared_outside_grid", 0),
            lambda r: r.__setitem__("rendering_method", "dummy"),
            lambda r: r["stages"].pop(),
            lambda r: render_named(r, "lit-fog").__setitem__("means", render_named(r, "first-grid")["means"]),
            # Bound-by-default and pre-existing-override controls.
            lambda r: render_named(r, "bound-default-awaiting").__setitem__("mismatches", 30000),
            lambda r: render_named(r, "bound-default-missing-team").__setitem__("expected", "team 0 revision 1"),
            lambda r: render_named(r, "synthetic-detached").__setitem__("differing_bytes", 4000),
            lambda r: render_named(r, "synthetic-kept").__setitem__("differing_bytes", 0),
            lambda r: stage_named(r, "overrides-missing-team").__setitem__("unbinds", 4),
            lambda r: parameters_named(r, "after-disable", 8).__setitem__("eawr_fog_origin", "Nil:<null>"),
            lambda r: parameters_named(r, "active-unbind", 7).__setitem__("eawr_fog_bound", "Nil:<null>"),
            lambda r: parameters_named(r, "attached-awaiting", 8).__setitem__("eawr_fog_bound", "int:1"),
            lambda r: parameters_named(r, "active-unbind", 8).__setitem__("eawr_fog_origin", "Vector2:(-2.5, 0.25)"),
            lambda r: r.__setitem__("fog_parameters", []),
            # A sampler case accepted, or refused for the wrong reason.
            lambda r: r["unsupported"].__setitem__(-1, {"asset": 15, "reason": "fog uniform eawr_fog_texture"}),
            lambda r: r.__setitem__("unsupported", [u for u in r["unsupported"] if u["asset"] != 12]),
        ]
        for index, mutate in enumerate(mutations):
            report = copy.deepcopy(sample_report())
            mutate(report)
            self.assertNotEqual(verify_report(report), [], f"mutation {index} was accepted")

    def test_early_exit_verifier(self):
        report = sample_report()
        report.update(status="fog_renderer_probe_failed", failures=["early exit requested"], steps_completed=9)
        self.assertEqual(verify_report(report, early_exit=True), [])
        report["texture_memory"]["after_destroy"] = 101
        self.assertNotEqual(verify_report(report, early_exit=True), [])


@unittest.skipUnless(os.environ.get("EAWR_GODOT_FOG_RENDERER_RUNTIME_TEST"),
                     "set EAWR_GODOT_FOG_RENDERER_RUNTIME_TEST to run the pinned graphical fog renderer probe")
class GodotFogRendererProbe(unittest.TestCase):
    def run_probe(self, temporary, name, extra):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        report_path = pathlib.Path(temporary) / f"{name}.json"
        completed = subprocess.run(
            [executable, "--path", str(pathlib.Path(temporary) / "project"), "--rendering-driver", "vulkan", "--",
             "--eawr-fog-report", str(report_path)] + extra,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=TIMEOUT_SECONDS)
        output = completed.stdout + "\n" + completed.stderr
        destination = os.environ.get("EAWR_FOG_RENDERER_PROBE_OUTPUT")
        if destination:
            target = pathlib.Path(destination)
            target.mkdir(parents=True, exist_ok=True)
            (target / f"{name}.log").write_text(output, encoding="utf-8")
            if report_path.is_file():
                shutil.copy2(report_path, target / f"{name}.json")
        self.assertIsNotNone(HARNESS.engine_banner(completed.stdout, HARNESS.PINNED_GODOT_IDENTITY), output)
        self.assertEqual(unexpected_engine_lines(output), [], output)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        return completed, output, report

    def test_production_binding_and_lifecycle(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        library = os.environ.get("EAWR_FOG_RENDERER_PROBE_LIBRARY")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        self.assertTrue(library and pathlib.Path(library).is_file(),
                        "EAWR_FOG_RENDERER_PROBE_LIBRARY must name the probe")
        with tempfile.TemporaryDirectory(prefix="eawr-fog-renderer-probe-") as temporary:
            project = pathlib.Path(temporary) / "project"
            shutil.copytree(PROJECT, project)
            (project / "bin").mkdir()
            shutil.copy2(library, project / "bin" / pathlib.Path(library).name)
            # Register the extension in the temporary copy as an editor import
            # would, without starting the editor.
            (project / ".godot").mkdir()
            (project / ".godot" / "extension_list.cfg").write_text(
                "res://eawr_fog_renderer_probe.gdextension\n", encoding="utf-8")

            completed, output, report = self.run_probe(temporary, "fog-renderer-probe", [])
            self.assertEqual(completed.returncode, 0, output)
            self.assertIn("EAWR fog renderer probe passed", completed.stdout)
            self.assertEqual(verify_report(report), [], json.dumps(report, indent=1))

            # Leave right after the first bound grid: the renderer is destroyed
            # with fog enabled, consumers attached and a texture live.
            early, early_output, early_report = self.run_probe(
                temporary, "fog-renderer-probe-early-exit", ["--eawr-fog-early-exit"])
            self.assertEqual(early.returncode, 1, early_output)
            self.assertEqual(verify_report(early_report, early_exit=True), [], json.dumps(early_report, indent=1))

    def test_batchmesh_alpha_fog_graphical(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        library = os.environ.get("EAWR_FOG_RENDERER_PROBE_LIBRARY")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        self.assertTrue(library and pathlib.Path(library).is_file(),
                        "EAWR_FOG_RENDERER_PROBE_LIBRARY must name the probe")
        with tempfile.TemporaryDirectory(prefix="eawr-batchmesh-alpha-fog-") as temporary:
            project = pathlib.Path(temporary) / "project"
            shutil.copytree(PROJECT, project)
            (project / "bin").mkdir()
            shutil.copy2(library, project / "bin" / pathlib.Path(library).name)
            (project / ".godot").mkdir()
            (project / ".godot" / "extension_list.cfg").write_text(
                "res://eawr_fog_renderer_probe.gdextension\n", encoding="utf-8")
            completed, output, report = self.run_probe(
                temporary, "batchmesh-alpha-fog", ["--eawr-batchmesh-alpha-fog"])
            self.assertEqual(completed.returncode, 0, output)
            self.assertEqual(verify_alpha_report(report), [], json.dumps(report, indent=1))


if __name__ == "__main__":
    unittest.main()
