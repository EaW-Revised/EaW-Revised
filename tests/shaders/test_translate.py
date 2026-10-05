from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

from tools.shaders.fx_parser import FxError, IncludeResolver, lex, parse_effect, source_without_effect_wrappers
from tools.shaders.translate import (evaluate_dynamic_bool, evaluate_fixed_function,
                                      fixed_function_ir, select_compatible, COMPATIBILITY,
                                      ShaderPolicyError, _find_tool, _resolve_output_path,
                                      is_editor_technique, main, normalize_legacy_hlsl,
                                      skin_transform, translate)


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures"
TOOLCHAIN = ROOT / "out" / "tools" / "glslang-16.5.0" / "bin"
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
GLSLANG = TOOLCHAIN / f"glslang{EXE_SUFFIX}"
SPIRV_VAL = TOOLCHAIN / f"spirv-val{EXE_SUFFIX}"


class LexerAndResolverTests(unittest.TestCase):
    def test_comments_strings_and_nested_blocks(self):
        tokens = lex('string path = "x//y"; /* { ignored } */ float f(){ return (1 < 2) ? 1 : 0; }')
        self.assertIn("x//y", [token.value.strip('"') for token in tokens])
        effect = parse_effect('float4 f(float4 x) { if (x.x < 1) { x.x += 1; } return x; }')
        self.assertEqual([], effect.techniques)

    def test_depth_first_case_insensitive_include_and_comment(self):
        result = IncludeResolver(FIXTURES).resolve("Synthetic.fx")
        self.assertEqual(["Common.fxh"], [item["path"] for item in result.includes])
        self.assertNotIn("not-active.fxh", [item["path"] for item in result.includes])

    def test_cycle_reports_original_path(self):
        with self.assertRaises(FxError) as raised:
            IncludeResolver(FIXTURES).resolve("Cycle.fx")
        self.assertEqual("SHD_INCLUDE_CYCLE", raised.exception.code)
        self.assertIn("cycle-a.fxh", str(raised.exception))

    def test_missing_include_retains_direct_and_nested_source_locations(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (root / "MissingLine.fx").open(
                    "w", encoding="utf-8", newline="\n") as stream:
                stream.write("// one\n// two\n// three\n#include \"missing.fxh\"\n")
            with self.assertRaises(FxError) as raised:
                IncludeResolver(root).resolve("MissingLine.fx")
            self.assertEqual(("SHD_INCLUDE_NOT_FOUND", "MissingLine.fx", 4, 1),
                             (raised.exception.code, raised.exception.path,
                              raised.exception.line, raised.exception.column))

            (root / "Nested.fx").write_text("#include \"nested.fxh\"\n", encoding="utf-8")
            with (root / "nested.fxh").open(
                    "w", encoding="utf-8", newline="\n") as stream:
                stream.write("// one\n// two\n#include \"missing-again.fxh\"\n")
            with self.assertRaises(FxError) as raised:
                IncludeResolver(root).resolve("Nested.fx")
            self.assertEqual(("nested.fxh", 3, 1),
                             (raised.exception.path, raised.exception.line, raised.exception.column))


class ParserTests(unittest.TestCase):
    def setUp(self):
        self.resolved = IncludeResolver(FIXTURES).resolve("Synthetic.fx")
        self.effect = parse_effect(self.resolved.text, "Synthetic.fx")

    def test_parameters_samplers_techniques_and_cleanup(self):
        parameters = {item.name: item for item in self.effect.parameters}
        self.assertEqual("VIEWPROJECTION", parameters["ViewProjection"].semantic)
        self.assertEqual("{ 1, 0.5, 0.25, 1 }", parameters["Tint"].default)
        self.assertEqual("BaseTexture", self.effect.samplers[0].texture)
        self.assertEqual(["Preferred", "Fixed"], [item.name for item in self.effect.techniques])
        self.assertEqual("synthetic_vs", self.effect.techniques[0].passes[0].vertex_shader.entry)
        self.assertFalse(self.effect.techniques[1].passes[1].draw)

    def test_wrapper_removal_preserves_logic(self):
        lowered = source_without_effect_wrappers(self.resolved.text, self.effect)
        self.assertIn("return tex2D(BaseSampler, input.uv) * input.color;", lowered)
        self.assertNotIn("technique Preferred", lowered)
        self.assertIn("sampler2D BaseSampler", lowered)

    def test_dynamic_state_remains_predicate(self):
        expression = self.effect.techniques[0].passes[0].states["AlphaBlendEnable"]
        self.assertFalse(evaluate_dynamic_bool(expression, {"Opacity": 1.0}))
        self.assertTrue(evaluate_dynamic_bool(expression, {"Opacity": 0.5}))

    def test_corpus_gap_shader_objects_stateblock_and_directive_comment(self):
        resolved = IncludeResolver(FIXTURES).resolve("CorpusGaps.fx")
        effect = parse_effect(resolved.text, "CorpusGaps.fx")
        lowered = source_without_effect_wrappers(resolved.text, effect)
        shader_objects = {
            (item.type.casefold(), item.name)
            for item in effect.parameters
            if item.type.casefold() in {"vertexshader", "pixelshader"}
        }
        self.assertEqual(
            {("vertexshader", "corpus_vs_bin"), ("pixelshader", "corpus_ps_bin")},
            shader_objects,
        )
        self.assertNotIn("compile vs_1_1", lowered)
        self.assertNotIn("compile ps_1_1", lowered)
        self.assertNotIn("stateblock_state", lowered)
        self.assertNotIn("unused_path", lowered)

    def test_legacy_vector_aggregate_and_editor_technique(self):
        resolved = IncludeResolver(FIXTURES).resolve("LegacyAggregate.fx")
        effect = parse_effect(resolved.text, "LegacyAggregate.fx")
        lowered = normalize_legacy_hlsl(
            source_without_effect_wrappers(resolved.text, effect)
        )
        self.assertIn("float2(-1, 0)", lowered)
        self.assertIn("float2(0, -1)", lowered)
        already_explicit = "const float2 Values[2] = { float2(1, 2), float2(3, 4) };"
        self.assertEqual(already_explicit, normalize_legacy_hlsl(already_explicit))
        self.assertEqual(["max_viewport"], [
            item.name for item in effect.techniques if is_editor_technique(item)
        ])
        self.assertEqual(["runtime"], [
            item.name for item in effect.techniques if not is_editor_technique(item)
        ])


class BehaviourContractTests(unittest.TestCase):
    def test_skin_vector_left_orientation(self):
        palette = [(0.0, 0.0, 0.0, 0.0)] * (23 * 3)
        palette.extend([(1, 0, 0, 10), (0, 1, 0, 20), (0, 0, 1, 30)])
        position, normal = skin_transform((2, 3, 4, 1), (0, 1, 0), palette, 23)
        self.assertEqual((12, 23, 34), position)
        self.assertEqual((0, 1, 0), normal)

    def test_fixed_color_alpha_are_independent(self):
        states = {
            "Texture[0]": "BaseSampler", "ColorOp[0]": "BLENDTEXTUREALPHA",
            "ColorArg1[0]": "TFACTOR", "ColorArg2[0]": "TEXTURE",
            "AlphaOp[0]": "SELECTARG1", "AlphaArg1[0]": "TEXTURE",
            "ColorOp[1]": "MODULATE2X", "ColorArg1[1]": "DIFFUSE", "ColorArg2[1]": "CURRENT",
            "AlphaOp[1]": "SELECTARG1", "AlphaArg1[1]": "DIFFUSE", "ColorOp[2]": "DISABLE",
        }
        from tools.shaders.fx_parser import Pass
        ir = fixed_function_ir(Pass("p0", states, None, None, 1))
        result = evaluate_fixed_function(ir, (0.5, 0.5, 0.5, 0.4),
                                         {0: (0.2, 0.4, 0.6, 0.25)}, (1, 0, 0, 1))
        for actual, expected in zip(result, (0.4, 0.3, 0.45, 0.4)):
            self.assertAlmostEqual(expected, actual)

    def test_missing_normal_uses_declared_fallback(self):
        selected, failures = select_compatible("meshbumpcolorize", {
            "profiles": ["vs_1_1", "ps_1_1", "ps_2_0"], "vertex_layout": "tangent",
            "textures": ["BaseTexture"], "fixed_transform": True, "fixed_lighting": True,
            "texture_stages": 2,
        })
        self.assertEqual("t0", selected)
        self.assertIn("texture:NormalTexture", failures["sph_t2"])
        self.assertIn("normal-map bump detail is not represented",
                      COMPATIBILITY["meshbumpcolorize"]["t0"]["degradation"])

    def test_shield_fixed_arithmetic_and_cleanup_shape(self):
        from tools.shaders.fx_parser import Pass
        states = {
            "Texture[0]": "BaseSampler", "ColorOp[0]": "SELECTARG1", "ColorArg1[0]": "TEXTURE",
            "AlphaOp[0]": "SELECTARG1", "AlphaArg1[0]": "TEXTURE",
            "Texture[1]": "WaveSampler", "ColorOp[1]": "MODULATE", "ColorArg1[1]": "CURRENT",
            "ColorArg2[1]": "TEXTURE", "AlphaOp[1]": "SELECTARG1", "AlphaArg1[1]": "CURRENT",
            "ColorOp[2]": "MODULATE", "ColorArg1[2]": "CURRENT", "ColorArg2[2]": "TFACTOR",
            "AlphaOp[2]": "SELECTARG1", "AlphaArg1[2]": "CURRENT", "ColorOp[3]": "DISABLE",
        }
        ir = fixed_function_ir(Pass("draw", states, None, None, 1))
        result = evaluate_fixed_function(ir, (0, 0, 0, 0),
                                         {0: (0.2, 0.4, 0.8, 0.5), 1: (0.5, 0.25, 0.5, 1)},
                                         (0.5, 1, 0.25, 1))
        for actual, expected in zip(result, (0.05, 0.1, 0.1, 0.5)):
            self.assertAlmostEqual(expected, actual)

    def test_fixed_blend_current_alpha_uses_prior_stage_alpha(self):
        from tools.shaders.fx_parser import Pass
        states = {
            "Texture[0]": "BaseSampler", "ColorOp[0]": "SELECTARG1",
            "ColorArg1[0]": "TEXTURE", "AlphaOp[0]": "SELECTARG1",
            "AlphaArg1[0]": "TEXTURE", "Texture[1]": "CloudSampler",
            "ColorOp[1]": "BLENDCURRENTALPHA", "ColorArg1[1]": "TEXTURE",
            "ColorArg2[1]": "CURRENT", "AlphaOp[1]": "SELECTARG1",
            "AlphaArg1[1]": "CURRENT", "ColorOp[2]": "DISABLE",
        }
        ir = fixed_function_ir(Pass("draw", states, None, None, 1))
        result = evaluate_fixed_function(
            ir, (0, 0, 0, 0),
            {0: (0.2, 0.2, 0.2, 0.25), 1: (1.0, 1.0, 1.0, 0.8)},
            (1, 1, 1, 1),
        )
        for actual, expected in zip(result, (0.4, 0.4, 0.4, 0.25)):
            self.assertAlmostEqual(expected, actual)

    def test_primalpha_live_equation_has_no_fade(self):
        from tools.shaders.fx_parser import Pass
        states = {
            "Texture[0]": "TextureSampler", "ColorOp[0]": "MODULATE", "ColorArg1[0]": "DIFFUSE",
            "ColorArg2[0]": "TEXTURE", "AlphaOp[0]": "MODULATE", "AlphaArg1[0]": "DIFFUSE",
            "AlphaArg2[0]": "TEXTURE", "ColorOp[1]": "DISABLE",
        }
        ir = fixed_function_ir(Pass("draw", states, None, None, 1))
        result = evaluate_fixed_function(ir, (0.5, 0.25, 1, 0.4), {0: (0.2, 0.8, 0.5, 0.5)}, (1, 1, 1, 1))
        for actual, expected in zip(result, (0.1, 0.2, 0.5, 0.2)):
            self.assertAlmostEqual(expected, actual)

    def test_unknown_fixed_operation_is_explicit_error(self):
        from tools.shaders.fx_parser import Pass
        with self.assertRaises(FxError) as raised:
            fixed_function_ir(Pass("draw", {"ColorOp[0]": "UNSUPPORTED_MAGIC"}, None, None, 17))
        self.assertEqual("SHD_UNSUPPORTED_TEXTURE_OP", raised.exception.code)


class PolicyContractTests(unittest.TestCase):
    def test_output_confinement_rejects_escape_before_partial_output(self):
        allowed = ROOT / "out" / "shaders"
        allowed.mkdir(parents=True, exist_ok=True)
        valid = allowed / "synthetic-policy" / "run"
        self.assertEqual(valid.resolve(), _resolve_output_path(valid))
        with tempfile.TemporaryDirectory(prefix="p010-outside-") as temporary:
            outside = Path(temporary) / "translation"
            with self.assertRaises(ShaderPolicyError) as raised:
                translate(FIXTURES, "Synthetic.fx", outside,
                          GLSLANG, SPIRV_VAL,
                          allow_unpinned_synthetic=True)
            self.assertEqual("SHD_OUTPUT_OUTSIDE_CONFINEMENT", raised.exception.code)
            self.assertFalse(outside.exists())
        traversal = allowed / ".." / "outside-shaders" / "translation"
        with self.assertRaises(ShaderPolicyError):
            _resolve_output_path(traversal)
        self.assertFalse((ROOT / "out" / "outside-shaders" / "translation").exists())
        with tempfile.TemporaryDirectory(dir=allowed, prefix="policy-file-") as parent_temp:
            blocker = Path(parent_temp) / "not-a-directory"
            blocker.write_bytes(b"fixture")
            with self.assertRaises(ShaderPolicyError):
                _resolve_output_path(blocker / "translation")

    def test_existing_symlink_parent_cannot_escape(self):
        allowed = ROOT / "out" / "shaders"
        allowed.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=allowed, prefix="policy-parent-") as parent_temp, \
                tempfile.TemporaryDirectory(prefix="policy-outside-") as outside_temp:
            link = Path(parent_temp) / "redirect"
            try:
                link.symlink_to(Path(outside_temp), target_is_directory=True)
            except OSError as error:
                self.skipTest(f"directory symlinks unavailable: {error}")
            with self.assertRaises(ShaderPolicyError):
                _resolve_output_path(link / "translation")

    @unittest.skipUnless(GLSLANG.is_file() and SPIRV_VAL.is_file(),
                         "pinned local shader toolchain is not installed")
    def test_missing_wrong_swapped_and_valid_explicit_tools(self):
        glslang = GLSLANG
        spirv_val = SPIRV_VAL
        with self.assertRaises(ShaderPolicyError) as missing:
            _find_tool(TOOLCHAIN / f"missing{EXE_SUFFIX}", None, "unused", "glslang", "glslang")
        self.assertEqual("SHD_TOOL_NOT_FOUND", missing.exception.code)
        with self.assertRaises(ShaderPolicyError) as wrong:
            _find_tool(Path(sys.executable), None, "unused", "glslang", "glslang")
        self.assertEqual("SHD_TOOL_IDENTITY_MISMATCH", wrong.exception.code)
        with self.assertRaises(ShaderPolicyError) as swapped_compiler:
            _find_tool(spirv_val, None, "unused", "glslang", "glslang")
        self.assertEqual("SHD_TOOL_IDENTITY_MISMATCH", swapped_compiler.exception.code)
        with self.assertRaises(ShaderPolicyError) as swapped_validator:
            _find_tool(glslang, None, "unused", "spirv-val", "spirv_tools")
        self.assertEqual("SHD_TOOL_IDENTITY_MISMATCH", swapped_validator.exception.code)
        self.assertEqual(glslang.resolve(),
                         _find_tool(glslang, None, "unused", "glslang", "glslang"))
        self.assertEqual(spirv_val.resolve(),
                         _find_tool(spirv_val, None, "unused", "spirv-val", "spirv_tools"))
        self.assertEqual(
            glslang.resolve(),
            _find_tool(None, TOOLCHAIN.parent, f"bin/{GLSLANG.name}", "glslang", "glslang"),
        )
        self.assertEqual(
            spirv_val.resolve(),
            _find_tool(None, TOOLCHAIN.parent, f"bin/{SPIRV_VAL.name}", "spirv-val", "spirv_tools"),
        )

    @unittest.skipUnless(GLSLANG.is_file() and SPIRV_VAL.is_file(),
                         "pinned local shader toolchain is not installed")
    def test_cli_policy_diagnostics_precede_output_creation(self):
        allowed = ROOT / "out" / "shaders"
        allowed.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=allowed, prefix="policy-cli-") as parent:
            output = Path(parent) / "result"
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = main([
                    "--source-root", str(FIXTURES), "--effect", "Synthetic.fx",
                    "--out", str(output), "--glslang", str(SPIRV_VAL),
                    "--spirv-val", str(GLSLANG),
                    "--allow-unpinned-synthetic",
                ])
            self.assertEqual(2, result)
            self.assertIn("SHD_TOOL_IDENTITY_MISMATCH", stderr.getvalue())
            self.assertFalse(output.exists())
        with tempfile.TemporaryDirectory(prefix="policy-cli-outside-") as outside:
            output = Path(outside) / "result"
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = main([
                    "--source-root", str(FIXTURES), "--effect", "Synthetic.fx",
                    "--out", str(output), "--toolchain-root", str(TOOLCHAIN.parent),
                    "--allow-unpinned-synthetic",
                ])
            self.assertEqual(2, result)
            self.assertIn("SHD_OUTPUT_OUTSIDE_CONFINEMENT", stderr.getvalue())
            self.assertFalse(output.exists())


@unittest.skipUnless(GLSLANG.is_file() and SPIRV_VAL.is_file(),
                     "pinned local shader toolchain is not installed")
class ToolchainIntegrationTests(unittest.TestCase):
    def test_synthetic_translation_is_repeatable_and_validated(self):
        generated = ROOT / "out" / "shaders"
        generated.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=generated) as first, tempfile.TemporaryDirectory(dir=generated) as second:
            args = (FIXTURES, "Synthetic.fx")
            manifest_a = translate(*args, Path(first), GLSLANG, SPIRV_VAL,
                                   allow_unpinned_synthetic=True)
            manifest_b = translate(*args, Path(second), GLSLANG, SPIRV_VAL,
                                   allow_unpinned_synthetic=True)
            self.assertEqual(manifest_a["manifest_id"], manifest_b["manifest_id"])
            source_names = ("fx_parser.py", "fx_tokens.py", "fx_include.py", "fx_ir.py", "fx_declarations.py",
                            "toolchain.json", "translate.py", "translate_toolchain.py", "translate_state.py",
                            "translate_compile.py")
            expected_sources = {name: hashlib.sha256((ROOT / "tools" / "shaders" / name).read_bytes()).hexdigest()
                                for name in source_names}
            self.assertEqual(expected_sources, manifest_a["translator"]["source_sha256"])
            hashes_a = sorted(item["spirv_sha256"] for artifact in manifest_a["artifacts"]
                              for item in artifact.get("stages", []))
            hashes_b = sorted(item["spirv_sha256"] for artifact in manifest_b["artifacts"]
                              for item in artifact.get("stages", []))
            self.assertEqual(hashes_a, hashes_b)
            self.assertTrue(all(item["validation"]["exit_code"] == 0 for artifact in manifest_a["artifacts"]
                                for item in artifact.get("stages", [])))


if __name__ == "__main__":
    unittest.main()
