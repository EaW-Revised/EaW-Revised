"""TerrainWater t1 (ps_1_1 assembly fallback) semantic path.

Portable tests use only original synthetic programs, a synthetic manifest and the
committed D3D9 readback of original synthetic textures.  Private-input tests (the
pinned Petroglyph source, the pinned glslang/spirv-val and a pinned wgpu venv) skip
unless present; point ``EAWR_T1_PRIVATE_ROOT`` at a checkout holding ``shaders/`` and
``out/tools/`` and ``EAWR_T1_WGPU_PYTHON`` at the venv interpreter to run them.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent / "terrainwater_t1"
sys.path.insert(0, str(HERE))

import cases as case_data  # noqa: E402
import ps11_eval  # noqa: E402
from tools.shaders import ps11  # noqa: E402
from tools.shaders.terrainwater_t1 import (T1Error, bind_external_state,  # noqa: E402
                                           _t1_render_state, effective_bump_coefficients,
                                           external_state_requirements,
                                           vertex_wrapper)

PRIVATE = Path(os.environ.get("EAWR_T1_PRIVATE_ROOT", ROOT))
EXE = ".exe" if os.name == "nt" else ""
GLSLANG = PRIVATE / "out" / "tools" / "glslang-16.5.0" / "bin" / f"glslang{EXE}"
SPIRV_VAL = PRIVATE / "out" / "tools" / "glslang-16.5.0" / "bin" / f"spirv-val{EXE}"
SOURCE_ROOT = PRIVATE / "shaders" / "petroglyph-foc" / "Shaders"
ARCHIVE = PRIVATE / "shaders" / "foc_shaders.zip"
PRIVATE_READY = GLSLANG.is_file() and SPIRV_VAL.is_file() and SOURCE_ROOT.is_dir() and ARCHIVE.is_file()
WGPU_PYTHON = Path(os.environ.get("EAWR_T1_WGPU_PYTHON", ROOT / "out" / "venv-wgpu" / "Scripts" / f"python{EXE}"))
FIXTURE = HERE / "d3d9_readback.json"

# Original synthetic programs (not taken from any effect file).
SYNTH_BEM = "ps.1.1\ntex t0\ntexbem t2, t0\ntex t1\nmul r1, t2, v0\nadd r0, r1, v1\nmov r0.a, t1\n"


def synthetic_manifest(escapes=True) -> dict:
    program = ps11.parse("ps.1.1\ntex t0\ntexbem t1, t0\ntex t2\nmul r0, t1, v0\nadd r0.rgb, r0, t2\n")
    ranges = {0: (-1.0, 1.0), 1: (0.0, 1.0), 2: (0.0, 1.0)}
    found = ps11.analyse_ranges(program, ranges)
    if not escapes:
        found = []
    return {"pixel": {"program": ps11.program_record(program, found),
                      "bindings": {"cbuffer": ps11.BINDING_CBUFFER}},
            "pass_state": {"effect_sampler_state": {"1": {"address_u": "clamp", "address_v": "clamp"}}}}


def good_binding(**changes) -> dict:
    binding = {
        "usage": "synthetic",
        "fog": {"enabled": False},
        "pipeline": {"cull_mode": "ccw", "alpha_test": {"enabled": False, "func": "always", "ref": 0},
                     "separate_alpha_blend": False, "blend_op": "add", "srgb_write": False,
                     "color_write_mask": "rgba", "render_target_format": "A8R8G8B8"},
        "pixel_shader_1x_max_value": 2.0,
        "bump_env": {"1": {"m00": 0.25, "m01": -0.125, "m10": 0.0625, "m11": 0.375,
                           "convention": "documented", "provenance": "synthetic"}},
        "stages": {
            "0": {"format": "V8U8", "min_filter": "point", "mag_filter": "point", "mip_levels": 1,
                  "mip_filter": "none", "srgb_read": False,
                  "texture_transform": "disable", "address_u": "wrap", "address_v": "wrap"},
            "1": {"format": "A8R8G8B8", "min_filter": "linear", "mag_filter": "linear", "mip_levels": 1,
                  "mip_filter": "none", "srgb_read": False,
                  "texture_transform": "disable"},
            "2": {"format": "X8R8G8B8", "min_filter": "point", "mag_filter": "point", "mip_levels": 1,
                  "mip_filter": "none", "srgb_read": False,
                  "texture_transform": "disable", "address_u": "wrap", "address_v": "wrap"},
        },
    }
    for path, value in changes.items():
        target = binding
        keys = path.split("__")
        for key in keys[:-1]:
            target = target[key]
        if value is KeyError:
            del target[keys[-1]]
        else:
            target[keys[-1]] = value
    return binding


class Ps11ParserTests(unittest.TestCase):
    def test_bounded_program_parses_and_records_stage_roles(self):
        program = ps11.parse("asm\n{\n ps .1 .1\n" + SYNTH_BEM.split("\n", 1)[1] + "}")
        self.assertEqual([0, 2, 1], program.sampled_stages)
        self.assertEqual({2: 0}, program.bump_stages)
        self.assertEqual(["tex", "texbem", "tex", "mul", "add", "mov"],
                         [item["opcode"] for item in program.describe()])
        self.assertEqual("a", program.instructions[-1].mask)

    def test_every_construct_outside_the_subset_fails_closed_with_a_name(self):
        cases = {
            "ps.1.4\ntex t0\nmov r0, t0": "EAWR-SHD-PS11-VERSION",
            "ps.1.1\ntex t0\nmov_x2 r0, t0": "EAWR-SHD-PS11-UNSUPPORTED-MODIFIER",
            "ps.1.1\ntex t0\nmov r0, t0_bx2": "EAWR-SHD-PS11-UNSUPPORTED-OPERAND",
            "ps.1.1\ntex t0\nmov r0, -t0": "EAWR-SHD-PS11-UNSUPPORTED-OPERAND",
            "ps.1.1\ntex t0\nmov r0, t0.a": "EAWR-SHD-PS11-UNSUPPORTED-OPERAND",
            "ps.1.1\ntex t0\nmov r0, c0": "EAWR-SHD-PS11-UNSUPPORTED-REGISTER",
            "ps.1.1\ntex t0\nmov r0.rgb, t0\n+mov r0.a, t0": "EAWR-SHD-PS11-UNSUPPORTED-MODIFIER",
            "ps.1.1\ntex t0\ntexbeml t1, t0\nmov r0, t1": "EAWR-SHD-PS11-UNSUPPORTED-INSTRUCTION",
            "ps.1.1\ntex t0\ncnd r0, r0.a, t0, v0": "EAWR-SHD-PS11-UNSUPPORTED-INSTRUCTION",
            "ps.1.1\ntex t0\nmov r0.rg, t0": "EAWR-SHD-PS11-UNSUPPORTED-MASK",
            "ps.1.1\ntex t0.rgb\nmov r0, t0": "EAWR-SHD-PS11-UNSUPPORTED-MASK",
            "ps.1.1\ntex t0\ntexbem t1, t0\nadd r0, t1, t0": "EAWR-SHD-PS11-BEM-SOURCE-REREAD",
            "ps.1.1\ntexbem t1, t0\nmov r0, t1": "EAWR-SHD-PS11-UNWRITTEN-READ",
            "ps.1.1\ntex t1\ntexbem t0, t1\nmov r0, t0": "EAWR-SHD-PS11-ORDER",
            "ps.1.1\ntex t0\nmov r0, t0\ntex t1": "EAWR-SHD-PS11-ORDER",
            "ps.1.1\ntex t0\nmov r0.rgb, t0": "EAWR-SHD-PS11-OUTPUT",
            "ps.1.1\ntex t0\nmov r1.a, t0\nmov r0, r1": "EAWR-SHD-PS11-UNWRITTEN-READ",
            "ps.1.1\ntex t4\nmov r0, t4": "EAWR-SHD-PS11-UNSUPPORTED-REGISTER",
        }
        for text, code in cases.items():
            with self.subTest(text=text):
                with self.assertRaises(ps11.Ps11Error) as raised:
                    ps11.parse(text)
                self.assertEqual(code, raised.exception.code)

    def test_range_analysis_marks_only_writes_that_leave_unit_interval(self):
        program = ps11.parse(SYNTH_BEM)
        escapes = ps11.analyse_ranges(program, {0: (-1.0, 1.0), 1: (0.0, 1.0), 2: (0.0, 1.0)})
        self.assertEqual([("add", [0, 1, 2, 3])], [(item["opcode"], item["components"]) for item in escapes])
        self.assertEqual([0.0, 2.0], escapes[0]["interval"][0])
        with self.assertRaises(ps11.Ps11Error) as raised:
            ps11.analyse_ranges(program, {0: (-1.0, 1.0)})
        self.assertEqual("EAWR-SHD-PS11-STAGE-RANGE-UNRESOLVED", raised.exception.code)
        tight = ps11.analyse_ranges(program, {0: (-1.0, 1.0), 1: (0.0, 1.0), 2: (0.0, 1.0)},
                                    {0: (0.0, 1.0), 1: (0.0, 0.0)})
        self.assertEqual([], tight)

    def test_lowering_is_explicit_about_texbem_masks_clamps_and_interface(self):
        program = ps11.parse(SYNTH_BEM)
        escapes = ps11.analyse_ranges(program, {0: (-1.0, 1.0), 1: (0.0, 1.0), 2: (0.0, 1.0)})
        hlsl = ps11.lower_to_hlsl(program, "entry", escapes)
        self.assertIn("float2 bem2 = float2(dot(eawr_bem[2].xy, t0.rg), dot(eawr_bem[2].zw, t0.rg));", hlsl)
        self.assertIn("eawr_tex2.Sample(eawr_smp2, i.tc2 + bem2)", hlsl)
        self.assertIn("r0 = (clamp((r1 + v1), -eawr_range.xxxx, eawr_range.xxxx));", hlsl)
        self.assertIn("r1 = ((t2 * v0));", hlsl)
        self.assertIn("r0.a = (t1).a;", hlsl)
        self.assertIn("[[vk::location(8)]] float4 v0 : COLOR0;", hlsl)
        self.assertIn("[[vk::location(2)]] float2 tc2 : TEXCOORD2;", hlsl)
        self.assertIn("return saturate(r0);", hlsl)
        self.assertNotIn("identity", hlsl.casefold())

    def test_vertex_wrapper_clamps_colour_outputs_before_interpolation(self):
        class Field:
            def __init__(self, name, type_, semantic):
                self.name, self.type, self.semantic = name, type_, semantic
        fields = [Field("P", "float4", "POSITION"), Field("T", "float2", "TEXCOORD1"),
                  Field("D", "float4", "COLOR0"), Field("F", "float", "FOG")]
        text = vertex_wrapper("OUT_T", "IN_T", fields, "vs_entry")
        self.assertIn("float4 P : SV_POSITION;", text)
        self.assertIn("r.D = saturate(o.D);", text)
        self.assertIn("r.T = o.T;", text)
        self.assertIn("[[vk::location(1)]] float2 T : TEXCOORD1;", text)
        with self.assertRaises(T1Error):
            vertex_wrapper("O", "I", [Field("N", "float3", "NORMAL")], "vs")


class ExternalStateBindingTests(unittest.TestCase):
    def assert_code(self, code, binding, manifest=None):
        with self.assertRaises(T1Error) as raised:
            bind_external_state(manifest or synthetic_manifest(), binding)
        self.assertEqual(code, raised.exception.code)

    def test_requirements_name_every_unowned_state(self):
        manifest = synthetic_manifest()
        program = ps11.parse("ps.1.1\ntex t0\ntexbem t1, t0\ntex t2\nmul r0, t1, v0\nadd r0.rgb, r0, t2\n")
        codes = {item["code"] for item in external_state_requirements(
            program, {1: {"address_u": "clamp"}}, manifest["pixel"]["program"]["range_escapes"])}
        self.assertEqual({"EAWR-SHD-T1-BUMPENV-UNRESOLVED", "EAWR-SHD-T1-BUMPENV-CONVENTION-UNRESOLVED",
                          "EAWR-SHD-T1-BUMP-FORMAT-UNRESOLVED", "EAWR-SHD-T1-TEXBEM-LOD-UNRESOLVED",
                          "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED", "EAWR-SHD-T1-STAGE-FORMAT-UNRESOLVED",
                          "EAWR-SHD-T1-FOG-UNRESOLVED", "EAWR-SHD-PS11-RANGE-UNRESOLVED",
                          "EAWR-SHD-T1-CULL-UNRESOLVED", "EAWR-SHD-T1-ALPHA-TEST-UNRESOLVED",
                          "EAWR-SHD-T1-SEPARATE-ALPHA-BLEND-UNRESOLVED", "EAWR-SHD-T1-BLENDOP-UNRESOLVED",
                          "EAWR-SHD-T1-SRGB-READ-UNRESOLVED", "EAWR-SHD-T1-SRGB-WRITE-UNRESOLVED",
                          "EAWR-SHD-T1-COLOR-WRITE-UNRESOLVED",
                          "EAWR-SHD-T1-RENDER-TARGET-FORMAT-UNRESOLVED",
                          "EAWR-SHD-T1-REAL-MAP-UNRESOLVED"}, codes)
        self.assertTrue(all(item["real_map_status"] == "unresolved_for_real_map"
                            for item in external_state_requirements(program, {}, [])
                            if "real_map_status" in item))

    def test_manifest_render_state_omits_unowned_baseline_literals(self):
        unowned = _t1_render_state(SimpleNamespace(states={}))
        for key in ("cull_mode", "alpha_test_enable", "fog_enable"):
            self.assertNotIn(key, unowned)
        owned = _t1_render_state(SimpleNamespace(states={"CullMode": "CCW",
                                                        "AlphaTestEnable": "FALSE",
                                                        "FogEnable": "FALSE"}))
        for key in ("cull_mode", "alpha_test_enable", "fog_enable"):
            self.assertEqual("effect", owned[key]["source"])

    def test_complete_binding_produces_convention_explicit_coefficients(self):
        bound = bind_external_state(synthetic_manifest(), good_binding())
        self.assertEqual([0.25, 0.0625, -0.125, 0.375], bound["bump_coefficients"]["1"])
        self.assertEqual("clamp", bound["samplers"]["1"]["address_u"])
        self.assertEqual("one", bound["samplers"]["2"]["alpha_source"])
        self.assertEqual(80, len(bytes.fromhex(bound["cbuffer"]["bytes"])))
        transposed = bind_external_state(synthetic_manifest(), good_binding(bump_env__1__convention="transposed"))
        self.assertEqual([0.25, -0.125, 0.0625, 0.375], transposed["bump_coefficients"]["1"])
        self.assertEqual((1, 3, 2, 4), effective_bump_coefficients(1, 2, 3, 4, "documented"))

    def test_zero_matrix_is_accepted_only_when_declared(self):
        zero = {"m00": 0.0, "m01": 0.0, "m10": 0.0, "m11": 0.0,
                "convention": "documented", "provenance": "declared zero"}
        bound = bind_external_state(synthetic_manifest(), good_binding(bump_env__1=zero))
        self.assertEqual([0.0, 0.0, 0.0, 0.0], bound["bump_coefficients"]["1"])
        self.assert_code("EAWR-SHD-T1-BUMPENV-UNRESOLVED", good_binding(bump_env={}))
        self.assert_code("EAWR-SHD-T1-BUMPENV-UNRESOLVED", good_binding(bump_env__1__m11=KeyError))
        self.assert_code("EAWR-SHD-T1-BUMPENV-UNRESOLVED", good_binding(bump_env__1__provenance=" "))
        self.assert_code("EAWR-SHD-T1-BUMPENV-UNRESOLVED", good_binding(bump_env__1__m00=math.nan))

    def test_asymmetric_matrix_requires_a_declared_index_convention(self):
        self.assert_code("EAWR-SHD-T1-BUMPENV-CONVENTION-UNRESOLVED", good_binding(bump_env__1__convention=KeyError))
        self.assert_code("EAWR-SHD-T1-BUMPENV-CONVENTION-UNRESOLVED", good_binding(bump_env__1__convention="guess"))
        symmetric = {"m00": 0.5, "m01": 0.2, "m10": 0.2, "m11": -0.5,
                     "convention": "documented", "provenance": "synthetic"}
        bound = bind_external_state(synthetic_manifest(), good_binding(bump_env__1=symmetric))
        self.assertEqual([0.5, 0.2, 0.2, -0.5], bound["bump_coefficients"]["1"])

    def test_binding_shape_and_enums_fail_closed(self):
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE", None)
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE", [])
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-UNKNOWN-KEY", good_binding(typo=True))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-UNKNOWN-KEY", good_binding(stages__0__typo=True))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-UNKNOWN-KEY", good_binding(bump_env__9={}))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-UNKNOWN-KEY", good_binding(pipeline__typo=True))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE", good_binding(stages__0__mip_levels=True))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE", good_binding(stages__0__mip_levels=1.0))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-UNKNOWN-ENUM", good_binding(stages__0__mip_filter="junk"))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-UNKNOWN-ENUM", good_binding(stages__0__min_filter="junk"))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE", good_binding(stages=[]))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE", good_binding(bump_env__1=[]))
        self.assert_code("EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE", good_binding(stages__0__format=[]))
        symmetric = {"m00": 0.5, "m01": 0.2, "m10": 0.2, "m11": -0.5,
                     "provenance": "synthetic"}
        self.assert_code("EAWR-SHD-T1-BUMPENV-CONVENTION-UNRESOLVED", good_binding(bump_env__1=symmetric))
        symmetric["convention"] = "junk"
        self.assert_code("EAWR-SHD-T1-BUMPENV-CONVENTION-UNRESOLVED", good_binding(bump_env__1=symmetric))

    def test_pipeline_state_is_explicit_and_bounded(self):
        self.assert_code("EAWR-SHD-T1-PIPELINE-STATE-UNRESOLVED", good_binding(pipeline=KeyError))
        for key, code, wrong in (
            ("cull_mode", "EAWR-SHD-T1-CULL-UNRESOLVED", "none"),
            ("alpha_test", "EAWR-SHD-T1-ALPHA-TEST-UNRESOLVED", {"enabled": True, "func": "always", "ref": 0}),
            ("separate_alpha_blend", "EAWR-SHD-T1-SEPARATE-ALPHA-BLEND-UNRESOLVED", True),
            ("blend_op", "EAWR-SHD-T1-BLENDOP-UNRESOLVED", "subtract"),
            ("srgb_write", "EAWR-SHD-T1-SRGB-WRITE-UNRESOLVED", True),
            ("color_write_mask", "EAWR-SHD-T1-COLOR-WRITE-UNRESOLVED", "rgb"),
            ("render_target_format", "EAWR-SHD-T1-RENDER-TARGET-FORMAT-UNRESOLVED", "X8R8G8B8"),
        ):
            with self.subTest(state=key):
                self.assert_code(code, good_binding(**{"pipeline__" + key: KeyError}))
                self.assert_code(code, good_binding(**{"pipeline__" + key: wrong}))
        self.assert_code("EAWR-SHD-T1-ALPHA-TEST-UNRESOLVED",
                         good_binding(pipeline__alpha_test__func=KeyError))
        self.assert_code("EAWR-SHD-T1-ALPHA-TEST-UNRESOLVED",
                         good_binding(pipeline__alpha_test__ref=KeyError))
        self.assert_code("EAWR-SHD-T1-SRGB-READ-UNRESOLVED", good_binding(stages__0__srgb_read=KeyError))
        self.assert_code("EAWR-SHD-T1-SRGB-READ-UNRESOLVED", good_binding(stages__1__srgb_read=True))
        self.assert_code("EAWR-SHD-T1-REAL-MAP-UNRESOLVED", good_binding(usage="real_map"))
        bound = bind_external_state(synthetic_manifest(), good_binding())
        self.assertEqual("rgba", bound["pipeline"]["color_write_mask"])
        self.assertTrue(bound["pipeline"]["blend_applies_to_alpha"])

    def test_formats_lod_sampler_fog_and_range_fail_closed(self):
        self.assert_code("EAWR-SHD-T1-BUMP-FORMAT-UNSIGNED", good_binding(stages__0__format="A8R8G8B8"))
        self.assert_code("EAWR-SHD-T1-BUMP-FORMAT-UNRESOLVED", good_binding(stages__0__format=KeyError))
        self.assert_code("EAWR-SHD-T1-STAGE-FORMAT-UNRESOLVED", good_binding(stages__2__format="V8U8"))
        self.assert_code("EAWR-SHD-T1-STAGE-FORMAT-UNRESOLVED", good_binding(stages__1__format=KeyError))
        self.assert_code("EAWR-SHD-T1-TEXBEM-LOD-UNRESOLVED", good_binding(stages__1__mip_levels=4))
        self.assert_code("EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED", good_binding(stages__0__min_filter=KeyError))
        self.assert_code("EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED", good_binding(stages__2__address_u=KeyError))
        self.assert_code("EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED", good_binding(stages__0__texture_transform="count2"))
        self.assert_code("EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED", good_binding(stages__2__mip_levels=3))
        self.assert_code("EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED", good_binding(stages={}))
        self.assert_code("EAWR-SHD-T1-EFFECT-STATE-CONFLICT", good_binding(stages__1__address_u="wrap"))
        self.assert_code("EAWR-SHD-T1-FOG-UNRESOLVED", good_binding(fog=KeyError))
        self.assert_code("EAWR-SHD-T1-FOG-UNSUPPORTED", good_binding(fog={"enabled": True}))
        self.assert_code("EAWR-SHD-PS11-RANGE-UNRESOLVED", good_binding(pixel_shader_1x_max_value=KeyError))
        self.assert_code("EAWR-SHD-PS11-RANGE-UNRESOLVED", good_binding(pixel_shader_1x_max_value=0.5))
        bound = bind_external_state(synthetic_manifest(escapes=False), good_binding(pixel_shader_1x_max_value=KeyError))
        self.assertEqual(0.0, bound["pixel_shader_1x_max_value"])

    def test_range_accepts_oracle_caps_and_float32_max(self):
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        for value in (fixture["caps"]["pixel_shader_1x_max_value"], 3.4028234663852886e38):
            with self.subTest(value=value):
                bound = bind_external_state(synthetic_manifest(),
                                            good_binding(pixel_shader_1x_max_value=value))
                packed = bytes.fromhex(bound["cbuffer"]["bytes"])
                self.assertEqual(value, bound["pixel_shader_1x_max_value"])
                self.assertEqual(struct.pack("<f", value), packed[64:68])
        self.assert_code("EAWR-SHD-PS11-RANGE-UNRESOLVED",
                         good_binding(pixel_shader_1x_max_value=3.402824e38))

    def test_optional_range_value_is_validated_when_present(self):
        manifest = synthetic_manifest(escapes=False)
        for value in ("junk", math.nan, math.inf, 0.0, -1.0, True, 3.402824e38):
            with self.subTest(value=value):
                self.assert_code("EAWR-SHD-PS11-RANGE-UNRESOLVED",
                                 good_binding(pixel_shader_1x_max_value=value), manifest)
        bound = bind_external_state(manifest, good_binding(pixel_shader_1x_max_value=2.0))
        self.assertEqual(2.0, bound["pixel_shader_1x_max_value"])


class D3D9OracleTests(unittest.TestCase):
    """The independent evaluator against the committed legacy D3D9 synthetic draw."""

    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        cls.readback = {item["name"]: item["rgba"] for item in cls.fixture["cases"]}
        cls.cases = case_data.build_cases()

    def test_fixture_is_bound_to_current_cases_and_harness(self):
        text = case_data.to_text(self.cases).encode("utf-8")
        self.assertEqual(hashlib.sha256(text).hexdigest(), self.fixture["cases_sha256"])
        source = (HERE / "d3d9_oracle.cpp").read_bytes().replace(b"\r\n", b"\n")
        self.assertEqual(hashlib.sha256(source).hexdigest(), self.fixture["producer"]["source_sha256"])
        self.assertEqual({c["name"] for c in self.cases}, set(self.readback))
        self.assertEqual("HAL", self.fixture["runtime"]["device_type"])

    def test_evaluator_matches_d3d9_under_the_observed_convention(self):
        for case in self.cases:
            with self.subTest(case=case["name"]):
                expected, ambiguous = ps11_eval.render_case(case, convention="transposed")
                result = ps11_eval.compare(expected, self.readback[case["name"]], case_data.TOLERANCE_LSB)
                self.assertEqual([], result["failures"][:3])
                self.assertGreater(result["compared"], 240)
                self.assertLessEqual(ambiguous, 8)

    def test_documented_convention_diverges_only_on_asymmetric_off_diagonals(self):
        for case in self.cases:
            m00, m01, m10, m11 = case["bumpenv"]
            expected, _ = ps11_eval.render_case(case, convention="documented")
            failures = ps11_eval.compare(expected, self.readback[case["name"]], case_data.TOLERANCE_LSB)["failures"]
            uses_bem = any(draw["kind"] == case_data.KIND_T1 for draw in case["draws"])
            with self.subTest(case=case["name"]):
                if m01 == m10 or not uses_bem or case["name"] == "cleanup_restores_wrap":
                    self.assertEqual([], failures[:3])
                else:
                    self.assertGreater(len(failures), 50)

    def test_zero_matrix_equals_unperturbed_lookup_and_rgb_only_fow(self):
        case = next(c for c in self.cases if c["name"] == "bem_zero_matrix_point")
        stages = case["stages"]
        inputs = {"tc0": [0.3, 0.7], "tc1": [0.41, 0.62], "tc2": [0.1, 0.9],
                  "v0": [1.0, 0.5, 0.25, 0.6], "v1": [0.0, 0.0, 0.0, 0.0]}
        out = ps11_eval.run_program(ps11_eval.T1_PROGRAM, inputs, case["textures"], stages, case["bumpenv"], False)
        reflection = ps11_eval.sample(case["textures"][1], stages[1], 0.41, 0.62, False)
        fow = ps11_eval.sample(case["textures"][2], stages[2], 0.1, 0.9, False)
        self.assertAlmostEqual(0.6 * reflection[3], out[3])
        self.assertAlmostEqual(0.5 * reflection[1] * fow[1], out[1])
        self.assertEqual((-1.0, 1.0, 1.0, 1.0), ps11_eval.decode_texel(
            {"format": "V8U8", "texels": [(-127, 127)]}, 0))

    def test_range_clamp_model(self):
        case = next(c for c in self.cases if c["name"] == "range_escape_specular")
        # The recorded device reports PixelShader1xMaxValue = FLT_MAX: no clamp observed.
        self.assertGreater(self.fixture["caps"]["pixel_shader_1x_max_value"], 1e38)
        unbounded, _ = ps11_eval.render_case(case, convention="transposed")
        clamped, _ = ps11_eval.render_case(case, convention="transposed", max_value=1.0)
        self.assertEqual([], ps11_eval.compare(unbounded, self.readback[case["name"]], 2)["failures"][:1])
        self.assertGreater(len(ps11_eval.compare(clamped, self.readback[case["name"]], 2)["failures"]), 20)


class CorpusContractTests(unittest.TestCase):
    def test_accepted_bundle_still_lists_t1_as_unsupported_assembly(self):
        bundle = json.loads((ROOT / "plan" / "inventories" / "shader-corpus.json").read_text(encoding="utf-8"))
        self.assertEqual("9a8181637e88dd901bedbba6033870042a4219e87a0ff0172ef085afbc435c02", bundle["bundle_id"])
        water = next(item for item in bundle["effects"] if item["path"] == "Terrain/TerrainWater.fx")
        self.assertEqual("partial", water["translation"]["status"])
        self.assertEqual([("t1", "legacy_shader_assembly")],
                         [(item["technique"], item["kind"]) for item in water["translation"]["unsupported_constructs"]])

    def test_default_translator_does_not_route_through_the_t1_path(self):
        for name in ("translate.py", "corpus.py"):
            text = (ROOT / "tools" / "shaders" / name).read_text(encoding="utf-8")
            self.assertNotIn("terrainwater_t1", text)
            self.assertNotIn("ps11", text)


@unittest.skipUnless(PRIVATE_READY, "private shader source or pinned toolchain unavailable")
class PrivateBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from tools.shaders.terrainwater_t1 import build
        base = ROOT / "out" / "shaders"
        base.mkdir(parents=True, exist_ok=True)
        cls.work = Path(tempfile.mkdtemp(prefix="t1-test-", dir=base))
        cls.first = build(SOURCE_ROOT, cls.work / "a", GLSLANG, SPIRV_VAL, ARCHIVE)
        cls.second = build(SOURCE_ROOT, cls.work / "b", GLSLANG, SPIRV_VAL, ARCHIVE)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def test_stages_validate_and_reproduce(self):
        self.assertEqual(["vert", "frag"], [item["stage"] for item in self.first["stages"]])
        for item in self.first["stages"]:
            self.assertEqual((0, 0), (item["exit_code"], item["validation"]["exit_code"]))
        self.assertEqual([s["spirv_sha256"] for s in self.first["stages"]],
                         [s["spirv_sha256"] for s in self.second["stages"]])

    def test_manifest_records_pass_cleanup_and_no_selection_claim(self):
        m = self.first
        self.assertEqual(("t1", "t1_p0"), (m["technique"], m["pass"]))
        self.assertEqual({"1": {"address_u": "clamp", "address_v": "clamp"}}, m["pass_state"]["effect_sampler_state"])
        self.assertEqual({"1": {"address_u": "wrap", "address_v": "wrap"}}, m["pass_state"]["cleanup"]["restores"])
        state = m["pass_state"]["render_state"]
        self.assertEqual((True, "SRCALPHA", "INVSRCALPHA"), (state["alpha_blend_enable"]["value"],
                                                             state["source_blend"], state["destination_blend"]))
        self.assertEqual((True, True, "lessequal"), (state["depth_test_enable"]["value"],
                                                     state["depth_write_enable"]["value"], state["depth_compare"]["value"]))
        self.assertEqual({"1": 0}, {k: v for k, v in m["pixel"]["program"]["bump_stages"].items()})
        self.assertEqual(["tex", "texbem", "tex", "mad", "mul"],
                         [item["opcode"] for item in m["pixel"]["program"]["instructions"]])
        self.assertEqual("rgb", m["pixel"]["program"]["instructions"][-1]["mask"])
        saturated = {item["semantic"]: item["vs_1_1_saturate"] for item in m["vertex"]["outputs"]}
        self.assertTrue(saturated["COLOR0"] and saturated["COLOR1"] and not saturated["TEXCOORD1"])
        self.assertFalse(m["renderer_selected"])
        self.assertEqual("not_selected", m["map_selection"])
        self.assertEqual({"godot": "not_tested", "rendering_server_material": False}, m["renderer_ingestion"])

    @unittest.skipUnless(WGPU_PYTHON.is_file(), "pinned wgpu venv unavailable")
    def test_translated_spirv_matches_evaluator_d3d9_and_rejects_controls(self):
        sys.path.insert(0, str(HERE))
        import reflect
        manifest_dir = self.work / "a"
        offsets = reflect.globals_offsets(GLSLANG, manifest_dir)
        (manifest_dir / "globals.json").write_text(json.dumps(offsets), encoding="utf-8")
        result = subprocess.run([str(WGPU_PYTHON), str(HERE / "wgpu_harness.py"), "--manifest-dir", str(manifest_dir),
                                 "--globals-reflection", str(manifest_dir / "globals.json"),
                                 "--d3d9-readback", str(FIXTURE)], capture_output=True, text=True, check=False)
        self.assertEqual(0, result.returncode, result.stdout[-2000:])
        report = json.loads(result.stdout)
        self.assertEqual(("0.23.0", "25.0.2.1"), (report["wgpu"], report["wgpu_native"]))
        self.assertGreaterEqual(len(report["pixel"]), 19)
        for item in report["pixel"] + report["full"]:
            with self.subTest(case=item["case"], convention=item["convention"]):
                self.assertEqual([], item["vs_evaluator"]["failures"][:3])
                self.assertGreater(item["vs_evaluator"]["compared"], 240)
                if "vs_d3d9_readback" in item:
                    self.assertEqual(0, item["vs_d3d9_readback"]["failures"])
        for item in report["full"]:
            self.assertGreater(item["drawn_pixels"], 100)
            for name, failures in item["negative_controls"].items():
                with self.subTest(control=name, convention=item["convention"]):
                    self.assertGreater(failures, 5)


if __name__ == "__main__":
    unittest.main()
