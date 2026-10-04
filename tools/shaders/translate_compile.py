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


from tools.shaders.translate_state import (
    COMPATIBILITY,
    EFFECTS,
    is_editor_technique,
    lower_fixed_function,
    normalize_legacy_hlsl,
    render_state_descriptor,
)
from tools.shaders.translate_toolchain import (
    ARCHIVE_SHA256,
    TOOL_VERSION,
    _resolve_output_path,
    _validate_tool_identity,
    _write_text_lf,
    canonical_json,
    sha256_file,
)

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
                "fx_tokens.py": sha256_file(Path(__file__).with_name("fx_tokens.py")),
                "fx_include.py": sha256_file(Path(__file__).with_name("fx_include.py")),
                "fx_ir.py": sha256_file(Path(__file__).with_name("fx_ir.py")),
                "fx_declarations.py": sha256_file(Path(__file__).with_name("fx_declarations.py")),
                "toolchain.json": sha256_file(Path(__file__).with_name("toolchain.json")),
                "translate.py": sha256_file(Path(__file__).with_name("translate.py")),
                "translate_toolchain.py": sha256_file(Path(__file__).with_name("translate_toolchain.py")),
                "translate_state.py": sha256_file(Path(__file__).with_name("translate_state.py")),
                "translate_compile.py": sha256_file(Path(__file__).with_name("translate_compile.py")),
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
