import hashlib
import json
import os
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCENE_HASH = "de673739583babf0a4541feafc6bd1f0c40da36788a962e7d49b284be7611a18"


class GodotPrototypeContract(unittest.TestCase):
    def test_frozen_scene_and_selected_pass(self):
        path = ROOT / "prototypes/common/scene.json"
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), SCENE_HASH)
        scene = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(scene["material"]["runtime_technique"], "sph_t0")
        self.assertEqual(scene["material"]["runtime_pass"], "sph_t0_p0")

    def test_server_owned_scene(self):
        project = ROOT / "prototypes/godot/project"
        source = (ROOT / "prototypes/godot/src/prototype_host.cpp").read_text(encoding="utf-8")
        self.assertNotIn("MeshInstance3D", source)
        self.assertNotIn("MeshInstance3D", (project / "main.tscn").read_text(encoding="utf-8"))
        for token in ("mesh_create", "material_create", "instance_create", "free_rid"):
            self.assertIn(token, source)

    def test_no_godot_types_cross_core_boundaries(self):
        paths = (
            ROOT / "include/eawr/sim", ROOT / "src/sim",
            ROOT / "include/eawr/assets", ROOT / "src/assets",
            ROOT / "include/eawr/vfs", ROOT / "src/vfs",
        )
        for folder in paths:
            for path in folder.rglob("*"):
                if path.suffix in {".cpp", ".hpp", ".h"}:
                    text = path.read_text(encoding="utf-8")
                    self.assertNotIn("godot_cpp", text, str(path))

    def test_adapter_is_programmable_not_pbr(self):
        source = (ROOT / "prototypes/godot/src/shader_adapter.hpp").read_text(encoding="utf-8")
        for token in ("eawr_sph_r", "half_direction", "16.0", "base_sample.a", "unshaded",
                      "eawr_linear_to_srgb(eawr_linear_rgb)"):
            self.assertIn(token, source)
        self.assertIn("eawr_srgb_to_linear(base_sample.rgb)", source)
        for forbidden in ("StandardMaterial", "roughness", "metallic", "schlick"):
            self.assertNotIn(forbidden, source)

    def test_independent_modern_shader_smoke(self):
        source = (ROOT / "prototypes/godot/src/modern_shader_smoke.hpp").read_text(encoding="utf-8")
        for token in ("void vertex()", "void fragment()", "authored_texture",
                      "authored_accent", "authored_displacement"):
            self.assertIn(token, source)
        self.assertNotIn("MeshGloss", source)

    def test_dependency_pins_match(self):
        pins = json.loads((ROOT / "prototypes/godot/dependencies.json").read_text(encoding="utf-8"))
        self.assertEqual(pins["godot"]["version"], "4.7.2-stable")
        self.assertEqual(pins["godot_cpp"]["version"], "10.0.0-stable")
        self.assertEqual(pins["godot"]["license"], "MIT")
        self.assertEqual(pins["godot_cpp"]["license"], "MIT")
        self.assertEqual(pins["arm_gnu_toolchain"]["version"], "14.2.Rel1")
        self.assertEqual(pins["arm_gnu_toolchain"]["target"], "aarch64-none-linux-gnu")

    def test_report_when_supplied(self):
        report_path = os.environ.get("EAWR_GODOT_REPORT")
        if not report_path:
            self.skipTest("pass a private report path to validate measured evidence")
        report = json.loads(pathlib.Path(report_path).read_text(encoding="utf-8"))
        for key in ("backend", "versions", "platform", "hardware", "scene_hash",
                    "settings", "samples", "metrics", "effort", "limitations", "artifacts"):
            self.assertIn(key, report)
        self.assertEqual(report["scene_hash"]["scene_json"], SCENE_HASH)
        self.assertEqual(report["material"]["technique"], "sph_t0")
        self.assertEqual(report["material"]["pass"], "sph_t0_p0")
        self.assertFalse(report["material"]["stock_material_substitute"])
        if not report["platform"]["headless"]:
            self.assertEqual(len(report["samples"]["cpu_submission_ms"]), 600)
            self.assertFalse(report["samples"]["cross_prototype_comparable"])
            self.assertEqual(len(report["samples"]["cpu_snapshot_adaptation_ms"]), 600)
            self.assertTrue(report["samples"]["common_cross_prototype_candidate"])
            self.assertTrue(report["samples"]["common_cross_prototype_comparable"])
            self.assertEqual(report["samples"]["common_comparison_status"],
                             "matching SDL boundary implemented")
            self.assertEqual(len(report["samples"]["frame_cadence_ms"]), 600)
            self.assertTrue(report["samples"]["frame_cadence_cross_prototype_comparable"])
            self.assertGreater(report["metrics"]["process_rss_bytes"], 0)
            self.assertTrue(report["modern_shader"]["passed"])
            self.assertTrue(report["capture"]["baseline_camera_restored_before_capture"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
