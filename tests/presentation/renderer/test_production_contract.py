import hashlib
import json
import pathlib
import re
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import combined_source, mode_source, source_text  # noqa: E402
SCENE_HASH = "de673739583babf0a4541feafc6bd1f0c40da36788a962e7d49b284be7611a18"
RENDERER_SOURCES = ("renderer.cpp", "renderer_fog.cpp", "renderer_instances.cpp", "renderer_internal.hpp",
                    "renderer_upload.cpp")


def renderer_source() -> str:
    return combined_source(*(f"src/presentation/godot/{name}" for name in RENDERER_SOURCES))


def production_hover_sources():
    return {name: source_text(path) for name, path in {
        "panel_header": "src/presentation/godot/ui/production_view.hpp",
        "panel": "src/presentation/godot/ui/production_view.cpp",
        "map": "apps/viewer/src/map_mode_hud.cpp",
        "hud_header": "src/presentation/godot/ui/tactical_hud.hpp",
        "hud": "src/presentation/godot/ui/tactical_hud_resources.cpp",
    }.items()}


def production_hover_polling_violations(sources):
    """Bind the allocation-tested cache to the actual three viewer adapters.

    These deliberately constrain the borrowing API, direct handoff and ordering,
    rather than counting cache publications (which cannot see a caller's copy).
    """
    def compact(source):
        return re.sub(r"\s+", "", re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S))

    text = {key: compact(value) for key, value in sources.items()}
    violations = []
    if "constHover*hovered_entry()const;" not in text["panel_header"]:
        violations.append("panel return must borrow the retained hover")
    getter = text["panel"].split("EawrProductionPanel::hovered_entry()const{", 1)[1].split("}", 1)[0]
    if getter != "return!dragging_pool_&&is_visible_in_tree()?hover_.get():nullptr;":
        violations.append("panel getter must directly borrow the allocation-tested cache")
    handoff = text["map"].split("voidMapMode::State::sync_cards(){", 1)[1].split("if(live_session)", 1)[0]
    if handoff != ("if(!hud||!battle)return;constauto*production_hover="
                   "hud->shell_shown()&&hud->production()?hud->production()->hovered_entry():nullptr;"
                   "hud->set_production_hover(production_hover);"):
        violations.append("MapMode must pass the borrowed pointer without an owning local")
    parameter = "set_production_hover(constEawrProductionPanel::Hover*hovered)"
    if parameter + ";" not in text["hud_header"] or "TacticalHud::" + parameter + "{" not in text["hud"]:
        violations.append("HUD parameter must borrow the panel hover")
    hud = text["hud"].split("TacticalHud::set_production_hover(", 1)[1].split("{", 1)[1]
    guard = "if(state.production_hover?hovered&&*state.production_hover==*hovered:!hovered)return;"
    if guard not in hud or hud.split(guard, 1)[0] != "State&state=*state_;if(!state.cards)return;":
        violations.append("HUD unchanged guard must precede owned storage and content resolution")
    return violations


class ProductionRendererContract(unittest.TestCase):
    def test_production_hover_polling_borrows_through_panel_map_and_hud(self):
        self.assertEqual(production_hover_polling_violations(production_hover_sources()), [])

    def test_production_hover_contract_rejects_old_owning_polling(self):
        # Keep ProductionHoverCache intact: restore only the old adapter ownership.
        # This must fail for the original string-copy path, not a missing cache API.
        sources = production_hover_sources()
        sources["panel_header"] = sources["panel_header"].replace(
            "const Hover* hovered_entry() const;", "std::optional<Hover> hovered_entry() const;")
        sources["panel"] = sources["panel"].replace(
            "const EawrProductionPanel::Hover* EawrProductionPanel::hovered_entry() const {\n"
            "    return !dragging_pool_ && is_visible_in_tree() ? hover_.get() : nullptr;\n}",
            "std::optional<EawrProductionPanel::Hover> EawrProductionPanel::hovered_entry() const {\n"
            "    const auto* retained = !dragging_pool_ && is_visible_in_tree() ? hover_.get() : nullptr;\n"
            "    return retained ? std::optional(*retained) : std::nullopt;\n}")
        sources["map"] = sources["map"].replace(
            "const auto* production_hover = hud->shell_shown() && hud->production()\n"
            "        ? hud->production()->hovered_entry() : nullptr;",
            "const auto production_hover = hud->shell_shown() && hud->production()\n"
            "        ? hud->production()->hovered_entry() : std::nullopt;")
        for key in ("hud_header", "hud"):
            sources[key] = sources[key].replace(
                "set_production_hover(const EawrProductionPanel::Hover* hovered)",
                "set_production_hover(const std::optional<EawrProductionPanel::Hover>& hovered)")
        sources["hud"] = sources["hud"].replace(
            "if (state.production_hover ? hovered && *state.production_hover == *hovered : !hovered) return;",
            "if (state.production_hover == hovered) return;").replace(
                "state.production_hover = hovered ? std::optional(*hovered) : std::nullopt;",
                "state.production_hover = hovered;")
        original = production_hover_sources()
        for key in sources:
            self.assertNotEqual(sources[key], original[key], key)
        violations = production_hover_polling_violations(sources)
        self.assertIn("panel return must borrow the retained hover", violations)
        self.assertIn("MapMode must pass the borrowed pointer without an owning local", violations)
        self.assertIn("HUD parameter must borrow the panel hover", violations)
        self.assertIn("HUD unchanged guard must precede owned storage and content resolution", violations)

    def test_production_hover_contract_rejects_copy_between_borrowed_apis(self):
        sources = production_hover_sources()
        # Even unchanged pointer APIs must not allow MapMode to copy the pointee.
        sources["map"] = sources["map"].replace(
            "hud->set_production_hover(production_hover);",
            "const auto owned = production_hover ? std::optional(*production_hover) : std::nullopt;\n"
            "    hud->set_production_hover(owned ? &*owned : nullptr);")
        self.assertIn("MapMode must pass the borrowed pointer without an owning local",
                      production_hover_polling_violations(sources))

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
            expected = stored_values(code(re.search(pattern, prototype, re.S).group(1)))
            if name == "meshgloss_shader_alpha":
                # FW-19 adds the live unit's opacity to this explicit-alpha
                # shader. Keep the frozen prototype and compare every other
                # byte, including the authored material alpha calculation.
                expected = expected.replace("shader_type spatial;", "shader_type spatial;\n"
                    "instance uniform float eawr_unit_opacity = 1.0;")
                expected = expected.replace("ALPHA = eawr_vertex_diffuse.a;",
                    "ALPHA = eawr_vertex_diffuse.a;\n    ALPHA *= eawr_unit_opacity;")
            self.assertEqual(
                expected,
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
        renderer = source_text("src/presentation/godot/renderer_upload.cpp")
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
        # The cached plan preserves pass priority and stable entity ordering;
        # capture evidence comes from those same uploaded pieces, even when
        # an unchanged frame needs no transform updates.
        self.assertIn("order_.plan(instances, upload_generation_", renderer)
        self.assertIn("pieces = order_.pieces()", renderer)
        self.assertRegex(renderer, r"if \(reordered\)\s*\{\s*submission_evidence_\.clear\(\)")
        self.assertIn("if (!pieces[index].uploaded) continue;", renderer)
        self.assertIn("instances[pieces[index].source]", renderer)
        self.assertIn("submission_evidence_.push_back({source.entity_id, source.asset_id, pieces[index].pass})", renderer)
        plan = (ROOT / "src/presentation/godot/submission_plan.hpp").read_text(encoding="utf-8")
        self.assertIn("upload_generation == generation_ && same_sequence(instances)", plan)
        self.assertIn("order_planned_pieces(instances, pieces_, scratch_)", plan)
        self.assertIn("render_pass_priority(pass)", plan)
        self.assertIn("instances[left.source].entity_id < instances[right.source].entity_id", plan)
        self.assertIn("std::stable_sort(first, last, by_entity)", plan)
        self.assertIn("ResourceLeaseLedger", renderer)
        self.assertIn("diagnostics_.push(valid.error())", renderer)
        self.assertIn("get_shader_parameter_list", renderer)
        self.assertIn("eawr_compile_probe", renderer)
        self.assertIn("is_valid_material_route", contract)
        self.assertIn("shader_compile_failed", contract)


if __name__ == "__main__":
    unittest.main()
