"""WP-43 legacy/ family adapters through the production GodotRenderer.

The probe (tests/presentation/renderer/legacy) is a separately built synthetic
GDExtension. It draws invented quads with MeshAdditive, MeshAdditiveOffset,
MeshSolidColor and MeshGlossColorize, samples fixed pixels, and records the
fail-closed upload diagnostics and the shadow-variant policy. This runner
holds that report to each family's declared arithmetic (stored 8-bit values,
tolerance 3 per channel for predicted values, 1 for equal-scene controls).
Nothing here is a retail comparison.

The offline cases always run. The graphical run needs:
  EAWR_GODOT_LEGACY_FAMILY_RUNTIME_TEST=1
  EAWR_GODOT_EXECUTABLE=<pinned Godot 4.7.2 console binary>
  EAWR_LEGACY_FAMILY_PROBE_LIBRARY=<built libeawr_legacy_family_probe.* library>
  EAWR_LEGACY_FAMILY_PROBE_OUTPUT=<optional directory for the report, PNGs and log>
"""

import copy
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
PROJECT = pathlib.Path(__file__).resolve().parent / "legacy" / "project"
HARNESS_PATH = ROOT / "apps/viewer/tools/qualify_package_runtime.py"
HARNESS_SPEC = importlib.util.spec_from_file_location("qualify_package_runtime", HARNESS_PATH)
assert HARNESS_SPEC is not None and HARNESS_SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(HARNESS_SPEC)
HARNESS_SPEC.loader.exec_module(HARNESS)

TIMEOUT_SECONDS = 300
PREDICTED = 3.0
EQUAL = 1.0
PASSED_MARKER = "EAWR legacy family probe passed"
ALLOWED_WARNINGS = tuple(
    line.strip() for line in os.environ.get("EAWR_ALLOW_ENGINE_WARNINGS", "").splitlines()
    if line.strip())

POINTS = ("tl", "tr", "bl", "br", "outside")
QUADRANTS = ("tl", "tr", "bl", "br")
# The probe's invented inputs (stored 8-bit values).
BACKDROP = (40.0, 60.0, 80.0)
ADDITIVE_TEXELS = {"tl": (200, 100, 50), "tr": (100, 180, 30), "bl": (20, 60, 160), "br": (150, 40, 120)}
ADDITIVE_COLOR = (0.5, 0.75, 1.0)
FRONT_SOLID = 0.2 * 255.0
LAYER_TEXEL = 100.0
# UVOffset (0.5, 0) moves each screen quadrant to its horizontal neighbour.
OFFSET_SOURCE = {"tl": "tr", "tr": "tl", "bl": "br", "br": "bl"}

REJECTIONS = {
    "additive_fixed_function": "'MeshAdditive.fx' has no Godot adapter for technique 't1' (supported: 't0')",
    "additive_opaque_pass": "'MeshAdditive.fx' cannot draw in the opaque render pass",
    "additive_scalar_color": "binds parameter 'Color' as a scalar",
    "offset_texture_offset": "binds parameter 'UVOffset' as a texture",
    "solid_transparent_pass": "'MeshSolidColor.fx' cannot draw in the transparent render pass",
    "colorize_distinct_gloss": "gate MULTITEX-01",
    "colorize_fixed_function": "technique 'sph_t1' (supported: 'sph_t0')",
    "aldefault": "unknown legacy material family 'alDefault.fx'",
}
CAPTURES = (
    "empty", "backdrop", "solid", "additive", "additive_zero_rate", "additive_defaults", "additive_culled",
    "additive_layers", "occluder_only", "additive_occluded", "solid_over_additive", "offset", "offset_zero",
    "gloss_reference", "colorize_white", "colorize_black", "colorize_specular", "additive_shadows_on",
)


def add(destination, texel, color=(1.0, 1.0, 1.0)):
    """Stored ONE/ONE result: destination + texel x saturate(colour), saturated."""
    return tuple(min(255.0, d + t * min(1.0, max(0.0, c))) for d, t, c in zip(destination, texel, color))


def within(actual, expected, tolerance):
    return actual is not None and len(actual) == 3 and all(
        abs(a - e) <= tolerance for a, e in zip(actual, expected))


def verify_report(report):
    """Return the report's contract failures (empty when it proves every case)."""
    failures = []
    if report.get("schema") != "eawr-legacy-family-probe-v1":
        failures.append("unknown schema")
    if report.get("status") != "legacy_family_probe_passed" or report.get("failures"):
        failures.append("probe reported failures: %r" % (report.get("failures"),))
    if report.get("steps_total", 0) == 0 or report.get("steps_completed") != report.get("steps_total"):
        failures.append("not every step ran")
    if report.get("rendering_method") != "forward_plus":
        failures.append("unexpected rendering method")
    if report.get("shadow_receiving_materials") != 1:
        failures.append("exactly the lit MeshGlossColorize must compile as a shadow-receiving variant")
    if report.get("shadow_variant_failures") != 0:
        failures.append("an unlit family was counted as a shadow-variant failure")
    if report.get("resources_after_rejections") != 0:
        failures.append("a rejected upload left a resource")
    rejections = report.get("rejections", {})
    for label, fragment in REJECTIONS.items():
        message = rejections.get(label, "")
        if not message.startswith("EAWR-RENDER-0001 ") or fragment not in message:
            failures.append("%s did not fail closed as %r: %r" % (label, fragment, message))
    if report.get("submitted_passes") != {"MeshSolidColor.fx": "opaque", "MeshAdditive.fx": "transparent"}:
        failures.append("MeshSolidColor/MeshAdditive were not submitted in the opaque/transparent passes")

    captures = report.get("captures", {})
    for name in CAPTURES:
        if set(captures.get(name, {})) != set(POINTS):
            failures.append("capture %s is missing sampled points" % name)
    if failures:
        return failures

    def point(name, label):
        return tuple(captures[name][label])

    def expect(name, label, expected, tolerance, why):
        if not within(point(name, label), expected, tolerance):
            failures.append("%s %s: %s; got %r, expected %r" % (name, label, why, point(name, label), expected))

    def same(name, other, labels, why):
        for label in labels:
            expect(name, label, point(other, label), EQUAL, why + " (equal to %s)" % other)

    for label in POINTS:
        expect("backdrop", label, BACKDROP, PREDICTED, "MeshSolidColor writes its stored colour")
    for label in ("tl", "bl"):
        expect("solid", label, (255.0, 0.0, 127.5), PREDICTED, "over-unit float4 Color is saturated")
    for label in ("tr", "br"):
        expect("solid", label, (63.75, 127.5, 191.25), PREDICTED, "a float3 Color is written as stored")
    for label in QUADRANTS:
        texel = ADDITIVE_TEXELS[label]
        expect("additive", label, add(BACKDROP, texel, ADDITIVE_COLOR), PREDICTED,
               "ONE/ONE adds texel x Color.rgb; texture alpha is ignored")
        expect("additive_defaults", label, add(BACKDROP, texel), PREDICTED, "an unauthored Color is white")
        expect("solid_over_additive", label, add((FRONT_SOLID,) * 3, texel, ADDITIVE_COLOR), PREDICTED,
               "MeshSolidColor writes no depth, so the additive draw behind it still adds")
        expect("offset", label, add(BACKDROP, ADDITIVE_TEXELS[OFFSET_SOURCE[label]]), PREDICTED,
               "UVOffset.xy shifts the texture coordinate")
        expect("offset_zero", label, add(BACKDROP, texel), PREDICTED, "a zero UVOffset leaves the texture")
        expect("additive_layers", label, add(BACKDROP, (2 * LAYER_TEXEL,) * 3, (0.5,) * 3), PREDICTED,
               "both layers of one draw add because MeshAdditive writes no depth")
    same("additive_zero_rate", "additive", POINTS, "TIME is fixed at 0, so UVScrollRate cannot move a capture")
    same("additive_culled", "backdrop", POINTS, "the reversed winding is back-face culled")
    same("additive_shadows_on", "additive", POINTS, "an unlit family is unchanged under shadows")
    same("additive_occluded", "occluder_only", ("tl", "bl"), "depth-writing geometry in front hides the additive draw")
    same("additive_occluded", "additive", ("tr", "br"), "the unoccluded additive half is unchanged")
    if within(point("occluder_only", "tl"), point("additive", "tl"), 20.0):
        failures.append("occluder control does not discriminate from the additive draw")
    if within(point("additive_layers", "tl"), add(BACKDROP, (LAYER_TEXEL,) * 3, (0.5,) * 3), 20.0):
        failures.append("layer control does not discriminate a depth-writing draw")
    same("colorize_white", "gloss_reference", QUADRANTS,
         "white Colorization with zero Specular reproduces the accepted MeshGloss adapter")
    same("colorize_black", "gloss_reference", ("tl",), "texture alpha 0 leaves the texel uncolorized")
    for label in ("tr", "bl", "br"):
        expect("colorize_black", label, (0.0, 0.0, 0.0), PREDICTED,
               "texture alpha 1 multiplies by a black Colorization before lighting")
    same("colorize_specular", "colorize_white", ("bl",), "a zero gloss mask removes specular")
    for label in ("tl", "tr", "br"):
        if min(point("colorize_specular", label)) < 250.0:
            failures.append("colorize_specular %s: a full gloss mask must add the peak specular" % label)
    for name in ("solid", "gloss_reference"):
        expect(name, "outside", point("empty", "outside"), EQUAL, "nothing is drawn outside the quads")
    return failures


def sample_report():
    captures = {}

    def uniform(value):
        return {label: list(value) for label in POINTS}

    captures["empty"] = uniform((18.0, 22.0, 33.0))
    captures["backdrop"] = uniform(BACKDROP)
    captures["solid"] = dict(uniform((18.0, 22.0, 33.0)), tl=[255.0, 0.0, 127.0], bl=[255.0, 0.0, 127.0],
                             tr=[64.0, 127.0, 191.0], br=[64.0, 127.0, 191.0])
    additive = {label: list(add(BACKDROP, ADDITIVE_TEXELS[label], ADDITIVE_COLOR)) for label in QUADRANTS}
    defaults = {label: list(add(BACKDROP, ADDITIVE_TEXELS[label])) for label in QUADRANTS}
    captures["additive"] = dict(additive, outside=list(BACKDROP))
    captures["additive_zero_rate"] = copy.deepcopy(captures["additive"])
    captures["additive_shadows_on"] = copy.deepcopy(captures["additive"])
    captures["additive_defaults"] = dict(defaults, outside=list(BACKDROP))
    captures["offset_zero"] = copy.deepcopy(captures["additive_defaults"])
    captures["offset"] = dict({label: defaults[OFFSET_SOURCE[label]] for label in QUADRANTS}, outside=list(BACKDROP))
    captures["additive_culled"] = uniform(BACKDROP)
    captures["additive_layers"] = dict(uniform((140.0, 160.0, 180.0)), outside=list(BACKDROP))
    occluder = [61.0, 59.0, 255.0]
    captures["occluder_only"] = dict(uniform(BACKDROP), tl=occluder, bl=occluder)
    captures["additive_occluded"] = dict(captures["additive"], tl=occluder, bl=occluder)
    captures["solid_over_additive"] = dict(
        {label: list(add((FRONT_SOLID,) * 3, ADDITIVE_TEXELS[label], ADDITIVE_COLOR)) for label in QUADRANTS},
        outside=[18.0, 22.0, 33.0])
    reference = {"tl": [170.0, 114.0, 56.0], "tr": [170.0, 114.0, 56.0], "bl": [0.0, 152.0, 85.0],
                 "br": [242.0, 95.0, 46.0], "outside": [18.0, 22.0, 33.0]}
    captures["gloss_reference"] = reference
    captures["colorize_white"] = copy.deepcopy(reference)
    captures["colorize_black"] = dict(reference, tr=[0.0, 0.0, 0.0], bl=[0.0, 0.0, 0.0], br=[0.0, 0.0, 0.0])
    captures["colorize_specular"] = dict(reference, tl=[255.0] * 3, tr=[255.0] * 3, br=[255.0] * 3)
    return {
        "schema": "eawr-legacy-family-probe-v1",
        "status": "legacy_family_probe_passed",
        "rendering_method": "forward_plus",
        "steps_completed": 37,
        "steps_total": 37,
        "shadow_receiving_materials": 1,
        "shadow_variant_failures": 0,
        "resources_after_rejections": 0,
        "rejections": {label: "EAWR-RENDER-0001 legacy material family x " + fragment
                       for label, fragment in REJECTIONS.items()},
        "submitted_passes": {"MeshSolidColor.fx": "opaque", "MeshAdditive.fx": "transparent"},
        "captures": captures,
        "failures": [],
    }


def unexpected_engine_lines(text):
    unexpected = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line in ALLOWED_WARNINGS:
            continue
        if HARNESS.is_fatal_diagnostic(line) or line.startswith(HARNESS.ENGINE_DIAGNOSTIC_PREFIXES):
            unexpected.append(line)
    return unexpected


class LegacyFamilyReport(unittest.TestCase):
    def test_sample_passes(self):
        self.assertEqual(verify_report(sample_report()), [])

    def test_negative_controls(self):
        def mutated(change):
            report = copy.deepcopy(sample_report())
            change(report)
            return verify_report(report)

        def set_point(name, label, value):
            return lambda r: r["captures"][name].__setitem__(label, list(value))

        controls = {
            "alpha scales additive RGB": set_point("additive", "tr", BACKDROP),
            "linear colour policy": set_point("additive", "tl", (157.0, 150.0, 130.0)),
            "unclamped solid": set_point("solid", "tl", (255.0, 0.0, 200.0)),
            "additive depth write": set_point("additive_layers", "tl", (90.0, 110.0, 130.0)),
            "solid depth write": set_point("solid_over_additive", "tl", (51.0, 51.0, 51.0)),
            "occluder ignored": set_point("additive_occluded", "tl", (140.0, 135.0, 130.0)),
            "time scrolls": set_point("additive_zero_rate", "tl", (90.0, 195.0, 110.0)),
            "winding drawn": set_point("additive_culled", "tl", (140.0, 135.0, 130.0)),
            "offset ignored": set_point("offset", "tl", (240.0, 160.0, 130.0)),
            "colorize differs from MeshGloss": set_point("colorize_white", "br", (230.0, 95.0, 46.0)),
            "colorization ignored": set_point("colorize_black", "tr", (170.0, 114.0, 56.0)),
            "gloss ignored": set_point("colorize_specular", "bl", (255.0, 255.0, 255.0)),
            "specular missing": set_point("colorize_specular", "br", (242.0, 95.0, 46.0)),
            "shadow changed unlit": set_point("additive_shadows_on", "br", (100.0, 80.0, 180.0)),
            "unlit counted as receiver": lambda r: r.update(shadow_receiving_materials=4),
            "variant failure": lambda r: r.update(shadow_variant_failures=3),
            "leaked rejection": lambda r: r.update(resources_after_rejections=1),
            "gloss gate accepted": lambda r: r["rejections"].update(colorize_distinct_gloss="accepted"),
            "alDefault accepted": lambda r: r["rejections"].pop("aldefault"),
            "wrong pass": lambda r: r["submitted_passes"].update({"MeshAdditive.fx": "opaque"}),
            "missing capture": lambda r: r["captures"].pop("offset"),
            "probe failure": lambda r: r["failures"].append("x"),
            "skipped steps": lambda r: r.update(steps_completed=36),
        }
        for name, change in controls.items():
            with self.subTest(name):
                self.assertNotEqual(mutated(change), [])

    def test_probe_is_standalone_and_synthetic(self):
        source = (ROOT / "tests/presentation/renderer/legacy/legacy_family_probe.cpp").read_text(encoding="utf-8")
        self.assertIn("invented", source)
        for token in ("std::filesystem", "EAWR_EAW_GAME_ROOT", "SteamLibrary", ".meg"):
            self.assertNotIn(token, source)
        build = (ROOT / "tests/presentation/renderer/legacy/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("src/presentation/godot/renderer.cpp", build)
        self.assertIn("/W4 /WX", build)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_LEGACY_FAMILY_RUNTIME_TEST"),
                     "set EAWR_GODOT_LEGACY_FAMILY_RUNTIME_TEST to run the pinned graphical legacy family probe")
class GodotLegacyFamilyProbe(unittest.TestCase):
    def test_families_draw_their_declared_arithmetic(self):
        library = os.environ.get("EAWR_LEGACY_FAMILY_PROBE_LIBRARY")
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(library and pathlib.Path(library).is_file(),
                        "EAWR_LEGACY_FAMILY_PROBE_LIBRARY must name the probe")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        with tempfile.TemporaryDirectory(prefix="eawr-legacy-family-") as temporary:
            project = pathlib.Path(temporary) / "project"
            shutil.copytree(PROJECT, project)
            (project / "bin").mkdir()
            shutil.copy2(library, project / "bin" / pathlib.Path(library).name)
            (project / ".godot").mkdir()
            (project / ".godot" / "extension_list.cfg").write_text(
                "res://eawr_legacy_family_probe.gdextension\n", encoding="utf-8")
            report_path = pathlib.Path(temporary) / "legacy-family.json"
            images = pathlib.Path(temporary) / "png"
            images.mkdir()
            completed = subprocess.run(
                [executable, "--path", str(project), "--rendering-driver", "vulkan", "--",
                 "--eawr-legacy-report", str(report_path), "--eawr-legacy-output", str(images)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=TIMEOUT_SECONDS)
            output = completed.stdout + "\n" + completed.stderr
            destination = os.environ.get("EAWR_LEGACY_FAMILY_PROBE_OUTPUT")
            if destination:
                target = pathlib.Path(destination)
                target.mkdir(parents=True, exist_ok=True)
                (target / "legacy-family.log").write_text(output, encoding="utf-8")
                if report_path.is_file():
                    shutil.copy2(report_path, target / "legacy-family.json")
                for image in images.glob("*.png"):
                    shutil.copy2(image, target / image.name)
            self.assertIsNotNone(HARNESS.engine_banner(completed.stdout, HARNESS.PINNED_GODOT_IDENTITY), output)
            self.assertEqual(completed.returncode, 0, output)
            self.assertIn(PASSED_MARKER, completed.stdout)
            self.assertEqual(unexpected_engine_lines(output), [], output)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(verify_report(report), [], json.dumps(report, indent=1))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
