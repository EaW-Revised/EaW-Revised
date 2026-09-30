"""Production GodotRenderer resource-churn probe (#22).

The probe (tests/presentation/renderer/churn) is a separately built synthetic
GDExtension that drives the production renderer through shared references,
repeated scene switching, unavailable-asset replacement and recovery, a failed
upload after GPU allocation, skin-pose/asset lifetime, posed entity churn
and retirement (clear_skin_pose keeps the pose store bounded) and
shutdown/restart,
observing RenderingServer drawn objects and texture/buffer memory. This runner
verifies its report and the engine output, including Godot's exit-time RID
leak report, which a separate leak-control run proves is live.

The offline cases always run. The graphical run needs:
  EAWR_GODOT_RESOURCE_CHURN_RUNTIME_TEST=1
  EAWR_GODOT_EXECUTABLE=<pinned Godot 4.7.2 console binary>
  EAWR_RESOURCE_CHURN_PROBE_LIBRARY=<built libeawr_resource_churn_probe.* library>
  EAWR_RESOURCE_CHURN_PROBE_OUTPUT=<optional directory for reports and logs>
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
PROJECT = pathlib.Path(__file__).resolve().parent / "churn" / "project"
HARNESS_PATH = ROOT / "apps/viewer/tools/qualify_package_runtime.py"
HARNESS_SPEC = importlib.util.spec_from_file_location("qualify_package_runtime", HARNESS_PATH)
assert HARNESS_SPEC is not None and HARNESS_SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(HARNESS_SPEC)
HARNESS_SPEC.loader.exec_module(HARNESS)

TIMEOUT_SECONDS = 300
ALLOWED_WARNINGS = tuple(
    line.strip() for line in os.environ.get("EAWR_ALLOW_ENGINE_WARNINGS", "").splitlines()
    if line.strip())
LEAK_LINE = "ERROR: 1 RID allocations of type"
PASSED_MARKER = "EAWR resource churn probe passed"


def verify_report(report):
    """Return the report's contract failures (empty when it proves every case)."""
    failures = []
    if report.get("schema") != "eawr-resource-churn-probe-v1":
        failures.append("unknown schema")
    if report.get("status") != "resource_churn_probe_passed" or report.get("failures"):
        failures.append("probe reported failures: %r" % (report.get("failures"),))
    if report.get("steps_total", 0) == 0 or report.get("steps_completed") != report.get("steps_total"):
        failures.append("not every step ran")
    if report.get("rendering_method") != "forward_plus":
        failures.append("unexpected rendering method")
    memory = report.get("memory", {})
    baseline = memory.get("baseline", {})
    with_assets = memory.get("with_assets", {})
    for key in ("texture", "buffer"):
        if not isinstance(baseline.get(key), int) or not isinstance(with_assets.get(key), int):
            failures.append("missing %s memory" % key)
            continue
        if with_assets[key] <= baseline[key]:
            failures.append("uploaded assets not visible in %s memory" % key)
        for stage in ("after_shutdown", "after_restart"):
            if memory.get(stage, {}).get(key) != baseline[key]:
                failures.append("%s %s memory differs from the baseline" % (stage, key))
    if report.get("shared_references") != 2:
        failures.append("identical re-upload did not share one bundle")
    for flag in ("conflicting_upload_refused", "failed_upload_memory_restored",
                 "replacement_reads_back_new_mesh"):
        if report.get(flag) is not True:
            failures.append("%s not proven" % flag)
    if report.get("stale_pose_after_reupload") is not False:
        failures.append("a released asset's pose bound to its re-upload")
    churn = report.get("churn", {})
    if churn.get("switches", 0) < 300 or churn.get("full_scene_memory_drift") != 0:
        failures.append("scene-switch churn too short or memory drifted")
    posed = churn.get("posed_fresh_entities", 0)
    if posed < 2 * churn.get("cycles", 0) or posed == 0 or churn.get("posed_readback") != posed:
        failures.append("churn did not pose and read back every fresh skinned entity")
    if churn.get("max_skin_poses") != 2 or churn.get("skin_poses_after") != 0:
        failures.append("stored skin poses grew with retired churn entities")
    retire = report.get("pose_retire", {})
    if retire.get("posed", 0) == 0 or retire.get("kept_while_absent") != retire.get("posed"):
        failures.append("absent posed entities were not observed keeping their poses")
    if retire.get("after_clear") != 0:
        failures.append("clear_skin_pose left retired poses stored")
    if retire.get("cleared_live_reads_rest") is not True or retire.get("other_pose_kept") is not True:
        failures.append("clear_skin_pose did not return exactly the cleared live skeleton to rest")
    steady = report.get("steady_missing", {})
    if steady.get("history_tail_unchanged") is not True or steady.get("sentinel_retained") is not True:
        failures.append("steady missing-asset wait flooded the diagnostic history")
    live = report.get("shutdown_live", {})
    if not (live.get("instances", 0) >= 5 and live.get("skeletons", 0) >= 1 and live.get("red_references") == 2):
        failures.append("shutdown did not happen with live shared and skinned instances")
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
    return {
        "schema": "eawr-resource-churn-probe-v1",
        "status": "resource_churn_probe_passed",
        "rendering_method": "forward_plus",
        "memory": {
            "baseline": {"texture": 100, "buffer": 200},
            "with_assets": {"texture": 180, "buffer": 260},
            "churn_full": {"texture": 190, "buffer": 270},
            "after_shutdown": {"texture": 100, "buffer": 200},
            "after_restart": {"texture": 100, "buffer": 200},
        },
        "shared_references": 2,
        "conflicting_upload_refused": True,
        "failed_upload_memory_restored": True,
        "churn": {"cycles": 60, "switches": 360, "frames": 720, "full_scene_memory_drift": 0,
                  "posed_fresh_entities": 120, "posed_readback": 120, "max_skin_poses": 2,
                  "skin_poses_after": 0},
        "pose_retire": {"posed": 40, "kept_while_absent": 40, "after_clear": 0,
                        "cleared_live_reads_rest": True, "other_pose_kept": True},
        "steady_missing": {"frames": 92, "history_tail_unchanged": True, "sentinel_retained": True},
        "replacement_reads_back_new_mesh": True,
        "stale_pose_after_reupload": False,
        "shutdown_live": {"assets": 4, "instances": 5, "skeletons": 1, "skin_poses": 0, "red_references": 2},
        "steps_completed": 471,
        "steps_total": 471,
        "failures": [],
    }


class ResourceChurnReport(unittest.TestCase):
    def test_sample_passes(self):
        self.assertEqual(verify_report(sample_report()), [])

    def test_negative_controls(self):
        def mutated(change):
            report = copy.deepcopy(sample_report())
            change(report)
            return verify_report(report)

        controls = {
            "leaked texture": lambda r: r["memory"]["after_shutdown"].update(texture=101),
            "leaked buffer after restart": lambda r: r["memory"]["after_restart"].update(buffer=201),
            "invisible upload": lambda r: r["memory"]["with_assets"].update(texture=100),
            "stale content kept": lambda r: r.update(conflicting_upload_refused=False),
            "failed upload leak": lambda r: r.update(failed_upload_memory_restored=False),
            "old mesh drawn": lambda r: r.update(replacement_reads_back_new_mesh=False),
            "stale pose": lambda r: r.update(stale_pose_after_reupload=True),
            "memory drift": lambda r: r["churn"].update(full_scene_memory_drift=3),
            "short churn": lambda r: r["churn"].update(switches=12),
            "flooded history": lambda r: r["steady_missing"].update(history_tail_unchanged=False),
            "evicted sentinel": lambda r: r["steady_missing"].update(sentinel_retained=False),
            "idle shutdown": lambda r: r["shutdown_live"].update(instances=0),
            "unshared": lambda r: r.update(shared_references=1),
            "probe failure": lambda r: r["failures"].append("x"),
            "skipped steps": lambda r: r.update(steps_completed=470),
            "unbounded churn poses": lambda r: r["churn"].update(max_skin_poses=120),
            "retired churn pose left": lambda r: r["churn"].update(skin_poses_after=2),
            "unposed fresh entity": lambda r: r["churn"].update(posed_readback=119),
            "no fresh poses": lambda r: r["churn"].update(posed_fresh_entities=0, posed_readback=0),
            "retired pose kept after clear": lambda r: r["pose_retire"].update(after_clear=40),
            "absence growth unobserved": lambda r: r["pose_retire"].update(kept_while_absent=0),
            "cleared live pose kept": lambda r: r["pose_retire"].update(cleared_live_reads_rest=False),
            "clear touched another pose": lambda r: r["pose_retire"].update(other_pose_kept=False),
        }
        for name, change in controls.items():
            with self.subTest(name):
                self.assertNotEqual(mutated(change), [])

    def test_leak_lines_are_unexpected(self):
        line = "ERROR: 1 RID allocations of type 'N10RendererRD11MeshStorage4MeshE' were leaked at exit."
        self.assertEqual(unexpected_engine_lines(line), [line])


@unittest.skipUnless(os.environ.get("EAWR_GODOT_RESOURCE_CHURN_RUNTIME_TEST"),
                     "set EAWR_GODOT_RESOURCE_CHURN_RUNTIME_TEST to run the pinned graphical churn probe")
class GodotResourceChurnProbe(unittest.TestCase):
    def stage_project(self, temporary):
        library = os.environ.get("EAWR_RESOURCE_CHURN_PROBE_LIBRARY")
        self.assertTrue(library and pathlib.Path(library).is_file(),
                        "EAWR_RESOURCE_CHURN_PROBE_LIBRARY must name the probe")
        project = pathlib.Path(temporary) / "project"
        shutil.copytree(PROJECT, project)
        (project / "bin").mkdir()
        shutil.copy2(library, project / "bin" / pathlib.Path(library).name)
        # Register the extension as an editor import would, without the editor.
        (project / ".godot").mkdir()
        (project / ".godot" / "extension_list.cfg").write_text(
            "res://eawr_resource_churn_probe.gdextension\n", encoding="utf-8")
        return project

    def run_probe(self, temporary, name, extra):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        report_path = pathlib.Path(temporary) / ("%s.json" % name)
        completed = subprocess.run(
            [executable, "--path", str(pathlib.Path(temporary) / "project"), "--rendering-driver", "vulkan",
             "--", "--eawr-churn-report", str(report_path)] + extra,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=TIMEOUT_SECONDS)
        output = completed.stdout + "\n" + completed.stderr
        destination = os.environ.get("EAWR_RESOURCE_CHURN_PROBE_OUTPUT")
        if destination:
            target = pathlib.Path(destination)
            target.mkdir(parents=True, exist_ok=True)
            (target / ("%s.log" % name)).write_text(output, encoding="utf-8")
            if report_path.is_file():
                shutil.copy2(report_path, target / ("%s.json" % name))
        self.assertIsNotNone(HARNESS.engine_banner(completed.stdout, HARNESS.PINNED_GODOT_IDENTITY), output)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        return completed, output, report

    def test_churn_lifecycle_without_leaks(self):
        with tempfile.TemporaryDirectory(prefix="eawr-resource-churn-") as temporary:
            self.stage_project(temporary)
            completed, output, report = self.run_probe(temporary, "resource-churn", [])
            self.assertEqual(completed.returncode, 0, output)
            self.assertIn(PASSED_MARKER, completed.stdout)
            # Includes Godot's exit-time "RID allocations ... leaked" report.
            self.assertEqual(unexpected_engine_lines(output), [], output)
            self.assertEqual(verify_report(report), [], json.dumps(report, indent=1))

    def test_leak_control_is_reported_at_exit(self):
        # One deliberately unfreed RenderingServer mesh must produce Godot's
        # exit-time leak line; otherwise a clean run would prove nothing.
        with tempfile.TemporaryDirectory(prefix="eawr-resource-churn-leak-") as temporary:
            self.stage_project(temporary)
            completed, output, report = self.run_probe(
                temporary, "resource-churn-leak-control", ["--eawr-churn-leak-control"])
            self.assertIn(PASSED_MARKER, completed.stdout)
            self.assertTrue(report.get("leak_control"))
            leaks = [line.strip() for line in output.splitlines()
                     if line.strip().startswith(LEAK_LINE) and "leaked at exit" in line]
            self.assertEqual(len(leaks), 1, output)
            self.assertEqual(unexpected_engine_lines(output), leaks, output)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
