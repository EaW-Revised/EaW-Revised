#!/usr/bin/env python3
"""Opt-in semantic translation of TerrainWater technique ``t1`` (vs_1_1 + ps_1_1 asm).

This path is separate from ``translate.py``/``corpus.py`` on purpose: the accepted
descriptor bundle keeps reporting ``t1`` as ``legacy_shader_assembly`` and ``t0``/``t2``
are untouched.  Running it produces, below ignored ``out/shaders``:

* the vertex stage: the published ``vs_1_1`` entry compiled unchanged, wrapped by an
  entry that applies the vs_1_1 colour-output clamp (oD0/oD1 saturate to [0, 1]
  before interpolation) and assigns fixed inter-stage locations;
* the pixel stage: the pass's ps.1.1 program lowered by ``ps11.py``;
* a manifest with pass/cleanup state and every external state the effect does not
  own.  ``bind_external_state`` turns a caller's declaration of that state into
  shader constants and fails closed with a named ``EAWR-SHD-T1-*`` diagnostic for any
  state that is missing, contradictory or outside the verified semantics.

It never selects ``t1`` for a map and makes no RenderingServer/Godot claim: the
outputs are SPIR-V stages plus a binding contract.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from collections.abc import Mapping
from pathlib import Path
import re
import struct
import sys
from typing import Optional

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.shaders import ps11
    from tools.shaders.fx_parser import (FxError, IncludeResolver, parse_effect,
                                         prune_unreachable_functions,
                                         source_without_effect_wrappers)
    from tools.shaders.translate import (ARCHIVE_SHA256, ShaderPolicyError, _archive_path,
                                         _find_tool, _resolve_output_path,
                                         _validate_tool_identity, _write_text_lf,
                                         canonical_json, compile_stage,
                                         normalize_legacy_hlsl, preprocess_effect,
                                         render_state_descriptor,
                                         resolve_compiled_shader_symbols, sha256_file)
else:
    from . import ps11
    from .fx_parser import (FxError, IncludeResolver, parse_effect,
                            prune_unreachable_functions, source_without_effect_wrappers)
    from .translate import (ARCHIVE_SHA256, ShaderPolicyError, _archive_path, _find_tool,
                            _resolve_output_path, _validate_tool_identity,
                            _write_text_lf, canonical_json, compile_stage,
                            normalize_legacy_hlsl, preprocess_effect,
                            render_state_descriptor, resolve_compiled_shader_symbols,
                            sha256_file)


T1_VERSION = "1.0.0"
CONTRACT = {
    "path": "Terrain/TerrainWater.fx",
    "sha256": "7b3d060cdeb7fe7fde7f158cecd9ccc6d6cbbbd05e1e942615f928e2cd79bc35",
    "closure": [("AlamoEngine.fxh", "7a1ce5e065726652264cfc6cf17d8e45498c63af703daab807c257183342038e")],
    "technique": "t1",
}
VS_ENTRY = "eawr_t1_vs_main"
PS_ENTRY = "eawr_t1_ps_main"

SIGNED_BUMP_FORMATS = {"V8U8", "Q8W8V8U8", "V16U16", "RG8_SNORM", "RGBA8_SNORM", "RG16_SNORM"}
UNSIGNED_FORMATS = {"A8R8G8B8": "stored", "X8R8G8B8": "one", "RGBA8_UNORM": "stored",
                    "BGRA8_UNORM": "stored", "R5G6B5": "one", "DXT1": "stored", "DXT3": "stored",
                    "DXT5": "stored"}
FILTERS = {"point", "linear"}
ADDRESSES = {"wrap", "clamp"}
CONVENTIONS = {"documented", "transposed"}
FLOAT32_MAX = 3.4028234663852886e38


class T1Error(ValueError):
    """A named, fail-closed diagnostic for the t1 path."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code


# ---------------------------------------------------------------------------
# Source-derived program construction (private inputs, private outputs).
# ---------------------------------------------------------------------------

def _verify_source(resolved) -> None:
    if resolved.root["sha256"] != CONTRACT["sha256"]:
        raise T1Error("EAWR-SHD-T1-SOURCE-HASH", f"expected {CONTRACT['sha256']}, got {resolved.root['sha256']}")
    actual = [(item["path"].split("/")[-1].casefold(), item["sha256"]) for item in resolved.includes]
    expected = [(name.casefold(), digest) for name, digest in CONTRACT["closure"]]
    if actual != expected:
        raise T1Error("EAWR-SHD-T1-SOURCE-HASH", f"include closure mismatch: {actual}")


def _stage_textures(states: dict) -> dict[int, str]:
    textures = {}
    for key, value in states.items():
        match = re.fullmatch(r"\s*Texture\s*\[\s*(\d+)\s*\]\s*", key, re.IGNORECASE)
        if match:
            textures[int(match.group(1))] = value.strip().strip("()").strip()
    return textures


def _address_states(states: dict) -> dict[int, dict[str, str]]:
    result: dict[int, dict[str, str]] = {}
    for key, value in states.items():
        match = re.fullmatch(r"\s*Address([UVW])\s*\[\s*(\d+)\s*\]\s*", key, re.IGNORECASE)
        if match:
            result.setdefault(int(match.group(2)), {})[f"address_{match.group(1).lower()}"] = value.strip().casefold()
    return result


def _t1_render_state(draw) -> dict:
    """Keep effect-owned render states; external requirements own absent states."""
    state = render_state_descriptor(draw)
    for key in ("cull_mode", "alpha_test_enable", "fog_enable"):
        if state[key]["source"] == "renderer_baseline":
            del state[key]
    return state


def _signature(source: str, entry: str) -> tuple[str, str]:
    match = re.search(rf"\b(\w+)\s+{re.escape(entry)}\s*\(\s*(\w+)\s+\w+\s*\)", source)
    if not match:
        raise T1Error("EAWR-SHD-T1-VS-SIGNATURE", f"cannot resolve {entry}(struct) signature")
    return match.group(1), match.group(2)


def interface_location(semantic: str) -> Optional[int]:
    match = re.fullmatch(r"(TEXCOORD|COLOR)(\d*)", semantic.upper())
    if not match:
        return None
    index = int(match.group(2) or 0)
    return index if match.group(1) == "TEXCOORD" else ps11.COLOR_LOCATION_BASE + index


FOG_LOCATION = 12


def vertex_wrapper(output_struct: str, input_struct: str, fields: list, entry: str) -> str:
    """Return the vs_1_1 output-register wrapper appended to the pruned source."""
    declarations, copies = [], []
    for item in fields:
        semantic = (item.semantic or "").upper()
        if semantic == "POSITION":
            declarations.append(f"    float4 {item.name} : SV_POSITION;")
            copies.append(f"    r.{item.name} = o.{item.name};")
        elif semantic == "FOG":
            declarations.append(f"    [[vk::location({FOG_LOCATION})]] {item.type} {item.name} : FOG;")
            copies.append(f"    r.{item.name} = o.{item.name};")
        else:
            location = interface_location(semantic)
            if location is None:
                raise T1Error("EAWR-SHD-T1-VS-SIGNATURE", f"unmapped vertex output semantic {semantic}")
            declarations.append(f"    [[vk::location({location})]] {item.type} {item.name} : {semantic};")
            # vs_1_1 oD0/oD1 are clamped to [0, 1] per vertex, before interpolation.
            value = f"saturate(o.{item.name})" if semantic.startswith("COLOR") else f"o.{item.name}"
            copies.append(f"    r.{item.name} = {value};")
    return "\n".join([
        "", "// EAWR t1 wrapper: vs_1_1 output-register semantics and fixed locations.",
        "struct EAWR_T1_VS_OUTPUT", "{", *declarations, "};",
        f"EAWR_T1_VS_OUTPUT {VS_ENTRY}({input_struct} In)", "{",
        f"    {output_struct} o = {entry}(In);", "    EAWR_T1_VS_OUTPUT r;", *copies,
        "    return r;", "}", ""])


def build(source_root: Path, output: Path, glslang: Path, spirv_val: Path,
          source_archive: Optional[Path] = None) -> dict:
    output = _resolve_output_path(output)
    glslang_version = _validate_tool_identity(glslang, "glslang")
    spirv_version = _validate_tool_identity(spirv_val, "spirv_tools")
    archive = _archive_path(source_root, source_archive)
    archive_hash = sha256_file(archive)
    if archive_hash != ARCHIVE_SHA256:
        raise T1Error("EAWR-SHD-T1-SOURCE-HASH", f"archive hash mismatch: {archive_hash}")
    resolved = IncludeResolver(source_root).resolve(CONTRACT["path"])
    _verify_source(resolved)
    output.mkdir(parents=True, exist_ok=True)
    preprocessed, preprocess_record = preprocess_effect(glslang, output, resolved.text)
    effect = parse_effect(preprocessed, resolved.root["path"])
    resolve_compiled_shader_symbols(effect)
    technique = next((item for item in effect.techniques
                      if item.name.casefold() == CONTRACT["technique"]), None)
    if technique is None:
        raise T1Error("EAWR-SHD-T1-SHAPE", "technique t1 missing")
    draws = [item for item in technique.passes if item.draw]
    cleanups = [item for item in technique.passes if not item.draw]
    if len(draws) != 1 or len(cleanups) != 1 or technique.passes[-1] is not cleanups[0]:
        raise T1Error("EAWR-SHD-T1-SHAPE", "expected one draw pass followed by one cleanup pass")
    draw, cleanup = draws[0], cleanups[0]
    vs, ps = draw.vertex_shader, draw.pixel_shader
    if vs is None or vs.profile != "vs_1_1" or ps is None or ps.profile is not None:
        raise T1Error("EAWR-SHD-T1-SHAPE", "expected compiled vs_1_1 and a pixel-shader asm object")
    asm = next((item.default or "" for item in effect.parameters
                if item.type.casefold() == "pixelshader" and item.name.casefold() == (ps.entry or "").casefold()), "")
    if not asm.lstrip().casefold().startswith("asm"):
        raise T1Error("EAWR-SHD-T1-SHAPE", "pixel shader object is not an asm block")
    program = ps11.parse(asm)

    textures = _stage_textures(draw.states)
    for stage in program.sampled_stages:
        if stage not in textures:
            raise T1Error("EAWR-SHD-T1-SHAPE", f"stage {stage} is sampled but the pass binds no texture")
    for stage in program.bump_stages.values():
        if stage not in textures:
            raise T1Error("EAWR-SHD-T1-SHAPE", f"bump source stage {stage} has no texture")

    # Declared value ranges: the texbem source is signed by requirement; every other
    # sampled stage must be bound with an unsigned format (checked at binding time).
    stage_ranges = {stage: ((-1.0, 1.0) if stage in program.bump_stages.values() else (0.0, 1.0))
                    for stage in program.sampled_stages}
    escapes = ps11.analyse_ranges(program, stage_ranges)

    lowered = normalize_legacy_hlsl(source_without_effect_wrappers(preprocessed, effect))
    output_struct, input_struct = _signature(lowered, vs.entry)
    fields = effect.structs.get(output_struct)
    if not fields:
        raise T1Error("EAWR-SHD-T1-VS-SIGNATURE", f"output struct {output_struct} not parsed")
    vertex_source = (prune_unreachable_functions(lowered, [vs.entry])
                     + vertex_wrapper(output_struct, input_struct, fields, vs.entry))
    consumed = {f"TEXCOORD{stage}" for stage in program.sampled_stages} | {"COLOR0", "COLOR1"}
    produced = {(item.semantic or "").upper() for item in fields}
    missing = sorted(consumed - produced)
    if missing:
        raise T1Error("EAWR-SHD-T1-INTERFACE", f"pixel inputs not produced by the vertex stage: {missing}")
    pixel_source = ps11.lower_to_hlsl(program, PS_ENTRY, escapes)

    stages = []
    for stage, entry, source in (("vert", VS_ENTRY, vertex_source), ("frag", PS_ENTRY, pixel_source)):
        name = f"{stage}.hlsl"
        _write_text_lf(output / name, source)
        record = compile_stage(glslang, spirv_val, output, name, stage, entry)
        record.update({"stage": stage, "entry_point": entry, "source": name,
                       "source_sha256": sha256_file(output / name)})
        stages.append(record)

    addresses = _address_states(draw.states)
    restored = _address_states(cleanup.states)
    changed_keys = {key for key in draw.states if not re.match(r"\s*Texture\s*\[", key, re.IGNORECASE)}
    render_state = _t1_render_state(draw)
    manifest = {
        "schema_version": 1,
        "translator": {"name": "eawr-terrainwater-t1", "version": T1_VERSION,
                       "source_sha256": {name: sha256_file(Path(__file__).with_name(name))
                                         for name in ("terrainwater_t1.py", "ps11.py", "translate.py",
                                                      "fx_parser.py", "toolchain.json")}},
        "source": {"archive_sha256": archive_hash, "root": resolved.root, "includes": resolved.includes,
                   "preprocess": preprocess_record},
        "effect": CONTRACT["path"],
        "technique": technique.name,
        "annotations": dict(technique.annotations),
        "pass": draw.name,
        "vertex": {"source_entry": vs.entry, "source_profile": vs.profile, "entry": VS_ENTRY,
                   "output_struct": output_struct, "input_struct": input_struct,
                   "outputs": [{"name": item.name, "type": item.type, "semantic": item.semantic,
                                "location": (None if (item.semantic or "").upper() == "POSITION" else
                                             FOG_LOCATION if (item.semantic or "").upper() == "FOG" else
                                             interface_location(item.semantic or "")),
                                "vs_1_1_saturate": (item.semantic or "").upper().startswith("COLOR")}
                               for item in fields]},
        "pixel": {"entry": PS_ENTRY, "source_symbol": ps.entry, "program": ps11.program_record(program, escapes),
                  "stage_textures": {str(stage): textures[stage] for stage in sorted(textures)},
                  "declared_stage_ranges": {str(k): list(v) for k, v in stage_ranges.items()},
                  "bindings": {"cbuffer": ps11.BINDING_CBUFFER,
                               "textures": {str(s): ps11.texture_binding(s) for s in program.sampled_stages},
                               "samplers": {str(s): ps11.sampler_binding(s) for s in program.sampled_stages}}},
        "pass_state": {
            "render_state": render_state,
            "blend_alpha_rule": "resolved_from_external_separate_alpha_blend",
            "effect_sampler_state": {str(stage): value for stage, value in sorted(addresses.items())},
            "cleanup": {"pass": cleanup.name,
                        "restores": {str(stage): value for stage, value in sorted(restored.items())},
                        "unrestored_effect_states": sorted(
                            key for key in changed_keys
                            if key.strip().casefold() not in {k.strip().casefold() for k in cleanup.states}
                            and not re.match(r"\s*Address[UVW]\s*\[", key, re.IGNORECASE)),
                        "unrestored_texture_bindings": sorted(f"Texture[{stage}]" for stage in textures)},
        },
        "external_state": external_state_requirements(program, addresses, escapes),
        "stages": stages,
        "toolchain": {"glslang": glslang_version, "spirv_tools": spirv_version},
        "renderer_selected": False,
        "map_selection": "not_selected",
        "renderer_ingestion": {"godot": "not_tested", "rendering_server_material": False},
    }
    manifest["manifest_id"] = hashlib.sha256(canonical_json(manifest)).hexdigest()
    (output / "manifest.json").write_bytes(canonical_json(manifest))
    return manifest


def external_state_requirements(program: ps11.Program, effect_addresses: dict, escapes: list) -> list[dict]:
    """Every state the draw depends on that the effect does not set, with its diagnostic."""
    requirements = []
    requirements.append({"state": "binding usage", "code": "EAWR-SHD-T1-REAL-MAP-UNRESOLVED",
                         "synthetic_value": "synthetic", "real_map_status": "unresolved_for_real_map",
                         "why": "retail external state has not been established"})
    for state, code, pinned in (
        ("CullMode", "CULL-UNRESOLVED", "ccw"),
        ("AlphaTestEnable/Func/Ref", "ALPHA-TEST-UNRESOLVED", "disabled; func=always, ref=0 in synthetic contract"),
        ("SeparateAlphaBlendEnable", "SEPARATE-ALPHA-BLEND-UNRESOLVED", False),
        ("BlendOp", "BLENDOP-UNRESOLVED", "add"),
        ("SRGBWRITEENABLE", "SRGB-WRITE-UNRESOLVED", False),
        ("COLORWRITEENABLE", "COLOR-WRITE-UNRESOLVED", "rgba"),
        ("render target format", "RENDER-TARGET-FORMAT-UNRESOLVED", "A8R8G8B8"),
    ):
        requirements.append({"state": state, "code": "EAWR-SHD-T1-" + code,
                             "synthetic_value": pinned, "real_map_status": "unresolved_for_real_map",
                             "why": "the effect does not establish this pipeline state"})
    for stage, source in sorted(program.bump_stages.items()):
        requirements.append({"state": f"D3DTSS_BUMPENVMAT00..11[{stage}]", "code": "EAWR-SHD-T1-BUMPENV-UNRESOLVED",
                             "why": "texbem perturbation matrix; the effect never sets it"})
        requirements.append({"state": f"bump matrix index convention[{stage}]",
                             "code": "EAWR-SHD-T1-BUMPENV-CONVENTION-UNRESOLVED",
                             "why": "documented and observed native-driver off-diagonal conventions differ"})
        requirements.append({"state": f"texture format[{source}]", "code": "EAWR-SHD-T1-BUMP-FORMAT-UNRESOLVED",
                             "why": "texbem reads du/dv as signed; only signed formats are defined"})
        requirements.append({"state": f"mip levels[{stage}]", "code": "EAWR-SHD-T1-TEXBEM-LOD-UNRESOLVED",
                             "why": "LOD of a perturbed lookup is implementation-defined; single level required"})
    for stage in program.sampled_stages:
        requirements.append({"state": f"SRGBTEXTURE[{stage}]", "code": "EAWR-SHD-T1-SRGB-READ-UNRESOLVED",
                             "synthetic_value": False, "real_map_status": "unresolved_for_real_map",
                             "why": "the effect does not establish texture sRGB decoding"})
        owned = sorted(effect_addresses.get(stage, {}))
        requirements.append({"state": f"sampler/texture state[{stage}]", "code": "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED",
                             "why": "filters, non-projected texture transform and addresses not owned by the effect",
                             "effect_owned": owned})
        if stage not in program.bump_stages.values():
            requirements.append({"state": f"texture format[{stage}]", "code": "EAWR-SHD-T1-STAGE-FORMAT-UNRESOLVED",
                                 "why": "unsigned format and alpha source (stored or implicit 1) decide t.a"})
    requirements.append({"state": "FogEnable", "code": "EAWR-SHD-T1-FOG-UNRESOLVED",
                         "real_map_status": "unresolved_for_real_map",
                         "why": "the pass does not set fog; only fog disabled is supported"})
    if escapes:
        requirements.append({"state": "PixelShader1xMaxValue", "code": "EAWR-SHD-PS11-RANGE-UNRESOLVED",
                             "why": "intermediate values can leave [-1, 1]"})
    return requirements


# ---------------------------------------------------------------------------
# External-state binding (pure; no protected input).
# ---------------------------------------------------------------------------

def _require(condition: bool, code: str, message: str) -> None:
    if not condition:
        raise T1Error(code, message)


def _finite(value, code: str, name: str) -> float:
    _require(isinstance(value, (int, float)) and not isinstance(value, bool),
             code, f"{name} must be a finite number")
    try:
        number = float(value)
    except (OverflowError, ValueError):
        raise T1Error(code, f"{name} must be a finite number")
    _require(math.isfinite(number) and abs(number) <= FLOAT32_MAX, code,
             f"{name} must fit a finite shader float")
    return number


def _mapping(value, name: str, code: str = "EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE") -> Mapping:
    _require(isinstance(value, Mapping), code if value is None else "EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE",
             f"{name} must be a mapping")
    return value


def _keys(value: Mapping, allowed: set, name: str) -> None:
    unknown = [repr(key) for key in value if key not in allowed]
    _require(not unknown, "EAWR-SHD-T1-EXTERNAL-STATE-UNKNOWN-KEY",
             f"{name} has unknown key(s): {', '.join(unknown)}")


def _enum(value, allowed: set, name: str, missing_code: str) -> str:
    _require(value is not None, missing_code, f"{name} not declared")
    _require(isinstance(value, str), "EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE", f"{name} must be a string")
    _require(value in allowed, "EAWR-SHD-T1-EXTERNAL-STATE-UNKNOWN-ENUM", f"{name}: {value!r}")
    return value


def _pinned(value: Mapping, key: str, expected, code: str) -> None:
    _require(key in value and type(value[key]) is type(expected) and value[key] == expected,
             code, f"{key} must be explicitly declared as {expected!r} for the synthetic path")


def effective_bump_coefficients(m00: float, m01: float, m10: float, m11: float, convention: str) -> tuple:
    """(a, b, c, d) with du' = a*du + b*dv and dv' = c*du + d*dv."""
    if convention == "documented":
        return (m00, m10, m01, m11)
    if convention == "transposed":
        return (m00, m01, m10, m11)
    raise T1Error("EAWR-SHD-T1-BUMPENV-CONVENTION-UNRESOLVED", f"unknown convention {convention!r}")


def bind_external_state(manifest: dict, binding: dict) -> dict:
    """Validate a caller's external-state declaration and return shader constants.

    Nothing is defaulted: a missing key is an unresolved state and raises.
    """
    _require(isinstance(binding, Mapping), "EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE",
             "binding must be a mapping")
    _keys(binding, {"usage", "fog", "pipeline", "pixel_shader_1x_max_value", "bump_env", "stages"}, "binding")
    _require(binding.get("usage") == "synthetic", "EAWR-SHD-T1-REAL-MAP-UNRESOLVED",
             "only synthetic declarations are supported; retail external state remains unresolved")
    pipeline = _mapping(binding.get("pipeline"), "pipeline", "EAWR-SHD-T1-PIPELINE-STATE-UNRESOLVED")
    _keys(pipeline, {"cull_mode", "alpha_test", "separate_alpha_blend", "blend_op",
                     "srgb_write", "color_write_mask", "render_target_format"}, "pipeline")
    _pinned(pipeline, "cull_mode", "ccw", "EAWR-SHD-T1-CULL-UNRESOLVED")
    alpha_test = _mapping(pipeline.get("alpha_test"), "alpha_test", "EAWR-SHD-T1-ALPHA-TEST-UNRESOLVED")
    _keys(alpha_test, {"enabled", "func", "ref"}, "alpha_test")
    for key, expected in (("enabled", False), ("func", "always"), ("ref", 0)):
        _pinned(alpha_test, key, expected, "EAWR-SHD-T1-ALPHA-TEST-UNRESOLVED")
    _pinned(pipeline, "separate_alpha_blend", False, "EAWR-SHD-T1-SEPARATE-ALPHA-BLEND-UNRESOLVED")
    _pinned(pipeline, "blend_op", "add", "EAWR-SHD-T1-BLENDOP-UNRESOLVED")
    _pinned(pipeline, "srgb_write", False, "EAWR-SHD-T1-SRGB-WRITE-UNRESOLVED")
    _pinned(pipeline, "color_write_mask", "rgba", "EAWR-SHD-T1-COLOR-WRITE-UNRESOLVED")
    _pinned(pipeline, "render_target_format", "A8R8G8B8", "EAWR-SHD-T1-RENDER-TARGET-FORMAT-UNRESOLVED")

    program = manifest["pixel"]["program"]
    bump_stages = {int(k): v for k, v in program["bump_stages"].items()}
    sampled = program["sampled_stages"]
    effect_sampler = manifest["pass_state"]["effect_sampler_state"]

    fog = _mapping(binding.get("fog"), "fog", "EAWR-SHD-T1-FOG-UNRESOLVED")
    _keys(fog, {"enabled"}, "fog")
    _require("enabled" in fog, "EAWR-SHD-T1-FOG-UNRESOLVED", "fog state not declared")
    _require(fog["enabled"] is False, "EAWR-SHD-T1-FOG-UNSUPPORTED", "fixed-function fog after ps_1_1 is not lowered")

    bem = [[0.0, 0.0, 0.0, 0.0] for _ in range(4)]
    declared = _mapping(binding.get("bump_env"), "bump_env", "EAWR-SHD-T1-BUMPENV-UNRESOLVED")
    _keys(declared, {str(stage) for stage in bump_stages}, "bump_env")
    for stage in bump_stages:
        entry = declared.get(str(stage))
        entry = _mapping(entry, f"bump_env[{stage}]", "EAWR-SHD-T1-BUMPENV-UNRESOLVED")
        _keys(entry, {"m00", "m01", "m10", "m11", "convention", "provenance"}, f"bump_env[{stage}]")
        _require(all(k in entry for k in ("m00", "m01", "m10", "m11")),
                 "EAWR-SHD-T1-BUMPENV-UNRESOLVED", f"BUMPENVMAT00..11 for stage {stage} not declared")
        _require(isinstance(entry.get("provenance"), str) and entry["provenance"].strip() != "",
                 "EAWR-SHD-T1-BUMPENV-UNRESOLVED", f"stage {stage} matrix has no provenance")
        m = [_finite(entry[k], "EAWR-SHD-T1-BUMPENV-UNRESOLVED", k) for k in ("m00", "m01", "m10", "m11")]
        convention = entry.get("convention")
        _require(isinstance(convention, str) and convention in CONVENTIONS,
                 "EAWR-SHD-T1-BUMPENV-CONVENTION-UNRESOLVED",
                 f"stage {stage} index convention must be documented or transposed, even for a symmetric matrix")
        bem[stage] = list(effective_bump_coefficients(*m, convention))

    samplers = {}
    stages = _mapping(binding.get("stages"), "stages", "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED")
    _keys(stages, {str(stage) for stage in sampled}, "stages")
    for stage in sampled:
        entry = stages.get(str(stage))
        entry = _mapping(entry, f"stage {stage}", "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED")
        _keys(entry, {"format", "min_filter", "mag_filter", "mip_levels", "mip_filter",
                      "srgb_read", "texture_transform", "address_u", "address_v"}, f"stage {stage}")
        for key, allowed in (("min_filter", FILTERS), ("mag_filter", FILTERS)):
            _enum(entry.get(key), allowed, f"stage {stage} {key}", "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED")
        _require(entry.get("texture_transform") == "disable", "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED",
                 f"stage {stage} must declare a non-projected, untransformed coordinate (disable)")
        _pinned(entry, "srgb_read", False, "EAWR-SHD-T1-SRGB-READ-UNRESOLVED")
        address = {}
        for axis in ("u", "v"):
            owned = effect_sampler.get(str(stage), {}).get(f"address_{axis}")
            given = entry.get(f"address_{axis}")
            if owned is not None:
                _require(given is None or given == owned, "EAWR-SHD-T1-EFFECT-STATE-CONFLICT",
                         f"stage {stage} address_{axis} is set by the effect to {owned}")
                address[axis] = owned
            else:
                _enum(given, ADDRESSES, f"stage {stage} address_{axis}",
                      "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED")
                address[axis] = given
        levels = entry.get("mip_levels")
        _require(type(levels) is int, "EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE",
                 f"stage {stage} mip_levels must be an integer")
        _require(levels >= 1, "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED", f"stage {stage} mip_levels")
        mip_filter = _enum(entry.get("mip_filter"), {"none", "point", "linear"},
                           f"stage {stage} mip_filter", "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED")
        if stage in bump_stages:
            _require(levels == 1, "EAWR-SHD-T1-TEXBEM-LOD-UNRESOLVED", f"stage {stage} must be single-level")
        _require((levels == 1 and mip_filter == "none") or (levels > 1 and mip_filter != "none"),
                 "EAWR-SHD-T1-SAMPLER-STATE-UNRESOLVED", f"stage {stage} mip filter conflicts with mip levels")
        raw_fmt = entry.get("format")
        _require(raw_fmt is None or isinstance(raw_fmt, str), "EAWR-SHD-T1-EXTERNAL-STATE-WRONG-TYPE",
                 f"stage {stage} format must be a string")
        fmt = (raw_fmt or "").upper()
        if stage in bump_stages.values():
            _require(fmt != "", "EAWR-SHD-T1-BUMP-FORMAT-UNRESOLVED", f"stage {stage} format not declared")
            _require(fmt in SIGNED_BUMP_FORMATS, "EAWR-SHD-T1-BUMP-FORMAT-UNSIGNED",
                     f"stage {stage} format {fmt} is not a signed du/dv format")
            alpha = None
        else:
            _require(fmt in UNSIGNED_FORMATS, "EAWR-SHD-T1-STAGE-FORMAT-UNRESOLVED",
                     f"stage {stage} format {fmt or '<none>'} is not a declared unsigned format")
            alpha = UNSIGNED_FORMATS[fmt]
        samplers[str(stage)] = {"format": fmt, "alpha_source": alpha, "address_u": address["u"],
                                "address_v": address["v"], "min_filter": entry["min_filter"],
                                "mag_filter": entry["mag_filter"], "mip_levels": levels,
                                "mip_filter": mip_filter, "srgb_read": entry["srgb_read"]}

    max_value = 0.0
    if "pixel_shader_1x_max_value" in binding:
        max_value = _finite(binding["pixel_shader_1x_max_value"],
                            "EAWR-SHD-PS11-RANGE-UNRESOLVED", "pixel_shader_1x_max_value")
        _require(max_value > 0.0, "EAWR-SHD-PS11-RANGE-UNRESOLVED",
                 "pixel_shader_1x_max_value must be positive")
    if program["range_escapes"]:
        _require("pixel_shader_1x_max_value" in binding, "EAWR-SHD-PS11-RANGE-UNRESOLVED",
                 "pixel_shader_1x_max_value must be declared for escaping intermediates")
        _require(max_value >= 1.0, "EAWR-SHD-PS11-RANGE-UNRESOLVED", "ps_1_1 guarantees at least [-1, 1]")
    constants = [value for row in bem for value in row] + [max_value, 0.0, 0.0, 0.0]
    return {"bump_coefficients": {str(k): bem[k] for k in bump_stages}, "samplers": samplers,
            "usage": "synthetic", "pipeline": {**pipeline, "alpha_test": dict(alpha_test),
                                                "blend_applies_to_alpha": not pipeline["separate_alpha_blend"]},
            "pixel_shader_1x_max_value": max_value,
            "cbuffer": {"binding": manifest["pixel"]["bindings"]["cbuffer"],
                        "bytes": struct.pack("<20f", *constants).hex()}}


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-archive", type=Path)
    parser.add_argument("--out", type=Path, required=True, help="directory below repository out/shaders")
    parser.add_argument("--toolchain-root", type=Path)
    parser.add_argument("--glslang", type=Path)
    parser.add_argument("--spirv-val", type=Path)
    args = parser.parse_args(argv)
    try:
        output = _resolve_output_path(args.out)
        glslang = _find_tool(args.glslang, args.toolchain_root, "bin/glslang.exe", "glslang", "glslang")
        spirv_val = _find_tool(args.spirv_val, args.toolchain_root, "bin/spirv-val.exe", "spirv-val", "spirv_tools")
        manifest = build(args.source_root, output, glslang, spirv_val, args.source_archive)
    except (T1Error, ps11.Ps11Error, FxError, ShaderPolicyError) as error:
        print(str(error), file=sys.stderr)
        return 2
    except (OSError, RuntimeError, ValueError) as error:
        print(f"EAWR-SHD-T1-FAILED: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"manifest_id": manifest["manifest_id"],
                      "stages": {item["stage"]: item["spirv_sha256"] for item in manifest["stages"]}}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
