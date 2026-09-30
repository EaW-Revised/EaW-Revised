#!/usr/bin/env python3
"""Validate pinned capture metadata and compare decoded screenshot pixels."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import re
import sys
from typing import Any

from PIL import Image, ImageChops, ImageFilter

SHA256 = re.compile(r"[0-9a-f]{64}\Z")
IDENTITY_KEYS = {
    "vfs_profile", "scene", "inputs", "viewport", "camera", "simulation_tick",
    "animation_time_seconds", "particle_seed", "particle_time_seconds",
}
RUNTIME_KEYS = {
    "platform", "architecture", "engine", "engine_version", "backend", "graphics_api",
    "device", "driver", "driver_version", "build_revision",
}
METRIC_DIRECTIONS = {
    "rgb_mae": "max", "rgb_p95": "max", "alpha_mae": "max",
    "changed_pixel_ratio": "max", "edge_iou": "min",
}
POLICY_KEYS = {"schema_version", "policy_id", "profiles"}
APPROVAL_KEYS = {
    "schema_version", "approval_id", "contract_sha256", "policy_sha256",
    "policy_profile", "approved_before_candidate", "evidence", "rationale",
}


class HarnessError(ValueError):
    pass


class DuplicateJSONKeyError(ValueError):
    """Raised when a JSON object repeats a key instead of being canonical."""

    def __init__(self, key: str):
        self.key = key
        super().__init__(f"duplicate object key {key!r}")


def reject_duplicate_object_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build JSON objects without silently applying last-key-wins semantics."""

    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise DuplicateJSONKeyError(key)
        value[key] = item
    return value


def require_object(value: Any, keys: set[str], context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise HarnessError(f"{context}: expected an object")
    missing = sorted(keys - value.keys())
    extra = sorted(value.keys() - keys)
    if missing or extra:
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if extra:
            details.append("unsupported " + ", ".join(extra))
        raise HarnessError(f"{context}: " + "; ".join(details))
    return value


def require_text(value: Any, context: str, *, logical_path: bool = False) -> str:
    if not isinstance(value, str) or not value.strip():
        raise HarnessError(f"{context}: expected a non-empty string")
    if value.strip().lower() in {"unknown", "unspecified", "n/a", "none"}:
        raise HarnessError(f"{context}: provenance must be recorded, not {value!r}")
    if logical_path:
        normalized = value.replace("\\", "/")
        if normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
            raise HarnessError(f"{context}: expected a logical, not absolute, path")
        if ".." in normalized.split("/"):
            raise HarnessError(f"{context}: parent traversal is not allowed")
    return value


def require_hash(value: Any, context: str) -> str:
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        raise HarnessError(f"{context}: expected a lowercase SHA-256 digest")
    return value


def require_number(value: Any, context: str, minimum: float | None = None,
                   maximum: float | None = None) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise HarnessError(f"{context}: expected a finite number")
    number = float(value)
    if not math.isfinite(number):
        raise HarnessError(f"{context}: expected a finite number")
    if minimum is not None and number < minimum:
        raise HarnessError(f"{context}: must be at least {minimum}")
    if maximum is not None and number > maximum:
        raise HarnessError(f"{context}: must be at most {maximum}")
    return number


def require_integer(value: Any, context: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise HarnessError(f"{context}: expected an integer >= {minimum}")
    return value


def validate_identity(value: Any, context: str) -> dict[str, Any]:
    identity = require_object(value, IDENTITY_KEYS, context)
    require_text(identity["vfs_profile"], f"{context}.vfs_profile")
    scene = require_object(identity["scene"], {"logical_path", "sha256"}, f"{context}.scene")
    require_text(scene["logical_path"], f"{context}.scene.logical_path", logical_path=True)
    require_hash(scene["sha256"], f"{context}.scene.sha256")
    inputs = identity["inputs"]
    if not isinstance(inputs, list) or not inputs:
        raise HarnessError(f"{context}.inputs: expected at least one hashed input")
    logical_paths = []
    for index, item in enumerate(inputs):
        entry = require_object(item, {"logical_path", "sha256"}, f"{context}.inputs[{index}]")
        logical_paths.append(require_text(
            entry["logical_path"], f"{context}.inputs[{index}].logical_path", logical_path=True
        ))
        require_hash(entry["sha256"], f"{context}.inputs[{index}].sha256")
    if logical_paths != sorted(logical_paths) or len(set(logical_paths)) != len(logical_paths):
        raise HarnessError(f"{context}.inputs: logical paths must be unique and sorted")
    viewport = require_object(identity["viewport"], {"width", "height"}, f"{context}.viewport")
    require_integer(viewport["width"], f"{context}.viewport.width", 1)
    require_integer(viewport["height"], f"{context}.viewport.height", 1)
    camera = require_object(identity["camera"], {
        "projection", "position", "target", "up", "fov_degrees", "near", "far",
    }, f"{context}.camera")
    projection = require_text(camera["projection"], f"{context}.camera.projection")
    if projection not in {"perspective", "orthographic"}:
        raise HarnessError(f"{context}.camera.projection: unsupported projection")
    for vector_name in ("position", "target", "up"):
        vector = camera[vector_name]
        if not isinstance(vector, list) or len(vector) != 3:
            raise HarnessError(f"{context}.camera.{vector_name}: expected three numbers")
        for axis, component in enumerate(vector):
            require_number(component, f"{context}.camera.{vector_name}[{axis}]")
    require_number(camera["fov_degrees"], f"{context}.camera.fov_degrees", 0.0, 180.0)
    near = require_number(camera["near"], f"{context}.camera.near", 0.0)
    far = require_number(camera["far"], f"{context}.camera.far", 0.0)
    if near >= far:
        raise HarnessError(f"{context}.camera: near must be less than far")
    require_integer(identity["simulation_tick"], f"{context}.simulation_tick")
    require_number(identity["animation_time_seconds"], f"{context}.animation_time_seconds", 0.0)
    require_integer(identity["particle_seed"], f"{context}.particle_seed")
    require_number(identity["particle_time_seconds"], f"{context}.particle_time_seconds", 0.0)
    return identity


def validate_contract(value: Any) -> dict[str, Any]:
    contract = require_object(value, {"schema_version", "contract_id", "identity", "regions"}, "contract")
    if require_integer(contract["schema_version"], "contract.schema_version", 1) != 1:
        raise HarnessError("contract.schema_version: only version 1 is supported")
    require_text(contract["contract_id"], "contract.contract_id")
    identity = validate_identity(contract["identity"], "contract.identity")
    width, height = identity["viewport"]["width"], identity["viewport"]["height"]
    regions = contract["regions"]
    if not isinstance(regions, list) or not regions:
        raise HarnessError("contract.regions: expected at least one region")
    names: set[str] = set()
    full_frame_count = 0
    for index, region_value in enumerate(regions):
        context = f"contract.regions[{index}]"
        region = require_object(region_value, {
            "name", "rect", "pixel_channel_threshold", "edge_threshold", "threshold_rationale", "limits",
        }, context)
        name = require_text(region["name"], f"{context}.name")
        if name in names:
            raise HarnessError(f"{context}.name: duplicate region {name!r}")
        names.add(name)
        rect = region["rect"]
        if not isinstance(rect, list) or len(rect) != 4:
            raise HarnessError(f"{context}.rect: expected [x, y, width, height]")
        x = require_integer(rect[0], f"{context}.rect[0]")
        y = require_integer(rect[1], f"{context}.rect[1]")
        region_width = require_integer(rect[2], f"{context}.rect[2]", 1)
        region_height = require_integer(rect[3], f"{context}.rect[3]", 1)
        if x + region_width > width or y + region_height > height:
            raise HarnessError(f"{context}.rect: region falls outside the pinned viewport")
        if [x, y, region_width, region_height] == [0, 0, width, height]:
            full_frame_count += 1
        for threshold_name in ("pixel_channel_threshold", "edge_threshold"):
            threshold = require_integer(region[threshold_name], f"{context}.{threshold_name}")
            if threshold > 255:
                raise HarnessError(f"{context}.{threshold_name}: maximum is 255")
        require_text(region["threshold_rationale"], f"{context}.threshold_rationale")
        limits = require_object(region["limits"], set(METRIC_DIRECTIONS), f"{context}.limits")
        for metric, direction in METRIC_DIRECTIONS.items():
            limit_context = f"{context}.limits.{metric}"
            limit = require_object(limits[metric], {direction, "rationale"}, limit_context)
            bound = require_number(limit[direction], f"{limit_context}.{direction}", 0.0)
            if metric in {"rgb_mae", "rgb_p95", "alpha_mae"} and bound > 255.0:
                raise HarnessError(f"{limit_context}.{direction}: maximum is 255")
            if metric in {"changed_pixel_ratio", "edge_iou"} and bound > 1.0:
                raise HarnessError(f"{limit_context}.{direction}: maximum is 1")
            require_text(limit["rationale"], f"{limit_context}.rationale")
    if full_frame_count != 1:
        raise HarnessError("contract.regions: exactly one full-frame region is required")
    return contract


def validate_policy(value: Any) -> dict[str, Any]:
    policy = require_object(value, POLICY_KEYS, "policy")
    if require_integer(policy["schema_version"], "policy.schema_version", 1) != 1:
        raise HarnessError("policy.schema_version: only version 1 is supported")
    require_text(policy["policy_id"], "policy.policy_id")
    profiles = policy["profiles"]
    if not isinstance(profiles, list) or not profiles:
        raise HarnessError("policy.profiles: expected at least one profile")
    names: set[str] = set()
    for index, value in enumerate(profiles):
        context = f"policy.profiles[{index}]"
        profile = require_object(value, {"name", "limits", "evidence_requirement"}, context)
        name = require_text(profile["name"], f"{context}.name")
        if name in names:
            raise HarnessError(f"{context}.name: duplicate profile {name!r}")
        names.add(name)
        require_text(profile["evidence_requirement"], f"{context}.evidence_requirement")
        limits = require_object(profile["limits"], set(METRIC_DIRECTIONS), f"{context}.limits")
        for metric, direction in METRIC_DIRECTIONS.items():
            require_number(limits[metric], f"{context}.limits.{metric}", 0.0,
                           255.0 if metric in {"rgb_mae", "rgb_p95", "alpha_mae"} else 1.0)
            if direction == "min" and limits[metric] > 1.0:
                raise HarnessError(f"{context}.limits.{metric}: maximum is 1")
    return policy


def validate_approval(value: Any, contract: dict[str, Any], contract_hash: str,
                      policy: dict[str, Any], policy_hash: str) -> dict[str, Any]:
    approval = require_object(value, APPROVAL_KEYS, "approval")
    if require_integer(approval["schema_version"], "approval.schema_version", 1) != 1:
        raise HarnessError("approval.schema_version: only version 1 is supported")
    require_text(approval["approval_id"], "approval.approval_id")
    if require_hash(approval["contract_sha256"], "approval.contract_sha256") != contract_hash:
        raise HarnessError("approval.contract_sha256: does not match the supplied contract")
    if require_hash(approval["policy_sha256"], "approval.policy_sha256") != policy_hash:
        raise HarnessError("approval.policy_sha256: does not match the supplied policy")
    profile_name = require_text(approval["policy_profile"], "approval.policy_profile")
    profile = next((row for row in policy["profiles"] if row["name"] == profile_name), None)
    if profile is None:
        raise HarnessError(f"approval.policy_profile: unknown profile {profile_name!r}")
    if approval["approved_before_candidate"] is not True:
        raise HarnessError("approval.approved_before_candidate: must be true")
    evidence = require_object(approval["evidence"], {
        "baseline_capture_sha256", "repeat_capture_sha256", "repeat_report_sha256",
    }, "approval.evidence")
    for key in sorted(evidence):
        require_hash(evidence[key], f"approval.evidence.{key}")
    require_text(approval["rationale"], "approval.rationale")
    for region_index, region in enumerate(contract["regions"]):
        for metric, direction in METRIC_DIRECTIONS.items():
            bound = region["limits"][metric][direction]
            policy_bound = profile["limits"][metric]
            acceptable = bound <= policy_bound if direction == "max" else bound >= policy_bound
            if not acceptable:
                raise HarnessError(
                    f"contract.regions[{region_index}].limits.{metric}.{direction}: "
                    f"{bound} exceeds approved policy profile {profile_name!r} bound {policy_bound}"
                )
    return approval


def load_json(path: pathlib.Path, context: str) -> tuple[Any, bytes]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise HarnessError(f"{context}: cannot read {path.name}: {error}") from error
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=reject_duplicate_object_keys)
    except DuplicateJSONKeyError as error:
        raise HarnessError(f"{context}: {error}") from error
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise HarnessError(f"{context}: invalid UTF-8 JSON: {error}") from error
    return value, raw


def image_path_for(manifest_path: pathlib.Path, value: Any) -> pathlib.Path:
    relative = require_text(value, "manifest.image.path", logical_path=True)
    if pathlib.PureWindowsPath(relative).is_absolute() or pathlib.PurePosixPath(relative).is_absolute():
        raise HarnessError("manifest.image.path: absolute image paths are not allowed")
    try:
        base = manifest_path.parent.resolve()
        path = (base / relative).resolve()
    except (OSError, RuntimeError) as error:
        raise HarnessError(f"manifest.image.path: cannot resolve image path: {error}") from error
    try:
        path.relative_to(base)
    except ValueError as error:
        raise HarnessError("manifest.image.path: image must stay beside its manifest") from error
    if not path.is_file():
        raise HarnessError(f"manifest.image.path: image file {relative!r} is missing")
    return path


def load_capture(manifest_path: pathlib.Path, contract: dict[str, Any], contract_hash: str,
                 expected_role: str | None = None) -> dict[str, Any]:
    value, _ = load_json(manifest_path, "manifest")
    manifest = require_object(value, {
        "schema_version", "contract_sha256", "role", "image", "identity", "runtime",
    }, "manifest")
    if require_integer(manifest["schema_version"], "manifest.schema_version", 1) != 1:
        raise HarnessError("manifest.schema_version: only version 1 is supported")
    if require_hash(manifest["contract_sha256"], "manifest.contract_sha256") != contract_hash:
        raise HarnessError("manifest.contract_sha256: does not match the supplied frozen contract")
    role = require_text(manifest["role"], "manifest.role")
    if role not in {"baseline", "candidate"}:
        raise HarnessError("manifest.role: expected 'baseline' or 'candidate'")
    if expected_role is not None and role != expected_role:
        raise HarnessError(f"manifest.role: expected {expected_role!r}, got {role!r}")
    identity = validate_identity(manifest["identity"], "manifest.identity")
    if identity != contract["identity"]:
        raise HarnessError("manifest.identity: scene, inputs, viewport, camera, or time/seed differs from contract")
    runtime = require_object(manifest["runtime"], RUNTIME_KEYS, "manifest.runtime")
    for key in sorted(RUNTIME_KEYS):
        require_text(runtime[key], f"manifest.runtime.{key}")
    image = require_object(manifest["image"], {"path", "sha256"}, "manifest.image")
    expected_hash = require_hash(image["sha256"], "manifest.image.sha256")
    path = image_path_for(manifest_path, image["path"])
    try:
        actual_hash = hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as error:
        raise HarnessError(f"manifest.image.path: cannot read image {image['path']!r}: {error}") from error
    if actual_hash != expected_hash:
        raise HarnessError(f"manifest.image.sha256: digest mismatch for {image['path']!r}")
    try:
        with Image.open(path) as source:
            source.load()
            decoded = source.convert("RGBA")
    except (OSError, ValueError) as error:
        raise HarnessError(f"manifest.image.path: cannot decode image: {error}") from error
    expected_size = (contract["identity"]["viewport"]["width"], contract["identity"]["viewport"]["height"])
    if decoded.size != expected_size:
        raise HarnessError(
            f"manifest.image.path: decoded dimensions {decoded.width}x{decoded.height} "
            f"do not match contract {expected_size[0]}x{expected_size[1]}"
        )
    return {"manifest": manifest, "image_path": path, "pixels": decoded, "image_sha256": actual_hash}


def edge_mask(image: Image.Image, threshold: int) -> bytes:
    grayscale = image.convert("RGB").convert("L")
    edges = grayscale.filter(ImageFilter.FIND_EDGES)
    return bytes(pixel > threshold for pixel in edges.getdata())


def measure_region(left: Image.Image, right: Image.Image, region: dict[str, Any]) -> dict[str, float]:
    x, y, width, height = region["rect"]
    box = (x, y, x + width, y + height)
    difference = ImageChops.difference(left.crop(box), right.crop(box))
    pixel_count = width * height
    channel_histograms = [difference.getchannel(channel).histogram() for channel in range(3)]
    rgb_histogram = [sum(histogram[value] for histogram in channel_histograms) for value in range(256)]
    rgb_sample_count = pixel_count * 3
    rgb_absolute_sum = sum(value * count for value, count in enumerate(rgb_histogram))
    p95_target = math.ceil(0.95 * rgb_sample_count)
    cumulative = 0
    rgb_p95 = 0
    for value, count in enumerate(rgb_histogram):
        cumulative += count
        if cumulative >= p95_target:
            rgb_p95 = value
            break
    alpha_histogram = difference.getchannel(3).histogram()
    alpha_absolute_sum = sum(value * count for value, count in enumerate(alpha_histogram))
    difference_pixels = difference.tobytes()
    changed = sum(
        max(difference_pixels[offset], difference_pixels[offset + 1],
            difference_pixels[offset + 2], difference_pixels[offset + 3])
        > region["pixel_channel_threshold"]
        for offset in range(0, len(difference_pixels), 4)
    )
    left_edges = edge_mask(left.crop(box), region["edge_threshold"])
    right_edges = edge_mask(right.crop(box), region["edge_threshold"])
    intersection = sum(a and b for a, b in zip(left_edges, right_edges))
    union = sum(a or b for a, b in zip(left_edges, right_edges))
    return {
        "rgb_mae": rgb_absolute_sum / rgb_sample_count,
        "rgb_p95": float(rgb_p95),
        "alpha_mae": alpha_absolute_sum / pixel_count,
        "changed_pixel_ratio": changed / pixel_count,
        "edge_iou": intersection / union if union else 1.0,
    }


def compare_images(contract: dict[str, Any], baseline: dict[str, Any],
                   candidate: dict[str, Any], contract_hash: str,
                   approval: dict[str, Any], approval_hash: str,
                   policy: dict[str, Any], policy_hash: str) -> dict[str, Any]:
    outcomes = []
    passed = True
    for region in contract["regions"]:
        measurements = measure_region(baseline["pixels"], candidate["pixels"], region)
        checks: dict[str, Any] = {}
        for metric, direction in METRIC_DIRECTIONS.items():
            limit = region["limits"][metric]
            actual = measurements[metric]
            threshold = limit[direction]
            metric_passed = actual <= threshold if direction == "max" else actual >= threshold
            checks[metric] = {
                "actual": actual,
                "operator": direction,
                "limit": threshold,
                "rationale": limit["rationale"],
                "passed": metric_passed,
            }
            passed = passed and metric_passed
        outcomes.append({"name": region["name"], "rect": region["rect"], "metrics": checks})
    return {
        "schema_version": 1,
        "contract_id": contract["contract_id"],
        "contract_sha256": contract_hash,
        "approval": {
            "approval_id": approval["approval_id"],
            "approval_sha256": approval_hash,
            "approved_before_candidate": True,
        },
        "policy": {
            "policy_id": policy["policy_id"],
            "policy_sha256": policy_hash,
            "profile": approval["policy_profile"],
        },
        "status": "passed" if passed else "failed",
        "passed": passed,
        "provenance": {
            "baseline": baseline["manifest"]["runtime"],
            "candidate": candidate["manifest"]["runtime"],
        },
        "captures": {
            "baseline": {"role": "baseline", "image_sha256": baseline["image_sha256"]},
            "candidate": {"role": "candidate", "image_sha256": candidate["image_sha256"]},
        },
        "regions": outcomes,
    }


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except OSError as error:
        raise HarnessError(f"cannot write report {path}: {error}") from error


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="validate one capture against its contract")
    validate.add_argument("--contract", type=pathlib.Path, required=True)
    validate.add_argument("--manifest", type=pathlib.Path, required=True)
    compare = commands.add_parser("compare", help="compare baseline and candidate captures")
    compare.add_argument("--contract", type=pathlib.Path, required=True)
    compare.add_argument("--baseline", type=pathlib.Path, required=True)
    compare.add_argument("--candidate", type=pathlib.Path, required=True)
    compare.add_argument("--policy", type=pathlib.Path, required=True)
    compare.add_argument("--approval", type=pathlib.Path, required=True)
    compare.add_argument("--report", type=pathlib.Path)
    return parser.parse_args(argv)


def run(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        contract_value, contract_raw = load_json(args.contract, "contract")
        contract = validate_contract(contract_value)
        contract_hash = hashlib.sha256(contract_raw).hexdigest()
        if args.command == "validate":
            capture = load_capture(args.manifest, contract, contract_hash)
            result = {
                "schema_version": 1,
                "status": "valid",
                "contract_id": contract["contract_id"],
                "contract_sha256": contract_hash,
                "role": capture["manifest"]["role"],
                "image_sha256": capture["image_sha256"],
            }
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0
        baseline = load_capture(args.baseline, contract, contract_hash, "baseline")
        candidate = load_capture(args.candidate, contract, contract_hash, "candidate")
        policy_value, policy_raw = load_json(args.policy, "policy")
        policy = validate_policy(policy_value)
        policy_hash = hashlib.sha256(policy_raw).hexdigest()
        approval_value, approval_raw = load_json(args.approval, "approval")
        approval = validate_approval(
            approval_value, contract, contract_hash, policy, policy_hash
        )
        approval_hash = hashlib.sha256(approval_raw).hexdigest()
        result = compare_images(contract, baseline, candidate, contract_hash,
                                approval, approval_hash, policy, policy_hash)
        if args.report is not None:
            write_json(args.report, result)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["passed"] else 1
    except HarnessError as error:
        print(json.dumps({"status": "error", "message": str(error)}, sort_keys=True), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(run())
