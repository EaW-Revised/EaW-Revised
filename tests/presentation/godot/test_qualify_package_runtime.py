"""Synthetic tests for the exported-package runtime qualification harness.

A Python stand-in replaces the packaged executable, so these tests prove the
harness verdicts only; they are not package runtime evidence.
"""

import contextlib
import functools
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import struct
import sys
import tempfile
import time
import unittest
import zlib
from unittest.mock import patch


ROOT = pathlib.Path(__file__).resolve().parents[3]
MODULE_PATH = ROOT / "apps/viewer/tools/qualify_package_runtime.py"
FAKE_VIEWER = pathlib.Path(__file__).resolve().with_name("fake_packaged_viewer.py")
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import mode_source  # noqa: E402
SPEC = importlib.util.spec_from_file_location("qualify_package_runtime", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

PLATFORM = "windows-x86_64" if os.name == "nt" else "linux-x86_64"
DISPLAY = "windows" if os.name == "nt" else "x11"


def load_fake():
    spec = importlib.util.spec_from_file_location("fake_packaged_viewer", FAKE_VIEWER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FAKE = load_fake()


class HarnessRun(unittest.TestCase):
    def setUp(self):
        out = ROOT / "out"
        out.mkdir(exist_ok=True)
        self._package_dir = tempfile.TemporaryDirectory(prefix="eawr-fake-package-")
        self._evidence_root = tempfile.TemporaryDirectory(prefix="eawr-harness-test-", dir=str(out))
        self.package = pathlib.Path(self._package_dir.name)
        paths = MODULE.package_paths(self.package, PLATFORM)
        paths["executable"].write_bytes(b"synthetic executable")
        paths["extension"].write_bytes(b"synthetic extension")
        paths["pck"].write_bytes(b"GDPC synthetic")
        self.evidence = pathlib.Path(self._evidence_root.name) / "run"

    def tearDown(self):
        self._package_dir.cleanup()
        self._evidence_root.cleanup()

    def config(self, **overrides):
        values = dict(
            platform=PLATFORM,
            package=self.package,
            evidence_dir=self.evidence,
            rendering_driver="vulkan",
            display_driver=DISPLAY,
            timeout=60.0,
            command_prefix=(sys.executable, str(FAKE_VIEWER)),
            check_host_platform=False,
        )
        values.update(overrides)
        return MODULE.Qualification(**values)

    def run_mode(self, mode, **overrides):
        environment = {"EAWR_FAKE_VIEWER_MODE": mode,
                       "EAWR_FAKE_ENGINE_IDENTITY": MODULE.ENGINE_IDENTITIES[PLATFORM]}
        with patch.dict(os.environ, environment):
            passed, receipt, receipt_path = MODULE.qualify(self.config(**overrides))
        self.assertTrue(receipt_path.is_file(), "a receipt is retained for every launched run")
        self.assertEqual(json.loads(receipt_path.read_text(encoding="utf-8")), receipt)
        return passed, receipt

    def assert_failed(self, mode, fragment, **overrides):
        passed, receipt = self.run_mode(mode, **overrides)
        self.assertFalse(passed, receipt)
        self.assertEqual(receipt["verdict"], "failed")
        joined = "\n".join(receipt["failures"])
        self.assertIn(fragment, joined)
        return receipt


class PassingRun(HarnessRun):
    def test_passing_run_retains_complete_evidence(self):
        passed, receipt = self.run_mode("pass")
        self.assertTrue(passed, receipt["failures"])
        self.assertEqual(receipt["verdict"], "passed")
        self.assertEqual(receipt["evidence_class"], "synthetic-harness-test")
        command = receipt["command"]
        self.assertEqual(command["exit_code"], 0)
        self.assertFalse(command["timed_out"])
        self.assertEqual(pathlib.Path(command["cwd"]), self.package.resolve())
        self.assertIn("--rendering-driver", command["argv"])
        for stream in ("stdout", "stderr"):
            retained = pathlib.Path(command[stream]["path"])
            self.assertTrue(retained.is_file())
            self.assertEqual(hashlib.sha256(retained.read_bytes()).hexdigest(), command[stream]["sha256"])
        self.assertTrue(receipt["hashes"]["unchanged"])
        self.assertEqual(set(receipt["hashes"]["before"]["sha256"]), {"executable", "extension", "pck"})
        self.assertEqual(receipt["capture"]["width"], 1280)
        self.assertEqual(receipt["capture"]["sha256"], receipt["report"]["capture_sha256"])
        self.assertEqual(receipt["device_banner"]["adapter"], "Synthetic Adapter")
        self.assertIsNotNone(receipt["engine"]["banner"])
        self.assertIn(receipt["host"]["system"], {"Windows", "Linux", "Darwin"})

    def test_every_driver_passes_on_its_platforms(self):
        drivers = {"vulkan": "forward_plus"}
        if PLATFORM == "windows-x86_64":
            drivers["d3d12"] = "forward_plus"
        for driver, method in drivers.items():
            with self.subTest(driver=driver):
                self.evidence = pathlib.Path(self._evidence_root.name) / driver
                passed, receipt = self.run_mode("pass", rendering_driver=driver)
                self.assertTrue(passed, receipt["failures"])
                self.assertEqual(receipt["report"]["backend"]["rendering_method"], method)
                self.assertEqual(receipt["device_banner"]["adapter"], "Synthetic Adapter")
                self.assertEqual(receipt["backend_request"]["rendering_driver"], driver)

    def test_report_persistence_probe_requires_exit_two(self):
        passed, receipt = self.run_mode("pass", probe_report_persistence=True)
        self.assertTrue(passed, receipt["failures"])
        self.assertEqual(receipt["report_persistence_probe"]["exit_code"], 2)

    def test_swallowed_report_persistence_failure_is_rejected(self):
        self.assert_failed("swallow_report_failure", "probe exited 0, expected 2",
                           probe_report_persistence=True)

    def test_probe_rejects_exit_two_from_an_earlier_failure(self):
        self.assert_failed("early_failure", "did not reach the passing path",
                           probe_report_persistence=True)

    def test_matching_package_receipt_is_linked(self):
        hashes = MODULE.package_hashes(self.package, PLATFORM)["sha256"]
        receipt_file = pathlib.Path(self._evidence_root.name) / "package-receipt.json"
        receipt_file.write_text(json.dumps({
            "platform": PLATFORM,
            "godot_identity": MODULE.PINNED_GODOT_IDENTITY,
            "validation": {f"{name}_sha256": value for name, value in hashes.items()},
        }), encoding="utf-8")
        passed, receipt = self.run_mode("pass", package_receipt=receipt_file)
        self.assertTrue(passed, receipt["failures"])
        self.assertTrue(receipt["package_receipt"]["matched"])


class OptionalReportKeys(HarnessRun):
    def test_render_profile_is_accepted(self):
        # Viewers since #184/#220 record the applied render profile; older packages omit it.
        passed, receipt = self.run_mode("render_profile")
        self.assertTrue(passed, receipt["failures"])


class EngineWarningWaivers(HarnessRun):
    def test_exact_waiver_is_recorded_and_passes(self):
        passed, receipt = self.run_mode("vsync_warning", allowed_warnings=(FAKE.VSYNC_WARNING,))
        self.assertTrue(passed, receipt["failures"])
        self.assertEqual(receipt["engine_warning_waivers"], [FAKE.VSYNC_WARNING])
        self.assertEqual(receipt["allowed_engine_warnings"], {FAKE.VSYNC_WARNING: 1})

    def test_unwaived_warning_fails(self):
        self.assert_failed("vsync_warning", "Could not set V-Sync mode")

    def test_stale_waiver_fails(self):
        self.assert_failed("pass", "stale waiver", allowed_warnings=(FAKE.VSYNC_WARNING,))

    def test_errors_cannot_be_waived(self):
        with self.assertRaisesRegex(MODULE.QualificationError, "only exact WARNING"):
            MODULE.qualify(self.config(allowed_warnings=("ERROR: Shader compilation failed.",)))

    def test_fatal_leak_warning_cannot_be_waived(self):
        with patch.object(MODULE, "launch") as launch:
            with self.assertRaisesRegex(MODULE.QualificationError, "cannot be waived"):
                MODULE.qualify(self.config(allowed_warnings=(FAKE.LEAK_WARNING,)))
        launch.assert_not_called()

    def test_fatal_leak_warning_fails_even_if_listed_as_waiver(self):
        # Callers that bypass qualify() (the source-project runtime test) still
        # cannot waive a leak, crash or shutdown diagnostic.
        observed = {}
        missing, unexpected = MODULE.scan_engine_output(
            FAKE.SHADER_ERRORS + FAKE.LEAK_WARNING + "\n", (FAKE.LEAK_WARNING,), observed)
        self.assertEqual(missing, [])
        self.assertEqual(unexpected, [FAKE.LEAK_WARNING])
        self.assertEqual(observed, {})


class RejectedRuns(HarnessRun):
    def test_malformed_report(self):
        self.assert_failed("malformed_report", "malformed JSON")

    def test_missing_report(self):
        self.assert_failed("missing_report", "report missing")

    def test_stale_report(self):
        self.assert_failed("stale_report", "report is stale")

    def test_false_lifecycle_field(self):
        self.assert_failed("false_lifecycle", "runtime_failed_upload_registry_clean: expected true")

    def test_too_few_scene_switches(self):
        self.assert_failed("few_switches", "runtime_scene_switch_count")

    def test_unexpected_report_key(self):
        self.assert_failed("extra_key", "unexpected keys ['runtime_invented_field']")

    def test_missing_rejection_marker(self):
        self.assert_failed("no_rejection_marker", "lacks runtime exercise markers")

    def test_foreign_scene_identity(self):
        self.assert_failed("wrong_scene", "report.scene_sha256")

    def test_nonzero_exit(self):
        self.assert_failed("exit_nonzero", "process exited 1")

    def test_timeout_kills_and_fails(self):
        started = time.monotonic()
        receipt = self.assert_failed("timeout", "timed out", timeout=2.0)
        self.assertLess(time.monotonic() - started, 30)
        self.assertTrue(receipt["command"]["timed_out"])
        self.assertIsNone(receipt["command"]["exit_code"])

    def test_package_mutation(self):
        receipt = self.assert_failed("mutate_package", "hashes or file set changed")
        self.assertFalse(receipt["hashes"]["unchanged"])

    def test_unexpected_engine_error(self):
        self.assert_failed("unexpected_error", "unexpected engine/shutdown diagnostics")

    def test_shutdown_leak(self):
        self.assert_failed("leak", "were leaked at exit")

    def test_missing_compiler_diagnostic(self):
        self.assert_failed("no_shader_error", "lacks expected compiler diagnostics")

    def test_wrong_engine_banner(self):
        self.assert_failed("wrong_engine", "engine banner")

    def test_missing_device_banner(self):
        self.assert_failed("no_device_banner", "device banner")

    def test_opengl_fallback_fails_a_vulkan_run(self):
        receipt = self.assert_failed("gl_fallback", "expected exactly one vulkan device banner",
                                     rendering_driver="vulkan")
        self.assertIsNone(receipt["device_banner"])

    def test_report_backend_differs_from_engine_device(self):
        self.assert_failed("wrong_adapter", "report.backend.adapter_name")

    def test_capture_hash_mismatch(self):
        self.assert_failed("capture_mismatch", "does not match report")

    def test_corrupt_png(self):
        self.assert_failed("corrupt_png", "CRC mismatch")

    def test_png_with_valid_crcs_but_undecodable_image_data(self):
        self.assert_failed("undecodable_png", "image data is not a valid zlib stream")

    def test_wrong_typed_report_field_keeps_failed_receipt(self):
        receipt = self.assert_failed("unhashable_backend", "report.backend.rendering_method")
        self.assertEqual(receipt["report"]["backend"]["rendering_method"], [])

    def test_unanticipated_verification_error_still_retains_failed_receipt(self):
        with patch.object(MODULE, "verify_runtime_report", side_effect=TypeError("unanticipated")):
            receipt = self.assert_failed("pass", "could not be verified: TypeError: unanticipated")
        self.assertTrue(receipt["hashes"]["unchanged"])

    def test_cli_malformed_report_exits_two_with_retained_receipt(self):
        environment = {"EAWR_FAKE_VIEWER_MODE": "unhashable_backend",
                       "EAWR_FAKE_ENGINE_IDENTITY": MODULE.ENGINE_IDENTITIES[PLATFORM]}
        synthetic = functools.partial(MODULE.Qualification, check_host_platform=False,
                                      command_prefix=(sys.executable, str(FAKE_VIEWER)))
        with patch.dict(os.environ, environment), patch.object(MODULE, "Qualification", synthetic):
            with contextlib.redirect_stdout(io.StringIO()) as stdout:
                code = MODULE.main(["--platform", PLATFORM, "--package", str(self.package),
                                    "--rendering-driver", "vulkan", "--display-driver", DISPLAY,
                                    "--evidence-dir", str(self.evidence), "--timeout", "60"])
        self.assertEqual(code, 2)
        summary = json.loads(stdout.getvalue())
        receipt = json.loads(pathlib.Path(summary["receipt"]).read_text(encoding="utf-8"))
        self.assertEqual(receipt["verdict"], "failed")
        self.assertEqual(receipt["failures"], summary["failures"])
        self.assertIn("report.backend.rendering_method", "\n".join(receipt["failures"]))

    def test_wrong_capture_size(self):
        self.assert_failed("wrong_size", "capture is 640x360")


class Preconditions(HarnessRun):
    def test_non_fresh_evidence_directory_is_rejected_before_launch(self):
        self.evidence.mkdir(parents=True)
        (self.evidence / "runtime-report.json").write_text("{}", encoding="utf-8")
        with patch.object(MODULE, "launch") as launch:
            with self.assertRaisesRegex(MODULE.QualificationError, "not fresh"):
                MODULE.qualify(self.config())
        launch.assert_not_called()

    def test_evidence_outside_out_tree_is_rejected(self):
        with tempfile.TemporaryDirectory() as outside:
            with self.assertRaisesRegex(ValueError, "out/ tree"):
                MODULE.qualify(self.config(evidence_dir=pathlib.Path(outside) / "run"))

    def test_package_receipt_hash_mismatch_is_rejected_before_launch(self):
        receipt_file = pathlib.Path(self._evidence_root.name) / "package-receipt.json"
        receipt_file.write_text(json.dumps({
            "platform": PLATFORM,
            "godot_identity": MODULE.PINNED_GODOT_IDENTITY,
            "validation": {"executable_sha256": "0" * 64, "extension_sha256": "0" * 64,
                           "pck_sha256": "0" * 64},
        }), encoding="utf-8")
        with patch.object(MODULE, "launch") as launch:
            with self.assertRaisesRegex(MODULE.QualificationError, "does not match export receipt"):
                MODULE.qualify(self.config(package_receipt=receipt_file))
        launch.assert_not_called()

    def test_missing_package_file_is_rejected(self):
        MODULE.package_paths(self.package, PLATFORM)["extension"].unlink()
        with self.assertRaisesRegex(MODULE.QualificationError, "missing"):
            MODULE.qualify(self.config())

    def test_headless_or_unknown_backend_is_rejected(self):
        with self.assertRaisesRegex(MODULE.QualificationError, "unsupported rendering driver"):
            MODULE.qualify(self.config(rendering_driver="dummy"))
        for platform_name in ("linux-x86_64", "linux-arm64"):
            with self.assertRaisesRegex(MODULE.QualificationError, "'d3d12' is not valid for linux"):
                MODULE.qualify(self.config(platform=platform_name, rendering_driver="d3d12"))
        with self.assertRaisesRegex(MODULE.QualificationError, "display driver"):
            MODULE.qualify(self.config(display_driver="headless"))

    def test_foreign_host_is_rejected(self):
        with self.assertRaisesRegex(MODULE.QualificationError, "native Linux"):
            MODULE.check_host("linux-x86_64", {"system": "Windows", "machine": "AMD64"})
        with self.assertRaisesRegex(MODULE.QualificationError, "native Linux"):
            MODULE.check_host("linux-arm64", {"system": "Linux", "machine": "x86_64"})
        MODULE.check_host("windows-x86_64", {"system": "Windows", "machine": "AMD64"})

    def test_unlaunchable_package_still_writes_a_failed_receipt(self):
        with patch.object(MODULE, "launch", side_effect=OSError("exec format error")):
            passed, receipt, receipt_path = MODULE.qualify(self.config())
        self.assertFalse(passed)
        self.assertTrue(receipt_path.is_file())
        self.assertIn("could not be launched", "\n".join(receipt["failures"]))
        self.assertEqual(receipt["command"]["launch_error"], "exec format error")

    def test_cli_returns_two_for_failed_run(self):
        with patch.object(MODULE, "qualify", return_value=(False, {"verdict": "failed", "failures": ["x"]},
                                                           self.evidence / "receipt.json")):
            with contextlib.redirect_stdout(io.StringIO()):
                code = MODULE.main(["--platform", PLATFORM, "--package", str(self.package),
                                    "--rendering-driver", "vulkan", "--display-driver", DISPLAY])
        self.assertEqual(code, 2)


class SharedReportContract(unittest.TestCase):
    def valid(self):
        return FAKE.report(hashlib.sha256(b"capture").hexdigest())

    def test_valid_report_has_no_failures(self):
        self.assertEqual(MODULE.verify_runtime_report(self.valid()), [])

    def test_version_and_type_confusion_are_rejected(self):
        report = self.valid()
        report["schema_version"] = 2
        report["runtime_scene_switch_count"] = True
        report["runtime_capture_verified"] = 1
        failures = "\n".join(MODULE.verify_runtime_report(report))
        self.assertIn("schema_version", failures)
        self.assertIn("runtime_scene_switch_count", failures)
        self.assertIn("runtime_capture_verified", failures)

    def test_failed_status_and_other_mode_flags_are_rejected(self):
        report = self.valid()
        report["status"] = "failed"
        report["failure"] = "runtime RID release left live resources or instances"
        report["atlas_overlay_verified"] = True
        failures = "\n".join(MODULE.verify_runtime_report(report))
        self.assertIn("report.status", failures)
        self.assertIn("report.failure", failures)
        self.assertIn("atlas_overlay_verified", failures)

    def test_runtime_scene_identity_matches_viewer_source(self):
        source = mode_source("viewer_host")
        self.assertIn(f'"{MODULE.RUNTIME_SCENE_IDENTITY}"', source)
        for marker in MODULE.RUNTIME_MARKERS:
            self.assertIn(f'"{marker}"', source)

    def test_wrong_typed_nested_fields_are_failures_not_exceptions(self):
        report = self.valid()
        report["backend"]["rendering_method"] = []
        report["backend"]["adapter_name"] = {"name": "Synthetic Adapter"}
        report["capture_identity"] = ["viewport"]
        report["pass_order"] = {"opaque": 0}
        failures = "\n".join(MODULE.verify_runtime_report(report))
        for fragment in ("backend.rendering_method: expected non-empty text",
                         "backend.adapter_name: expected non-empty text",
                         "capture_identity.viewport", "pass_order"):
            self.assertIn(fragment, failures)

    def test_non_object_report(self):
        self.assertEqual(MODULE.verify_runtime_report([]), ["report: expected a JSON object"])

    def test_engine_output_scan(self):
        missing, unexpected = MODULE.scan_engine_output(FAKE.SHADER_ERRORS)
        self.assertEqual((missing, unexpected), ([], []))
        missing, unexpected = MODULE.scan_engine_output(
            "WARNING: ObjectDB instances leaked at exit\nEAWR viewer report output could not be opened\n")
        self.assertEqual(len(missing), 2)
        self.assertEqual(len(unexpected), 2)

    def test_device_banners_per_driver(self):
        # Engine output as Godot 4.7.2 prints it: lavapipe on the CI runner, a
        # D3D12 adapter.
        lavapipe = "Vulkan 1.4.318 - Forward+ - Using Device #0: Unknown - llvmpipe (LLVM 20.1.2, 256 bits)"
        d3d12 = "D3D12 12_0 - Forward+ - Using Device #0: AMD - AMD Radeon RX 7900 XTX"
        self.assertEqual(MODULE.parse_backend_banner(lavapipe, "vulkan"),
                         {"api": "1.4.318", "vendor": "Unknown",
                          "adapter": "llvmpipe (LLVM 20.1.2, 256 bits)", "line": lavapipe})
        self.assertEqual(MODULE.parse_backend_banner(d3d12, "d3d12")["api"], "12_0")
        for text, driver in ((lavapipe, "d3d12"),
                             (lavapipe.replace("Forward+", "Forward Mobile"), "vulkan"),
                             (lavapipe + "\n" + lavapipe, "vulkan")):
            with self.subTest(driver=driver, text=text):
                self.assertIsNone(MODULE.parse_backend_banner(text, driver))
        self.assertEqual({name: entry["rendering_method"] for name, entry in MODULE.RENDERING_DRIVERS.items()},
                         {"vulkan": "forward_plus", "d3d12": "forward_plus"})

    def test_engine_banner_identity_is_per_platform(self):
        official = "Godot Engine v4.7.2.stable.official.ed1daf0bf - https://godotengine.org"
        custom = ("Godot Engine v4.7.2.stable.custom_build.ed1daf0bf (2026-08-17 01:18:38 UTC)"
                  " - https://godotengine.org")
        self.assertEqual(MODULE.ENGINE_IDENTITIES["windows-x86_64"], MODULE.PINNED_GODOT_IDENTITY)
        self.assertEqual(MODULE.ENGINE_IDENTITIES["linux-x86_64"],
                         "4.7.2.stable.custom_build.ed1daf0bf")
        self.assertEqual(MODULE.engine_banner(official, MODULE.ENGINE_IDENTITIES["windows-x86_64"]), official)
        self.assertEqual(MODULE.engine_banner(custom, MODULE.ENGINE_IDENTITIES["linux-x86_64"]), custom)
        self.assertIsNone(MODULE.engine_banner(custom, MODULE.ENGINE_IDENTITIES["windows-x86_64"]))
        self.assertIsNone(MODULE.engine_banner(official, MODULE.ENGINE_IDENTITIES["linux-arm64"]))
        self.assertIsNone(MODULE.engine_banner(official + "\n" + official, MODULE.PINNED_GODOT_IDENTITY))
        self.assertIsNone(MODULE.engine_banner(official.replace("ed1daf0bf", "ed1daf0bf0"),
                                               MODULE.PINNED_GODOT_IDENTITY))

    def test_png_structure(self):
        self.assertEqual(MODULE.read_png_header(FAKE.png(4, 2)), (4, 2))
        with self.assertRaisesRegex(ValueError, "IEND"):
            MODULE.read_png_header(FAKE.png(4, 2)[:-12])
        with self.assertRaisesRegex(ValueError, "signature"):
            MODULE.read_png_header(b"GIF89a")

    def test_png_image_data_decodes_for_supported_formats(self):
        for color_type in (0, 2, 4, 6):
            self.assertEqual(MODULE.read_png_header(FAKE.png(4, 2, color_type)), (4, 2))
        # Every scanline filter type is accepted; each row is 1 filter byte + 16 RGBA bytes.
        rows = b"".join(bytes([kind]) + b"\x01" * 16 for kind in range(5))
        self.assertEqual(MODULE.read_png_header(FAKE.png(4, 5, idat=zlib.compress(rows))), (4, 5))

    def test_png_with_malformed_image_data_is_rejected(self):
        row = b"\x00" + b"\x00" * 16
        cases = {
            "not a valid zlib stream": FAKE.png(4, 2, idat=b"not zlib data"),
            "truncated": FAKE.png(4, 2, idat=zlib.compress(row * 2)[:-4]),
            "decodes to 17 bytes, expected 34": FAKE.png(4, 2, idat=zlib.compress(row)),
            "decodes to more than 34 bytes": FAKE.png(4, 2, idat=zlib.compress(row * 3)),
            "trailing bytes": FAKE.png(4, 2, idat=zlib.compress(row * 2) + b"\x00"),
            "scanline 1 has filter type 5": FAKE.png(4, 2, idat=zlib.compress(row + b"\x05" + row[1:])),
        }
        for fragment, data in cases.items():
            with self.subTest(fragment), self.assertRaisesRegex(ValueError, fragment):
                MODULE.read_png_header(data)

    def test_png_outside_the_supported_format_is_rejected(self):
        def with_header(width, height, depth, color_type, interlace):
            return (b"\x89PNG\r\n\x1a\n"
                    + FAKE.chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color_type,
                                                       0, 0, interlace))
                    + FAKE.chunk(b"IDAT", zlib.compress(b"\x00")) + FAKE.chunk(b"IEND", b""))
        cases = {
            "16-bit": with_header(1, 1, 16, 6, 0),
            "palette": with_header(1, 1, 8, 3, 0),
            "interlaced": with_header(1, 1, 8, 6, 1),
            "zero width": with_header(0, 1, 8, 6, 0),
            "oversized": with_header(0x7FFFFFFF, 0x7FFFFFFF, 8, 6, 0),
        }
        for label, data in cases.items():
            with self.subTest(label), self.assertRaisesRegex(ValueError, "unsupported PNG|dimensions"):
                MODULE.read_png_header(data)
        split = FAKE.png(4, 2)
        idat = zlib.compress(b"\x00" * 34)
        interleaved = (split[:33] + FAKE.chunk(b"IDAT", idat[:5]) + FAKE.chunk(b"tEXt", b"k\x00v")
                       + FAKE.chunk(b"IDAT", idat[5:]) + FAKE.chunk(b"IEND", b""))
        with self.assertRaisesRegex(ValueError, "not consecutive"):
            MODULE.read_png_header(interleaved)


if __name__ == "__main__":
    unittest.main(argv=[__file__])
