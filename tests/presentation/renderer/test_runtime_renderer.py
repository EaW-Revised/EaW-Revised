import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
# The exported-package harness owns the runtime report contract; this
# source-project run is held to the same checks. It is not package evidence.
HARNESS_PATH = ROOT / "apps/viewer/tools/qualify_package_runtime.py"
HARNESS_SPEC = importlib.util.spec_from_file_location("qualify_package_runtime", HARNESS_PATH)
assert HARNESS_SPEC is not None and HARNESS_SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(HARNESS_SPEC)
HARNESS_SPEC.loader.exec_module(HARNESS)


# Exact host-specific WARNING: lines to waive (for example WSLg V-Sync), one
# per line, mirroring the package harness's --allow-engine-warning.
ALLOWED_WARNINGS = tuple(
    line.strip() for line in os.environ.get("EAWR_ALLOW_ENGINE_WARNINGS", "").splitlines()
    if line.strip())


LEGACY_SELECTOR_MARKER = (
    "EAWR runtime: unknown legacy family, technique, pass and render pass and a non-spatial"
    " modern source rejected; comment-led spatial source compiled; registry unchanged")


class RuntimeRendererExercise(unittest.TestCase):
    def run_viewer(self, executable, report):
        # Separate streams keep engine lines whole; stdout then stderr is the
        # same combination the package harness scans.
        completed = subprocess.run(
            [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
             "--eawr-renderer-runtime-test", "--eawr-report", str(report)],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            timeout=HARNESS.DEFAULT_TIMEOUT_SECONDS,
        )
        return completed, completed.stdout + "\n" + completed.stderr

    def executable(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot console binary")
        return executable

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_compiler_pass_routing_and_rid_churn(self):
        with tempfile.TemporaryDirectory(prefix="eawr-renderer-runtime-") as temporary:
            report = pathlib.Path(temporary) / "runtime.json"
            completed, output = self.run_viewer(self.executable(), report)
            self.assertEqual(completed.returncode, 0, output)
            self.assertIn("SHADER ERROR: Unknown identifier", output)
            missing, unexpected = HARNESS.scan_engine_output(output, ALLOWED_WARNINGS)
            self.assertEqual(missing, [], output)
            self.assertEqual(unexpected, [], output)
            # The editor binary is the official build on every host.
            self.assertIsNotNone(HARNESS.engine_banner(completed.stdout, HARNESS.PINNED_GODOT_IDENTITY))
            # Proves the live unknown route/pass rejections ran.
            self.assertEqual(HARNESS.missing_runtime_markers(completed.stdout), [], output)
            # Source-project only (not a package marker): live legacy family/
            # technique/pass/render-pass and non-spatial rejections, and the
            # comment-led spatial source compiled through the anchored probe.
            self.assertIn(LEGACY_SELECTOR_MARKER, completed.stdout)
            self.assertIn("EAWR runtime: shader cache binding admission, byte and entry budgets passed",
                          completed.stdout)
            self.assertEqual(output.count("SHADER ERROR"), 1, output)
            result = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(HARNESS.verify_runtime_report(result), [])
            self.assertEqual(result["status"], "renderer_runtime_exercise_passed")
            self.assertTrue(result["renderer_runtime_exercise"])
            self.assertTrue(result["runtime_capture_verified"])
            self.assertTrue(result["runtime_overlap_verified"])
            self.assertTrue(result["runtime_post_dependency_verified"])
            # Also proves the live unknown material route/pass rejections.
            self.assertTrue(result["runtime_failed_upload_registry_clean"])
            self.assertGreaterEqual(result["runtime_scene_switch_count"], 40)
            self.assertTrue(result["runtime_shutdown_resources_empty"])
            self.assertTrue(result["capture_sha256"])
            self.assertTrue(result["scene_sha256"])
            self.assertEqual(result["pass_order"],
                             ["opaque", "alpha-tested", "transparent", "post"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the pinned graphical viewer exercise")
    def test_unpersisted_report_fails_the_exercise(self):
        with tempfile.TemporaryDirectory(prefix="eawr-renderer-runtime-") as temporary:
            # A directory cannot be opened as the report file.
            report = pathlib.Path(temporary) / "report-target"
            report.mkdir()
            completed, output = self.run_viewer(self.executable(), report)
            self.assertEqual(completed.returncode, 2, output)
            self.assertIn(HARNESS.REPORT_PERSISTENCE_MESSAGE, completed.stderr)
            # Only the passing path prints this before its report write.
            self.assertIn(HARNESS.PASSED_MARKER, completed.stdout.splitlines())
            self.assertEqual(list(report.iterdir()), [])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
