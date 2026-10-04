#!/usr/bin/env python3
"""Independent clean-room-safe P0-10 acceptance review.

Only hashes, exit codes, counts, and structural metadata are inspected. Protected
shader bodies, generated source, and rich compiler logs are never printed.
"""

from __future__ import annotations

import hashlib
import inspect
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from tools.shaders.fx_parser import (
    FxError,
    IncludeResolver,
    Pass,
    lex,
    parse_effect,
    prune_unreachable_functions,
    source_without_effect_wrappers,
)
from tools.shaders.translate import (
    EFFECTS,
    ShaderPolicyError,
    evaluate_fixed_function,
    fixed_function_ir,
    render_state_descriptor,
    skin_transform,
    translate,
)

FIXTURES = ROOT / "tests" / "shaders" / "fixtures"
TOOLCHAIN_ROOT = ROOT / "out" / "tools" / "glslang-16.5.0"
GLSLANG = TOOLCHAIN_ROOT / "bin" / "glslang.exe"
SPIRV_VAL = TOOLCHAIN_ROOT / "bin" / "spirv-val.exe"
ARCHIVE = ROOT / "shaders" / "foc_shaders.zip"
ARCHIVE_SHA256 = "88b9cb03322aab9be7451968ca514e9d2c810973d83f65459fd8cb52e7f96f8d"
EFFECTS_ORDER = ("MESHGLOSS", "MESHBUMPCOLORIZE", "RSKINGLOSSCOLORIZE", "MESHSHIELD", "PRIMALPHA")
EXPECTED_COUNTS = {
    "MESHGLOSS": (2, 0, 4),
    "MESHBUMPCOLORIZE": (3, 0, 6),
    "RSKINGLOSSCOLORIZE": (2, 0, 4),
    "MESHSHIELD": (3, 2, 6),
    "PRIMALPHA": (1, 0, 2),
}


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


class P010IndependentAcceptance(unittest.TestCase):
    blockers: list[str] = []

    @classmethod
    def setUpClass(cls) -> None:
        cls.blockers = []

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.blockers:
            raise AssertionError("; ".join(cls.blockers))

    def blocker(self, message: str) -> None:
        self.blockers.append(message)

    @unittest.skipUnless(ARCHIVE.is_file() and GLSLANG.is_file() and SPIRV_VAL.is_file(),
                         "requires the private shader archive and optional pinned shader tools")
    def test_archive_and_pinned_tool_identity(self) -> None:
        self.assertTrue(ARCHIVE.is_file())
        self.assertEqual(ARCHIVE_SHA256, digest(ARCHIVE))
        self.assertTrue(GLSLANG.is_file())
        self.assertTrue(SPIRV_VAL.is_file())
        glslang_version = subprocess.run([str(GLSLANG), "--version"], capture_output=True, text=True, check=False)
        validator_version = subprocess.run([str(SPIRV_VAL), "--version"], capture_output=True, text=True, check=False)
        self.assertEqual(0, glslang_version.returncode)
        self.assertIn("16.5.0", glslang_version.stdout + glslang_version.stderr)
        self.assertEqual(0, validator_version.returncode)
        self.assertIn("v2026.1", validator_version.stdout + validator_version.stderr)
        self.assertIn("fbe4f3a", validator_version.stdout + validator_version.stderr)
        module = __import__("tools.shaders.translate", fromlist=["_find_tool"])
        find_tool_source = inspect.getsource(module._find_tool)
        if "--version" not in find_tool_source and "sha256" not in find_tool_source:
            self.blocker("tools/shaders/translate.py:_find_tool accepts arbitrary existing tool files without pinned version/hash enforcement")

    def test_synthetic_structure_and_diagnostics(self) -> None:
        with tempfile.TemporaryDirectory(prefix="p010-fixtures-") as temporary:
            root = Path(temporary)
            write(root / "shared" / "Deep.Fxh", "#define INNER_FLAG 1\nfloat4 include_helper(float4 x) { return x; }\n")
            write(root / "shared" / "Common.fxh", "#include \"deep.fxh\"\n// { braces in a comment }\n")
            write(root / "cycle-a.fxh", "#include \"cycle-b.fxh\"\n")
            write(root / "cycle-b.fxh", "#include \"cycle-a.fxh\"\n")
            source = (
                "// #include \"not-active.fxh\"\n"
                "#include \"SHARED\\\\common.FXH\"\n"
                "#define ENABLE_ALT 1\n"
                "#if ENABLE_ALT\n"
                "float4 active_macro(float4 x) { if (x.x < 1) { return x; } return x; }\n"
                "#else\nfloat4 inactive_macro(float4 x) { return x; }\n#endif\n"
                "string marker = \"brace } and // text\";\n"
                "float4x3 SkinPalette[4] : SKINMATRIXARRAY < string UIName = \"Bones\"; > = 0;\n"
                "float4 Tint < string UIName = \"Tint\"; > = { 1, 0.5, 0.25, 1 };\n"
                "texture BaseTexture;\n"
                "sampler BaseSampler = sampler_state { Texture = <BaseTexture>; AddressU = Wrap; MinFilter = Linear; };\n"
                "struct Input { float4 position : POSITION; };\n"
                "struct Output { float4 position : POSITION; };\n"
                "Output entry(Input input) { Output output; output.position = input.position; return output; }\n"
                "technique Preferred { pass draw { ZWriteEnable = TRUE; CullMode = ccw; "
                "VertexShader = compile vs_1_1 entry(); PixelShader = compile ps_2_0 active_macro(); } }\n"
                "technique Fixed { pass cleanup { TextureTransformFlags[0] = DISABLE; } }\n"
            )
            write(root / "Adversarial.fx", source)
            resolved = IncludeResolver(root).resolve("adversarial.FX")
            self.assertEqual(["shared/Common.fxh", "shared/Deep.Fxh"], [item["path"] for item in resolved.includes])
            self.assertIn("active_macro", resolved.text)
            self.assertNotIn("inactive_macro", resolved.text)
            self.assertEqual("1", resolved.definitions["ENABLE_ALT"])
            effect = parse_effect(resolved.text, "adversarial.FX")
            parameters = {item.name: item for item in effect.parameters}
            self.assertEqual(("float4x3", "4", "SKINMATRIXARRAY"), (parameters["SkinPalette"].type, parameters["SkinPalette"].array_size, parameters["SkinPalette"].semantic))
            self.assertEqual("{ 1, 0.5, 0.25, 1 }", parameters["Tint"].default)
            self.assertEqual("BaseTexture", effect.samplers[0].texture)
            self.assertEqual("Wrap", effect.samplers[0].states["AddressU"])
            self.assertEqual("ccw", effect.techniques[0].passes[0].states["CullMode"])
            lowered = source_without_effect_wrappers(resolved.text, effect)
            self.assertIn("active_macro", lowered)
            self.assertNotIn("technique Preferred", lowered)
            self.assertIn("sampler2D BaseSampler", lowered)
            retained = prune_unreachable_functions(
                "float4 used(float4 x) { return helper(x); } float4 helper(float4 x) { return x; } float4 unused(float4 x) { return x; }",
                ["used"],
            )
            self.assertIn("float4 used", retained)
            self.assertIn("float4 helper", retained)
            self.assertNotIn("float4 unused", retained)

            write(root / "Escape.fx", "#include \"../../outside.fxh\"\n")
            with self.assertRaises(FxError) as raised:
                IncludeResolver(root).resolve("Escape.fx")
            self.assertEqual("SHD_INCLUDE_NOT_FOUND", raised.exception.code)
            with self.assertRaises(FxError) as raised:
                IncludeResolver(root).resolve("cycle-a.fxh")
            self.assertEqual("SHD_INCLUDE_CYCLE", raised.exception.code)
            write(root / "MissingLine.fx", "// one\n// two\n// three\n#include \"missing.fxh\"\n")
            with self.assertRaises(FxError) as raised:
                IncludeResolver(root).resolve("MissingLine.fx")
            self.assertEqual("SHD_INCLUDE_NOT_FOUND", raised.exception.code)
            if raised.exception.line != 4:
                self.blocker(f"tools/shaders/fx_parser.py:220-232 reports missing include at {raised.exception.line}:{raised.exception.column}, not source line 4")
            with self.assertRaises(FxError) as raised:
                lex('float4 bad() { return "unterminated; }', "bad.fx")
            self.assertEqual("SHD_UNTERMINATED_STRING", raised.exception.code)
            self.assertEqual("bad.fx", raised.exception.path)
            with self.assertRaises(FxError) as raised:
                parse_effect("technique Broken { pass p {", "broken.fx")
            self.assertEqual("SHD_UNBALANCED_BLOCK", raised.exception.code)
            self.assertEqual("broken.fx", raised.exception.path)

    def test_numeric_state_and_unsupported_contracts(self) -> None:
        palette = [(0.0, 0.0, 0.0, 0.0)] * (23 * 3)
        palette.extend([(1, 0, 0, 10), (0, 1, 0, 20), (0, 0, 1, 30)])
        position, normal = skin_transform((2, 3, 4, 1), (0, 1, 0), palette, 23)
        self.assertEqual((12, 23, 34), position)
        self.assertEqual((0, 1, 0), normal)
        primalpha = Pass(
            "draw",
            {
                "Texture[0]": "TextureSampler", "ColorOp[0]": "MODULATE",
                "ColorArg1[0]": "DIFFUSE", "ColorArg2[0]": "TEXTURE",
                "AlphaOp[0]": "MODULATE", "AlphaArg1[0]": "DIFFUSE",
                "AlphaArg2[0]": "TEXTURE", "ColorOp[1]": "DISABLE",
            },
            None,
            None,
            17,
        )
        self.assertEqual((0.1, 0.2, 0.5, 0.2), evaluate_fixed_function(
            fixed_function_ir(primalpha), (0.5, 0.25, 1, 0.4), {0: (0.2, 0.8, 0.5, 0.5)}, (1, 1, 1, 1)
        ))
        state = render_state_descriptor(Pass("state", {"AlphaBlendEnable": "(Opacity < 1.0f)"}, None, None, 1))
        self.assertEqual("dynamic", state["alpha_blend_enable"]["kind"])
        self.assertEqual("renderer_baseline", state["depth_compare"]["source"])
        with self.assertRaises(FxError) as raised:
            fixed_function_ir(Pass("bad", {"ColorOp[0]": "UNSUPPORTED_MAGIC"}, None, None, 9))
        self.assertEqual("SHD_UNSUPPORTED_TEXTURE_OP", raised.exception.code)

    @unittest.skipUnless(ARCHIVE.is_file() and GLSLANG.is_file() and SPIRV_VAL.is_file(),
                         "requires the private shader archive and optional pinned shader tools")
    def test_private_five_effect_translation_and_metadata(self) -> None:
        self.assertTrue(ARCHIVE.is_file())
        self.assertTrue(GLSLANG.is_file() and SPIRV_VAL.is_file())
        acceptance_root = ROOT / "out" / "shaders" / "acceptance"
        if acceptance_root.exists():
            shutil.rmtree(acceptance_root)
        manifests: dict[str, tuple[bytes, dict]] = {}
        inventory = json.loads((ROOT / "plan/inventories/shader-spike.json").read_text(encoding="utf-8"))
        inventory_by_effect = {item["effect"]: item for item in inventory["effects"]}
        for run_name in ("run1", "run2"):
            for effect_name in EFFECTS_ORDER:
                output = acceptance_root / run_name / effect_name
                result = subprocess.run(
                    ["python", "tools/shaders/translate.py", "--source-root", "shaders/petroglyph-foc/Shaders",
                     "--source-archive", "shaders/foc_shaders.zip", "--effect", effect_name,
                     "--out", str(output), "--toolchain-root", str(TOOLCHAIN_ROOT)],
                    cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
                )
                self.assertEqual(0, result.returncode, f"private translation failed for {effect_name}/{run_name}")
                manifest_path = output / "manifest.json"
                self.assertTrue(manifest_path.is_file())
                manifest_bytes = manifest_path.read_bytes()
                manifest = json.loads(manifest_bytes.decode("utf-8"))
                if run_name == "run1":
                    manifests[effect_name] = (manifest_bytes, manifest)
                else:
                    self.assertEqual(manifests[effect_name][0], manifest_bytes, f"manifest nondeterminism for {effect_name}")
                draw_count = sum(bool(item.get("draw")) for item in manifest["artifacts"])
                cleanup_count = sum(item.get("kind") == "cleanup" for item in manifest["artifacts"])
                stage_count = sum(len(item.get("stages", [])) for item in manifest["artifacts"])
                expected = EXPECTED_COUNTS[effect_name]
                self.assertEqual(expected, (draw_count, cleanup_count, stage_count))
                for artifact in manifest["artifacts"]:
                    if not artifact.get("draw"):
                        self.assertEqual("cleanup", artifact.get("kind"))
                        self.assertFalse(artifact.get("stages"))
                    for stage in artifact.get("stages", []):
                        self.assertEqual(0, stage["exit_code"])
                        self.assertEqual(0, stage["validation"]["exit_code"])
                        self.assertRegex(stage["spirv_sha256"], r"^[0-9a-f]{64}$")
                self.assertEqual(ARCHIVE_SHA256, manifest["source"]["archive_sha256"])
                self.assertEqual(EFFECTS[effect_name.casefold()]["sha256"], manifest["source"]["root"]["sha256"])
                self.assertEqual(inventory_by_effect[effect_name]["manifest_id"], manifest["manifest_id"])
                self.assertEqual(1, manifest["schema_version"])
                self.assertFalse(manifest["translation_selection"]["renderer_selected"])
                self.assertEqual("not_tested", manifest["renderer_ingestion"]["wgpu"])
                self.assertEqual("not_tested", manifest["renderer_ingestion"]["godot"])
                for field in ("parameters", "resources", "techniques", "artifacts", "toolchain"):
                    self.assertIn(field, manifest)
                for parameter in manifest["parameters"]:
                    for field in ("name", "type", "array_size", "semantic", "annotation", "default", "register", "line"):
                        self.assertIn(field, parameter)
                for resource in manifest["resources"]:
                    for field in ("binding", "sampler", "sampler_type", "source_register", "texture", "states"):
                        self.assertIn(field, resource)
                required_parameters = {
                    "MESHGLOSS": {"BaseTexture"},
                    "MESHBUMPCOLORIZE": {"BaseTexture", "NormalTexture", "Colorization"},
                    "RSKINGLOSSCOLORIZE": {"BaseTexture", "GlossTexture", "Colorization", "m_skinMatrixArray"},
                    "MESHSHIELD": {"BaseTexture", "WaveTexture", "DistortionTexture"},
                    "PRIMALPHA": set(),
                }
                self.assertTrue(required_parameters[effect_name].issubset({item["name"] for item in manifest["parameters"]}))
                if effect_name == "RSKINGLOSSCOLORIZE":
                    palette = next(item for item in manifest["parameters"] if item["name"] == "m_skinMatrixArray")
                    self.assertEqual("float4x3", palette["type"])
                    self.assertIn(palette["array_size"], {"24", "MAX_BONES"})
                    self.assertEqual("SKINMATRIXARRAY", palette["semantic"])
                if effect_name == "PRIMALPHA":
                    self.assertEqual("s0", manifest["resources"][0]["source_register"])
                for technique in manifest["techniques"]:
                    for pass_metadata in technique["passes"]:
                        self.assertIn("render_state", pass_metadata)
                        for state_name in ("alpha_blend_enable", "alpha_test_enable", "cull_mode", "depth_compare", "depth_test_enable", "depth_write_enable", "destination_blend", "fog_enable", "source_blend"):
                            self.assertIn(state_name, pass_metadata["render_state"])
                if effect_name == "MESHGLOSS":
                    sph = next(item for item in manifest["techniques"] if item["name"] == "sph_t0")
                    self.assertEqual("dynamic", sph["passes"][0]["render_state"]["alpha_blend_enable"]["kind"])
                if effect_name == "PRIMALPHA":
                    prim_state = next(item for item in manifest["techniques"] if item["name"] == "t1")["passes"][0]["render_state"]
                    self.assertFalse(prim_state["depth_write_enable"]["value"])
                    self.assertFalse(prim_state["alpha_test_enable"]["value"])
                    self.assertEqual("SRCALPHA", prim_state["source_blend"])
                    self.assertEqual("INVSRCALPHA", prim_state["destination_blend"])
                if effect_name == "MESHSHIELD":
                    shield_state = next(item for item in manifest["techniques"] if item["name"] == "t1")["passes"][0]["render_state"]
                    self.assertFalse(shield_state["depth_write_enable"]["value"])
                    self.assertEqual("NONE", shield_state["cull_mode"]["value"])
                    self.assertEqual("ONE", shield_state["source_blend"])
                    self.assertEqual("ONE", shield_state["destination_blend"])
                for artifact in manifest["artifacts"]:
                    for stage in artifact.get("stages", []):
                        source_paths = list((output / artifact["technique"]).rglob(stage["source"]))
                        self.assertEqual(1, len(source_paths))
                        self.assertGreater(source_paths[0].stat().st_size, 0)
                for artifact in manifest["artifacts"]:
                    if artifact.get("draw") and artifact.get("kind") == "fixed_function_lowering":
                        self.assertTrue(artifact.get("fixed_function", {}).get("stages"))
                self.assertNotIn(str(ROOT.resolve()), manifest_path.read_text(encoding="utf-8"))

        with tempfile.TemporaryDirectory(prefix="p010-outside-") as outside:
            outside_output = Path(outside) / "translation"
            with self.assertRaises(ShaderPolicyError) as raised:
                translate(FIXTURES, "Synthetic.fx", outside_output, GLSLANG, SPIRV_VAL, allow_unpinned_synthetic=True)
            self.assertEqual("SHD_OUTPUT_OUTSIDE_CONFINEMENT", raised.exception.code)
            self.assertFalse(outside_output.exists())
        with tempfile.TemporaryDirectory(prefix="p010-cli-failure-") as temporary:
            bad_output = Path(temporary) / "bad"
            failed = subprocess.run(
                ["python", "tools/shaders/translate.py", "--source-root", str(FIXTURES / "missing"),
                 "--effect", "Synthetic.fx", "--out", str(bad_output), "--glslang", str(GLSLANG),
                 "--spirv-val", str(SPIRV_VAL)],
                cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
            )
            self.assertNotEqual(0, failed.returncode)
            self.assertFalse(bad_output.exists())


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(P010IndependentAcceptance)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
