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


TOOL_VERSION = "1.1.0"
ARCHIVE_SHA256 = "88b9cb03322aab9be7451968ca514e9d2c810973d83f65459fd8cb52e7f96f8d"
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


def _write_text_lf(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


class ShaderPolicyError(RuntimeError):
    """A stable non-source diagnostic raised before protected processing."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message

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


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2,
                       allow_nan=False) + "\n").encode("utf-8")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


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


def _resolve_output_path(output: Path) -> Path:
    """Resolve and confine every source-derived artifact below ignored out/shaders."""
    allowed_lexical = REPOSITORY_ROOT / "out" / "shaders"
    allowed_resolved = allowed_lexical.resolve(strict=False)
    # A reparse point on the protected root itself would make the apparently
    # ignored tree write elsewhere.  Fail instead of blessing its target.
    if allowed_resolved != allowed_lexical:
        raise ShaderPolicyError(
            "SHD_OUTPUT_OUTSIDE_CONFINEMENT",
            "repository out/shaders resolves through a symlink or junction",
        )
    candidate_input = output if output.is_absolute() else Path.cwd() / output
    candidate = candidate_input.resolve(strict=False)
    try:
        relative = candidate.relative_to(allowed_resolved)
    except ValueError as error:
        raise ShaderPolicyError(
            "SHD_OUTPUT_OUTSIDE_CONFINEMENT",
            "output must resolve below repository out/shaders",
        ) from error
    if not relative.parts:
        raise ShaderPolicyError(
            "SHD_OUTPUT_OUTSIDE_CONFINEMENT",
            "output must be a child of repository out/shaders",
        )
    existing = candidate
    while not existing.exists() and existing != allowed_resolved:
        existing = existing.parent
    if existing.exists() and not existing.is_dir():
        raise ShaderPolicyError(
            "SHD_OUTPUT_OUTSIDE_CONFINEMENT",
            "output has a non-directory existing parent",
        )
    return candidate


def _platform_identity() -> str:
    system = platform.system().casefold()
    machine = platform.machine().casefold().replace("amd64", "x86_64")
    return f"{system}-{machine}"


def _toolchain_contract() -> dict:
    path = Path(__file__).with_name("toolchain.json")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ShaderPolicyError("SHD_TOOLCHAIN_CONTRACT_INVALID", str(error)) from error
    if value.get("schema_version") != 1:
        raise ShaderPolicyError("SHD_TOOLCHAIN_CONTRACT_INVALID", "expected schema_version 1")
    return value


def _validate_tool_identity(executable: Path, kind: str) -> str:
    candidate = executable.resolve()
    if not candidate.is_file():
        raise ShaderPolicyError("SHD_TOOL_NOT_FOUND", f"{kind} executable does not exist")
    contract = _toolchain_contract().get(kind)
    if not isinstance(contract, dict):
        raise ShaderPolicyError("SHD_TOOLCHAIN_CONTRACT_INVALID", f"missing {kind} contract")
    expected_hash = contract.get("executable_sha256", {}).get(_platform_identity())
    if expected_hash is not None:
        actual_hash = sha256_file(candidate)
        if actual_hash != expected_hash:
            raise ShaderPolicyError(
                "SHD_TOOL_IDENTITY_MISMATCH",
                f"{kind} SHA-256 mismatch for {_platform_identity()}: expected {expected_hash}, got {actual_hash}",
            )
    try:
        result = subprocess.run(
            [str(candidate), "--version"], text=True, capture_output=True,
            check=False, timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ShaderPolicyError(
            "SHD_TOOL_IDENTITY_MISMATCH", f"{kind} --version failed: {error}",
        ) from error
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        raise ShaderPolicyError(
            "SHD_TOOL_IDENTITY_MISMATCH",
            f"{kind} --version exited {result.returncode}",
        )
    if kind == "glslang":
        expected = re.escape(str(contract["version"]))
        matched = re.search(rf"(?im)^Glslang Version:\s+\d+:{expected}\s*$", output)
        requirement = f"Glslang Version ...:{contract['version']}"
    else:
        expected_version = re.escape(str(contract["version"]))
        commit = str(contract["source_commit"])
        matched = re.search(
            rf"(?im)^SPIRV-Tools\s+{expected_version}\s+{expected_version}-\d+-g{re.escape(commit[:7])}(?:\s|$)",
            output,
        )
        requirement = f"SPIRV-Tools {contract['version']} commit {commit[:7]}"
    if matched is None:
        raise ShaderPolicyError(
            "SHD_TOOL_IDENTITY_MISMATCH", f"{kind} does not report pinned identity {requirement}",
        )
    lines = output.splitlines()
    return lines[0].strip() if lines else requirement


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


def preprocess_effect(glslang: Path, output: Path, source: str) -> tuple[str, dict]:
    """Use the pinned HLSL preprocessor for macro expansion, then parse its text."""
    input_name = "resolved.fx.hlsl"
    output_name = "preprocessed.fx.hlsl"
    _write_text_lf(output / input_name, source)
    command = [str(glslang), "-D", "-E", "-S", "vert", input_name]
    result = subprocess.run(command, cwd=output, text=True, capture_output=True, check=False)
    log_name = "preprocess.log"
    _write_text_lf(output / log_name, result.stderr or "")
    if result.returncode != 0:
        raise RuntimeError(f"preprocessor failed; see {output / log_name}")
    _write_text_lf(output / output_name, result.stdout)
    return result.stdout, {
        "command": [Path(command[0]).name, *command[1:]],
        "exit_code": result.returncode,
        "input": input_name,
        "input_sha256": sha256_file(output / input_name),
        "output": output_name,
        "output_sha256": sha256_file(output / output_name),
        "log": log_name,
        "log_sha256": sha256_file(output / log_name),
    }


def resolve_compiled_shader_symbols(effect: Effect) -> None:
    """Resolve passes that reference a named compile expression/binary variable."""
    symbols = {}
    for parameter in effect.parameters:
        if parameter.default and parameter.default.lstrip().casefold().startswith("compile "):
            from tools.shaders.fx_parser import _shader_assignment
            symbols[parameter.name.casefold()] = _shader_assignment(parameter.default)
    for technique in effect.techniques:
        for pass_ir in technique.passes:
            for attribute in ("vertex_shader", "pixel_shader"):
                assignment = getattr(pass_ir, attribute)
                if assignment and assignment.profile is None and assignment.entry:
                    resolved = symbols.get(assignment.entry.casefold())
                    if resolved is not None:
                        setattr(pass_ir, attribute, resolved)


def compile_stage(glslang: Path, spirv_val: Path, work: Path, source_name: str,
                  stage: str, entry: str) -> dict:
    spv_name = f"{Path(source_name).stem}.spv"
    command = [str(glslang), "-D", "-V", "--target-env", "vulkan1.1",
               "--hlsl-dx9-compatible", "--auto-map-bindings", "--auto-map-locations",
               "-S", stage, "-e", entry, "-o", spv_name, source_name]
    compiled = subprocess.run(command, cwd=work, text=True, capture_output=True, check=False)
    compile_log = f"{source_name}.compile.log"
    _write_text_lf(work / compile_log, (compiled.stdout or "") + (compiled.stderr or ""))
    result = {
        "command": [Path(command[0]).name, *command[1:]],
        "exit_code": compiled.returncode,
        "log": compile_log,
        "log_sha256": sha256_file(work / compile_log),
    }
    if compiled.returncode != 0:
        raise RuntimeError(f"compiler failed for {source_name}; see {work / compile_log}")
    validate_command = [str(spirv_val), "--target-env", "vulkan1.1", spv_name]
    validated = subprocess.run(validate_command, cwd=work, text=True, capture_output=True, check=False)
    validate_log = f"{source_name}.validate.log"
    _write_text_lf(work / validate_log, (validated.stdout or "") + (validated.stderr or ""))
    result["validation"] = {
        "command": [Path(validate_command[0]).name, *validate_command[1:]],
        "exit_code": validated.returncode,
        "log": validate_log,
        "log_sha256": sha256_file(work / validate_log),
    }
    if validated.returncode != 0:
        raise RuntimeError(f"SPIR-V validation failed for {spv_name}; see {work / validate_log}")
    result["spirv"] = spv_name
    result["spirv_sha256"] = sha256_file(work / spv_name)
    return result


def _verify_contract(resolved, contract: dict) -> None:
    if resolved.root["sha256"] != contract["sha256"]:
        raise FxError("SHD_ROOT_HASH_MISMATCH", f"expected {contract['sha256']}, got {resolved.root['sha256']}", resolved.root["path"], 1, 1)
    actual = [(item["path"].split("/")[-1].casefold(), item["sha256"]) for item in resolved.includes]
    expected = [(name.casefold(), digest) for name, digest in contract["closure"]]
    if actual != expected:
        raise FxError("SHD_INCLUDE_CLOSURE_MISMATCH", f"expected {expected}, got {actual}", resolved.root["path"], 1, 1)


def _archive_path(source_root: Path, explicit: Optional[Path]) -> Path:
    if explicit is not None:
        return explicit.resolve()
    for parent in [source_root.resolve(), *source_root.resolve().parents]:
        candidate = parent / "foc_shaders.zip"
        if candidate.is_file():
            return candidate
    raise ValueError("protected corpus requires --source-archive (or an ancestor foc_shaders.zip)")


def _technique_dict(technique: Technique) -> dict:
    return {
        "name": technique.name,
        "annotations": dict(sorted(technique.annotations.items(), key=lambda item: item[0].casefold())),
        "line": technique.line,
        "passes": [{
            "name": item.name,
            "line": item.line,
            "draw": item.draw,
            "vertex_shader": asdict(item.vertex_shader) if item.vertex_shader else None,
            "pixel_shader": asdict(item.pixel_shader) if item.pixel_shader else None,
            "render_state": render_state_descriptor(item),
            "source_states": dict(sorted(item.states.items(), key=lambda state: state[0].casefold())),
        } for item in technique.passes],
    }


def translate(source_root: Path, effect_name: str, output: Path, glslang: Path,
              spirv_val: Path, source_archive: Optional[Path] = None,
              allow_unpinned_synthetic: bool = False,
              allow_pinned_public_corpus: bool = False) -> dict:
    output = _resolve_output_path(output)
    # Validate both selected executables before reading any protected source or
    # creating any output.  Direct API callers receive the same gate as the CLI.
    glslang_version = _validate_tool_identity(glslang, "glslang")
    spirv_version = _validate_tool_identity(spirv_val, "spirv_tools")
    key = effect_name.replace(".fx", "").replace("/", "").replace("\\", "").casefold()
    contract = EFFECTS.get(key)
    logical = contract["path"] if contract else effect_name
    if contract is None and not allow_unpinned_synthetic and not allow_pinned_public_corpus:
        raise ValueError(
            "effect is outside the bounded five; use corpus mode for the pinned public archive "
            "or --allow-unpinned-synthetic only for original fixtures"
        )
    if contract or allow_pinned_public_corpus:
        archive = _archive_path(source_root, source_archive)
        archive_hash = sha256_file(archive)
        if archive_hash != ARCHIVE_SHA256:
            raise ValueError(f"source archive hash mismatch: expected {ARCHIVE_SHA256}, got {archive_hash}")
    else:
        archive_hash = None
    resolver = IncludeResolver(source_root)
    resolved = resolver.resolve(logical)
    if contract:
        _verify_contract(resolved, contract)
    output.mkdir(parents=True, exist_ok=True)
    preprocessed, preprocess_record = preprocess_effect(glslang, output, resolved.text)
    effect = parse_effect(preprocessed, resolved.root["path"])
    resolve_compiled_shader_symbols(effect)
    techniques_by_name = {item.name.casefold(): item for item in effect.techniques}
    requested_order = (contract["order"] if contract else
                       [item.name for item in effect.techniques if not is_editor_technique(item)])
    runtime_techniques = []
    for name in requested_order:
        found = techniques_by_name.get(name.casefold())
        if found is None:
            raise FxError("SHD_REQUIRED_TECHNIQUE_MISSING", f"required technique missing: {name}", resolved.root["path"], 1, 1)
        runtime_techniques.append(found)

    lowered_programmable = normalize_legacy_hlsl(
        source_without_effect_wrappers(preprocessed, effect)
    )
    artifacts = []
    unsupported_constructs = []
    shader_object_defaults = {
        item.name.casefold(): (item.default or "").lstrip()
        for item in effect.parameters
        if item.type.casefold() in {"vertexshader", "pixelshader"}
    }
    for technique in runtime_techniques:
        technique_dir = output / technique.name
        technique_dir.mkdir(parents=True, exist_ok=True)
        for pass_index, pass_ir in enumerate(technique.passes):
            pass_dir = technique_dir / f"{pass_index:02d}-{pass_ir.name}"
            pass_dir.mkdir(parents=True, exist_ok=True)
            if not pass_ir.draw:
                artifacts.append({"technique": technique.name, "pass": pass_ir.name, "kind": "cleanup", "draw": False})
                continue
            stage_records = []
            fixed_ir = None
            if pass_ir.vertex_shader or pass_ir.pixel_shader:
                if pass_ir.vertex_shader is None or pass_ir.pixel_shader is None:
                    raise FxError("SHD_INCOMPLETE_PROGRAMMABLE_PASS", "draw pass requires both vertex and pixel shaders", resolved.root["path"], pass_ir.line, 1)
                unresolved = []
                for stage, assignment in (("vertex", pass_ir.vertex_shader),
                                          ("pixel", pass_ir.pixel_shader)):
                    if assignment.profile is None:
                        source = shader_object_defaults.get((assignment.entry or "").casefold(), "")
                        kind = ("legacy_shader_assembly" if source.casefold().startswith("asm")
                                else "unresolved_shader_object")
                        unresolved.append({"stage": stage, "kind": kind,
                                           "symbol": assignment.entry})
                if unresolved:
                    record = {
                        "technique": technique.name,
                        "pass": pass_ir.name,
                        "kind": "unsupported_programmable_pass",
                        "draw": True,
                        "unsupported_constructs": unresolved,
                    }
                    artifacts.append(record)
                    unsupported_constructs.extend({
                        "technique": technique.name, "pass": pass_ir.name, **item
                    } for item in unresolved)
                    continue
                stage_sources = [
                    ("vert", pass_ir.vertex_shader.entry,
                     prune_unreachable_functions(lowered_programmable, [pass_ir.vertex_shader.entry])),
                    ("frag", pass_ir.pixel_shader.entry,
                     prune_unreachable_functions(lowered_programmable, [pass_ir.pixel_shader.entry])),
                ]
                kind = "source_programmable"
            else:
                fixed_vs, fixed_ps, fixed_ir = lower_fixed_function(pass_ir)
                stage_sources = [("vert", "eawr_ffp_vs", fixed_vs), ("frag", "eawr_ffp_ps", fixed_ps)]
                kind = "fixed_function_lowering"
            for stage, entry, source in stage_sources:
                if not entry:
                    raise FxError("SHD_ENTRY_POINT_MISSING", f"missing {stage} entry point", resolved.root["path"], pass_ir.line, 1)
                source_name = f"{stage}.hlsl"
                source_path = pass_dir / source_name
                _write_text_lf(source_path, source)
                record = compile_stage(glslang, spirv_val, pass_dir, source_name, stage, entry)
                record.update({"stage": stage, "entry_point": entry, "source": source_name,
                               "source_sha256": sha256_file(source_path)})
                stage_records.append(record)
            artifacts.append({"technique": technique.name, "pass": pass_ir.name, "kind": kind,
                              "draw": True, "fixed_function": fixed_ir, "stages": stage_records})

    resources = []
    for binding, sampler in enumerate(effect.samplers):
        resources.append({"binding": binding, "sampler": sampler.name, "sampler_type": sampler.type,
                          "source_register": sampler.register, "texture": sampler.texture,
                          "states": dict(sorted(sampler.states.items(), key=lambda item: item[0].casefold()))})
    manifest = {
        "schema_version": 1,
        "translator": {
            "name": "eawr-shader-spike",
            "source_sha256": {
                "fx_parser.py": sha256_file(Path(__file__).with_name("fx_parser.py")),
                "toolchain.json": sha256_file(Path(__file__).with_name("toolchain.json")),
                "translate.py": sha256_file(Path(__file__)),
            },
            "version": TOOL_VERSION,
        },
        "source": {"archive_sha256": archive_hash, "root": resolved.root, "includes": resolved.includes,
                   "preprocessor_definitions": resolved.definitions, "preprocess": preprocess_record},
        "effect": logical,
        "parameters": [parameter_dict(item) for item in effect.parameters],
        "resources": resources,
        "structs": {name: [parameter_dict(field) for field in fields] for name, fields in sorted(effect.structs.items())},
        "runtime_compatibility_order": requested_order,
        "runtime_compatibility_requirements": COMPATIBILITY.get(key),
        "translation_selection": {
            "preferred_compatible_candidate": requested_order[0] if requested_order else None,
            "compiled_runtime_candidates": requested_order,
            "excluded_techniques": [
                {"name": item.name,
                 "reason": ("editor_maximum_viewport" if is_editor_technique(item)
                            else "not_in_approved_runtime_order")}
                for item in effect.techniques
                if item.name.casefold() not in {name.casefold() for name in requested_order}
            ],
            "renderer_selected": False,
        },
        "techniques": [_technique_dict(item) for item in effect.techniques],
        "artifacts": artifacts,
        "unsupported_constructs": unsupported_constructs,
        "toolchain": {
            "glslang": {"version": glslang_version, "flags": ["-D", "-V", "--target-env", "vulkan1.1", "--hlsl-dx9-compatible", "--auto-map-bindings", "--auto-map-locations"]},
            "spirv_tools": {"version": spirv_version, "flags": ["--target-env", "vulkan1.1"]},
        },
        "renderer_ingestion": {"wgpu": "not_tested", "godot": "not_tested"},
    }
    payload = dict(manifest)
    manifest["manifest_id"] = hashlib.sha256(canonical_json(payload)).hexdigest()
    (output / "manifest.json").write_bytes(canonical_json(manifest))
    return manifest


def _find_tool(explicit: Optional[Path], toolchain_root: Optional[Path], relative: str,
               fallback: str, kind: Optional[str] = None) -> Path:
    """Select a tool and enforce its pinned --version and SHA-256 identity."""
    if explicit is not None:
        candidate = explicit.resolve()
    elif toolchain_root is not None:
        candidate = (toolchain_root / relative).resolve()
    else:
        found = shutil.which(fallback)
        if not found:
            raise ShaderPolicyError(
                "SHD_TOOL_NOT_FOUND",
                f"{fallback} was not found; pass an explicit path or --toolchain-root",
            )
        candidate = Path(found).resolve()
    identity = kind or ("spirv_tools" if "spirv-val" in fallback else "glslang")
    _validate_tool_identity(candidate, identity)
    return candidate


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True, help="explicit directory containing effect sources")
    parser.add_argument("--source-archive", type=Path, help="foc_shaders.zip; auto-discovered in a source-root ancestor")
    parser.add_argument("--effect", required=True, help="one bounded effect name/path")
    parser.add_argument("--out", type=Path, required=True, help="explicit local generated-artifact directory")
    parser.add_argument("--toolchain-root", type=Path, help="directory containing pinned bin tools")
    parser.add_argument("--glslang", type=Path, help="explicit pinned glslang executable")
    parser.add_argument("--spirv-val", type=Path, help="explicit pinned SPIRV-Tools validator")
    parser.add_argument("--allow-unpinned-synthetic", action="store_true", help="permit an original synthetic fixture outside the bounded corpus")
    parser.add_argument("--pinned-public-corpus", action="store_true",
                        help="permit any effect from the archive after verifying the public archive hash")
    args = parser.parse_args(argv)
    try:
        output = _resolve_output_path(args.out)
        glslang = _find_tool(args.glslang, args.toolchain_root, "bin/glslang.exe", "glslang", "glslang")
        spirv_val = _find_tool(args.spirv_val, args.toolchain_root, "bin/spirv-val.exe", "spirv-val", "spirv_tools")
        translate(args.source_root, args.effect, output, glslang, spirv_val,
                  args.source_archive, args.allow_unpinned_synthetic,
                  args.pinned_public_corpus)
    except FxError as error:
        print(str(error), file=sys.stderr)
        return 2
    except ShaderPolicyError as error:
        print(str(error), file=sys.stderr)
        return 2
    except (OSError, ValueError, RuntimeError) as error:
        print(f"SHD_TRANSLATION_FAILED: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
