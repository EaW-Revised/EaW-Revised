"""Opt-in exploratory MC-50 Hull preview versus the frozen Hangar draw.

The structural assertions always run: they pin that the viewer routes the
fixed-scene selection through one plan, that the frozen Hangar identity still
matches the #22 capture manifest pins, and that only the exploratory Hull
preview reports a ``model_preview`` section, labelled exploratory and never
acceptance. The graphical assertions are opt-in: they need the pinned Godot
4.7.2 console binary, a real GPU and the installed read-only corpus roots
(``EAWR_EAW_GAME_ROOT`` and ``EAWR_REMAKE_MOD_ROOT``).
"""

import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

from PIL import Image


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "validation" / "p1_capture"))

import build_manifest  # noqa: E402
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import mode_source, source_text  # noqa: E402

PREVIEW_HEADER = ROOT / "apps/viewer/src/model_preview.hpp"
HULL_TEXTURE_SHA256 = "456e88d85c9569d173c2b68bc4cc59a51e5a201aa2bf4cac8ee23afc26fab7cb"


def header_constant(name):
    text = PREVIEW_HEADER.read_text(encoding="utf-8")
    match = re.search(rf"{name} =\s*\"([^\"]*)\";", text)
    if not match:
        raise AssertionError(f"{name} is not declared in model_preview.hpp")
    return match.group(1)


class HullPreviewContract(unittest.TestCase):
    def test_scene_builders_bind_retail_light_zero_specular(self):
        # The numerical R-LIT-04 contract lives in lighting_tests.cpp; ensure
        # both actual scene uploads use it instead of the raw environment RGB.
        for file, environment in (("map_mode_scene.cpp", "environment"),
                                  ("space_populate.cpp", "options.environment")):
            with self.subTest(scene=file):
                source = source_text("apps/viewer/src/" + file)
                self.assertIn(f"state.specular = lighting::sun_specular({environment});", source)
                self.assertNotIn(f"state.specular = {{{environment}.specular.r", source)

    def test_frozen_hangar_pins_match_the_capture_manifest(self):
        self.assertEqual(header_constant("pinned_model_path"), build_manifest.MODEL_LOGICAL_PATH)
        self.assertEqual(header_constant("pinned_model_sha256"), build_manifest.MODEL_SHA256)
        self.assertEqual(header_constant("hangar_texture_path"), build_manifest.TEXTURE_LOGICAL_PATH)
        self.assertEqual(header_constant("hangar_texture_sha256"), build_manifest.TEXTURE_SHA256)
        self.assertEqual(header_constant("hangar_program"), "MeshGloss.fx")

    def test_hull_carries_its_own_material_texture_and_pin(self):
        self.assertEqual(header_constant("hull_mesh"), "Hull")
        self.assertEqual(header_constant("hull_program"), "MeshBumpColorize.fx")
        self.assertEqual(header_constant("hull_base_texture"), "Rebel_Mon_Calamari_Tide.dds")
        self.assertEqual(header_constant("hull_texture_sha256"), HULL_TEXTURE_SHA256)
        self.assertNotEqual(HULL_TEXTURE_SHA256, build_manifest.TEXTURE_SHA256)

    def test_frozen_fixed_camera_defaults_are_unchanged(self):
        header = (ROOT / "include/eawr/presentation/renderer.hpp").read_text(encoding="utf-8")
        camera = re.search(r"struct FixedCamera final \{(.*?)\n\};", header, re.S).group(1)
        self.assertIn("width{1280}", camera)
        self.assertIn("height{720}", camera)
        self.assertIn("vertical_fov_degrees{45.0F}", camera)
        self.assertIn("near_plane{1.0F}", camera)
        self.assertIn("far_plane{20000.0F}", camera)
        self.assertIn("eye{0.0F, 420.0F, 1050.0F}", camera)
        self.assertEqual(build_manifest.CAMERA["position"], [0.0, 420.0, 1050.0])

    def test_viewer_selects_through_one_plan(self):
        source = mode_source("viewer_host")
        physical = (ROOT / "apps/viewer/src/viewer_host_scene.cpp").read_text(encoding="utf-8")
        self.assertNotIn("default_scene_model", source)
        for token in ("model_preview::plan_for(", "model_preview::select_submesh(",
                      "model_preview::texture_path_for(", "model_preview::rest_placement(",
                      "model_preview::world_bounds(", "model_preview::fit_camera(",
                      "adapt_snapshot(*fixed_capture_snapshot_)"):
            self.assertIn(token, source)
        # The frozen policy never writes the capture camera.
        load_scene = physical[physical.index("bool ViewerHost::load_scene()"):
                            physical.index("bool ViewerHost::apply_animation_pose(")]
        self.assertNotIn("CameraPolicy::frozen_fixed", load_scene)
        self.assertIn("CameraPolicy::legacy_mesh_bounds", load_scene)
        self.assertIn("CameraPolicy::hull_bounds_fit", load_scene)

    def test_only_the_exploratory_preview_reports_its_identity(self):
        source = mode_source("viewer_host")
        physical = (ROOT / "apps/viewer/src/viewer_host_report_scene.cpp").read_text(encoding="utf-8")
        writer = physical[physical.index("void ViewerHost::write_report_scene("):]
        guard = writer.index("if (model_preview_ && model_preview_->plan.exploratory) {")
        self.assertLess(guard, writer.index('\\"model_preview\\"'))
        for token in ('\\"exploratory\\": true, \\"acceptance\\": false',
                      "production renderer exploratory preview",
                      '\\"material_fidelity\\": \\"unqualified\\"',
                      "not included: Hull mesh only"):
            self.assertIn(token, writer)
        self.assertIn('"exploratory_preview_captured" : "passed"', source)
        self.assertIn("verify_preview_capture(capture)", source)

    def test_capture_manifest_refuses_a_preview_section(self):
        self.assertNotIn("model_preview", build_manifest.PRODUCTION_REPORT_REQUIRED_KEYS)
        self.assertNotIn("model_preview", build_manifest.PRODUCTION_REPORT_OPTIONAL_FALSE_KEYS)


def run_viewer(workspace, *arguments):
    executable = os.environ["EAWR_GODOT_EXECUTABLE"]
    report = workspace / "report.json"
    completed = subprocess.run(
        [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
         "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
         "--eawr-mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"],
         "--eawr-report", str(report), "--eawr-benchmark", *arguments],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        timeout=600,
    )
    result = json.loads(report.read_text(encoding="utf-8")) if report.exists() else None
    return completed, result


def capture_sha256s(path):
    image_bytes = path.read_bytes()
    with Image.open(path) as image:
        rgba_bytes = image.convert("RGBA").tobytes()
    return (hashlib.sha256(image_bytes).hexdigest(),
            hashlib.sha256(rgba_bytes).hexdigest())


@unittest.skipUnless(
    os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_GODOT_EXECUTABLE")
    and os.environ.get("EAWR_EAW_GAME_ROOT") and os.environ.get("EAWR_REMAKE_MOD_ROOT"),
    "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE, EAWR_EAW_GAME_ROOT and "
    "EAWR_REMAKE_MOD_ROOT for the graphical Hull preview runs")
class HullPreviewGraphical(unittest.TestCase):
    def test_hull_preview_frames_the_whole_hull(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hull-preview-") as temporary:
            workspace = pathlib.Path(temporary)
            capture = workspace / "hull.png"
            completed, result = run_viewer(
                workspace, "--eawr-mesh", "Hull", "--eawr-capture", str(capture))
            self.assertEqual(completed.returncode, 0, completed.stdout)
            self.assertEqual(result["status"], "exploratory_preview_captured", result["failure"])
            self.assertEqual(result["capture_sha256"],
                             hashlib.sha256(capture.read_bytes()).hexdigest())
            self.assertEqual(result["material_program"], "MeshBumpColorize.fx")
            self.assertEqual(result["texture"]["sha256"], HULL_TEXTURE_SHA256)
            preview = result["model_preview"]
            self.assertEqual(preview["kind"], "exploratory_hull")
            self.assertIs(preview["exploratory"], True)
            self.assertIs(preview["acceptance"], False)
            self.assertEqual(preview["material_fidelity"], "unqualified")
            self.assertEqual(preview["mesh"]["name"], "Hull")
            self.assertEqual(preview["material"]["technique"], "t0")
            self.assertEqual(preview["texture"]["sha256"], preview["texture"]["expected_sha256"])
            self.assertEqual(preview["model"]["sha256"], build_manifest.MODEL_SHA256)
            self.assertEqual(preview["rest_hierarchy"]["chain"], ["Root", "Hull"])
            self.assertIs(preview["rest_hierarchy"]["trusted"], True)
            camera = preview["camera"]
            self.assertEqual(camera["policy"], "hull_bounds_fit")
            self.assertEqual(camera["position"], result["capture_identity"]["camera"]["position"])
            for key in ("min_x", "min_y"):
                self.assertGreaterEqual(camera["corner_ndc"][key], -1.0)
            for key in ("max_x", "max_y"):
                self.assertLessEqual(camera["corner_ndc"][key], 1.0)
            self.assertGreater(camera["corner_depth"][0], camera["near"])
            self.assertLess(camera["corner_depth"][1], camera["far"])
            self.assertIs(preview["capture"]["inside_viewport"], True)
            self.assertGreater(preview["capture"]["drawn_pixels"], 16)
            # The acceptance manifest refuses this report shape as unsupported.
            self.assertEqual(set(result) - build_manifest.PRODUCTION_REPORT_REQUIRED_KEYS
                             - build_manifest.PRODUCTION_REPORT_OPTIONAL_FALSE_KEYS - build_manifest.PRODUCTION_REPORT_OPTIONAL_KEYS,
                             {"model_preview"})

    def test_default_run_keeps_the_frozen_hangar_report(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hangar-default-") as temporary:
            workspace = pathlib.Path(temporary)
            completed, result = run_viewer(
                workspace, "--eawr-capture", str(workspace / "hangar.png"))
            self.assertEqual(completed.returncode, 0, completed.stdout)
            self.assertEqual(result["status"], "passed", result["failure"])
            self.assertNotIn("model_preview", result)
            self.assertEqual(set(result) - build_manifest.PRODUCTION_REPORT_REQUIRED_KEYS
                             - build_manifest.PRODUCTION_REPORT_OPTIONAL_FALSE_KEYS - build_manifest.PRODUCTION_REPORT_OPTIONAL_KEYS, set())
            self.assertEqual(result["material_program"], "MeshGloss.fx")
            self.assertEqual(result["texture"], {"logical_path": build_manifest.TEXTURE_LOGICAL_PATH,
                                                 "sha256": build_manifest.TEXTURE_SHA256})
            self.assertEqual(result["capture_identity"]["camera"]["position"],
                             build_manifest.CAMERA["position"])

    def test_explicit_hangar_matches_default_in_the_same_build(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hangar-preservation-") as temporary:
            workspace = pathlib.Path(temporary)
            default_workspace = workspace / "default"
            explicit_workspace = workspace / "explicit"
            default_workspace.mkdir()
            explicit_workspace.mkdir()
            default_capture = default_workspace / "hangar.png"
            explicit_capture = explicit_workspace / "hangar.png"

            default_completed, default_report = run_viewer(
                default_workspace, "--eawr-capture", str(default_capture))
            explicit_completed, explicit_report = run_viewer(
                explicit_workspace, "--eawr-mesh", "Hangar",
                "--eawr-capture", str(explicit_capture))

            self.assertEqual(default_completed.returncode, 0, default_completed.stdout)
            self.assertEqual(explicit_completed.returncode, 0, explicit_completed.stdout)
            self.assertEqual(default_report["status"], "passed", default_report["failure"])
            self.assertEqual(explicit_report["status"], "passed", explicit_report["failure"])
            self.assertNotIn("model_preview", default_report)
            self.assertNotIn("model_preview", explicit_report)
            self.assertEqual(set(default_report), set(explicit_report))
            for report in (default_report, explicit_report):
                self.assertEqual(set(report) - build_manifest.PRODUCTION_REPORT_REQUIRED_KEYS
                                 - build_manifest.PRODUCTION_REPORT_OPTIONAL_FALSE_KEYS - build_manifest.PRODUCTION_REPORT_OPTIONAL_KEYS, set())

            default_png_sha, default_rgba_sha = capture_sha256s(default_capture)
            explicit_png_sha, explicit_rgba_sha = capture_sha256s(explicit_capture)
            self.assertEqual(default_report["capture_sha256"], default_png_sha)
            self.assertEqual(explicit_report["capture_sha256"], explicit_png_sha)
            self.assertEqual(default_png_sha, explicit_png_sha)
            self.assertEqual(default_rgba_sha, explicit_rgba_sha)

    def test_hull_preview_rejects_a_texture_override(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hull-reject-") as temporary:
            workspace = pathlib.Path(temporary)
            completed, result = run_viewer(
                workspace, "--eawr-mesh", "Hull", "--eawr-texture",
                build_manifest.TEXTURE_LOGICAL_PATH)
            self.assertEqual(completed.returncode, 2, completed.stdout)
            self.assertEqual(result["status"], "failed")
            self.assertIn("--eawr-texture is not accepted", result["failure"])
            self.assertNotIn("model_preview", result)


if __name__ == "__main__":
    unittest.main()
