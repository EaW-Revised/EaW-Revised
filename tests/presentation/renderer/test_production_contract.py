import hashlib
import json
import pathlib
import re
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import mode_source  # noqa: E402
SCENE_HASH = "de673739583babf0a4541feafc6bd1f0c40da36788a962e7d49b284be7611a18"
RENDERER_SOURCES = ("renderer.cpp", "renderer_fog.cpp", "renderer_instances.cpp", "renderer_internal.hpp",
                    "renderer_upload.cpp")


def renderer_source() -> str:
    return "".join((ROOT / "src/presentation/godot" / name).read_text(encoding="utf-8") for name in RENDERER_SOURCES)


class ProductionRendererContract(unittest.TestCase):
    def test_frozen_scene_is_unchanged(self):
        scene = ROOT / "prototypes/common/scene.json"
        self.assertEqual(hashlib.sha256(scene.read_bytes()).hexdigest(), SCENE_HASH)
        data = json.loads(scene.read_text(encoding="utf-8"))
        self.assertEqual(data["material"]["runtime_technique"], "sph_t0")
        self.assertEqual(data["material"]["runtime_pass"], "sph_t0_p0")

    def test_rendering_server_rids_without_entity_nodes(self):
        source = renderer_source()
        for token in ("mesh_create", "material_create", "texture_2d_create",
                      "instance_create", "skeleton_create", "skeleton_bone_set_transform",
                      "instance_attach_skeleton", "ARRAY_BONES", "ARRAY_WEIGHTS", "free_rid"):
            self.assertIn(token, source)
        self.assertNotIn("MeshInstance3D", source)

    def test_prototype_shader_lifecycle_was_ported(self):
        prototype = (ROOT / "prototypes/godot/src/shader_adapter.hpp").read_text(encoding="utf-8")
        production = (ROOT / "src/presentation/godot/shader_adapter.hpp").read_text(encoding="utf-8")

        def code(text: str) -> str:
            return "\n".join(line for line in text.split("\n") if not line.strip().startswith("//"))

        def stored_values(text: str) -> str:
            # The colour policy (docs/rendering.md): no source_color hint, and the
            # prototype's sRGB-output (Compatibility) branch on every backend.
            text = text.replace(": source_color, ", ": ")
            return re.sub(r"OUTPUT_IS_SRGB\n\s*\? (.*?) : [^;]*;", r"\1;", text)

        for name in ("meshgloss_shader_opaque", "meshgloss_shader_alpha"):
            pattern = rf"{name} = R\"GODOT\((.*?)\)GODOT\";"
            self.assertEqual(
                stored_values(code(re.search(pattern, prototype, re.S).group(1))),
                code(re.search(pattern, production, re.S).group(1)),
            )

    def test_simulation_does_not_include_presentation(self):
        for folder in (ROOT / "include/eawr/sim", ROOT / "src/sim"):
            for path in folder.rglob("*"):
                if path.suffix in {".cpp", ".hpp", ".h"}:
                    text = path.read_text(encoding="utf-8")
                    self.assertNotIn("eawr/presentation", text, str(path))
                    self.assertNotIn("godot_cpp", text, str(path))

    def test_capture_crosses_the_platform_boundary_in_memory(self):
        public_header = (ROOT / "include/eawr/presentation/renderer.hpp").read_text(
            encoding="utf-8"
        )
        godot_source = (ROOT / "src/presentation/godot/renderer.cpp").read_text(
            encoding="utf-8"
        )
        viewer_source = mode_source("viewer_host")

        self.assertNotIn("filesystem", public_header)
        self.assertIn("png_bytes", public_header)
        self.assertIn("save_png_to_buffer", godot_source)
        self.assertNotIn("save_png(", godot_source)
        self.assertIn("write_capture(options_->capture_path", viewer_source)
        self.assertIn("hash_bytes(capture.png_bytes)", viewer_source)

    def test_viewer_preserves_godot_resource_identifiers(self):
        viewer_source = mode_source("viewer_host")
        path_source = (ROOT / "apps/viewer/src/viewer_path.hpp").read_text(
            encoding="utf-8"
        )

        self.assertIn('ViewerPath scene_path{std::string{"res://', viewer_source)
        self.assertIn('ViewerPath replay_path{std::string{"res://', viewer_source)
        self.assertNotIn("std::filesystem::path scene_path", viewer_source)
        self.assertNotIn("std::filesystem::path replay_path", viewer_source)
        self.assertIn('value_.starts_with("res://")', path_source)
        self.assertIn('value_.starts_with("user://")', path_source)
        self.assertIn("std::ifstream input(path.native()", viewer_source)

    def test_material_coverage_schema_matches_executable_routing(self):
        coverage = json.loads((ROOT / "plan/inventories/godot-material-coverage.json").read_text(
            encoding="utf-8"
        ))
        renderer = (ROOT / "src/presentation/godot/renderer_upload.cpp").read_text(encoding="utf-8")
        contract = (ROOT / "src/presentation/godot/renderer_contract.cpp").read_text(
            encoding="utf-8"
        )

        self.assertEqual(coverage["schema_version"], 1)
        self.assertEqual(coverage["routes"]["legacy_effect"]["selectors"][0]["program"],
                         "MeshGloss.fx")
        self.assertIn("RSkinBumpColorize.fx",
                      [row["program"] for row in coverage["routes"]["legacy_effect"]["selectors"]])
        self.assertEqual(coverage["pass_execution"]["order"],
                         ["opaque", "alpha-tested", "transparent", "post"])
        self.assertIn("material_set_render_priority", renderer)
        self.assertIn("legacy_shader_source", renderer)
        self.assertIn("if (!selected) return MaterialUpload::failed", renderer)
        self.assertNotIn("source.program == \"MeshGloss.fx\"\n                ?", renderer)

    def test_capture_report_records_observed_identity_and_lifecycle(self):
        source = mode_source("viewer_host")
        renderer = renderer_source()
        contract = (ROOT / "src/presentation/godot/renderer_contract.cpp").read_text(
            encoding="utf-8"
        )
        for token in ("capture_identity", "simulation_tick", "runtime_overlap_verified",
                      "runtime_post_dependency_verified", "runtime_scene_switch_count",
                      "submission_evidence"):
            self.assertIn(token, source)
        self.assertNotIn(
            '"pass_order": ["opaque", "alpha-tested", "transparent", "post"]',
            source,
        )
        self.assertIn("order_pass_submissions(routed)", renderer)
        self.assertIn("ResourceLeaseLedger", renderer)
        self.assertIn("diagnostics_.push(valid.error())", renderer)
        self.assertIn("get_shader_parameter_list", renderer)
        self.assertIn("eawr_compile_probe", renderer)
        self.assertIn("is_valid_material_route", contract)
        self.assertIn("shader_compile_failed", contract)


if __name__ == "__main__":
    unittest.main()
