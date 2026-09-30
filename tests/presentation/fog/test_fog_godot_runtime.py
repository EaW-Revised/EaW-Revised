"""Isolated Godot exercise for the fog-stub-v1 texture adapter.

The probe GDExtension (tests/presentation/fog/godot) drives the real
GodotFogBackend through fog::TextureCache: it reads textures back from the
RenderingServer and renders the synthetic nearest shader. Each run copies the
synthetic project into a temporary directory, so neither the source tree nor
any live editor project is touched. The graphical run needs:

  EAWR_GODOT_FOG_RUNTIME_TEST=1
  EAWR_GODOT_EXECUTABLE=<pinned Godot 4.7.2 console binary>
  EAWR_FOG_PROBE_LIBRARY=<built libeawr_fog_probe.* library>
  EAWR_FOG_PROBE_OUTPUT=<optional directory that receives report and log>

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
PROJECT = pathlib.Path(__file__).resolve().parent / "godot" / "project"
HARNESS_PATH = ROOT / "apps/viewer/tools/qualify_package_runtime.py"
HARNESS_SPEC = importlib.util.spec_from_file_location("qualify_package_runtime", HARNESS_PATH)
assert HARNESS_SPEC is not None and HARNESS_SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(HARNESS_SPEC)
HARNESS_SPEC.loader.exec_module(HARNESS)

TIMEOUT_SECONDS = 300
ALLOWED_WARNINGS = tuple(
    line.strip() for line in os.environ.get("EAWR_ALLOW_ENGINE_WARNINGS", "").splitlines()
    if line.strip())

# Cumulative cache counters the probe must reach at each stage.
EXPECTED_STAGES = {
    "unbound-consumer": {"uploads": 0, "binds": 0, "live_textures": 0},
    "first-upload": {"uploads": 1, "upload_bytes": 6, "creates": 1, "binds": 1, "live_textures": 1},
    "identical-and-revision-only": {"uploads": 1, "upload_bytes": 6, "binds": 1, "metadata_only": 1},
    "origin-shift": {"uploads": 1, "upload_bytes": 6, "binds": 2, "metadata_only": 2},
    "cell-change": {"uploads": 2, "upload_bytes": 12, "updates": 1, "creates": 1, "metadata_only": 3},
    "rejected-revisions": {"uploads": 2, "upload_bytes": 12, "rejected": 2},
    "missing-team": {"uploads": 2, "rejected": 3, "unbinds": 1, "live_textures": 1},
    "backend-failure": {"uploads": 2, "rejected": 4, "reselects": 1, "live_textures": 1},
    "recreate": {"uploads": 3, "upload_bytes": 24, "creates": 2, "recreates": 1, "destroys": 1,
                 "live_textures": 1},
    "late-consumer": {"uploads": 3},
    "team-7": {"uploads": 4, "upload_bytes": 30, "creates": 3, "live_textures": 2},
    "team-switch-back": {"uploads": 4, "upload_bytes": 30, "reselects": 2, "live_textures": 2},
    "reset": {"destroys": 3, "live_textures": 0},
    "before-teardown": {"uploads": 5, "upload_bytes": 36, "creates": 4, "live_textures": 1},
}
EXPECTED_RENDERS = {
    "unbound-consumer": "dark",
    "first-upload": "team 0 revision 1",
    "origin-shift": "team 0 revision 3",
    "cell-change": "team 0 revision 5",
    "rejected-revisions": "team 0 revision 5",
    "missing-team": "dark",
    "backend-failure": "team 0 revision 5",
    "recreate": "team 0 revision 7",
    "late-consumer": "team 0 revision 7",
    "team-switch": "team 7 revision 1",
    "team-switch-back": "team 0 revision 7",
    "reset": "dark",
    "teardown": "dark",
}


def verify_report(report):
    """Return a list of failures for a probe report."""
    failures = []
    if report.get("schema") != "eawr-fog-godot-probe-v1":
        failures.append("schema")
    if report.get("status") != "fog_godot_probe_passed":
        failures.append(f"status {report.get('status')!r}")
    if report.get("failures"):
        failures.append(f"probe failures {report.get('failures')!r}")
    if report.get("cleanup_order_verified") is not True:
        failures.append("fog resources were not released before their backend and material references")
    if report.get("steps_completed") != report.get("steps_total") or not report.get("steps_total"):
        failures.append("not every step completed")
    if report.get("rendering_method") in (None, "", "headless", "dummy"):
        failures.append("not a graphical renderer")
    transfer = report.get("output_transfer", [])
    if len(transfer) != 256 or transfer[0] != 0 or transfer[255] != 255 \
            or any(b < a for a, b in zip(transfer, transfer[1:])):
        failures.append("output transfer calibration is missing or not monotonic")
    memory = report.get("texture_memory", {})
    if not memory.get("with_fog", 0) > memory.get("baseline", 0):
        failures.append("fog texture not observed in texture memory")
    if memory.get("after_teardown") != memory.get("baseline"):
        failures.append("texture memory did not return to baseline")
    stages = {stage.get("stage"): stage for stage in report.get("stages", [])}
    for name, expected in EXPECTED_STAGES.items():
        stage = stages.get(name)
        if stage is None:
            failures.append(f"missing stage {name}")
            continue
        for key, value in expected.items():
            if stage.get(key) != value:
                failures.append(f"stage {name}.{key}: {stage.get(key)!r} != {value!r}")
    renders = {render.get("stage"): render for render in report.get("renders", [])}
    for name, expected in EXPECTED_RENDERS.items():
        render = renders.get(name)
        if render is None:
            failures.append(f"missing render {name}")
            continue
        if render.get("expected") != expected:
            failures.append(f"render {name} showed {render.get('expected')!r}, not {expected!r}")
        if render.get("mismatches") != 0 or not render.get("compared", 0) > 8000:
            failures.append(f"render {name}: {render.get('mismatches')!r} mismatches of {render.get('compared')!r}")
        for sample in render.get("cell_samples", []):
            if abs(sample.get("measured_r", -99) - sample.get("expected_output", 99)) > 1:
                failures.append(f"render {name} cell {sample.get('cell')} sample")
    # Every rendered cell centre of the asymmetric grid is distinct, so a
    # transposed or flipped mapping cannot pass.
    first = renders.get("first-upload", {}).get("cell_samples", [])
    if [sample.get("cell_byte") for sample in first] != [10, 60, 110, 160, 210, 255]:
        failures.append("first-upload cell order is not row-major source +X, +Y")
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
    stages = [dict({"stage": name}, **values) for name, values in EXPECTED_STAGES.items()]
    renders = [{"stage": name, "expected": expected, "compared": 9000, "mismatches": 0, "cell_samples": []}
               for name, expected in EXPECTED_RENDERS.items()]
    renders[1]["cell_samples"] = [{"cell_byte": value, "measured_r": 0, "expected_output": 0}
                                  for value in (10, 60, 110, 160, 210, 255)]
    return {"schema": "eawr-fog-godot-probe-v1", "status": "fog_godot_probe_passed", "failures": [],
            "steps_completed": 19, "steps_total": 19, "rendering_method": "forward_plus",
            "output_transfer": list(range(256)),
            "cleanup_order_verified": True,
            "texture_memory": {"baseline": 100, "with_fog": 108, "after_teardown": 100},
            "stages": stages, "renders": renders}


class ReportVerifier(unittest.TestCase):
    def test_sample_passes(self):
        self.assertEqual(verify_report(sample_report()), [])

    def test_negative_controls(self):
        mutations = [
            lambda r: r["renders"][2].__setitem__("mismatches", 1),
            lambda r: r["renders"][5].__setitem__("expected", "team 0 revision 5"),
            lambda r: r["stages"][2].__setitem__("uploads", 2),
            lambda r: r["stages"][4].__setitem__("upload_bytes", 18),
            lambda r: r["texture_memory"].__setitem__("after_teardown", 108),
            lambda r: r["renders"][1]["cell_samples"].reverse(),
            lambda r: r["renders"][1]["cell_samples"][0].__setitem__("measured_r", 3),
            lambda r: r.__setitem__("steps_completed", 18),
            lambda r: r["output_transfer"].__setitem__(10, 0),
            lambda r: r.__setitem__("rendering_method", "dummy"),
            lambda r: r["stages"].pop(),
        ]
        for index, mutate in enumerate(mutations):
            report = copy.deepcopy(sample_report())
            mutate(report)
            self.assertNotEqual(verify_report(report), [], f"mutation {index} was accepted")


@unittest.skipUnless(os.environ.get("EAWR_GODOT_FOG_RUNTIME_TEST"),
                     "set EAWR_GODOT_FOG_RUNTIME_TEST to run the pinned graphical fog probe")
class GodotFogProbe(unittest.TestCase):
    def test_upload_readback_render_and_cleanup(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        library = os.environ.get("EAWR_FOG_PROBE_LIBRARY")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        self.assertTrue(library and pathlib.Path(library).is_file(), "EAWR_FOG_PROBE_LIBRARY must name the probe")
        with tempfile.TemporaryDirectory(prefix="eawr-fog-probe-") as temporary:
            project = pathlib.Path(temporary) / "project"
            shutil.copytree(PROJECT, project)
            (project / "bin").mkdir()
            shutil.copy2(library, project / "bin" / pathlib.Path(library).name)
            report_path = pathlib.Path(temporary) / "report.json"

            # Register the extension in the temporary copy exactly as an editor
            # import would, without starting the editor (the project has no
            # imported assets). Editor mode is never used by this exercise.
            (project / ".godot").mkdir()
            (project / ".godot" / "extension_list.cfg").write_text(
                "res://eawr_fog_probe.gdextension\n", encoding="utf-8")

            completed = subprocess.run(
                [executable, "--path", str(project), "--rendering-driver", "vulkan", "--",
                 "--eawr-fog-report", str(report_path)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
                timeout=TIMEOUT_SECONDS)
            output = completed.stdout + "\n" + completed.stderr
            destination = os.environ.get("EAWR_FOG_PROBE_OUTPUT")
            if destination:
                target = pathlib.Path(destination)
                target.mkdir(parents=True, exist_ok=True)
                (target / "fog-probe.log").write_text(output, encoding="utf-8")
                if report_path.is_file():
                    shutil.copy2(report_path, target / "fog-probe-report.json")
            self.assertEqual(completed.returncode, 0, output)
            self.assertIsNotNone(HARNESS.engine_banner(completed.stdout, HARNESS.PINNED_GODOT_IDENTITY), output)
            self.assertEqual(unexpected_engine_lines(output), [], output)
            self.assertEqual([line for line in completed.stdout.splitlines() if line.startswith("EAWR fog probe ")],
                             [line for line in completed.stdout.splitlines() if line.startswith("EAWR fog probe passed")])
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(verify_report(report), [], json.dumps(report, indent=1))

            # Force frame-budget exhaustion after the late consumer has
            # rendered. The expected exit is failure, but cleanup must still
            # release the cache before its dependencies.
            early_report_path = pathlib.Path(temporary) / "early-report.json"
            early = subprocess.run(
                [executable, "--path", str(project), "--rendering-driver", "vulkan", "--",
                 "--eawr-fog-report", str(early_report_path), "--eawr-fog-test-frame-budget"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
                timeout=TIMEOUT_SECONDS)
            early_output = early.stdout + "\n" + early.stderr
            self.assertEqual(early.returncode, 1, early_output)
            self.assertIsNotNone(HARNESS.engine_banner(early.stdout, HARNESS.PINNED_GODOT_IDENTITY), early_output)
            self.assertEqual(unexpected_engine_lines(early_output), [], early_output)
            early_report = json.loads(early_report_path.read_text(encoding="utf-8"))
            self.assertEqual(early_report.get("status"), "fog_godot_probe_failed")
            self.assertEqual(early_report.get("failures"), ["frame budget exhausted"])
            self.assertIs(early_report.get("cleanup_order_verified"), True, json.dumps(early_report, indent=1))
            self.assertLess(early_report.get("steps_completed", 0), early_report.get("steps_total", 0))
            early_stages = {stage.get("stage"): stage for stage in early_report.get("stages", [])}
            self.assertIn("late-consumer", early_stages)
            self.assertEqual(early_stages["late-consumer"].get("binds"),
                             early_stages["recreate"].get("binds", -2) + 1)
            early_renders = {render.get("stage"): render for render in early_report.get("renders", [])}
            self.assertEqual(early_renders.get("late-consumer", {}).get("expected"), "team 0 revision 7")
            self.assertEqual(early_renders.get("late-consumer", {}).get("mismatches"), 0)


if __name__ == "__main__":
    unittest.main()
