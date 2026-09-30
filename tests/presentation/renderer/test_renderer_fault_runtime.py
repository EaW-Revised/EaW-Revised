"""Production GodotRenderer fault/lifecycle probe (#22, WP-08).

The probe (tests/presentation/renderer/faults) is a separately built synthetic
GDExtension. It compiles the unchanged production renderer.cpp with a
test-only forced include that can replace the text of one fixed adapter
source at a time, and drives these failure branches, one scenario per Godot
process:

  production-adapters             control: every fixed source, nothing armed
  legacy-compile-failure          review finding M1, default adapter shaders
  shadow-variant-compile-failure  M1, shadow-receiving variants only
  fog-variant-compile-failure     fog-stub-v1 variant rejected at declare and at
                                  enable_fog, and one over the sampler limit
  fog-shadow-variant-failure      shadow-receiving fog variant failures
  modern-compile-failure          public API, after texture/shader RIDs exist
  material-admission              bindings that would draw wrong silently, and
                                  shaders over the portable sampler and uniform
                                  block limits
  upload-failure-kinds            texture, mesh and selector failures
  backend-unavailable             partial construction on a host without a world

Every unsupported or failing material must end in a bounded renderer
diagnostic, never a crash, a registry entry or a leaked RID. Each report is
checked case by case (code, exact message, registry, shadow
counters, armed-slot reads, RenderingServer memory where the backend exposes
it). The engine output of each process may hold only the diagnostics that
scenario expects, and never a leak line; one leak control per RID owner type
proves Godot's exit-time leak report is live on the backend used.

Backends: "rd" runs the project's forward_plus renderer on the Vulkan
RenderingDevice driver, on a display (for example under xvfb-run with Mesa
lavapipe, a software rasteriser); "headless" runs Godot's dummy
RenderingServer, which parses and generates shader code with the front end
only and compiles nothing. Neither is GPU driver evidence.

The offline cases always run. The Godot run needs:
  EAWR_GODOT_RENDERER_FAULT_RUNTIME_TEST=1
  EAWR_GODOT_EXECUTABLE=<pinned Godot 4.7.2 binary>
  EAWR_RENDERER_FAULT_PROBE_LIBRARY=<built libeawr_renderer_fault_probe.* library>
  EAWR_RENDERER_FAULT_BACKEND=rd|headless (default rd)
  EAWR_RENDERER_FAULT_PROBE_OUTPUT=<optional directory for reports and logs>
  EAWR_ALLOW_ENGINE_WARNINGS=<optional exact WARNING: lines, one per line>
"""

import copy
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
PROJECT = pathlib.Path(__file__).resolve().parent / "faults" / "project"
HARNESS_PATH = ROOT / "apps/viewer/tools/qualify_package_runtime.py"
HARNESS_SPEC = importlib.util.spec_from_file_location("qualify_package_runtime", HARNESS_PATH)
assert HARNESS_SPEC is not None and HARNESS_SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(HARNESS_SPEC)
HARNESS_SPEC.loader.exec_module(HARNESS)

TIMEOUT_SECONDS = 180
ALLOWED_WARNINGS = tuple(
    line.strip() for line in os.environ.get("EAWR_ALLOW_ENGINE_WARNINGS", "").splitlines()
    if line.strip())
BACKENDS = ("rd", "headless")
COMPILE = "EAWR-RENDER-0007"
UPLOAD = "EAWR-RENDER-0005"
INVALID = "EAWR-RENDER-0001"

# (label, program, technique, pass name, redirected slot) per fixed adapter.
LEGACY = (
    ("MeshGloss.fx opaque", "MeshGloss.fx", "sph_t0", "sph_t0_p0", "meshgloss_shader_opaque"),
    ("MeshGloss.fx transparent", "MeshGloss.fx", "sph_t0", "sph_t0_p0", "meshgloss_shader_alpha"),
    ("RSkinGloss.fx", "RSkinGloss.fx", "sph_t1", "sph_t1_p0", "rskin_shader_opaque"),
    ("BatchMeshGloss.fx", "BatchMeshGloss.fx", "sph_t1", "sph_t1_p0", "fixed_mesh_shader_opaque"),
    ("BatchMeshAlpha.fx", "BatchMeshAlpha.fx", "sph_t1", "sph_t1_p0", "fixed_mesh_shader_alpha"),
)
FOG = (
    ("BatchMeshGloss", 70, "fixed_mesh_shader_opaque", "fixed_mesh_shader_opaque_fog"),
    ("BatchMeshAlpha", 71, "fixed_mesh_shader_alpha", "fixed_mesh_shader_alpha_fog"),
)
UNKNOWN = "SHADER ERROR: Unknown identifier in expression: 'eawr_wp08_undeclared_identifier'."
REDEFINED = "SHADER ERROR: Redefinition of 'eawr_shadow_floor'."
COMPILE_FAILED = "ERROR: Shader compilation failed."
DETACHED_WORLD = 'ERROR: Condition "!is_inside_world()" is true. Returning: Ref<World3D>()'
FOG_SKIP = "RenderingServer returns no shader code on this backend, so no fog-stub-v1 variant can attach"
FOG_REJECTED = "Godot rejected the {family} fog-stub-v1 variant for asset {asset}: it does not declare fog uniform eawr_fog_texture"
SAMPLER_LIMIT = ("declares {count} material samplers; at most 5 are portable: the Compatibility fallback binds its own"
                 " samplers within GL 3.3's 16 texture units")

SCENARIOS = (
    "production-adapters", "legacy-compile-failure", "shadow-variant-compile-failure",
    "fog-variant-compile-failure", "fog-shadow-variant-failure", "modern-compile-failure",
    "material-admission", "upload-failure-kinds", "backend-unavailable",
)

# (case, asset, code, message) of every refused upload in material-admission.
ADMISSION_REFUSED = (
    ("modern undeclared binding", 110, INVALID,
     "modern spatial material for asset 110 binds 'eawr_absent', which its shader does not declare"),
    ("modern scalar onto vec4", 111, INVALID,
     "modern spatial material for asset 111 binds 'eawr_tint' as a scalar, but its shader declares Vector4"),
    ("modern float3 onto vec4", 112, INVALID,
     "modern spatial material for asset 112 binds 'eawr_tint' as a float3, but its shader declares Vector4"),
    ("modern texture onto float", 113, INVALID,
     "modern spatial material for asset 113 binds 'eawr_gain' as a texture, but its shader declares float"),
    ("modern duplicate binding", 114, INVALID,
     "modern spatial material for asset 114 binds 'eawr_gain' more than once"),
    ("modern unbound sampler", 115, INVALID,
     "modern spatial material for asset 115 leaves sampler 'eawr_s0' unbound"),
    ("modern six samplers", 116, COMPILE,
     "modern spatial shader for asset 116 " + SAMPLER_LIMIT.format(count=6)),
    ("modern 48 samplers", 117, COMPILE,
     "modern spatial shader for asset 117 " + SAMPLER_LIMIT.format(count=48)),
    ("modern uniform block over limit", 118, COMPILE,
     "modern spatial shader for asset 118 needs a 16400-byte material uniform block; Vulkan and GL 3.3 guarantee"
     " 16384 bytes"),
    ("legacy float3 onto vec4", 119, INVALID,
     "MeshGloss.fx sph_t0/sph_t0_p0 material for asset 119 binds 'Diffuse' as a float3, but its shader declares"
     " Vector4"),
    ("legacy texture onto scalar", 120, INVALID,
     "MeshGloss.fx sph_t0/sph_t0_p0 material for asset 120 binds 'Shininess' as a texture, but its shader declares"
     " float"),
)
ADMISSION_ACCEPTED = (
    "modern five samplers", "modern uniform block at limit", "modern fitting conversions",
    "modern fog texture unbound", "legacy undeclared authored parameters", "modern engine-supplied sampler unbound",
)

# Exit-time leak line expected from one unfreed RID of each owner type. The
# dummy server's skeleton_create returns an invalid RID, so there is no
# skeleton owner (and no skeleton leak report) on headless runs.
LEAK_TYPES = {
    "rd": {
        "texture": "N10RendererRD14TextureStorage7TextureE",
        "shader": "N10RendererRD15MaterialStorage6ShaderE",
        "material": "N10RendererRD15MaterialStorage8MaterialE",
        "mesh": "N10RendererRD11MeshStorage4MeshE", "instance": "N17RendererSceneCull8InstanceE",
        "skeleton": "N10RendererRD11MeshStorage8SkeletonE",
    },
    "headless": {
        "texture": "PN13RendererDummy14TextureStorage12DummyTextureE",
        "shader": "N13RendererDummy15MaterialStorage11DummyShaderE",
        "material": "N13RendererDummy15MaterialStorage13DummyMaterialE",
        "mesh": "N13RendererDummy9DummyMeshE", "instance": "N17RendererSceneCull8InstanceE", "skeleton": None,
    },
}
LEAK_RE = re.compile(r"^ERROR: (\d+) RID allocations of type '([^']+)' were leaked at exit\.$")


def failed(case, asset, code, message, slot=None):
    return {"case": case, "asset": asset, "accepted": False, "code": code, "message": message,
            "recorded": True, "registry_clean": True, "shadow_counters_unchanged": True,
            "slot": slot, "slot_reads": 1 if slot else 0}


def accepted(case, asset, slot=None, **extra):
    row = {"case": case, "asset": asset, "accepted": True, "new_diagnostics": 0, "silent": True,
           "slot": slot, "slot_reads": 1 if slot else 0}
    row.update(extra)
    return row


def adapter_message(program, technique, pass_name, asset, shadow=False):
    return "Godot rejected the %s %s/%s%s adapter shader compilation for asset %d" % (
        program, technique, pass_name, " shadow-receiving" if shadow else "", asset)


def expected_cases(scenario, backend):
    """Every case the scenario must report, in order, with its checked fields."""
    cases = []
    if scenario == "production-adapters":
        for index, (label, _, _, _, slot) in enumerate(LEGACY):
            cases.append(accepted("default " + label, 10 + index, slot, shadow_receiving=0))
        for index, (label, _, _, _, slot) in enumerate(LEGACY):
            cases.append(accepted("shadow-receiving " + label, 20 + index, slot, shadow_receiving=index + 1,
                                  shadow_variant_failures=0))
    elif scenario == "legacy-compile-failure":
        for index, (label, program, technique, pass_name, slot) in enumerate(LEGACY):
            asset = 30 + index
            cases.append(failed("default " + label, asset, COMPILE,
                                adapter_message(program, technique, pass_name, asset), slot))
            cases.append(accepted("recovered " + label, asset, slot))
    elif scenario == "shadow-variant-compile-failure":
        for index, (label, _, _, _, slot) in enumerate(LEGACY):
            cases.append(accepted("armed text compiles unshadowed " + label, 40 + index, slot))
        for index, (label, program, technique, pass_name, slot) in enumerate(LEGACY):
            asset = 50 + index
            cases.append(failed("shadow-receiving " + label, asset, COMPILE,
                                adapter_message(program, technique, pass_name, asset, shadow=True), slot))
    elif scenario == "fog-variant-compile-failure":
        if backend == "headless":
            return []
        for family, asset, base_slot, fog_slot in FOG:
            message = FOG_REJECTED.format(family=family, asset=asset)
            cases.append(accepted("fog base " + family, asset, base_slot))
            cases.append({"case": "fog declare failure " + family, "asset": asset, "accepted": False,
                          "code": COMPILE, "message": message, "recorded": True, "declarations_unchanged": True,
                          "slot": fog_slot, "slot_reads": 1, "recovered_attached": True})
            cases.append({"case": "fog enable failure " + family, "asset": asset, "accepted": False,
                          "code": COMPILE, "message": message, "recorded": True,
                          "fog_disabled_declaration_kept": True, "default_material_kept": True,
                          "slot": fog_slot, "slot_reads": 1, "recovered_attached": True})
        cases.append(accepted("fog base sampler limit", 72, "fixed_mesh_shader_opaque"))
        cases.append({"case": "fog variant sampler limit", "asset": 72, "accepted": False, "code": COMPILE,
                      "message": "BatchMeshGloss fog-stub-v1 variant for asset 72 " + SAMPLER_LIMIT.format(count=6),
                      "recorded": True, "declarations_unchanged": True})
    elif scenario == "fog-shadow-variant-failure":
        if backend == "headless":
            return []
        cases.append(accepted("fog base unshadowed BatchMeshGloss", 80, "fixed_mesh_shader_opaque"))
        for label, asset, message in (
                ("no rewritable mode", 81,
                 "BatchMeshGloss fog-stub-v1 variant has no shadow-receiving form for asset 81"),
                ("variant-only compile failure", 82, FOG_REJECTED.format(family="BatchMeshGloss", asset=82))):
            cases.append(accepted("fog base shadow-receiving BatchMeshGloss %d" % asset, asset,
                                  "fixed_mesh_shader_opaque"))
            cases.append({"case": "fog shadow " + label, "asset": asset, "accepted": False,
                          "code": COMPILE, "message": message, "recorded": True, "declarations_unchanged": True,
                          "slot": "fixed_mesh_shader_opaque_fog", "slot_reads": 1})
    elif scenario == "modern-compile-failure":
        cases.append(failed("modern undeclared identifier", 90, COMPILE,
                            "Godot rejected modern spatial shader compilation for asset 90"))
        cases.append(failed("modern non-spatial", 91, COMPILE,
                            "modern spatial shader must begin with 'shader_type spatial;'"))
        cases.append(accepted("modern recovered", 90))
    elif scenario == "material-admission":
        for case, asset, code, message in ADMISSION_REFUSED:
            cases.append(failed(case, asset, code, message))
        for index, case in enumerate(ADMISSION_ACCEPTED):
            cases.append(accepted(case, 130 + index))
    elif scenario == "upload-failure-kinds":
        cases.append(failed("texture without mips", 100, UPLOAD,
                            "Godot rejected mesh, texture, or material resources for asset 100"))
        cases.append(failed("mesh bone out of range", 101, UPLOAD,
                            "Godot rejected mesh, texture, or material resources for asset 101"))
        cases.append(failed("unknown legacy family", 102, INVALID,
                            "unknown legacy material family 'UnknownEffect.fx' has no implemented Godot adapter"))
    elif scenario == "backend-unavailable":
        cases.append({"case": "detached host", "construction_recorded": True, "upload_refused": True,
                      "capture_refused": True, "registry_empty": True,
                      "memory_restored": True if backend == "rd" else None})
    return cases


def expected_evidence(scenario, backend):
    rd = backend == "rd"
    evidence = {}
    if scenario == "production-adapters":
        evidence["fog_attached"] = 4 if rd else 0
    elif scenario == "shadow-variant-compile-failure":
        evidence["shadow_receiving_after_failures"] = 0
    elif scenario == "fog-shadow-variant-failure" and rd:
        evidence.update(armed_fog_text_attaches_unshadowed=True, shadow_fog_recovered_attached=True)
    return evidence


def verify_report(report, scenario, backend):
    """Return the report's contract failures (empty when it proves the scenario)."""
    failures = []
    rd = backend == "rd"
    if report.get("schema") != "eawr-renderer-fault-probe-v1" or report.get("scenario") != scenario:
        failures.append("unknown schema or scenario")
    if report.get("status") != "renderer_fault_probe_passed" or report.get("failures"):
        failures.append("probe reported failures: %r" % (report.get("failures"),))
    if report.get("steps_total", 0) < 2 or report.get("steps_completed") != report.get("steps_total"):
        failures.append("not every step ran")
    if report.get("leak_control") is not None:
        failures.append("a leak control report is not scenario evidence")
    if rd:
        if report.get("rendering_method") != "forward_plus" or report.get("rendering_driver") != "vulkan":
            failures.append("not the forward_plus/vulkan renderer")
        if report.get("display_server") == "headless" or not report.get("adapter"):
            failures.append("an rd run must name its display server and adapter")
        if report.get("memory_visible") is not True or report.get("shader_code_readback") is not True:
            failures.append("an rd run must expose RenderingServer memory and shader code")
    else:
        if report.get("display_server") != "headless" or report.get("adapter"):
            failures.append("a headless run must use the headless display server without an adapter")
        if report.get("memory_visible") is not False or report.get("shader_code_readback") is not False:
            failures.append("the headless dummy server exposes neither memory nor shader code")
    fog_scenario = scenario in ("fog-variant-compile-failure", "fog-shadow-variant-failure")
    if fog_scenario and not rd:
        if report.get("skipped") != FOG_SKIP:
            failures.append("a headless fog scenario must record why it cannot attach")
    elif report.get("skipped") is not None:
        failures.append("scenario was skipped")
    if scenario != "backend-unavailable" and report.get("skipped") is None:
        expected_restore = True if rd else None
        if report.get("teardown_memory_restored") is not expected_restore:
            failures.append("teardown memory evidence is wrong for the backend")
    cases = report.get("cases", [])
    wanted = expected_cases(scenario, backend)
    if [case.get("case") for case in cases] != [case["case"] for case in wanted]:
        failures.append("case list differs: %r" % ([case.get("case") for case in cases],))
    for case, want in zip(cases, wanted):
        for key, value in want.items():
            got = case.get(key)
            if key == "slot_reads" and value:
                if not isinstance(got, int) or got < value:
                    failures.append("%s: the armed slot was not read" % want["case"])
            elif got != value:
                failures.append("%s: %s is %r, expected %r" % (want["case"], key, got, value))
        # Refused uploads carry per-case memory evidence; fog declarations do not.
        if want.get("accepted") is False and "registry_clean" in want and "memory_restored" not in want:
            restored = case.get("memory_restored")
            if restored is not (True if rd else None):
                failures.append("%s: memory evidence %r is wrong for the backend" % (want["case"], restored))
    for key, value in expected_evidence(scenario, backend).items():
        if report.get(key) != value:
            failures.append("%s is %r, expected %r" % (key, report.get(key), value))
    if scenario == "production-adapters" and rd and not report.get("drawn_objects", 0) >= 11:
        failures.append("the production adapters were not all drawn")
    if scenario == "material-admission" and rd and not report.get("admitted_drawn_objects", 0) >= 6:
        failures.append("the admitted boundary materials were not all drawn")
    return failures


def expected_engine_lines(scenario, backend):
    """Exact engine diagnostic lines (with counts) a scenario's output must hold."""
    rd = backend == "rd"
    return {
        "legacy-compile-failure": {UNKNOWN: 5, COMPILE_FAILED: 5},
        "shadow-variant-compile-failure": {REDEFINED: 5, COMPILE_FAILED: 5},
        "fog-variant-compile-failure": {UNKNOWN: 4, COMPILE_FAILED: 4} if rd else {},
        "fog-shadow-variant-failure": {REDEFINED: 1, COMPILE_FAILED: 1} if rd else {},
        "modern-compile-failure": {UNKNOWN: 1, COMPILE_FAILED: 1},
        "backend-unavailable": {DETACHED_WORLD: 1},
    }.get(scenario, {})


def check_engine_output(text, scenario, backend):
    """Return (problems, facts) for one scenario's combined engine output."""
    problems = []
    expected = dict(expected_engine_lines(scenario, backend))
    seen = {line: 0 for line in expected}
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if HARNESS.is_fatal_diagnostic(line):
            problems.append("fatal engine line: " + line)
            continue
        if not line.startswith(HARNESS.ENGINE_DIAGNOSTIC_PREFIXES):
            continue
        if line in ALLOWED_WARNINGS and HARNESS.is_waivable_warning(line):
            continue
        if line in seen:
            seen[line] += 1
            continue
        problems.append("unexpected engine line: " + line)
    for line, count in expected.items():
        if seen[line] != count:
            problems.append("expected %d x %r, saw %d" % (count, line, seen[line]))
    return problems, {"expected_lines": seen}


def leak_lines(text):
    return [LEAK_RE.match(raw.strip()) for raw in text.splitlines() if LEAK_RE.match(raw.strip())]


def sample_report(scenario, backend):
    """A report that satisfies verify_report, for the offline negative controls."""
    rd = backend == "rd"
    fog_skip = scenario in ("fog-variant-compile-failure", "fog-shadow-variant-failure") and not rd
    report = {
        "schema": "eawr-renderer-fault-probe-v1", "scenario": scenario, "status": "renderer_fault_probe_passed",
        "display_server": "X11" if rd else "headless", "rendering_driver": "vulkan",
        "rendering_method": "forward_plus", "adapter": "llvmpipe" if rd else "",
        "memory_visible": rd, "shader_code_readback": rd, "skipped": FOG_SKIP if fog_skip else None,
        "leak_control": None, "steps_completed": 9, "steps_total": 9, "failures": [],
        "teardown_memory_restored": None if fog_skip else (True if rd else None),
        "cases": [],
    }
    for want in expected_cases(scenario, backend):
        case = copy.deepcopy(want)
        if case.get("accepted") is False and "registry_clean" in case:
            case["memory_restored"] = True if rd else None
        report["cases"].append(case)
    report.update(expected_evidence(scenario, backend))
    if scenario == "production-adapters":
        report["drawn_objects"] = 18 if rd else 0
    if scenario == "material-admission":
        report["admitted_drawn_objects"] = 6 if rd else 0
    return report


class RendererFaultReport(unittest.TestCase):
    def test_samples_pass(self):
        for backend in BACKENDS:
            for scenario in SCENARIOS:
                with self.subTest(backend=backend, scenario=scenario):
                    self.assertEqual(verify_report(sample_report(scenario, backend), scenario, backend), [])

    def test_negative_controls(self):
        def mutated(scenario, backend, change):
            report = sample_report(scenario, backend)
            change(report)
            return verify_report(report, scenario, backend)

        def case(report, name):
            return next(item for item in report["cases"] if item["case"] == name)

        controls = {
            "probe failure": ("legacy-compile-failure", "rd", lambda r: r["failures"].append("x")),
            "skipped steps": ("legacy-compile-failure", "rd", lambda r: r.update(steps_completed=8)),
            "headless posing as rd": ("legacy-compile-failure", "rd", lambda r: r.update(display_server="headless")),
            "rd without memory": ("modern-compile-failure", "rd", lambda r: r.update(memory_visible=False)),
            "headless claiming memory": ("modern-compile-failure", "headless",
                                         lambda r: r.update(memory_visible=True)),
            "wrong compile code": ("legacy-compile-failure", "rd",
                                   lambda r: case(r, "default RSkinGloss.fx").update(code=UPLOAD)),
            "variant not named": ("shadow-variant-compile-failure", "rd",
                                  lambda r: case(r, "shadow-receiving BatchMeshGloss.fx").update(
                                      message=adapter_message("BatchMeshGloss.fx", "sph_t1", "sph_t1_p0", 53))),
            "failure left in registry": ("legacy-compile-failure", "headless",
                                         lambda r: case(r, "default MeshGloss.fx opaque").update(registry_clean=False)),
            "failure not recorded": ("modern-compile-failure", "rd",
                                     lambda r: case(r, "modern undeclared identifier").update(recorded=False)),
            "partial RIDs leaked": ("modern-compile-failure", "rd",
                                    lambda r: case(r, "modern undeclared identifier").update(memory_restored=False)),
            "failed variant counted": ("shadow-variant-compile-failure", "rd",
                                       lambda r: case(r, "shadow-receiving RSkinGloss.fx").update(
                                           shadow_counters_unchanged=False)),
            "armed slot never read": ("legacy-compile-failure", "rd",
                                      lambda r: case(r, "default BatchMeshAlpha.fx").update(slot_reads=0)),
            "no recovery": ("legacy-compile-failure", "rd",
                            lambda r: case(r, "recovered MeshGloss.fx transparent").update(accepted=False)),
            "armed text broken unshadowed": ("shadow-variant-compile-failure", "rd",
                                             lambda r: case(r, "armed text compiles unshadowed RSkinGloss.fx").update(
                                                 accepted=False)),
            "fog declare accepted": ("fog-variant-compile-failure", "rd",
                                     lambda r: case(r, "fog declare failure BatchMeshAlpha").update(accepted=True)),
            "fog declaration kept": ("fog-variant-compile-failure", "rd",
                                     lambda r: case(r, "fog declare failure BatchMeshGloss").update(
                                         declarations_unchanged=False)),
            "fog enable left on": ("fog-variant-compile-failure", "rd",
                                   lambda r: case(r, "fog enable failure BatchMeshAlpha").update(
                                       fog_disabled_declaration_kept=False)),
            "fog default material lost": ("fog-variant-compile-failure", "rd",
                                          lambda r: case(r, "fog enable failure BatchMeshGloss").update(
                                              default_material_kept=False)),
            "fog not recovered": ("fog-variant-compile-failure", "rd",
                                  lambda r: case(r, "fog enable failure BatchMeshGloss").update(
                                      recovered_attached=False)),
            "fog variant over the sampler limit": ("fog-variant-compile-failure", "rd",
                                                   lambda r: case(r, "fog variant sampler limit").update(
                                                       accepted=True)),
            "binding refusal code": ("material-admission", "headless",
                                     lambda r: case(r, "modern unbound sampler").update(code=COMPILE)),
            "sampler limit not enforced": ("material-admission", "rd",
                                           lambda r: case(r, "modern six samplers").update(accepted=True)),
            "uniform block limit message": ("material-admission", "rd",
                                            lambda r: case(r, "modern uniform block over limit").update(message="x")),
            "refused admission left in registry": ("material-admission", "rd",
                                                   lambda r: case(r, "legacy float3 onto vec4").update(
                                                       registry_clean=False)),
            "boundary material refused": ("material-admission", "rd",
                                          lambda r: case(r, "modern five samplers").update(accepted=False)),
            "admitted materials undrawn": ("material-admission", "rd",
                                           lambda r: r.update(admitted_drawn_objects=3)),
            "fog skipped on rd": ("fog-variant-compile-failure", "rd", lambda r: r.update(skipped=FOG_SKIP)),
            "fog skip unexplained": ("fog-shadow-variant-failure", "headless", lambda r: r.update(skipped=None)),
            "fog shadow form message": ("fog-shadow-variant-failure", "rd",
                                        lambda r: case(r, "fog shadow no rewritable mode").update(message="x")),
            "production fog detached": ("production-adapters", "rd", lambda r: r.update(fog_attached=2)),
            "production variant missing": ("production-adapters", "rd",
                                           lambda r: case(r, "shadow-receiving BatchMeshAlpha.fx").update(
                                               shadow_receiving=4)),
            "production undrawn": ("production-adapters", "rd", lambda r: r.update(drawn_objects=9)),
            "teardown leak": ("upload-failure-kinds", "rd", lambda r: r.update(teardown_memory_restored=False)),
            "missing case": ("upload-failure-kinds", "headless", lambda r: r["cases"].pop()),
            "unavailable upload accepted": ("backend-unavailable", "rd",
                                            lambda r: case(r, "detached host").update(upload_refused=False)),
            "unavailable renderer allocated": ("backend-unavailable", "rd",
                                               lambda r: case(r, "detached host").update(memory_restored=False)),
        }
        for name, (scenario, backend, change) in controls.items():
            with self.subTest(name):
                self.assertNotEqual(mutated(scenario, backend, change), [])

    def test_engine_output_controls(self):
        compile_output = "\n".join([UNKNOWN, COMPILE_FAILED] * 5)
        self.assertEqual(check_engine_output(compile_output, "legacy-compile-failure", "rd")[0], [])
        self.assertNotEqual(check_engine_output(compile_output, "shadow-variant-compile-failure", "rd")[0], [])
        self.assertNotEqual(check_engine_output(
            "\n".join([UNKNOWN, COMPILE_FAILED] * 4), "legacy-compile-failure", "rd")[0], [])
        leak = "ERROR: 1 RID allocations of type 'N10RendererRD15MaterialStorage6ShaderE' were leaked at exit."
        self.assertNotEqual(check_engine_output(leak, "upload-failure-kinds", "rd")[0], [])
        self.assertNotEqual(check_engine_output("ERROR: anything", "upload-failure-kinds", "headless")[0], [])
        self.assertNotEqual(check_engine_output(UNKNOWN, "backend-unavailable", "rd")[0], [])

    def test_leak_line_parsing(self):
        mesh = "N10RendererRD11MeshStorage4MeshE"
        matches = leak_lines("x\nERROR: 1 RID allocations of type '%s' were leaked at exit.\n" % mesh)
        self.assertEqual([(m.group(1), m.group(2)) for m in matches], [("1", mesh)])


@unittest.skipUnless(os.environ.get("EAWR_GODOT_RENDERER_FAULT_RUNTIME_TEST"),
                     "set EAWR_GODOT_RENDERER_FAULT_RUNTIME_TEST to run the pinned renderer fault probe")
class GodotRendererFaultProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.backend = os.environ.get("EAWR_RENDERER_FAULT_BACKEND", "rd")
        if cls.backend not in BACKENDS:
            raise unittest.SkipTest("EAWR_RENDERER_FAULT_BACKEND must be rd or headless")
        cls.temporary = tempfile.TemporaryDirectory(prefix="eawr-renderer-fault-")
        library = os.environ.get("EAWR_RENDERER_FAULT_PROBE_LIBRARY")
        if not library or not pathlib.Path(library).is_file():
            raise AssertionError("EAWR_RENDERER_FAULT_PROBE_LIBRARY must name the probe")
        project = pathlib.Path(cls.temporary.name) / "project"
        shutil.copytree(PROJECT, project)
        (project / "bin").mkdir()
        shutil.copy2(library, project / "bin" / pathlib.Path(library).name)
        (project / ".godot").mkdir()
        (project / ".godot" / "extension_list.cfg").write_text(
            "res://eawr_renderer_fault_probe.gdextension\n", encoding="utf-8")
        cls.project = project

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_probe(self, name, extra):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        report_path = pathlib.Path(self.temporary.name) / ("%s-%s.json" % (self.backend, name))
        driver = ["--headless"] if self.backend == "headless" else ["--rendering-driver", "vulkan"]
        completed = subprocess.run(
            [executable] + driver + ["--audio-driver", "Dummy", "--path", str(self.project), "--",
                                     "--eawr-fault-report", str(report_path)] + extra,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=TIMEOUT_SECONDS)
        output = completed.stdout + "\n" + completed.stderr
        destination = os.environ.get("EAWR_RENDERER_FAULT_PROBE_OUTPUT")
        if destination:
            target = pathlib.Path(destination)
            target.mkdir(parents=True, exist_ok=True)
            (target / ("%s-%s.log" % (self.backend, name))).write_text(output, encoding="utf-8")
            if report_path.is_file():
                shutil.copy2(report_path, target / report_path.name)
        self.assertIsNotNone(HARNESS.engine_banner(completed.stdout, HARNESS.PINNED_GODOT_IDENTITY), output)
        self.assertTrue(report_path.is_file(), output)
        return completed, output, json.loads(report_path.read_text(encoding="utf-8"))

    def test_scenarios(self):
        for scenario in SCENARIOS:
            with self.subTest(scenario=scenario):
                completed, output, report = self.run_probe(scenario, ["--eawr-fault-scenario", scenario])
                self.assertEqual(completed.returncode, 0, output)
                self.assertIn("EAWR renderer fault probe passed (%s)" % scenario, completed.stdout)
                problems, _ = check_engine_output(output, scenario, self.backend)
                self.assertEqual(problems, [], output)
                self.assertEqual(verify_report(report, scenario, self.backend), [], json.dumps(report, indent=1))

    def test_leak_controls(self):
        # One unfreed RID per owner type must produce exactly one exit-time
        # leak line naming that owner; otherwise a clean scenario proves nothing.
        for owner, expected in LEAK_TYPES[self.backend].items():
            with self.subTest(owner=owner):
                completed, output, report = self.run_probe(
                    "leak-control-" + owner,
                    ["--eawr-fault-scenario", "leak-control", "--eawr-fault-leak-control", owner])
                self.assertEqual(completed.returncode, 0, output)
                self.assertEqual(report.get("leak_control"), owner)
                found = [(m.group(1), m.group(2)) for m in leak_lines(output)]
                self.assertEqual(found, [("1", expected)] if expected else [], output)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
