import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

from PIL import Image


ROOT = pathlib.Path(__file__).resolve().parents[3]
TOOL = ROOT / "tools/validation/p1_capture/check_migration_pair.py"


def identity():
    return {
        "vfs_profile": "synthetic-profile-v1",
        "scene": {"logical_path": "res://fixture/scene.json", "sha256": "1" * 64},
        "inputs": [{"logical_path": "res://fixture/input.bin", "sha256": "2" * 64}],
        "viewport": {"width": 12, "height": 8},
        "camera": {
            "projection": "perspective",
            "position": [0.0, 2.0, 5.0],
            "target": [0.0, 0.0, 0.0],
            "up": [0.0, 1.0, 0.0],
            "fov_degrees": 45.0,
            "near": 0.1,
            "far": 100.0,
        },
        "simulation_tick": 120,
        "animation_time_seconds": 0.0,
        "particle_seed": 17,
        "particle_time_seconds": 0.0,
    }


def contract():
    limits = {
        "rgb_mae": {"max": 0.0, "rationale": "Synthetic preflight fixture."},
        "rgb_p95": {"max": 0.0, "rationale": "Synthetic preflight fixture."},
        "alpha_mae": {"max": 0.0, "rationale": "Synthetic preflight fixture."},
        "changed_pixel_ratio": {"max": 0.0, "rationale": "Synthetic preflight fixture."},
        "edge_iou": {"min": 1.0, "rationale": "Synthetic preflight fixture."},
    }
    return {
        "schema_version": 1,
        "contract_id": "synthetic-migration-pair-v1",
        "identity": identity(),
        "regions": [{
            "name": "full-frame",
            "rect": [0, 0, 12, 8],
            "pixel_channel_threshold": 0,
            "edge_threshold": 8,
            "threshold_rationale": "Synthetic preflight fixture.",
            "limits": limits,
        }],
    }


def runtime():
    return {
        "platform": "windows",
        "architecture": "x86_64",
        "engine": "Godot",
        "engine_version": "4.7.2-stable",
        "backend": "gl_compatibility",
        "graphics_api": "OpenGL 4.5",
        "device": "synthetic-adapter",
        "driver": "synthetic-driver",
        "driver_version": "1.0",
        "build_revision": "prototype-revision",
    }


def insert_duplicate_key(path, key, original, duplicate):
    raw = path.read_text(encoding="utf-8")
    needle = f"{json.dumps(key)}: {json.dumps(original)}"
    replacement = f"{needle}, {json.dumps(key)}: {json.dumps(duplicate)}"
    if raw.count(needle) != 1:
        raise AssertionError(f"expected one occurrence of {needle!r} in {path}")
    path.write_text(raw.replace(needle, replacement, 1), encoding="utf-8")


class MigrationPairPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.contract_path = self.root / "contract.json"
        self.write_json(self.contract_path, contract())
        self.contract_hash = hashlib.sha256(self.contract_path.read_bytes()).hexdigest()
        self.baseline_image = self.root / "baseline.png"
        self.candidate_image = self.root / "candidate.png"
        image = Image.new("RGBA", (12, 8), (12, 18, 22, 255))
        image.save(self.baseline_image)
        image.save(self.candidate_image)
        self.baseline_manifest = self.write_manifest("baseline", self.baseline_image)
        self.candidate_manifest = self.write_manifest("candidate", self.candidate_image)

    def tearDown(self):
        self.temporary.cleanup()

    def write_json(self, path, value):
        path.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")

    def write_manifest(self, role, image_path, **overrides):
        value = {
            "schema_version": 1,
            "contract_sha256": self.contract_hash,
            "role": role,
            "image": {
                "path": image_path.name,
                "sha256": hashlib.sha256(image_path.read_bytes()).hexdigest(),
            },
            "identity": identity(),
            "runtime": runtime(),
        }
        value.update(overrides)
        path = self.root / f"{role}.json"
        self.write_json(path, value)
        return path

    def invoke(self, mode="same-backend", *extra):
        return subprocess.run(
            [sys.executable, str(TOOL), "check", "--contract", str(self.contract_path),
             "--baseline", str(self.baseline_manifest), "--candidate", str(self.candidate_manifest),
             "--mode", mode, *map(str, extra)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def result(self, mode="same-backend", *extra):
        completed = self.invoke(mode, *extra)
        return completed, json.loads(completed.stdout)

    def test_equal_image_and_identity_passes_while_engine_label_and_revision_differ(self):
        candidate = json.loads(self.candidate_manifest.read_text(encoding="utf-8"))
        candidate["runtime"]["engine"] = "Godot 4.7.2-stable"
        candidate["runtime"]["build_revision"] = "production-revision"
        self.write_json(self.candidate_manifest, candidate)

        completed, result = self.result()

        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertEqual(result["status"], "passed")
        self.assertTrue(result["passed"])
        self.assertFalse(result["visual_comparison"])
        self.assertEqual(result["provenance"]["same_backend_check_applied"], True)
        self.assertTrue(result["provenance"]["allowed_differences"]["engine"]["different"])
        self.assertTrue(result["provenance"]["allowed_differences"]["build_revision"]["different"])

    def test_each_same_backend_runtime_tuple_field_mismatch_fails(self):
        fields = (
            "platform", "architecture", "engine_version", "backend",
            "graphics_api", "device", "driver", "driver_version",
        )
        for field in fields:
            with self.subTest(field=field):
                candidate = json.loads(self.candidate_manifest.read_text(encoding="utf-8"))
                candidate["runtime"] = runtime()
                candidate["runtime"][field] = f"different-{field}"
                self.write_json(self.candidate_manifest, candidate)

                completed, result = self.result()

                self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
                self.assertEqual(result["status"], "failed")
                self.assertEqual(result["reason_code"], "runtime_provenance_mismatch")
                self.assertEqual(result["errors"][0]["field"], field)
                self.assertEqual(result["runtime_tuples"]["baseline"][field], runtime()[field])
                self.assertEqual(result["runtime_tuples"]["candidate"][field], f"different-{field}")

    def test_empty_and_placeholder_runtime_values_fail_with_both_tuples(self):
        for placeholder in ("", "unknown", "unspecified", "n/a", "none"):
            with self.subTest(placeholder=placeholder):
                candidate = json.loads(self.candidate_manifest.read_text(encoding="utf-8"))
                candidate["runtime"]["device"] = placeholder
                self.write_json(self.candidate_manifest, candidate)

                completed, result = self.result()

                self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
                self.assertEqual(result["reason_code"], "runtime_provenance_mismatch")
                self.assertEqual(result["errors"][0]["field"], "device")
                self.assertIn("baseline", result["runtime_tuples"])
                self.assertIn("candidate", result["runtime_tuples"])

    def test_scene_digest_mismatch_is_structured_and_retains_runtime_tuples(self):
        candidate = json.loads(self.candidate_manifest.read_text(encoding="utf-8"))
        candidate["identity"]["scene"]["sha256"] = "f" * 64
        self.write_json(self.candidate_manifest, candidate)

        completed, result = self.result()

        self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
        self.assertEqual(result["reason_code"], "capture_validation_error")
        self.assertIn("candidate", result["runtime_tuples"])
        self.assertTrue(any("scene" in error["message"] for error in result["errors"]))

    def test_image_digest_mismatch_is_structured(self):
        candidate = json.loads(self.candidate_manifest.read_text(encoding="utf-8"))
        candidate["image"]["sha256"] = "f" * 64
        self.write_json(self.candidate_manifest, candidate)

        completed, result = self.result()

        self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
        self.assertEqual(result["reason_code"], "capture_validation_error")
        self.assertTrue(any("digest mismatch" in error["message"] for error in result["errors"]))

    def test_duplicate_object_keys_fail_closed_for_contract_and_both_manifests(self):
        cases = (
            ("contract", self.contract_path, "contract_id", contract()["contract_id"],
             "contradictory-contract", "contract_validation_error"),
            ("baseline runtime", self.baseline_manifest, "device", runtime()["device"],
             "contradictory-baseline-device", "capture_input_error"),
            ("candidate identity", self.candidate_manifest, "logical_path",
             identity()["scene"]["logical_path"], "res://contradictory/scene.json",
             "capture_input_error"),
        )
        for label, path, key, original, duplicate, reason_code in cases:
            with self.subTest(label=label):
                original_raw = path.read_text(encoding="utf-8")
                try:
                    insert_duplicate_key(path, key, original, duplicate)
                    completed, result = self.result()
                    self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
                    self.assertEqual(result["status"], "failed")
                    self.assertFalse(result["passed"])
                    self.assertEqual(result["reason_code"], reason_code)
                    self.assertIn("duplicate object key", result["errors"][0]["message"])
                    self.assertIn(repr(key), result["errors"][0]["message"])
                finally:
                    path.write_text(original_raw, encoding="utf-8")

    def test_single_windows_backslash_parent_segments_are_rejected_for_scene_and_input(self):
        for label, update in (
            ("scene", lambda value: value["identity"]["scene"].update(
                logical_path=r"..\outside\scene.json")),
            ("input", lambda value: value["identity"]["inputs"][0].update(
                logical_path=r"..\outside\input.bin")),
        ):
            with self.subTest(label=label):
                changed = contract()
                update(changed)
                self.write_json(self.contract_path, changed)
                changed_hash = hashlib.sha256(self.contract_path.read_bytes()).hexdigest()
                for manifest_path in (self.baseline_manifest, self.candidate_manifest):
                    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                    manifest["contract_sha256"] = changed_hash
                    manifest["identity"] = changed["identity"]
                    self.write_json(manifest_path, manifest)

                completed, result = self.result()

                self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
                self.assertEqual(result["status"], "failed")
                self.assertFalse(result["passed"])
                self.assertEqual(result["reason_code"], "contract_validation_error")
                self.assertIn("parent traversal", result["errors"][0]["message"])

    def test_legitimate_backslash_logical_paths_remain_accepted_and_unchanged(self):
        changed = contract()
        changed["identity"]["scene"]["logical_path"] = r"res://fixture\scene.json"
        changed["identity"]["inputs"][0]["logical_path"] = r"res://fixture\input.bin"
        self.write_json(self.contract_path, changed)
        changed_hash = hashlib.sha256(self.contract_path.read_bytes()).hexdigest()
        for manifest_path in (self.baseline_manifest, self.candidate_manifest):
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["contract_sha256"] = changed_hash
            manifest["identity"] = changed["identity"]
            self.write_json(manifest_path, manifest)

        completed, result = self.result()

        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(
            json.loads(self.baseline_manifest.read_text(encoding="utf-8"))["identity"],
            changed["identity"],
        )

    def test_windows_linux_pair_is_rejected_in_same_backend_mode(self):
        candidate = json.loads(self.candidate_manifest.read_text(encoding="utf-8"))
        candidate["runtime"]["platform"] = "linux"
        self.write_json(self.candidate_manifest, candidate)

        completed, result = self.result()

        self.assertEqual(completed.returncode, 1, completed.stderr + completed.stdout)
        self.assertEqual(result["errors"][0]["field"], "platform")
        self.assertEqual(result["runtime_tuples"]["baseline"]["platform"], "windows")
        self.assertEqual(result["runtime_tuples"]["candidate"]["platform"], "linux")

    def test_cross_platform_mode_does_not_apply_same_backend_tuple_rule(self):
        candidate = json.loads(self.candidate_manifest.read_text(encoding="utf-8"))
        candidate["runtime"].update({
            "platform": "linux",
            "architecture": "aarch64",
            "device": "linux-adapter",
            "driver": "linux-driver",
            "driver_version": "2.0",
        })
        self.write_json(self.candidate_manifest, candidate)

        completed, result = self.result("cross-platform")

        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertEqual(result["status"], "passed")
        self.assertFalse(result["provenance"]["same_backend_check_applied"])
        self.assertEqual(result["provenance"]["required_match_fields"], [])
        self.assertEqual(result["reason_code"], "cross_platform_tuple_check_not_applicable")

    def test_report_contains_structured_result(self):
        report = self.root / "reports/migration.json"
        completed, result = self.result("same-backend", "--report", report)

        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertEqual(json.loads(report.read_text(encoding="utf-8")), result)


if __name__ == "__main__":
    unittest.main()
