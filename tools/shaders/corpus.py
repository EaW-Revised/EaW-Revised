#!/usr/bin/env python3
"""Translate and inventory the pinned public material-effect corpus.

Generated HLSL, logs and SPIR-V stay below ignored ``out/shaders``.  The checked-in
descriptor contains metadata and hashes only; it never embeds an original shader body
or generated stage bytecode.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Optional
import zipfile

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.shaders.fx_parser import FxError, IncludeResolver, parameter_dict, parse_effect
    from tools.shaders.translate import (
        ARCHIVE_SHA256, REPOSITORY_ROOT, TOOL_VERSION, _find_tool,
        _resolve_output_path, _technique_dict, canonical_json,
        resolve_compiled_shader_symbols, sha256_file, translate,
    )
else:
    from .fx_parser import FxError, IncludeResolver, parameter_dict, parse_effect
    from .translate import (
        ARCHIVE_SHA256, REPOSITORY_ROOT, TOOL_VERSION, _find_tool,
        _resolve_output_path, _technique_dict, canonical_json,
        resolve_compiled_shader_symbols, sha256_file, translate,
    )


SCHEMA_VERSION = 1
CORPUS_VERSION = "1.0.0"


def _spec(path: str, family: str, pipeline: str, role: str = "material",
          skip_reason: Optional[str] = None) -> dict:
    return {"path": path, "family": family, "pipeline": pipeline,
            "role": role, "skip_reason": skip_reason}


MATERIAL_EFFECTS = (
    _spec("BatchMeshAlpha.fx", "BATCHMESH", "instanced_mesh"),
    _spec("BatchMeshGloss.fx", "BATCHMESH", "instanced_mesh"),
    _spec("MeshAdditive.fx", "MESH", "rigid_mesh"),
    _spec("MeshAdditiveOffset.fx", "MESH", "rigid_mesh"),
    _spec("MeshAdditiveReflection.fx", "MESH", "rigid_mesh"),
    _spec("MeshAdditiveVColor.fx", "MESH", "rigid_mesh"),
    _spec("MeshAlpha.fx", "MESH", "rigid_mesh"),
    _spec("MeshAlphaGloss.fx", "MESH", "rigid_mesh"),
    _spec("MeshAlphaScroll.fx", "MESH", "rigid_mesh"),
    _spec("MeshBumpColorize.fx", "MESH", "rigid_mesh"),
    _spec("MeshBumpReflectColorize.fx", "MESH", "rigid_mesh"),
    _spec("MeshCollision.fx", "MESH", "debug_visualization", "debug",
          "collision-geometry visualization is an editor/debug pipeline, not a runtime material"),
    _spec("MeshGloss.fx", "MESH", "rigid_mesh"),
    _spec("MeshGlossColorize.fx", "MESH", "rigid_mesh"),
    _spec("MeshHeat.fx", "MESH", "rigid_mesh"),
    _spec("MeshLightVisualize.fx", "MESH", "debug_visualization", "debug",
          "light visualization is an editor/debug pipeline, not a runtime material"),
    _spec("MeshOccludedUnit.fx", "MESH", "rigid_mesh"),
    _spec("MeshShadowVolume.fx", "MESH", "shadow_volume", "engine_pipeline",
          "shadow-volume extrusion is replaced by the renderer shadow pipeline"),
    _spec("MeshShield.fx", "MESH", "shield"),
    _spec("MeshSolidColor.fx", "MESH", "rigid_mesh"),
    _spec("Nebula.fx", "NEBULA", "nebula"),
    _spec("Planet.fx", "PLANET", "planet"),
    _spec("RSkinAdditive.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinAdditiveVColor.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinAlpha.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinAlphaGloss.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinBumpColorize.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinBumpReflectColorize.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinGloss.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinGlossColorize.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinHeat.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinOccludedUnit.fx", "RSKIN", "skinned_mesh"),
    _spec("RSkinShadowVolume.fx", "RSKIN", "shadow_volume", "engine_pipeline",
          "skinned shadow-volume extrusion is replaced by the renderer shadow pipeline"),
    _spec("Skydome.fx", "SKYDOME", "skydome"),
    _spec("Terrain/SpaceFogOfWar.fx", "TERRAIN", "fog_of_war"),
    _spec("Terrain/TerrainClouds.fx", "TERRAIN", "terrain_overlay"),
    _spec("Terrain/TerrainFogOfWar.fx", "TERRAIN", "fog_of_war"),
    _spec("Terrain/TerrainIce.fx", "TERRAIN", "terrain_surface"),
    _spec("Terrain/TerrainLava.fx", "TERRAIN", "terrain_surface"),
    _spec("Terrain/TerrainPassability.fx", "TERRAIN", "debug_visualization", "debug",
          "passability visualization is an editor/debug overlay, not a runtime material"),
    _spec("Terrain/TerrainRenderBaked.fx", "TERRAIN", "terrain_surface"),
    _spec("Terrain/TerrainRenderBump.fx", "TERRAIN", "terrain_surface"),
    _spec("Terrain/TerrainRenderBumpDual.fx", "TERRAIN", "terrain_surface"),
    _spec("Terrain/TerrainWater.fx", "WATER", "terrain_water"),
    _spec("TerrainMeshBump.fx", "TERRAIN", "terrain_mesh"),
    _spec("TerrainMeshGloss.fx", "TERRAIN", "terrain_mesh"),
)


REMAKE_BYTECODE_ONLY = (
    "MESHADDITIVE2X", "MESHALPHAGIRDERS", "MESHBUMPCOLORIZEDETAIL",
    "MESHBUMPCOLORIZEVERTEX", "MESHBUMPREFLECTCOLORIZE1",
    "MESHBUMPSPECGLOWCOLORIZE", "MESHGLOSSFORSTARFIELDS",
    "MESHSHIELDENGINES", "MESHSHIELDFARSEER", "PLANET_DEATHSTAR",
    "SKYDOMEOLD",
)


PIPELINES = {
    "rigid_mesh": {"vertex_domain": "rigid", "draw_domain": "model",
                   "binding_contract": "descriptor_parameter_semantics"},
    "skinned_mesh": {"vertex_domain": "matrix_palette", "draw_domain": "model",
                     "binding_contract": "descriptor_parameter_semantics_and_skin_palette"},
    "instanced_mesh": {"vertex_domain": "batched_instance", "draw_domain": "model_batch",
                       "binding_contract": "descriptor_parameter_semantics_and_instance_data"},
    "planet": {"vertex_domain": "rigid", "draw_domain": "planet",
               "binding_contract": "descriptor_parameter_semantics"},
    "nebula": {"vertex_domain": "rigid", "draw_domain": "space_volume",
               "binding_contract": "descriptor_parameter_semantics"},
    "skydome": {"vertex_domain": "rigid", "draw_domain": "sky",
                "binding_contract": "descriptor_parameter_semantics"},
    "terrain_surface": {"vertex_domain": "terrain", "draw_domain": "terrain",
                        "binding_contract": "descriptor_parameter_semantics"},
    "terrain_mesh": {"vertex_domain": "rigid", "draw_domain": "terrain_model",
                     "binding_contract": "descriptor_parameter_semantics"},
    "terrain_overlay": {"vertex_domain": "terrain", "draw_domain": "terrain_overlay",
                        "binding_contract": "descriptor_parameter_semantics"},
    "terrain_water": {"vertex_domain": "terrain", "draw_domain": "water",
                      "binding_contract": "descriptor_parameter_semantics"},
    "fog_of_war": {"vertex_domain": "terrain_or_screen", "draw_domain": "fog_of_war",
                   "binding_contract": "descriptor_parameter_semantics"},
    "shield": {"vertex_domain": "rigid", "draw_domain": "shield",
               "binding_contract": "descriptor_parameter_semantics"},
    "shadow_volume": {"vertex_domain": "renderer_owned", "draw_domain": "shadow",
                      "binding_contract": "not_consumed_material"},
    "debug_visualization": {"vertex_domain": "debug", "draw_domain": "debug",
                            "binding_contract": "not_consumed_material"},
}


def classify_public_exclusion(path: str) -> tuple[str, str]:
    normalized = path.replace("\\", "/")
    name = Path(normalized).stem.casefold()
    lower = normalized.casefold()
    if lower.startswith("dev/"):
        return "developer_sample", "developer/tool sample is not a shipped runtime material family"
    if lower.startswith("scenecomposite/") or name.startswith("scene_"):
        return "post_process", "scene compositing is renderer-owned post-processing"
    if lower.startswith("engine/phase") or lower in {"engine/global.fx", "alamoengine.fx"}:
        return "engine_pipeline", "engine phase orchestration is replaced by the renderer pipeline"
    if lower.startswith("engine/frameeffect") or name.startswith("scene"):
        return "post_process", "frame/scene effect is renderer-owned post-processing"
    if lower.startswith("engine/stencildarken") or name == "blobstencilmasked":
        return "fixed_function_pipeline", "stencil darkening is replaced by renderer shadow/post-processing passes"
    if lower.startswith("engine/prim"):
        return "primitive_or_particle", "engine primitive/particle effects are owned by their dedicated pipeline"
    if name in {"fillratetest", "almissingshader", "terrainbrush"}:
        return "debug_or_ui", "diagnostic/editor effect is not a runtime material"
    if name in {"grass", "tree"}:
        return "vegetation", "vegetation material family is outside issue #23's enumerated corpus"
    return "non_material", "effect is outside issue #23's enumerated material families"


def verify_public_source_tree(source_root: Path, source_archive: Path) -> dict:
    """Prove that the extracted FX/FXH tree is byte-exact to the pinned archive."""
    with zipfile.ZipFile(source_archive) as archive:
        archived = {
            name[len("Shaders/"):]: archive.read(name)
            for name in archive.namelist()
            if name.startswith("Shaders/") and not name.endswith("/")
            and Path(name).suffix.casefold() in {".fx", ".fxh"}
        }
    actual = {
        item.relative_to(source_root).as_posix(): item.read_bytes()
        for item in source_root.rglob("*")
        if item.is_file() and item.suffix.casefold() in {".fx", ".fxh"}
    }
    if set(actual) != set(archived):
        missing = sorted(set(archived) - set(actual), key=str.casefold)
        extra = sorted(set(actual) - set(archived), key=str.casefold)
        raise ValueError(f"public source tree membership mismatch: missing={missing}, extra={extra}")
    mismatched = sorted(
        (path for path in archived if actual[path] != archived[path]), key=str.casefold
    )
    if mismatched:
        raise ValueError(f"public source tree content mismatch: {mismatched}")
    entries = [{"path": path, "sha256": hashlib.sha256(actual[path]).hexdigest()}
               for path in sorted(actual, key=str.casefold)]
    return {
        "status": "byte_exact_to_pinned_archive", "files": len(entries),
        "tree_id": hashlib.sha256(canonical_json(entries)).hexdigest(),
    }


def _effect_metadata(effect) -> dict:
    semantics = sorted({
        item.semantic for item in effect.parameters if item.semantic
    } | {
        item.semantic for fields in effect.structs.values() for item in fields if item.semantic
    }, key=str.casefold)
    return {
        "parameters": [parameter_dict(item) for item in effect.parameters
                       if item.type.casefold() not in {"vertexshader", "pixelshader"}],
        "semantics": semantics,
        "samplers": [{
            "name": item.name, "type": item.type, "texture": item.texture,
            "register": item.register,
            "states": dict(sorted(item.states.items(), key=lambda pair: pair[0].casefold())),
        } for item in effect.samplers],
        "structs": {name: [parameter_dict(field) for field in fields]
                    for name, fields in sorted(effect.structs.items())},
        "techniques": [_technique_dict(item) for item in effect.techniques],
    }


def inspect_effect(source_root: Path, logical_path: str) -> tuple[dict, dict]:
    resolved = IncludeResolver(source_root).resolve(logical_path)
    effect = parse_effect(resolved.text, logical_path)
    resolve_compiled_shader_symbols(effect)
    return resolved.root, _effect_metadata(effect)


def _stage_records(manifest: dict) -> list[dict]:
    records = []
    for artifact in manifest["artifacts"]:
        for stage in artifact.get("stages", []):
            records.append({
                "technique": artifact["technique"], "pass": artifact["pass"],
                "stage": stage["stage"], "entry_point": stage["entry_point"],
                "source_sha256": stage["source_sha256"],
                "spirv_sha256": stage["spirv_sha256"],
                "compile_exit_code": stage["exit_code"],
                "validation_exit_code": stage["validation"]["exit_code"],
            })
    return records


def descriptor_from_manifest(spec: dict, manifest: dict) -> dict:
    stages = _stage_records(manifest)
    unsupported = manifest.get("unsupported_constructs", [])
    parameters = [item for item in manifest["parameters"]
                  if item["type"].casefold() not in {"vertexshader", "pixelshader"}]
    return {
        "name": Path(spec["path"]).stem, "path": spec["path"],
        "family": spec["family"], "pipeline": spec["pipeline"], "role": spec["role"],
        "source": {"sha256": manifest["source"]["root"]["sha256"],
                   "includes": manifest["source"]["includes"]},
        "parameters": parameters,
        "semantics": sorted({
            item["semantic"] for item in parameters if item.get("semantic")
        } | {
            item["semantic"] for fields in manifest["structs"].values()
            for item in fields if item.get("semantic")
        }, key=str.casefold),
        "samplers": manifest["resources"], "structs": manifest["structs"],
        "techniques": manifest["techniques"],
        "runtime_techniques": manifest["runtime_compatibility_order"],
        "excluded_techniques": manifest["translation_selection"]["excluded_techniques"],
        "translation": {
            "status": "partial" if unsupported else "validated",
            "manifest_id": manifest["manifest_id"],
            "draw_passes": sum(1 for item in manifest["artifacts"] if item.get("draw")),
            "cleanup_passes": sum(1 for item in manifest["artifacts"] if not item.get("draw")),
            "validated_stages": len(stages),
            "stages": stages,
            "unsupported_constructs": unsupported,
        },
    }


def skipped_descriptor(spec: dict, source_root: Path) -> dict:
    root, metadata = inspect_effect(source_root, spec["path"])
    return {
        "name": Path(spec["path"]).stem, "path": spec["path"],
        "family": spec["family"], "pipeline": spec["pipeline"], "role": spec["role"],
        "source": {"sha256": root["sha256"]}, **metadata,
        "translation": {"status": "classified_not_translated",
                        "reason": spec["skip_reason"], "validated_stages": 0,
                        "unsupported_constructs": []},
    }


def failed_descriptor(spec: dict, source_root: Path, error: Exception) -> dict:
    root, metadata = inspect_effect(source_root, spec["path"])
    code = getattr(error, "code", "SHD_TRANSLATION_FAILED")
    return {
        "name": Path(spec["path"]).stem, "path": spec["path"],
        "family": spec["family"], "pipeline": spec["pipeline"], "role": spec["role"],
        "source": {"sha256": root["sha256"]}, **metadata,
        "translation": {"status": "failed", "cause": code,
                        "follow_up": "inspect the ignored per-effect compiler log and add a synthetic regression",
                        "validated_stages": 0, "unsupported_constructs": []},
    }


# Keys that copy the published source's expressions or positions (parameter default values,
# declaration line numbers). The committed bundle keeps only the interface (names, types,
# semantics, registers); the full per-effect manifests stay in the ignored output tree.
SOURCE_ONLY_KEYS = frozenset({"default", "line"})


def without_source_positions(value: object) -> object:
    if isinstance(value, dict):
        return {key: without_source_positions(item) for key, item in value.items()
                if key not in SOURCE_ONLY_KEYS}
    if isinstance(value, list):
        return [without_source_positions(item) for item in value]
    return value


def build_bundle(source_root: Path, source_archive: Path, output: Path,
                 glslang: Path, spirv_val: Path) -> dict:
    if sha256_file(source_archive) != ARCHIVE_SHA256:
        raise ValueError("public shader archive SHA-256 does not match the pinned corpus")
    source_tree = verify_public_source_tree(source_root, source_archive)
    effects = []
    first_manifest = None
    for spec in MATERIAL_EFFECTS:
        if spec["skip_reason"]:
            effects.append(skipped_descriptor(spec, source_root))
            continue
        effect_output = output / Path(spec["path"]).stem
        try:
            manifest = translate(
                source_root, spec["path"], effect_output, glslang, spirv_val,
                source_archive=source_archive, allow_pinned_public_corpus=True,
            )
            first_manifest = first_manifest or manifest
            effects.append(descriptor_from_manifest(spec, manifest))
        except (FxError, OSError, RuntimeError, ValueError) as error:
            effects.append(failed_descriptor(spec, source_root, error))
    effects = [without_source_positions(item) for item in effects]

    target_paths = {item["path"].casefold() for item in MATERIAL_EFFECTS}
    public_paths = sorted(
        (item.relative_to(source_root).as_posix() for item in source_root.rglob("*.fx")),
        key=str.casefold,
    )
    exclusions = []
    for path in public_paths:
        if path.casefold() in target_paths:
            continue
        classification, reason = classify_public_exclusion(path)
        exclusions.append({"path": path, "classification": classification, "reason": reason})

    counts = {
        "public_fx": len(public_paths), "requested_family": len(MATERIAL_EFFECTS),
        "validated": sum(item["translation"]["status"] == "validated" for item in effects),
        "partial": sum(item["translation"]["status"] == "partial" for item in effects),
        "failed": sum(item["translation"]["status"] == "failed" for item in effects),
        "classified_not_translated": sum(
            item["translation"]["status"] == "classified_not_translated" for item in effects
        ),
        "public_excluded_other_family": len(exclusions),
        "remake_bytecode_only_deferred": len(REMAKE_BYTECODE_ONLY),
        "validated_stages": sum(item["translation"]["validated_stages"] for item in effects),
    }
    bundle = {
        "schema_version": SCHEMA_VERSION, "corpus_version": CORPUS_VERSION,
        "archive_sha256": ARCHIVE_SHA256,
        "source_tree_verification": source_tree,
        "translator": {"name": "eawr-shader-corpus", "version": TOOL_VERSION},
        "backend_contract": "backend_neutral_spirv_and_material_descriptors",
        "renderer_ingestion": "not_claimed_renderingserver_requires_reviewed_adaptation",
        "counts": counts, "pipelines": PIPELINES, "effects": effects,
        "public_source_exclusions": exclusions,
        "remake_bytecode_only": [{
            "effect": name, "status": "phase_5_deferred", "cause": "mod_specific_fxo_without_public_source"
        } for name in REMAKE_BYTECODE_ONLY],
        "toolchain": first_manifest["toolchain"] if first_manifest else None,
    }
    bundle["bundle_id"] = hashlib.sha256(canonical_json(bundle)).hexdigest()
    return bundle


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True,
                        help="ignored generated-artifact root below out/shaders")
    parser.add_argument("--descriptor", type=Path,
                        default=REPOSITORY_ROOT / "plan" / "inventories" / "shader-corpus.json")
    parser.add_argument("--toolchain-root", type=Path)
    parser.add_argument("--glslang", type=Path)
    parser.add_argument("--spirv-val", type=Path)
    args = parser.parse_args(argv)
    try:
        output = _resolve_output_path(args.out)
        glslang = _find_tool(args.glslang, args.toolchain_root, "bin/glslang.exe", "glslang", "glslang")
        spirv_val = _find_tool(args.spirv_val, args.toolchain_root, "bin/spirv-val.exe", "spirv-val", "spirv_tools")
        descriptor = args.descriptor.resolve()
        if descriptor != (REPOSITORY_ROOT / "plan" / "inventories" / "shader-corpus.json").resolve():
            raise ValueError("descriptor must be plan/inventories/shader-corpus.json")
        bundle = build_bundle(args.source_root.resolve(), args.source_archive.resolve(),
                              output, glslang, spirv_val)
        descriptor.write_bytes(canonical_json(bundle))
        print(json.dumps({"bundle_id": bundle["bundle_id"], "counts": bundle["counts"]},
                         sort_keys=True))
        return 2 if bundle["counts"]["failed"] else 0
    except (OSError, RuntimeError, ValueError) as error:
        message = re.sub(r"[A-Za-z]:\\[^\r\n]+", "<local-path>", str(error))
        print(f"SHD_CORPUS_FAILED: {message}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
