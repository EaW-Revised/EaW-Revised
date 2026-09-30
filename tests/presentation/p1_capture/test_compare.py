import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

from PIL import Image, ImageDraw


ROOT = pathlib.Path(__file__).resolve().parents[3]
TOOL = ROOT / "tools/validation/p1_capture/compare.py"


def identity():
    return {
        "vfs_profile": "fixture-profile-v1",
        "scene": {"logical_path": "res://fixture/scene.json", "sha256": "1" * 64},
        "inputs": [
            {"logical_path": "res://fixture/model.bin", "sha256": "2" * 64},
            {"logical_path": "res://fixture/texture.bin", "sha256": "3" * 64},
        ],
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
        "rgb_mae": {"max": 0.0, "rationale": "Exact synthetic reference pixels."},
        "rgb_p95": {"max": 0.0, "rationale": "Exact synthetic reference pixels."},
        "alpha_mae": {"max": 0.0, "rationale": "Exact synthetic reference pixels."},
        "changed_pixel_ratio": {"max": 0.0, "rationale": "No fixture pixel may change."},
        "edge_iou": {"min": 1.0, "rationale": "Fixture geometry must match exactly."},
    }
    regions = []
    for name, rect in (("full-frame", [0, 0, 12, 8]), ("material-swatch", [3, 2, 6, 4])):
        regions.append({
            "name": name,
            "rect": rect,
            "pixel_channel_threshold": 0,
            "edge_threshold": 8,
            "threshold_rationale": "Synthetic fixture uses exact, opaque pixel values.",
            "limits": limits,
        })
    return {
        "schema_version": 1,
        "contract_id": "synthetic-p1-capture-v1",
        "identity": identity(),
        "regions": regions,
    }


def runtime():
    return {
        "platform": "synthetic-windows-x64",
        "architecture": "x86_64",
        "engine": "fixture-renderer",
        "engine_version": "1.0.0",
        "backend": "fixture-backend",
        "graphics_api": "fixture-api",
        "device": "fixture-device",
        "driver": "fixture-driver",
        "driver_version": "1.0",
        "build_revision": "fixture-build-001",
    }


def policy():
    return {
        "schema_version": 1,
        "policy_id": "synthetic-exact-policy-v1",
        "profiles": [{
            "name": "exact-repeat",
            "evidence_requirement": "Two decoded baseline captures compared exactly before candidate evaluation.",
            "limits": {
                "rgb_mae": 0.0,
                "rgb_p95": 0.0,
                "alpha_mae": 0.0,
                "changed_pixel_ratio": 0.0,
                "edge_iou": 1.0,
            },
        }],
    }


def invoke(*arguments):
    return subprocess.run(
        [sys.executable, str(TOOL), *map(str, arguments)],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def insert_duplicate_key(path, key, original, duplicate):
    raw = path.read_text(encoding="utf-8")
    needle = f"{json.dumps(key)}: {json.dumps(original)}"
    replacement = f"{needle}, {json.dumps(key)}: {json.dumps(duplicate)}"
    if raw.count(needle) != 1:
        raise AssertionError(f"expected one occurrence of {needle!r} in {path}")
    path.write_text(raw.replace(needle, replacement, 1), encoding="utf-8")


class CaptureComparatorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.contract_path = self.root / "contract.json"
        self.contract_value = contract()
        self.write_json(self.contract_path, self.contract_value)
        self.contract_hash = hashlib.sha256(self.contract_path.read_bytes()).hexdigest()
        self.policy_path = self.root / "policy.json"
        self.write_json(self.policy_path, policy())
        policy_hash = hashlib.sha256(self.policy_path.read_bytes()).hexdigest()
        self.approval_path = self.root / "approval.json"
        self.write_json(self.approval_path, {
            "schema_version": 1,
            "approval_id": "synthetic-approval-v1",
            "contract_sha256": self.contract_hash,
            "policy_sha256": policy_hash,
            "policy_profile": "exact-repeat",
            "approved_before_candidate": True,
            "evidence": {
                "baseline_capture_sha256": "4" * 64,
                "repeat_capture_sha256": "4" * 64,
                "repeat_report_sha256": "5" * 64,
            },
            "rationale": "The synthetic baseline repeated with zero decoded-pixel variance.",
        })
        self.baseline_image = self.root / "baseline.png"
        self.candidate_image = self.root / "candidate.png"
        self.write_fixture(self.baseline_image)
        self.write_fixture(self.candidate_image)
        self.baseline_manifest = self.write_manifest("baseline", self.baseline_image)
        self.candidate_manifest = self.write_manifest("candidate", self.candidate_image)

    def tearDown(self):
        self.temporary.cleanup()

    def write_json(self, path, value):
        path.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")

    def write_fixture(self, path, *, shift=0, swatch=(210, 60, 30)):
        image = Image.new("RGB", (12, 8), (12, 18, 22))
        ImageDraw.Draw(image).rectangle((4 + shift, 3, 6 + shift, 4), fill=swatch)
        image.save(path)

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

    def compare(self, *arguments):
        return invoke(
            "compare", "--contract", self.contract_path,
            "--policy", self.policy_path, "--approval", self.approval_path,
            *arguments,
        )

    def test_unchanged_decoded_pixels_pass_and_report_region_metrics(self):
        report_path = self.root / "reports/comparison.json"
        result = self.compare(
            "--baseline", self.baseline_manifest,
            "--candidate", self.candidate_manifest,
            "--report", report_path,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "passed")
        self.assertEqual([region["name"] for region in report["regions"]],
                         ["full-frame", "material-swatch"])
        self.assertEqual(report["regions"][0]["metrics"]["rgb_mae"]["actual"], 0.0)
        self.assertEqual(report["provenance"]["candidate"]["driver_version"], "1.0")

    def test_displaced_object_fails_geometry_and_full_frame_metrics(self):
        self.write_fixture(self.candidate_image, shift=2)
        self.candidate_manifest = self.write_manifest("candidate", self.candidate_image)
        result = self.compare(
            "--baseline", self.baseline_manifest,
            "--candidate", self.candidate_manifest,
        )
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        report = json.loads(result.stdout)
        full_frame = report["regions"][0]
        self.assertFalse(full_frame["metrics"]["edge_iou"]["passed"])
        self.assertFalse(full_frame["metrics"]["changed_pixel_ratio"]["passed"])

    def test_wrong_material_fails_named_region(self):
        self.write_fixture(self.candidate_image, swatch=(10, 220, 80))
        self.candidate_manifest = self.write_manifest("candidate", self.candidate_image)
        result = self.compare(
            "--baseline", self.baseline_manifest,
            "--candidate", self.candidate_manifest,
        )
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        report = json.loads(result.stdout)
        swatch = next(region for region in report["regions"] if region["name"] == "material-swatch")
        self.assertFalse(swatch["metrics"]["rgb_mae"]["passed"])

    def test_legacy_ppm_baseline_compares_against_png_candidate(self):
        ppm_image = self.root / "baseline.ppm"
        self.write_fixture(ppm_image)
        ppm_manifest = self.write_manifest("baseline", ppm_image)
        result = self.compare(
            "--baseline", ppm_manifest,
            "--candidate", self.candidate_manifest,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = json.loads(result.stdout)
        self.assertEqual(report["status"], "passed")

    def test_alpha_channel_change_is_measured_and_rejected(self):
        image = Image.open(self.candidate_image).convert("RGBA")
        image.putpixel((0, 0), (12, 18, 22, 128))
        image.save(self.candidate_image)
        self.candidate_manifest = self.write_manifest("candidate", self.candidate_image)
        result = self.compare(
            "--baseline", self.baseline_manifest,
            "--candidate", self.candidate_manifest,
        )
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        report = json.loads(result.stdout)
        alpha = report["regions"][0]["metrics"]["alpha_mae"]
        self.assertGreater(alpha["actual"], 0.0)
        self.assertFalse(alpha["passed"])

    def test_committed_policy_rejects_permissive_contract_even_with_matching_approval(self):
        permissive = contract()
        for region in permissive["regions"]:
            region["limits"]["rgb_mae"]["max"] = 255.0
            region["limits"]["rgb_p95"]["max"] = 255.0
            region["limits"]["alpha_mae"]["max"] = 255.0
            region["limits"]["changed_pixel_ratio"]["max"] = 1.0
            region["limits"]["edge_iou"]["min"] = 0.0
        self.write_json(self.contract_path, permissive)
        permissive_hash = hashlib.sha256(self.contract_path.read_bytes()).hexdigest()
        approval = json.loads(self.approval_path.read_text(encoding="utf-8"))
        approval["contract_sha256"] = permissive_hash
        self.write_json(self.approval_path, approval)
        baseline = json.loads(self.baseline_manifest.read_text(encoding="utf-8"))
        baseline["contract_sha256"] = permissive_hash
        self.write_json(self.baseline_manifest, baseline)
        candidate = json.loads(self.candidate_manifest.read_text(encoding="utf-8"))
        candidate["contract_sha256"] = permissive_hash
        self.write_json(self.candidate_manifest, candidate)
        result = self.compare(
            "--baseline", self.baseline_manifest, "--candidate", self.candidate_manifest,
        )
        self.assertEqual(result.returncode, 2, result.stderr + result.stdout)
        self.assertIn("exceeds approved policy", result.stderr)

    def test_missing_or_mismatched_capture_metadata_fails_closed(self):
        alterations = (
            ("scene hash", lambda value: value["identity"]["scene"].update(sha256="f" * 64)),
            ("input identity", lambda value: value["identity"]["inputs"][0].update(sha256="f" * 64)),
            ("camera", lambda value: value["identity"]["camera"]["position"].__setitem__(0, 9.0)),
            ("viewport", lambda value: value["identity"]["viewport"].update(width=11)),
            ("animation time", lambda value: value["identity"].update(animation_time_seconds=1.0)),
            ("particle seed", lambda value: value["identity"].update(particle_seed=18)),
            ("driver provenance", lambda value: value["runtime"].update(driver="unknown")),
            ("missing device", lambda value: value["runtime"].pop("device")),
            ("contract digest", lambda value: value.update(contract_sha256="f" * 64)),
        )
        for label, alter in alterations:
            with self.subTest(label=label):
                changed = json.loads(self.baseline_manifest.read_text(encoding="utf-8"))
                alter(changed)
                bad_manifest = self.root / "bad.json"
                self.write_json(bad_manifest, changed)
                result = invoke("validate", "--contract", self.contract_path,
                                "--manifest", bad_manifest)
                self.assertEqual(result.returncode, 2, result.stderr + result.stdout)
                self.assertEqual(json.loads(result.stderr)["status"], "error")

    def test_duplicate_object_keys_fail_closed_at_contract_and_nested_manifest_levels(self):
        cases = (
            ("contract", self.contract_path, "contract_id", contract()["contract_id"],
             "contradictory-contract"),
            ("baseline runtime", self.baseline_manifest, "device", runtime()["device"],
             "contradictory-baseline-device"),
            ("candidate identity", self.candidate_manifest, "logical_path",
             identity()["scene"]["logical_path"], "res://contradictory/scene.json"),
        )
        for label, path, key, original, duplicate in cases:
            with self.subTest(label=label):
                original_raw = path.read_text(encoding="utf-8")
                try:
                    insert_duplicate_key(path, key, original, duplicate)
                    result = self.compare(
                        "--baseline", self.baseline_manifest,
                        "--candidate", self.candidate_manifest,
                    )
                    self.assertEqual(result.returncode, 2, result.stderr + result.stdout)
                    error = json.loads(result.stderr)
                    self.assertEqual(error["status"], "error")
                    self.assertIn("duplicate object key", error["message"])
                    self.assertIn(repr(key), error["message"])
                finally:
                    path.write_text(original_raw, encoding="utf-8")

    def test_single_windows_backslash_parent_segments_are_rejected_for_scene_and_input(self):
        original_raw = self.contract_path.read_text(encoding="utf-8")
        try:
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
                    result = self.compare(
                        "--baseline", self.baseline_manifest,
                        "--candidate", self.candidate_manifest,
                    )
                    self.assertEqual(result.returncode, 2, result.stderr + result.stdout)
                    error = json.loads(result.stderr)
                    self.assertEqual(error["status"], "error")
                    self.assertIn("parent traversal", error["message"])
        finally:
            self.contract_path.write_text(original_raw, encoding="utf-8")

    def test_legitimate_backslash_logical_paths_remain_accepted_and_unchanged(self):
        changed = contract()
        changed["identity"]["scene"]["logical_path"] = r"res://fixture\scene.json"
        changed["identity"]["inputs"][0]["logical_path"] = r"res://fixture\model.bin"
        changed["identity"]["inputs"][1]["logical_path"] = r"res://fixture\texture.bin"
        self.write_json(self.contract_path, changed)
        self.contract_hash = hashlib.sha256(self.contract_path.read_bytes()).hexdigest()

        for manifest_path in (self.baseline_manifest, self.candidate_manifest):
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["contract_sha256"] = self.contract_hash
            manifest["identity"] = changed["identity"]
            self.write_json(manifest_path, manifest)
        approval = json.loads(self.approval_path.read_text(encoding="utf-8"))
        approval["contract_sha256"] = self.contract_hash
        self.write_json(self.approval_path, approval)

        result = self.compare(
            "--baseline", self.baseline_manifest,
            "--candidate", self.candidate_manifest,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(
            json.loads(self.baseline_manifest.read_text(encoding="utf-8"))["identity"],
            changed["identity"],
        )

    def test_image_digest_and_decoded_viewport_are_verified(self):
        self.write_fixture(self.candidate_image, swatch=(1, 2, 3))
        result = invoke("validate", "--contract", self.contract_path,
                        "--manifest", self.candidate_manifest)
        self.assertEqual(result.returncode, 2)
        self.assertIn("digest mismatch", result.stderr)

        Image.new("RGB", (11, 8), (0, 0, 0)).save(self.candidate_image)
        self.candidate_manifest = self.write_manifest("candidate", self.candidate_image)
        result = invoke("validate", "--contract", self.contract_path,
                        "--manifest", self.candidate_manifest)
        self.assertEqual(result.returncode, 2)
        self.assertIn("do not match contract", result.stderr)

    def test_contract_rejects_cropping_missing_full_frame_and_unjustified_limits(self):
        invalid = json.loads(json.dumps(self.contract_value))
        invalid["regions"][0]["rect"] = [1, 0, 11, 8]
        self.write_json(self.contract_path, invalid)
        result = invoke("validate", "--contract", self.contract_path,
                        "--manifest", self.baseline_manifest)
        self.assertEqual(result.returncode, 2)
        self.assertIn("exactly one full-frame", result.stderr)

        invalid = contract()
        invalid["regions"][0]["limits"]["rgb_mae"]["rationale"] = ""
        self.write_json(self.contract_path, invalid)
        result = invoke("validate", "--contract", self.contract_path,
                        "--manifest", self.baseline_manifest)
        self.assertEqual(result.returncode, 2)
        self.assertIn("rationale", result.stderr)

    def test_manifest_cannot_reference_an_image_outside_its_directory(self):
        for image_path in ("../baseline.png", r"..\baseline.png"):
            with self.subTest(image_path=image_path):
                value = json.loads(self.baseline_manifest.read_text(encoding="utf-8"))
                value["image"]["path"] = image_path
                directory = self.root / "subdir"
                directory.mkdir(exist_ok=True)
                path = directory / "baseline.json"
                self.write_json(path, value)
                result = invoke("validate", "--contract", self.contract_path, "--manifest", path)
                self.assertEqual(result.returncode, 2)
                self.assertIn("parent traversal", result.stderr)


if __name__ == "__main__":
    unittest.main()
