"""Synthetic tests for the P1-01 Windows report-to-manifest builder.

Every fixture is generated here: the PNG is drawn with Pillow, reports and
receipts are synthetic, and only the tracked public scene and replay fixtures
are copied (their frozen hashes are part of the identity under test).  No Godot
process, retail game or private asset is used.
"""

import contextlib
import copy
import hashlib
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

from PIL import Image


ROOT = pathlib.Path(__file__).resolve().parents[3]
TOOL_DIRECTORY = ROOT / "tools/validation/p1_capture"
TOOL = TOOL_DIRECTORY / "build_manifest.py"
COMPARE = TOOL_DIRECTORY / "compare.py"
PREFLIGHT = TOOL_DIRECTORY / "check_migration_pair.py"
SCENE_SOURCE = ROOT / "prototypes/common/scene.json"
REPLAY_SOURCE = ROOT / "tests/replay/fixtures/original-v1.eawr-replay"

if str(TOOL_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(TOOL_DIRECTORY))

import build_manifest  # noqa: E402


REVISION = "0123456789abcdef0123456789abcdef01234567"
VENDOR = "ATI Technologies Inc."
ADAPTER = "AMD Radeon RX 7900 XTX"
GL_API = "4.6.0 Core Profile Context 26.9.1.260826"
DRIVER_VERSION = "32.0.31041.3013"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    return path


def region(name, rect):
    limit = "Synthetic builder fixture."
    return {
        "name": name,
        "rect": rect,
        "pixel_channel_threshold": 0,
        "edge_threshold": 8,
        "threshold_rationale": limit,
        "limits": {
            "rgb_mae": {"max": 0.0, "rationale": limit},
            "rgb_p95": {"max": 0.0, "rationale": limit},
            "alpha_mae": {"max": 0.0, "rationale": limit},
            "changed_pixel_ratio": {"max": 0.0, "rationale": limit},
            "edge_iou": {"min": 1.0, "rationale": limit},
        },
    }


def contract():
    return {
        "schema_version": 1,
        "contract_id": "synthetic-windows-builder-v1",
        "identity": copy.deepcopy(build_manifest.FROZEN_IDENTITY),
        "regions": [region("full-frame", [0, 0, 1280, 720]),
                    region("frozen-model-footprint", [620, 320, 40, 30])],
    }


def prototype_report():
    return {
        "schema_version": 1,
        "backend": {"engine": "Godot", "adapter": "RenderingServer GDExtension",
                    "rendering_method": "gl_compatibility", "api": GL_API},
        "versions": {"godot": "4.7.2-stable", "godot_cpp": "10.0.0-stable", "gdextension_api": "4.7"},
        "platform": {"os": "Windows", "distribution": "", "version": "10.0.26200", "headless": False},
        "hardware": {"adapter_vendor": VENDOR, "adapter_name": ADAPTER},
        "scene_hash": {"scene_json": build_manifest.SCENE_SHA256, "model": build_manifest.MODEL_SHA256,
                       "texture": build_manifest.TEXTURE_SHA256},
        "settings": {"resolution": [1280, 720], "vsync": True, "warmup_frames": 120, "sample_frames": 600,
                     "fixed_camera_frame": 120, "axis_conversion": "x,z,-y", "texture_colour_space": "srgb",
                     "resize_probe": False, "output_transfer": "synthetic", "vsync_observed_mode": 1,
                     "tone_mapper": "linear", "exposure": 1.0},
        "controls": {"interactive_orbit": "left-mouse drag", "interactive_zoom": "mouse wheel",
                     "probe_requested": False, "probe_exercised": False,
                     "probe_orbit_yaw_radians": 0.0, "probe_zoom_distance": 1129.0},
        "capture": {"baseline_camera_restored_before_capture": True, "closeup_contract": "supplemental_bounds_v1",
                    "closeup_done": False, "closeup_snapshot_index": 0, "closeup_distance": 90.0,
                    "bounds_center": [0.0, -2.766709, -104.65802], "bounds_radius": 27.696695,
                    "eye": [0.0, 30.658451, -21.095116]},
        "samples": {"cpu_submission_ms": [0.01]},
        "metrics": {"draw_calls_last_frame": 1},
        "effort": {"measurement": "synthetic"},
        "limitations": ["synthetic"],
        "artifacts": {"capture": "synthetic"},
        "material": {"effect": "MeshGloss.fx", "technique": "sph_t0", "pass": "sph_t0_p0",
                     "stock_material_substitute": False},
        "modern_shader": {"passed": True},
        "snapshot": {"source": "synthetic", "snapshot_count": 6, "replay_hashes": ["a" * 64] * 5},
        "status": "passed",
        "failure": "",
    }


def production_report(image_sha256):
    return {
        "schema_version": 1,
        "status": "passed",
        "failure": "",
        "backend": {"engine": "Godot 4.7.2-stable", "rendering_method": "gl_compatibility",
                    "adapter_vendor": VENDOR, "adapter_name": ADAPTER, "driver_api": GL_API},
        "versions": {"godot": "4.7.2-stable", "godot_cpp": "10.0.0-stable", "material_schema": 1},
        "scene_sha256": build_manifest.SCENE_SHA256,
        # The host records the CLI path it was given; it must never reach the manifest.
        "replay": {"logical_path": "Z:/synthetic/absolute/original-v1.eawr-replay",
                   "sha256": build_manifest.REPLAY_SHA256},
        "snapshot_count": 6,
        "capture_sha256": image_sha256,
        "capture_identity": {"vfs_profile": "remake", "viewport": {"width": 1280, "height": 720},
                             "camera": {"projection": "perspective", "position": [0, 420, 1050],
                                        "target": [0, 0, 0], "up": [0, 1, 0], "fov_degrees": 45,
                                        "near": 1, "far": 20000},
                             "simulation_tick": 1, "capture_frame": 120, "animation_time_seconds": 0,
                             "particle_seed": 0, "particle_time_seconds": 0.0},
        "model": {"logical_path": build_manifest.MODEL_LOGICAL_PATH, "sha256": build_manifest.MODEL_SHA256},
        "texture": {"logical_path": build_manifest.TEXTURE_LOGICAL_PATH,
                    "sha256": build_manifest.TEXTURE_SHA256},
        "material_program": "MeshGloss.fx",
        "animation": {"logical_path": "", "sha256": "", "bone_count": 51},
        "animation_time_seconds": 0,
        "skin_palette_bound": True,
        "animation_capture_verified": True,
        "renderer_runtime_exercise": False,
        "runtime_capture_verified": False,
        "runtime_overlap_verified": False,
        "runtime_post_dependency_verified": False,
        "runtime_failed_upload_registry_clean": False,
        "runtime_scene_switch_count": 0,
        "runtime_shutdown_resources_empty": False,
        "tactical_camera_verified": False,
        "atlas_overlay_verified": False,
        "pass_order": ["opaque"],
    }


def draw_png(path, size=(1280, 720)):
    image = Image.new("RGBA", size, (2, 3, 6, 255))
    for y in range(320, 350):
        for x in range(620, 660):
            if x < size[0] and y < size[1]:
                image.putpixel((x, y), (120 + (x % 7), 90, 60 + (y % 5), 255))
    image.save(path, format="PNG")
    return path


class Fixture:
    """One synthetic run directory plus shared evidence inputs."""

    def __init__(self, root, host):
        self.root = root
        self.host = host
        self.evidence = root / "evidence"
        self.run = root / "run"
        self.evidence.mkdir()
        self.run.mkdir()
        self.contract = write_json(self.evidence / "contract.json", contract())
        self.scene = self.evidence / "scene.json"
        shutil.copyfile(SCENE_SOURCE, self.scene)
        self.replay = self.evidence / "original-v1.eawr-replay"
        shutil.copyfile(REPLAY_SOURCE, self.replay)
        self.image = draw_png(self.run / "frame.png")
        self.image_sha256 = sha256(self.image.read_bytes())
        self.report = self.run / "report.json"
        self.build_receipt = self.evidence / "build-receipt.txt"
        self.build_receipt.write_text(f"synthetic build receipt {host} {REVISION}\n", encoding="utf-8")
        self.runtime_receipt = self.evidence / "runtime-receipt.json"
        write_json(self.runtime_receipt, {"synthetic": "windows-adapters", "host": host})
        self.derivation = self.evidence / "capture-derivation.json"
        self.supplement = self.evidence / "supplement.json"
        self.output = self.run / "manifest.json"
        if host == "prototype":
            self.write_report(prototype_report())
            self.write_derivation(self.derivation_value())
        else:
            self.write_report(production_report(self.image_sha256))
        self.write_supplement(self.supplement_value())

    def write_report(self, value):
        write_json(self.report, value)

    def derivation_value(self):
        return {
            "schema_version": 1,
            "kind": "p1-01-prototype-capture-derivation",
            "source_revision": REVISION,
            "source_logical_path": "prototypes/godot/src/prototype_host.cpp",
            "source_sha256": "b" * 64,
            "capture_process_frame_zero_based": 119,
            "snapshot_index": 1,
            "simulation_tick": 1,
            "animation_time_seconds": 0.0,
            "particle_seed": 0,
            "particle_time_seconds": 0.0,
            "replay_sha256": build_manifest.REPLAY_SHA256,
            "capture_sha256": self.image_sha256,
            "rationale": "Synthetic: capture at frame_ + 1 == 120 after applying snapshots[119 / 100].",
        }

    def write_derivation(self, value):
        write_json(self.derivation, value)

    def supplement_value(self):
        value = {
            "schema_version": 1,
            "kind": "p1-01-windows-manifest-supplement",
            "host": self.host,
            "platform": "windows",
            "architecture": "x86_64",
            "driver_version": DRIVER_VERSION,
            "build_revision": REVISION,
            "build_receipt_sha256": sha256(self.build_receipt.read_bytes()),
            "runtime_receipt_sha256": sha256(self.runtime_receipt.read_bytes()),
        }
        if self.host == "prototype":
            value["capture_derivation_receipt_sha256"] = sha256(self.derivation.read_bytes())
        return value

    def write_supplement(self, value):
        write_json(self.supplement, value)

    def rebind_supplement(self, **changes):
        value = self.supplement_value()
        value.update(changes)
        self.write_supplement(value)

    def argv(self, **overrides):
        options = {
            "--host": self.host,
            "--contract": str(self.contract),
            "--report": str(self.report),
            "--scene": str(self.scene),
            "--replay": str(self.replay),
            "--image": str(self.image),
            "--supplement": str(self.supplement),
            "--build-receipt": str(self.build_receipt),
            "--runtime-receipt": str(self.runtime_receipt),
            "--role": "baseline" if self.host == "prototype" else "candidate",
            "--output": str(self.output),
        }
        if self.host == "prototype":
            options["--prototype-capture-derivation"] = str(self.derivation)
        for key, value in overrides.items():
            option = "--" + key.replace("_", "-")
            if value is None:
                options.pop(option, None)
            else:
                options[option] = str(value)
        argv = ["build"]
        for key, value in options.items():
            argv.extend([key, value])
        return argv


def run_builder(argv):
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        code = build_manifest.run(argv)
    return code, json.loads(stdout.getvalue()), stdout.getvalue()


class BuilderTestCase(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._temporary.name)

    def tearDown(self):
        self._temporary.cleanup()

    def fixture(self, host, name=None):
        self._count = getattr(self, "_count", 0) + 1
        directory = self.root / f"{self._count:03d}-{name or host}"
        directory.mkdir()
        return Fixture(directory, host)

    def assert_refused(self, fixture, expected_code, message_fragment, **overrides):
        code, receipt, _ = run_builder(fixture.argv(**overrides))
        self.assertEqual(code, expected_code, receipt)
        self.assertEqual(receipt["exit_code"], expected_code)
        self.assertEqual(receipt["status"], "failed" if expected_code == 1 else "error")
        self.assertIn(message_fragment, " ".join(receipt["errors"]))
        self.assertFalse(receipt["manifest"]["written"])
        self.assertFalse(receipt["visual_comparison"])
        self.assertFalse(receipt["acceptance"])
        self.assertFalse(fixture.output.exists(), "a failed build must not leave a manifest")
        return receipt

__all__ = [name for name in globals() if not name.startswith('__')]
