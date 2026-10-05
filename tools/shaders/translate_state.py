#!/usr/bin/env python3
"""Translate a bounded legacy Direct3D effect into validated SPIR-V stages.

Private inputs and every source-derived output belong under ignored ``out/shaders``.
Checked-in code contains only the independently written parser/lowering and synthetic
fixtures.  Run with ``--help`` for the reproducible corpus command.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
import platform
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Optional

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.shaders.fx_parser import (Effect, FxError, IncludeResolver, Pass,
                                         Technique, parameter_dict, parse_effect,
                                         prune_unreachable_functions,
                                         source_without_effect_wrappers)
else:
    from .fx_parser import (Effect, FxError, IncludeResolver, Pass, Technique,
                            parameter_dict, parse_effect,
                            prune_unreachable_functions,
                            source_without_effect_wrappers)


EFFECTS = {
    "meshgloss": {
        "path": "MeshGloss.fx",
        "sha256": "d44c399eb75bfa27f274a85d80c24727f4ef73c4b08d485c965c14ad88b5abd0",
        "closure": [
            ("Gloss.fxh", "4556b4dfd48492bdebecd1469b44e0febe257bf97bba2d8f69e579945e3cec71"),
            ("AlamoEngine.fxh", "7a1ce5e065726652264cfc6cf17d8e45498c63af703daab807c257183342038e"),
        ],
        "order": ["sph_t0", "sph_t1"],
    },
    "meshbumpcolorize": {
        "path": "MeshBumpColorize.fx",
        "sha256": "ff9d1c2dd058eb9f9f95d058dfd8d5ca9c704d06e6e52d9151318c199f30605e",
        "closure": [
            ("BumpColorize.fxh", "d7e16b36b168e1ca97a0136a0db64c59d5a05dc416a2b05486e57ce9e15cde21"),
            ("AlamoEngine.fxh", "7a1ce5e065726652264cfc6cf17d8e45498c63af703daab807c257183342038e"),
        ],
        "order": ["sph_t2", "sph_t1", "t0"],
    },
    "rskinglosscolorize": {
        "path": "RSkinGlossColorize.fx",
        "sha256": "befa202bf009de1797eaaabc397f5ef038984963c7f8348fbb69944dd0cf1406",
        "closure": [
            ("AlamoEngineSkinning.fxh", "52ba3eb0aee53df0dfe15cc43534774f6a3d6a9220db3ec0c701bcf78d712002"),
            ("GlossColorize.fxh", "e053c82b5e909dcf663c960ca01a84862d447160d3a48d034c65cb26b254ea67"),
            ("AlamoEngine.fxh", "7a1ce5e065726652264cfc6cf17d8e45498c63af703daab807c257183342038e"),
        ],
        "order": ["sph_t0", "sph_t1"],
    },
    "meshshield": {
        "path": "MeshShield.fx",
        "sha256": "7f5111fee8ba8d61580e30ce78a7cc575567411c4354e64e20a9b62f566f524e",
        "closure": [
            ("Shield.fxh", "0c4805568f0393931916bf9acf5dea1736eb4738239ce5a8c21fbcca75d0e5ce"),
            ("AlamoEngine.fxh", "7a1ce5e065726652264cfc6cf17d8e45498c63af703daab807c257183342038e"),
        ],
        "order": ["t0", "t1", "t2"],
    },
    "primalpha": {
        "path": "Engine/PrimAlpha.fx",
        "sha256": "936301924b4bf7e9c65423913a398205f0b4fe353db27feb661cdc2cd9c38376",
        "closure": [
            ("AlamoEngine.fxh", "7a1ce5e065726652264cfc6cf17d8e45498c63af703daab807c257183342038e"),
        ],
        "order": ["t1"],
    },
}


COMPATIBILITY = {
    "meshgloss": {
        "sph_t0": {"profiles": ["vs_1_1", "ps_1_1"], "textures": ["BaseTexture"], "dynamic_states": True},
        "sph_t1": {"fixed_transform": True, "fixed_lighting": True, "textures": ["BaseTexture"], "texture_stages": 1},
    },
    "meshbumpcolorize": {
        "sph_t2": {"profiles": ["vs_1_1", "ps_2_0"], "vertex_layout": "tangent", "textures": ["BaseTexture", "NormalTexture"]},
        "sph_t1": {"profiles": ["vs_1_1", "ps_1_1"], "vertex_layout": "tangent", "textures": ["BaseTexture", "NormalTexture"]},
        "t0": {"fixed_transform": True, "fixed_lighting": True, "textures": ["BaseTexture"], "texture_stages": 2,
               "degradation": ["normal-map bump detail is not represented"]},
    },
    "rskinglosscolorize": {
        "sph_t0": {"profiles": ["vs_1_1", "ps_1_1"], "textures": ["BaseTexture", "GlossTexture"], "vertex_constants": 72},
        "sph_t1": {"cpu_skin": True, "fixed_transform": True, "fixed_lighting": True, "textures": ["BaseTexture"], "texture_stages": 2},
    },
    "meshshield": {
        "t0": {"profiles": ["vs_1_1", "ps_2_0"], "textures": ["BaseTexture", "WaveTexture", "DistortionTexture"]},
        "t1": {"fixed_transform": True, "textures": ["BaseTexture", "WaveTexture"], "texture_stages": 3, "transformed_coordinates": 2},
        "t2": {"fixed_transform": True, "textures": ["BaseTexture"], "texture_stages": 2, "transformed_coordinates": 1},
    },
    "primalpha": {
        "t1": {"fixed_transform": True, "caller_sampler_zero": True, "texture_stages": 1},
    },
}


def _split_top_level_expressions(text: str) -> list[str]:
    """Split an initializer without treating nested call arguments as elements."""
    pieces: list[str] = []
    start = 0
    depth = 0
    for index, character in enumerate(text):
        if character in "([":
            depth += 1
        elif character in ")]":
            depth = max(0, depth - 1)
        elif character == "," and depth == 0:
            pieces.append(text[start:index].strip())
            start = index + 1
    final = text[start:].strip()
    if final:
        pieces.append(final)
    return pieces


def normalize_legacy_hlsl(text: str) -> str:
    """Normalize narrowly-scoped DX9 HLSL syntax rejected by pinned glslang.

    The DirectX compiler accepts flat scalar aggregate initialization for vector
    arrays. glslang requires each vector element to use an explicit constructor.
    Only a complete ``float2``/``float3``/``float4`` array declaration with a
    scalar-only, evenly divisible initializer is rewritten; nested aggregates
    and expressions containing braces are left untouched.
    """
    pattern = re.compile(
        r"(?P<head>\b(?:static\s+)?(?:const\s+)?float(?P<width>[234])\s+"
        r"[A-Za-z_]\w*\s*\[\s*(?P<count>\d+)\s*\]\s*=\s*)"
        r"\{(?P<body>[^{}]*)\}(?P<tail>\s*;)",
        re.DOTALL,
    )

    def replace(match: re.Match[str]) -> str:
        width = int(match.group("width"))
        count = int(match.group("count"))
        values = _split_top_level_expressions(match.group("body"))
        if not values or len(values) != count * width:
            return match.group(0)
        grouped = [
            f"float{width}({', '.join(values[index:index + width])})"
            for index in range(0, len(values), width)
        ]
        return f"{match.group('head')}{{ {', '.join(grouped)} }}{match.group('tail')}"

    return pattern.sub(replace, text)


def is_editor_technique(technique: Technique) -> bool:
    """Return whether a source technique is an editor maximum-viewport preview."""
    return technique.name.casefold() == "max_viewport"


def _normal(value: str) -> str:
    return re.sub(r"\s+", "", value).casefold()


def select_compatible(effect_key: str, capabilities: dict) -> tuple[Optional[str], dict[str, list[str]]]:
    """Select only after checking every declared compatibility condition."""
    key = effect_key.casefold()
    failures: dict[str, list[str]] = {}
    for technique in EFFECTS[key]["order"]:
        requirement = COMPATIBILITY[key][technique]
        missing: list[str] = []
        profiles = {item.casefold() for item in capabilities.get("profiles", [])}
        for profile in requirement.get("profiles", []):
            if profile.casefold() not in profiles:
                missing.append(f"profile:{profile}")
        textures = {item.casefold() for item in capabilities.get("textures", [])}
        for texture in requirement.get("textures", []):
            if texture.casefold() not in textures:
                missing.append(f"texture:{texture}")
        for flag in ("fixed_transform", "fixed_lighting", "cpu_skin", "caller_sampler_zero", "dynamic_states"):
            if requirement.get(flag) and not capabilities.get(flag, False):
                missing.append(flag)
        if requirement.get("vertex_layout") and capabilities.get("vertex_layout") != requirement["vertex_layout"]:
            missing.append(f"vertex_layout:{requirement['vertex_layout']}")
        for numeric in ("texture_stages", "transformed_coordinates", "vertex_constants"):
            if capabilities.get(numeric, 0) < requirement.get(numeric, 0):
                missing.append(f"{numeric}:{requirement[numeric]}")
        if not missing:
            return technique, failures
        failures[technique] = missing
    return None, failures


def evaluate_dynamic_bool(expression: str, values: dict[str, float]) -> bool:
    """Evaluate the intentionally tiny dynamic-state predicate grammar."""
    expression = expression.strip()
    while expression.startswith("(") and expression.endswith(")"):
        expression = expression[1:-1].strip()
    match = re.fullmatch(r"\s*([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\s*(<=|>=|<|>|==|!=)\s*(-?(?:\d+(?:\.\d*)?|\.\d+))[fF]?\s*", expression)
    if not match or match.group(1) not in values:
        raise ValueError(f"unsupported dynamic-state predicate: {expression}")
    left, right = float(values[match.group(1)]), float(match.group(3))
    return {"<": left < right, ">": left > right, "<=": left <= right, ">=": left >= right,
            "==": left == right, "!=": left != right}[match.group(2)]


def skin_transform(position: tuple[float, float, float, float], normal: tuple[float, float, float],
                   palette: list[tuple[float, float, float, float]], selector: int):
    """Apply source vector-left float4x3 palette storage (three float4 constants/bone)."""
    if selector < 0 or selector * 3 + 2 >= len(palette):
        raise ValueError("selector outside supplied palette")
    rows = palette[selector * 3:selector * 3 + 3]
    out_position = tuple(sum(position[i] * row[i] for i in range(4)) for row in rows)
    out_normal = tuple(sum(normal[i] * row[i] for i in range(3)) for row in rows)
    length = sum(value * value for value in out_normal) ** 0.5
    normalized = tuple(value / length for value in out_normal) if length else out_normal
    return out_position, normalized


def _state(states: dict[str, str], name: str, stage: Optional[int] = None) -> Optional[str]:
    wanted = _normal(name if stage is None else f"{name}[{stage}]")
    for key, value in states.items():
        if _normal(key) == wanted:
            return value.strip()
    return None


def _bool_state(states: dict[str, str], name: str, default: bool) -> dict:
    value = _state(states, name)
    if value is None:
        return {"kind": "literal", "value": default, "source": "renderer_baseline"}
    folded = value.casefold()
    if folded in {"true", "1"}:
        return {"kind": "literal", "value": True, "source": "effect"}
    if folded in {"false", "0"}:
        return {"kind": "literal", "value": False, "source": "effect"}
    return {"kind": "dynamic", "expression": value, "source": "effect"}


def render_state_descriptor(pass_ir: Pass) -> dict:
    states = pass_ir.states
    cull = _state(states, "CullMode") or "CCW"
    zfunc = _state(states, "ZFunc") or "LESSEQUAL"
    return {
        "alpha_blend_enable": _bool_state(states, "AlphaBlendEnable", False),
        "alpha_test_enable": _bool_state(states, "AlphaTestEnable", False),
        "cull_mode": {"kind": "literal", "value": cull, "source": "effect" if _state(states, "CullMode") else "renderer_baseline"},
        "depth_compare": {"kind": "literal", "value": zfunc, "source": "effect" if _state(states, "ZFunc") else "renderer_baseline"},
        "depth_test_enable": _bool_state(states, "ZEnable", True),
        "depth_write_enable": _bool_state(states, "ZWriteEnable", True),
        "destination_blend": _state(states, "DestBlend") or "ZERO",
        "fog_enable": _bool_state(states, "FogEnable", False),
        "source_blend": _state(states, "SrcBlend") or "ONE",
        "unmapped": {key: value for key, value in sorted(states.items(), key=lambda item: item[0].casefold())
                     if not any(_normal(key).startswith(_normal(prefix)) for prefix in (
                         "AlphaBlendEnable", "AlphaTestEnable", "CullMode", "ZFunc", "ZEnable",
                         "ZWriteEnable", "DestBlend", "FogEnable", "SrcBlend", "ColorOp[",
                         "ColorArg", "AlphaOp[", "AlphaArg", "Texture[", "TexCoordIndex[",
                         "TextureTransformFlags["))},
    }


def _enum(value: Optional[str]) -> Optional[str]:
    if value is None:
        return None
    return value.strip().strip("()").upper()


SUPPORTED_OPS = {"SELECTARG1", "SELECTARG2", "MODULATE", "MODULATE2X", "MODULATE4X", "ADD", "SUBTRACT", "BLENDTEXTUREALPHA", "BLENDCURRENTALPHA", "DISABLE"}


SUPPORTED_ARGS = {"CURRENT", "DIFFUSE", "TEXTURE", "TFACTOR"}


def fixed_function_ir(pass_ir: Pass) -> dict:
    stages = []
    for index in range(8):
        color_op = _enum(_state(pass_ir.states, "ColorOp", index))
        alpha_op = _enum(_state(pass_ir.states, "AlphaOp", index))
        if color_op is None:
            if index == 0:
                # D3D9 stage-zero defaults; explicit descriptor makes the baseline visible.
                color_op = "MODULATE"
                alpha_op = alpha_op or "SELECTARG1"
            else:
                break
        if color_op == "DISABLE":
            break
        alpha_op = alpha_op or color_op
        if color_op not in SUPPORTED_OPS or alpha_op not in SUPPORTED_OPS:
            raise FxError("SHD_UNSUPPORTED_TEXTURE_OP", f"stage {index}: {color_op}/{alpha_op}", "<effect>", pass_ir.line, 1)
        color_args = [_enum(_state(pass_ir.states, f"ColorArg{arg}", index)) for arg in (1, 2)]
        alpha_args = [_enum(_state(pass_ir.states, f"AlphaArg{arg}", index)) for arg in (1, 2)]
        color_args = [arg or ("TEXTURE" if index == 0 and slot == 0 else "CURRENT") for slot, arg in enumerate(color_args)]
        alpha_args = [arg or ("TEXTURE" if index == 0 and slot == 0 else "CURRENT") for slot, arg in enumerate(alpha_args)]
        for argument in [*color_args, *alpha_args]:
            base = argument.replace("COMPLEMENT", "").replace("ALPHAREPLICATE", "")
            if base not in SUPPORTED_ARGS:
                raise FxError("SHD_UNSUPPORTED_TEXTURE_ARG", f"stage {index}: {argument}", "<effect>", pass_ir.line, 1)
        texture = _state(pass_ir.states, "Texture", index)
        stages.append({
            "index": index,
            "color_op": color_op,
            "color_args": color_args,
            "alpha_op": alpha_op,
            "alpha_args": alpha_args,
            "texture": texture,
            "texcoord_index": _state(pass_ir.states, "TexCoordIndex", index) or str(index),
            "texture_transform": _state(pass_ir.states, "TextureTransformFlags", index) or "DISABLE",
        })
    if not stages:
        raise FxError("SHD_EMPTY_FIXED_FUNCTION", "fixed-function draw has no active texture stages", "<effect>", pass_ir.line, 1)
    return {"stages": stages, "texture_factor": _state(pass_ir.states, "TextureFactor"),
            "lighting": _bool_state(pass_ir.states, "Lighting", True)}


def evaluate_fixed_function(ir: dict, diffuse: tuple[float, float, float, float],
                            samples: dict[int, tuple[float, float, float, float]],
                            texture_factor: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    """Reference evaluator for the parsed independent color/alpha cascades."""
    current = tuple(float(value) for value in diffuse)

    def argument(name: str, sample: tuple[float, float, float, float]):
        upper = name.upper()
        base = upper.replace("COMPLEMENT", "").replace("ALPHAREPLICATE", "")
        value = {"CURRENT": current, "DIFFUSE": diffuse, "TEXTURE": sample, "TFACTOR": texture_factor}[base]
        if "ALPHAREPLICATE" in upper:
            value = (value[3],) * 4
        if "COMPLEMENT" in upper:
            value = tuple(1.0 - item for item in value)
        return value

    def operation(name: str, first, second, texture_alpha: float):
        if name == "SELECTARG1":
            return first
        if name == "SELECTARG2":
            return second
        if name == "MODULATE":
            return tuple(a * b for a, b in zip(first, second))
        if name == "MODULATE2X":
            return tuple(2.0 * a * b for a, b in zip(first, second))
        if name == "MODULATE4X":
            return tuple(4.0 * a * b for a, b in zip(first, second))
        if name == "ADD":
            return tuple(a + b for a, b in zip(first, second))
        if name == "SUBTRACT":
            return tuple(a - b for a, b in zip(first, second))
        if name == "BLENDTEXTUREALPHA":
            return tuple(b * (1.0 - texture_alpha) + a * texture_alpha for a, b in zip(first, second))
        if name == "BLENDCURRENTALPHA":
            return tuple(b * (1.0 - current[3]) + a * current[3] for a, b in zip(first, second))
        raise ValueError(f"unsupported operation: {name}")

    for stage in ir["stages"]:
        sample = samples.get(stage["index"], (1.0, 1.0, 1.0, 1.0))
        c1, c2 = (argument(item, sample) for item in stage["color_args"])
        a1, a2 = (argument(item, sample) for item in stage["alpha_args"])
        rgb = operation(stage["color_op"], c1[:3], c2[:3], sample[3])
        alpha = operation(stage["alpha_op"], (a1[3],), (a2[3],), sample[3])[0]
        current = (*rgb, alpha)
    return current


def _arg_expr(argument: str, channel: str, sample: str) -> str:
    upper = argument.upper()
    base = upper.replace("COMPLEMENT", "").replace("ALPHAREPLICATE", "")
    source = {"CURRENT": "current", "DIFFUSE": "input.diffuse", "TEXTURE": sample, "TFACTOR": "u_texture_factor"}[base]
    if "ALPHAREPLICATE" in upper:
        source = f"({source}).aaaa"
    if "COMPLEMENT" in upper:
        source = f"(1.0 - ({source}))"
    return f"({source}).{channel}"


def _op_expr(op: str, first: str, second: str, texture_alpha: str) -> str:
    return {
        "SELECTARG1": first,
        "SELECTARG2": second,
        "MODULATE": f"({first} * {second})",
        "MODULATE2X": f"(2.0 * {first} * {second})",
        "MODULATE4X": f"(4.0 * {first} * {second})",
        "ADD": f"({first} + {second})",
        "SUBTRACT": f"({first} - {second})",
        "BLENDTEXTUREALPHA": f"lerp({second}, {first}, {texture_alpha})",
        "BLENDCURRENTALPHA": f"lerp({second}, {first}, current.a)",
    }[op]


def lower_fixed_function(pass_ir: Pass) -> tuple[str, str, dict]:
    ir = fixed_function_ir(pass_ir)
    stages = ir["stages"]
    lighting = ir["lighting"]
    if lighting["kind"] != "literal":
        raise FxError("SHD_DYNAMIC_FIXED_LIGHTING", "dynamic fixed-function lighting is not supported", "<effect>", pass_ir.line, 1)
    sampler_declarations = "\n".join(f"sampler2D stage{stage['index']}_sampler;" for stage in stages if stage["texture"] is not None or stage["index"] == 0)
    lighting_declarations = """
float3x3 u_normal_matrix;
float4 u_global_ambient;
float4 u_material_ambient;
float4 u_material_diffuse;
float4 u_material_specular;
float4 u_material_emissive;
float u_material_power;
float3 u_eye_position;
float3 u_light_direction[3];
float4 u_light_diffuse[3];
float4 u_light_specular[3];
""" if lighting["value"] else ""
    lighting_body = """
    float3 normal = normalize(mul(input.normal, u_normal_matrix));
    float3 view_direction = normalize(u_eye_position - input.position.xyz);
    float3 lit = u_material_emissive.rgb + u_global_ambient.rgb * u_material_ambient.rgb;
    [unroll] for (int light_index = 0; light_index < 3; ++light_index) {
        float3 to_light = normalize(-u_light_direction[light_index]);
        float ndotl = max(dot(normal, to_light), 0.0);
        lit += ndotl * u_material_diffuse.rgb * u_light_diffuse[light_index].rgb;
        float3 reflected = reflect(-to_light, normal);
        float specular = ndotl > 0.0 ? pow(max(dot(reflected, view_direction), 0.0), u_material_power) : 0.0;
        lit += specular * u_material_specular.rgb * u_light_specular[light_index].rgb;
    }
    output.diffuse = float4(lit, u_material_diffuse.a);
""" if lighting["value"] else "    output.diffuse = input.diffuse;\n"
    vertex = f"""// generated fixed-function vertex equivalent; values use source vector-left order
float4x4 u_world_view_projection;
float3x3 u_texture_transform[8];
{lighting_declarations}
struct FfpInput {{ float4 position : POSITION; float3 normal : NORMAL; float4 diffuse : COLOR0; float2 uv : TEXCOORD0; }};
struct FfpVarying {{ float4 position : SV_Position; float4 diffuse : COLOR0; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float2 uv2 : TEXCOORD2; float2 uv3 : TEXCOORD3; }};
FfpVarying eawr_ffp_vs(FfpInput input) {{
    FfpVarying output;
    output.position = mul(input.position, u_world_view_projection);
{lighting_body}
    output.uv0 = mul(float3(input.uv, 1.0), u_texture_transform[0]).xy;
    output.uv1 = mul(float3(input.uv, 1.0), u_texture_transform[1]).xy;
    output.uv2 = mul(float3(input.uv, 1.0), u_texture_transform[2]).xy;
    output.uv3 = mul(float3(input.uv, 1.0), u_texture_transform[3]).xy;
    return output;
}}
"""
    ir["generated_uniform_contract"] = {
        "transform": ["u_world_view_projection", "u_texture_transform"],
        "lighting": (["u_normal_matrix", "u_global_ambient", "u_material_ambient", "u_material_diffuse",
                      "u_material_specular", "u_material_emissive", "u_material_power", "u_eye_position",
                      "u_light_direction", "u_light_diffuse", "u_light_specular"] if lighting["value"] else []),
    }
    lines = [sampler_declarations, "float4 u_texture_factor;",
             "struct FfpVarying { float4 position : SV_Position; float4 diffuse : COLOR0; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float2 uv2 : TEXCOORD2; float2 uv3 : TEXCOORD3; };",
             "float4 eawr_ffp_ps(FfpVarying input) : SV_Target0 {", "  float4 current = input.diffuse;"]
    for stage in stages:
        index = stage["index"]
        sample = f"tex2D(stage{index}_sampler, input.uv{min(index, 3)})" if stage["texture"] is not None or index == 0 else "float4(1.0, 1.0, 1.0, 1.0)"
        lines.append(f"  float4 sample{index} = {sample};")
        c1 = _arg_expr(stage["color_args"][0], "rgb", f"sample{index}")
        c2 = _arg_expr(stage["color_args"][1], "rgb", f"sample{index}")
        a1 = _arg_expr(stage["alpha_args"][0], "a", f"sample{index}")
        a2 = _arg_expr(stage["alpha_args"][1], "a", f"sample{index}")
        rgb = _op_expr(stage["color_op"], c1, c2, f"sample{index}.a")
        alpha = _op_expr(stage["alpha_op"], a1, a2, f"sample{index}.a")
        lines.append(f"  current = float4({rgb}, {alpha});")
    lines.extend(["  return current;", "}", ""])
    return vertex, "\n".join(lines), ir
