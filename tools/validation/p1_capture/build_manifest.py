#!/usr/bin/env python3
"""Assemble one audited P1-01 Windows capture manifest from a host report.

The prototype and production Godot hosts emit ``report.json`` rather than the
strict capture manifest consumed by ``compare.py``.  This builder checks one
host report against the frozen fixed-scene identity, independently hashes the
scene, replay and PNG supplied on the command line, binds the measured runtime
supplement to hash-checked retained receipts, and only then exclusively creates
``manifest.json`` beside the image.

It never launches Godot or a retail game, never reads private assets, never
queries git (a binary revision is never inferred from the current checkout),
and never compares pixels.  A successful build is manifest assembly only:
``visual_comparison`` and ``acceptance`` are always false in its receipt.

Exit status: 0 assembled and validated; 1 evidence mismatch, invalid or
missing evidence; 2 command-line, path-policy or tool error.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import pathlib
import re
import shutil
import stat
import sys
import tempfile
from typing import Any, Mapping, NoReturn

# ``compare.py`` owns the manifest/contract schema; reuse it rather than
# duplicating validators.  The directory is not a package (same pattern as
# ``check_migration_pair.py``).
_TOOL_DIRECTORY = pathlib.Path(__file__).resolve().parent
if str(_TOOL_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(_TOOL_DIRECTORY))

import compare as capture_compare  # noqa: E402  (path setup is deliberate)

from PIL import Image  # noqa: E402  (compare.py already requires Pillow)


TOOL = "build_manifest"
SUPPLEMENT_KIND = "p1-01-windows-manifest-supplement"
DERIVATION_KIND = "p1-01-prototype-capture-derivation"
SHA1_REVISION = re.compile(r"[0-9a-f]{40}\Z")
WINDOWS_DRIVER_VERSION = re.compile(r"[0-9]+(\.[0-9]+)+\Z")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
JSON_INTEGER_MIN = -(2 ** 63)
JSON_INTEGER_MAX = 2 ** 63 - 1
WINDOWS_DRIVE = re.compile(r"[A-Za-z]:\Z")
# Device names Windows resolves regardless of directory or extension.
WINDOWS_RESERVED_NAMES = frozenset(
    {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}
    | {f"{prefix}{suffix}" for prefix in ("COM", "LPT") for suffix in "123456789¹²³"}
)
WINDOWS_INVALID_CHARACTERS = frozenset('<>"|?*') | frozenset(chr(code) for code in range(1, 32))

# Frozen fixed-scene identity from out/architecture/P1-01-windows-pair-plan.md
# and the retained Linux contract.  Logical paths are fixed repository/VFS
# strings; CLI paths never flow into the manifest.
SCENE_LOGICAL_PATH = "prototypes/common/scene.json"
SCENE_SHA256 = "de673739583babf0a4541feafc6bd1f0c40da36788a962e7d49b284be7611a18"
MODEL_LOGICAL_PATH = "data/art/models/rebel_mon_calamari_mc_50.alo"
MODEL_SHA256 = "9fd06b06d1626d8a11bea184fe73768745a6774062d44c9c39636e16202b62fe"
TEXTURE_LOGICAL_PATH = "data/art/textures/hangar_3.dds"
TEXTURE_SHA256 = "24cfdf7a9a6d9156e9d218b2d2f6f4a1545dae63096534471dbb08da9af9572b"
REPLAY_LOGICAL_PATH = "tests/replay/fixtures/original-v1.eawr-replay"
REPLAY_SHA256 = "fcf7f050a4ae8be540def3e4609fc7ff68a21e4ed4fce59e46c1fc62e59a4f90"
REPLAY_FILE_NAME = "original-v1.eawr-replay"
REPORT_VFS_PROFILE = "remake"
MANIFEST_VFS_PROFILE = "remake-effective"
VIEWPORT = (1280, 720)
CAMERA = {
    "projection": "perspective",
    "position": [0.0, 420.0, 1050.0],
    "target": [0.0, 0.0, 0.0],
    "up": [0.0, 1.0, 0.0],
    "fov_degrees": 45.0,
    "near": 1.0,
    "far": 20000.0,
}
SIMULATION_TICK = 1
CAPTURE_FRAME = 120
PROTOTYPE_CAPTURE_PROCESS_FRAME = 119  # zero-based frame_ where frame_ + 1 == 120
PROTOTYPE_SNAPSHOT_INDEX = 1  # (119 / 100) % snapshot_count
SNAPSHOT_COUNT = 6
ENGINE_VERSION = "4.7.2-stable"
GODOT_CPP_VERSION = "10.0.0-stable"
BACKEND = "gl_compatibility"
MATERIAL_PROGRAM = "MeshGloss.fx"
PROTOTYPE_SOURCE_LOGICAL_PATH = "prototypes/godot/src/prototype_host.cpp"

FROZEN_IDENTITY: dict[str, Any] = {
    "vfs_profile": MANIFEST_VFS_PROFILE,
    "scene": {"logical_path": SCENE_LOGICAL_PATH, "sha256": SCENE_SHA256},
    "inputs": [
        {"logical_path": MODEL_LOGICAL_PATH, "sha256": MODEL_SHA256},
        {"logical_path": TEXTURE_LOGICAL_PATH, "sha256": TEXTURE_SHA256},
        {"logical_path": REPLAY_LOGICAL_PATH, "sha256": REPLAY_SHA256},
    ],
    "viewport": {"width": VIEWPORT[0], "height": VIEWPORT[1]},
    "camera": CAMERA,
    "simulation_tick": SIMULATION_TICK,
    "animation_time_seconds": 0.0,
    "particle_seed": 0,
    "particle_time_seconds": 0.0,
}

PROTOTYPE_REPORT_KEYS = {
    "schema_version", "backend", "versions", "platform", "hardware", "scene_hash",
    "settings", "controls", "capture", "samples", "metrics", "effort", "limitations",
    "artifacts", "material", "modern_shader", "snapshot", "status", "failure",
}
PRODUCTION_REPORT_REQUIRED_KEYS = {
    "schema_version", "status", "failure", "backend", "versions", "scene_sha256", "replay",
    "snapshot_count", "capture_sha256", "capture_identity", "model", "texture",
    "material_program", "animation", "animation_time_seconds", "skin_palette_bound",
    "animation_capture_verified", "renderer_runtime_exercise", "runtime_capture_verified",
    "runtime_overlap_verified", "runtime_post_dependency_verified",
    "runtime_failed_upload_registry_clean", "runtime_scene_switch_count",
    "runtime_shutdown_resources_empty", "pass_order",
}
# Emitted by the current writer but absent from the retained Linux reports.
# Both must be false for the fixed-scene capture; their sections must be absent.
PRODUCTION_REPORT_OPTIONAL_FALSE_KEYS = {"tactical_camera_verified", "atlas_overlay_verified"}
# Written by current viewers (#184/#220), absent from reports retained before it.
PRODUCTION_REPORT_OPTIONAL_KEYS = {"render_profile"}
PRODUCTION_REPORT_FORBIDDEN_SECTIONS = {"tactical_camera", "camera_input", "atlas", "mode"}

SUPPLEMENT_COMMON_KEYS = {
    "schema_version", "kind", "host", "platform", "architecture", "driver_version",
    "build_revision", "build_receipt_sha256", "runtime_receipt_sha256",
}
DERIVATION_KEYS = {
    "schema_version", "kind", "source_revision", "source_logical_path", "source_sha256",
    "capture_process_frame_zero_based", "snapshot_index", "simulation_tick",
    "animation_time_seconds", "particle_seed", "particle_time_seconds",
    "replay_sha256", "capture_sha256", "rationale",
}

HUMAN_REVIEW_GAPS = [
    "the builder cannot prove the host report is truthful; it checks internal consistency only",
    "build_revision and receipt hashes are references to retained evidence, not clean-source build attestations",
    "the extension DLL identity loaded by the run is not proven by this builder",
    "the physical host, Windows session and selected GPU adapter are not proven; labels may match without being the same machine",
    "driver_version comes from the supplement; the builder cannot prove it belongs to the renderer-selected adapter",
    "VFS layer resolution/provenance of model and texture is not re-derived; hashes are taken from the host report",
    "a decodable 1280x720 PNG does not prove visible Hangar geometry; visual content needs independent review",
    "contract/approval chronology and review are not checked here",
    "prototype capture tick is attested by the capture-derivation receipt, not measured by the prototype report",
    "a valid manifest is not P1-01 or issue #22 acceptance; same-backend preflight, pixel comparison and independent review remain",
]


class EvidenceError(ValueError):
    """Evidence is missing, malformed or inconsistent (exit 1)."""


class UsageError(ValueError):
    """Command-line, path-policy or tool failure (exit 2)."""


class _Parser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        raise UsageError(f"command line: {message}")


# ----------------------------------------------------------------- JSON input

def _reject_constant(name: str) -> NoReturn:
    raise ValueError(f"non-finite JSON number {name} is not allowed")


def _finite_float(text: str) -> float:
    # A literal such as 1e999 overflows to inf under the default parse_float.
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(f"non-finite JSON number {text[:32]} is not allowed")
    return value


def _bounded_int(text: str) -> int:
    # Every integer must fit a signed 64-bit value, so later float() and
    # comparison paths cannot overflow.  Check the digit count first so an
    # oversized literal is never converted.
    digits = text[1:] if text.startswith("-") else text
    if len(digits) <= 19:
        value = int(text)
        if JSON_INTEGER_MIN <= value <= JSON_INTEGER_MAX:
            return value
    raise ValueError(f"JSON integer {text[:32]}{'...' if len(text) > 32 else ''} is outside the signed 64-bit range")


def load_strict_json(raw: bytes, context: str) -> Any:
    """Parse UTF-8 JSON rejecting duplicate keys, a BOM and non-finite or
    oversized numbers anywhere in the document, including ignored fields."""

    if raw.startswith(b"\xef\xbb\xbf"):
        raise EvidenceError(f"{context}: UTF-8 byte-order mark is not allowed")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise EvidenceError(f"{context}: invalid UTF-8: {error}") from error
    try:
        return json.loads(
            text,
            object_pairs_hook=capture_compare.reject_duplicate_object_keys,
            parse_constant=_reject_constant,
            parse_float=_finite_float,
            parse_int=_bounded_int,
        )
    except capture_compare.DuplicateJSONKeyError as error:
        raise EvidenceError(f"{context}: {error}") from error
    except ValueError as error:
        raise EvidenceError(f"{context}: invalid JSON: {error}") from error


# ------------------------------------------------------------ typed accessors

def _object(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise EvidenceError(f"{context}: expected an object")
    return value


def _field(container: Mapping[str, Any], key: str, context: str) -> Any:
    if key not in container:
        raise EvidenceError(f"{context}.{key}: missing")
    return container[key]


def _exact_keys(value: Any, keys: set[str], context: str) -> dict[str, Any]:
    try:
        return capture_compare.require_object(value, keys, context)
    except capture_compare.HarnessError as error:
        raise EvidenceError(str(error)) from error


def _text(value: Any, context: str) -> str:
    try:
        return capture_compare.require_text(value, context)
    except capture_compare.HarnessError as error:
        raise EvidenceError(str(error)) from error


def _hash(value: Any, context: str) -> str:
    try:
        return capture_compare.require_hash(value, context)
    except capture_compare.HarnessError as error:
        raise EvidenceError(str(error)) from error


def _integer(value: Any, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise EvidenceError(f"{context}: expected an integer, got {type(value).__name__}")
    return value


def _number(value: Any, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise EvidenceError(f"{context}: expected a number, got {type(value).__name__}")
    try:
        number = float(value)
    except OverflowError as error:
        raise EvidenceError(f"{context}: number is out of range") from error
    if not math.isfinite(number):
        raise EvidenceError(f"{context}: expected a finite number")
    return number


def _bool(value: Any, context: str) -> bool:
    if not isinstance(value, bool):
        raise EvidenceError(f"{context}: expected a boolean, got {type(value).__name__}")
    return value


def _expect(actual: Any, expected: Any, context: str) -> None:
    if actual != expected:
        raise EvidenceError(f"{context}: expected {expected!r}, got {actual!r}")


def _expect_text(container: Mapping[str, Any], key: str, expected: str, context: str) -> str:
    value = _field(container, key, context)
    if not isinstance(value, str):
        raise EvidenceError(f"{context}.{key}: expected a string")
    _expect(value, expected, f"{context}.{key}")
    return value


def _expect_integer(container: Mapping[str, Any], key: str, expected: int, context: str) -> int:
    value = _integer(_field(container, key, context), f"{context}.{key}")
    _expect(value, expected, f"{context}.{key}")
    return value


def _expect_number(container: Mapping[str, Any], key: str, expected: float, context: str) -> float:
    value = _number(_field(container, key, context), f"{context}.{key}")
    _expect(value, expected, f"{context}.{key}")
    return value


def _expect_bool(container: Mapping[str, Any], key: str, expected: bool, context: str) -> bool:
    value = _bool(_field(container, key, context), f"{context}.{key}")
    _expect(value, expected, f"{context}.{key}")
    return value


def _expect_vector(value: Any, expected: list[float], context: str) -> None:
    if not isinstance(value, list) or len(value) != len(expected):
        raise EvidenceError(f"{context}: expected {len(expected)} numbers")
    for index, (component, wanted) in enumerate(zip(value, expected)):
        _expect(_number(component, f"{context}[{index}]"), wanted, f"{context}[{index}]")


def _require_status_passed(report: Mapping[str, Any], context: str) -> None:
    _expect_text(report, "status", "passed", context)
    _expect_text(report, "failure", "", context)
    _expect_integer(report, "schema_version", 1, context)


# ------------------------------------------------------------- path handling

def _check_cli_path_text(text: str, option: str) -> None:
    if not text or "\x00" in text:
        raise UsageError(f"{option}: empty path or NUL character")
    if text.startswith(("\\\\", "//")):
        raise UsageError(f"{option}: UNC and device paths are not allowed")
    windows = pathlib.PureWindowsPath(text)
    if windows.drive and not windows.root:
        raise UsageError(f"{option}: drive-relative paths are not allowed")
    rest = text
    if windows.drive:
        if not WINDOWS_DRIVE.match(windows.drive):
            raise UsageError(f"{option}: UNC and device paths are not allowed")
        rest = text[len(windows.drive):]
    parts = rest.replace("\\", "/").split("/")
    if ".." in parts:
        raise UsageError(f"{option}: parent traversal is not allowed")
    # Every component must name exactly one ordinary file or directory: no
    # NTFS alternate data stream (``frame.png:manifest.json``), no device
    # name, and no trailing dot/space that Windows silently strips.
    for part in parts:
        if not part or part == ".":
            continue
        if ":" in part:
            raise UsageError(
                f"{option}: ':' after the drive prefix is not allowed (NTFS alternate data stream)"
            )
        if WINDOWS_INVALID_CHARACTERS.intersection(part):
            raise UsageError(f"{option}: reserved Windows path character in a path component")
        if part.endswith((".", " ")):
            raise UsageError(f"{option}: path component ends with a dot or space")
        if part.split(".", 1)[0].rstrip(" ").upper() in WINDOWS_RESERVED_NAMES:
            raise UsageError(f"{option}: reserved Windows device name in a path component")


def _is_link_or_reparse(path: pathlib.Path) -> bool:
    status = os.lstat(path)
    if stat.S_ISLNK(status.st_mode):
        return True
    return bool(getattr(status, "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT)


def _check_no_link_chain(path: pathlib.Path, option: str) -> None:
    """Refuse a symlink/junction/reparse point anywhere on the absolute path."""

    for component in (path, *path.parents):
        try:
            linked = _is_link_or_reparse(component)
        except FileNotFoundError:
            continue
        except OSError as error:
            raise UsageError(f"{option}: cannot inspect path component: {error}") from error
        if linked:
            raise UsageError(f"{option}: symlink, junction or reparse point on the path is not allowed")


def _input_path(text: str, option: str) -> pathlib.Path:
    _check_cli_path_text(text, option)
    path = pathlib.Path(os.path.abspath(text))
    _check_no_link_chain(path, option)
    if not path.exists():
        raise EvidenceError(f"{option}: evidence file {path.name!r} is missing")
    if not path.is_file():
        raise UsageError(f"{option}: expected a regular file")
    return path


def _file_key(path: pathlib.Path) -> tuple[Any, ...]:
    status = os.stat(path)
    if status.st_ino:
        return status.st_dev, status.st_ino
    # Filesystems without stable file IDs: fall back to the case-folded path.
    return ("path", os.path.normcase(os.path.realpath(path)))


def _read(path: pathlib.Path, option: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise EvidenceError(f"{option}: cannot read {path.name!r}: {error}") from error


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --------------------------------------------------------- evidence checking

def _validate_contract(raw: bytes) -> tuple[dict[str, Any], str]:
    value = load_strict_json(raw, "contract")
    try:
        contract = capture_compare.validate_contract(value)
    except capture_compare.HarnessError as error:
        raise EvidenceError(str(error)) from error
    if contract["identity"] != FROZEN_IDENTITY:
        raise EvidenceError(
            "contract.identity: does not equal the frozen P1-01 fixed-scene identity "
            "(scene, inputs, remake-effective profile, 1280x720 camera, tick 1, zero animation/particle)"
        )
    return contract, _sha256(raw)


def _validate_image(raw: bytes, path: pathlib.Path) -> dict[str, Any]:
    if not raw.startswith(PNG_SIGNATURE):
        raise EvidenceError("image: not a PNG file (signature mismatch)")
    try:
        with Image.open(path) as source:
            image_format = source.format
            mode = source.mode
            size = source.size
            source.load()
    except (OSError, ValueError, Image.DecompressionBombError) as error:
        raise EvidenceError(f"image: cannot decode PNG: {error}") from error
    if image_format != "PNG":
        raise EvidenceError(f"image: decoded format {image_format!r} is not PNG")
    if size != VIEWPORT:
        raise EvidenceError(
            f"image: decoded dimensions {size[0]}x{size[1]} do not match {VIEWPORT[0]}x{VIEWPORT[1]}"
        )
    return {"format": image_format, "mode": mode, "width": size[0], "height": size[1]}


def _validate_prototype_report(report: dict[str, Any]) -> dict[str, Any]:
    context = "report"
    _exact_keys(report, PROTOTYPE_REPORT_KEYS, context)
    _require_status_passed(report, context)

    backend = _object(report["backend"], "report.backend")
    _expect_text(backend, "engine", "Godot", "report.backend")
    _expect_text(backend, "adapter", "RenderingServer GDExtension", "report.backend")
    _expect_text(backend, "rendering_method", BACKEND, "report.backend")
    graphics_api = _text(_field(backend, "api", "report.backend"), "report.backend.api")

    versions = _exact_keys(report["versions"], {"godot", "godot_cpp", "gdextension_api"}, "report.versions")
    _expect_text(versions, "godot", ENGINE_VERSION, "report.versions")
    _expect_text(versions, "godot_cpp", GODOT_CPP_VERSION, "report.versions")
    _expect_text(versions, "gdextension_api", "4.7", "report.versions")

    platform = _object(report["platform"], "report.platform")
    _expect_text(platform, "os", "Windows", "report.platform")
    _expect_bool(platform, "headless", False, "report.platform")

    hardware = _object(report["hardware"], "report.hardware")
    vendor = _text(_field(hardware, "adapter_vendor", "report.hardware"), "report.hardware.adapter_vendor")
    name = _text(_field(hardware, "adapter_name", "report.hardware"), "report.hardware.adapter_name")

    scene_hash = _exact_keys(report["scene_hash"], {"scene_json", "model", "texture"}, "report.scene_hash")
    _expect_text(scene_hash, "scene_json", SCENE_SHA256, "report.scene_hash")
    _expect_text(scene_hash, "model", MODEL_SHA256, "report.scene_hash")
    _expect_text(scene_hash, "texture", TEXTURE_SHA256, "report.scene_hash")

    settings = _object(report["settings"], "report.settings")
    resolution = _field(settings, "resolution", "report.settings")
    if not isinstance(resolution, list) or len(resolution) != 2:
        raise EvidenceError("report.settings.resolution: expected [width, height]")
    _expect([_integer(item, "report.settings.resolution[]") for item in resolution],
            list(VIEWPORT), "report.settings.resolution")
    _expect_integer(settings, "warmup_frames", CAPTURE_FRAME, "report.settings")
    _expect_integer(settings, "fixed_camera_frame", CAPTURE_FRAME, "report.settings")
    _expect_text(settings, "axis_conversion", "x,z,-y", "report.settings")
    _expect_text(settings, "texture_colour_space", "srgb", "report.settings")
    _expect_bool(settings, "vsync", True, "report.settings")
    _expect_bool(settings, "resize_probe", False, "report.settings")
    vsync_mode = _integer(_field(settings, "vsync_observed_mode", "report.settings"),
                          "report.settings.vsync_observed_mode")
    if vsync_mode < 0:
        raise EvidenceError("report.settings.vsync_observed_mode: headless/unknown VSync mode")

    controls = _object(report["controls"], "report.controls")
    _expect_bool(controls, "probe_requested", False, "report.controls")
    _expect_bool(controls, "probe_exercised", False, "report.controls")

    capture = _object(report["capture"], "report.capture")
    _expect_bool(capture, "baseline_camera_restored_before_capture", True, "report.capture")
    # capture.eye/bounds describe the supplemental closeup and are never used
    # as the baseline camera; a completed closeup means a different run mode.
    _expect_bool(capture, "closeup_done", False, "report.capture")

    material = _object(report["material"], "report.material")
    _expect_text(material, "effect", MATERIAL_PROGRAM, "report.material")
    _expect_text(material, "technique", "sph_t0", "report.material")
    _expect_text(material, "pass", "sph_t0_p0", "report.material")
    _expect_bool(material, "stock_material_substitute", False, "report.material")

    snapshot = _object(report["snapshot"], "report.snapshot")
    _expect_integer(snapshot, "snapshot_count", SNAPSHOT_COUNT, "report.snapshot")
    replay_hashes = _field(snapshot, "replay_hashes", "report.snapshot")
    if not isinstance(replay_hashes, list) or len(replay_hashes) != SNAPSHOT_COUNT - 1:
        raise EvidenceError(f"report.snapshot.replay_hashes: expected {SNAPSHOT_COUNT - 1} tick hashes")
    for index, item in enumerate(replay_hashes):
        _hash(item, f"report.snapshot.replay_hashes[{index}]")

    return {
        "engine": "Godot",
        "graphics_api": graphics_api,
        "vendor": vendor,
        "adapter_name": name,
        "vsync_observed_mode": vsync_mode,
    }


def _validate_production_report(report: dict[str, Any], image_sha256: str,
                                replay_sha256: str) -> dict[str, Any]:
    context = "report"
    keys = set(report)
    forbidden = sorted(keys & PRODUCTION_REPORT_FORBIDDEN_SECTIONS)
    if forbidden:
        raise EvidenceError(
            "report: not a fixed-scene capture report (contains " + ", ".join(forbidden) + ")"
        )
    missing = sorted(PRODUCTION_REPORT_REQUIRED_KEYS - keys)
    extra = sorted(keys - PRODUCTION_REPORT_REQUIRED_KEYS - PRODUCTION_REPORT_OPTIONAL_FALSE_KEYS
                   - PRODUCTION_REPORT_OPTIONAL_KEYS)
    if missing or extra:
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if extra:
            details.append("unsupported " + ", ".join(extra))
        raise EvidenceError("report: " + "; ".join(details))
    _require_status_passed(report, context)
    for key in sorted(PRODUCTION_REPORT_OPTIONAL_FALSE_KEYS & keys):
        _expect_bool(report, key, False, context)

    backend = _exact_keys(report["backend"], {
        "engine", "rendering_method", "adapter_vendor", "adapter_name", "driver_api",
    }, "report.backend")
    engine = _expect_text(backend, "engine", f"Godot {ENGINE_VERSION}", "report.backend")
    _expect_text(backend, "rendering_method", BACKEND, "report.backend")
    vendor = _text(backend["adapter_vendor"], "report.backend.adapter_vendor")
    name = _text(backend["adapter_name"], "report.backend.adapter_name")
    graphics_api = _text(backend["driver_api"], "report.backend.driver_api")

    versions = _exact_keys(report["versions"], {"godot", "godot_cpp", "material_schema"}, "report.versions")
    _expect_text(versions, "godot", ENGINE_VERSION, "report.versions")
    _expect_text(versions, "godot_cpp", GODOT_CPP_VERSION, "report.versions")
    _expect_integer(versions, "material_schema", 1, "report.versions")

    _expect_text(report, "scene_sha256", SCENE_SHA256, context)
    replay = _exact_keys(report["replay"], {"logical_path", "sha256"}, "report.replay")
    _expect_text(replay, "sha256", REPLAY_SHA256, "report.replay")
    _expect(replay["sha256"], replay_sha256, "report.replay.sha256 (vs independently hashed --replay)")
    # replay.logical_path is the CLI path the host was given (often absolute);
    # it is never used as a manifest logical path.  Only its file name is checked.
    replay_path_text = _text(replay["logical_path"], "report.replay.logical_path")
    if replay_path_text.replace("\\", "/").rsplit("/", 1)[-1] != REPLAY_FILE_NAME:
        raise EvidenceError(f"report.replay.logical_path: does not name {REPLAY_FILE_NAME}")

    _expect_integer(report, "snapshot_count", SNAPSHOT_COUNT, context)
    capture_sha = _hash(report["capture_sha256"], "report.capture_sha256")
    _expect(capture_sha, image_sha256, "report.capture_sha256 (vs independently hashed --image)")

    identity = _exact_keys(report["capture_identity"], {
        "vfs_profile", "viewport", "camera", "simulation_tick", "capture_frame",
        "animation_time_seconds", "particle_seed", "particle_time_seconds",
    }, "report.capture_identity")
    _expect_text(identity, "vfs_profile", REPORT_VFS_PROFILE, "report.capture_identity")
    viewport = _exact_keys(identity["viewport"], {"width", "height"}, "report.capture_identity.viewport")
    _expect_integer(viewport, "width", VIEWPORT[0], "report.capture_identity.viewport")
    _expect_integer(viewport, "height", VIEWPORT[1], "report.capture_identity.viewport")
    camera = _exact_keys(identity["camera"], set(CAMERA), "report.capture_identity.camera")
    _expect_text(camera, "projection", CAMERA["projection"], "report.capture_identity.camera")
    for vector in ("position", "target", "up"):
        _expect_vector(camera[vector], CAMERA[vector], f"report.capture_identity.camera.{vector}")
    for scalar in ("fov_degrees", "near", "far"):
        _expect_number(camera, scalar, CAMERA[scalar], "report.capture_identity.camera")
    _expect_integer(identity, "simulation_tick", SIMULATION_TICK, "report.capture_identity")
    _expect_integer(identity, "capture_frame", CAPTURE_FRAME, "report.capture_identity")
    _expect_number(identity, "animation_time_seconds", 0.0, "report.capture_identity")
    _expect_integer(identity, "particle_seed", 0, "report.capture_identity")
    _expect_number(identity, "particle_time_seconds", 0.0, "report.capture_identity")

    model = _exact_keys(report["model"], {"logical_path", "sha256"}, "report.model")
    _expect_text(model, "logical_path", MODEL_LOGICAL_PATH, "report.model")
    _expect_text(model, "sha256", MODEL_SHA256, "report.model")
    texture = _exact_keys(report["texture"], {"logical_path", "sha256"}, "report.texture")
    _expect_text(texture, "logical_path", TEXTURE_LOGICAL_PATH, "report.texture")
    _expect_text(texture, "sha256", TEXTURE_SHA256, "report.texture")
    _expect_text(report, "material_program", MATERIAL_PROGRAM, context)

    animation = _exact_keys(report["animation"], {"logical_path", "sha256", "bone_count"}, "report.animation")
    _expect_text(animation, "logical_path", "", "report.animation")
    _expect_text(animation, "sha256", "", "report.animation")
    _integer(animation["bone_count"], "report.animation.bone_count")
    _expect_number(report, "animation_time_seconds", 0.0, context)
    _bool(report["skin_palette_bound"], "report.skin_palette_bound")
    _bool(report["animation_capture_verified"], "report.animation_capture_verified")

    for key in ("renderer_runtime_exercise", "runtime_capture_verified", "runtime_overlap_verified",
                "runtime_post_dependency_verified", "runtime_failed_upload_registry_clean",
                "runtime_shutdown_resources_empty"):
        _expect_bool(report, key, False, context)
    _expect_integer(report, "runtime_scene_switch_count", 0, context)
    _expect(report["pass_order"], ["opaque"], "report.pass_order")

    return {"engine": engine, "graphics_api": graphics_api, "vendor": vendor, "adapter_name": name}


def _validate_supplement(value: Any, host: str) -> dict[str, Any]:
    keys = set(SUPPLEMENT_COMMON_KEYS)
    if host == "prototype":
        keys.add("capture_derivation_receipt_sha256")
    supplement = _exact_keys(value, keys, "supplement")
    _expect_integer(supplement, "schema_version", 1, "supplement")
    _expect_text(supplement, "kind", SUPPLEMENT_KIND, "supplement")
    _expect_text(supplement, "host", host, "supplement")
    _expect_text(supplement, "platform", "windows", "supplement")
    _expect_text(supplement, "architecture", "x86_64", "supplement")
    driver_version = _text(supplement["driver_version"], "supplement.driver_version")
    if not WINDOWS_DRIVER_VERSION.fullmatch(driver_version):
        raise EvidenceError("supplement.driver_version: expected a measured dotted numeric Windows driver version")
    revision = supplement["build_revision"]
    if not isinstance(revision, str) or not SHA1_REVISION.fullmatch(revision) or set(revision) == {"0"}:
        raise EvidenceError("supplement.build_revision: expected a full 40-hex lowercase source revision")
    for key in sorted(keys):
        if key.endswith("_sha256"):
            _hash(supplement[key], f"supplement.{key}")
    return supplement


def _validate_derivation(value: Any, revision: str, image_sha256: str,
                         replay_sha256: str) -> dict[str, Any]:
    receipt = _exact_keys(value, DERIVATION_KEYS, "capture_derivation")
    context = "capture_derivation"
    _expect_integer(receipt, "schema_version", 1, context)
    _expect_text(receipt, "kind", DERIVATION_KIND, context)
    _expect_text(receipt, "source_revision", revision, context)
    _expect_text(receipt, "source_logical_path", PROTOTYPE_SOURCE_LOGICAL_PATH, context)
    _hash(receipt["source_sha256"], "capture_derivation.source_sha256")
    _expect_integer(receipt, "capture_process_frame_zero_based", PROTOTYPE_CAPTURE_PROCESS_FRAME, context)
    _expect_integer(receipt, "snapshot_index", PROTOTYPE_SNAPSHOT_INDEX, context)
    _expect_integer(receipt, "simulation_tick", SIMULATION_TICK, context)
    _expect_number(receipt, "animation_time_seconds", 0.0, context)
    _expect_integer(receipt, "particle_seed", 0, context)
    _expect_number(receipt, "particle_time_seconds", 0.0, context)
    _expect_text(receipt, "replay_sha256", replay_sha256, context)
    _expect_text(receipt, "capture_sha256", image_sha256, context)
    _text(receipt["rationale"], "capture_derivation.rationale")
    return receipt


# ------------------------------------------------------------------ assembly

def _field_sources(host: str) -> dict[str, str]:
    common = {
        "contract_sha256": "SHA-256 of --contract bytes; contract identity must equal the frozen plan identity",
        "image.path": "file name of --image; manifest is created in the same directory",
        "image.sha256": "independent SHA-256 of --image bytes",
        "identity.scene": "fixed logical path; independent SHA-256 of --scene equals frozen hash",
        "identity.inputs[replay]": "fixed logical path; independent SHA-256 of --replay equals frozen hash",
        "runtime.platform": "supplement.platform (measured windows)",
        "runtime.architecture": "supplement.architecture (x86_64)",
        "runtime.engine_version": "report.versions.godot",
        "runtime.backend": "report.backend.rendering_method",
        "runtime.driver_version": "supplement.driver_version",
        "runtime.build_revision": "supplement.build_revision (retained build receipt reference; never git HEAD)",
    }
    if host == "prototype":
        common.update({
            "identity.vfs_profile": "frozen remake-effective; prototype report does not record a profile",
            "identity.inputs[model,texture]": "fixed logical paths; report.scene_hash.model/texture equal frozen hashes",
            "identity.viewport": "report.settings.resolution",
            "identity.camera": "frozen baseline camera; report.capture.baseline_camera_restored_before_capture=true "
                               "(report.capture.eye is the supplemental closeup and is ignored)",
            "identity.simulation_tick": "capture-derivation receipt (report has no capture tick)",
            "identity.animation_and_particle": "capture-derivation receipt zero values",
            "runtime.engine": "report.backend.engine",
            "runtime.graphics_api": "report.backend.api",
            "runtime.device": "report.hardware.adapter_vendor + ' ' + report.hardware.adapter_name",
            "runtime.driver": "report.hardware.adapter_vendor",
            "platform_corroboration": "report.platform.os=Windows, headless=false",
        })
    else:
        common.update({
            "identity.vfs_profile": "report.capture_identity.vfs_profile 'remake' mapped to 'remake-effective'",
            "identity.inputs[model,texture]": "report.model/texture logical paths and hashes equal frozen values",
            "identity.viewport": "report.capture_identity.viewport",
            "identity.camera": "report.capture_identity.camera",
            "identity.simulation_tick": "report.capture_identity.simulation_tick (capture_frame 120)",
            "identity.animation_and_particle": "report.capture_identity and report.animation (empty)",
            "runtime.engine": "report.backend.engine",
            "runtime.graphics_api": "report.backend.driver_api",
            "runtime.device": "report.backend.adapter_vendor + ' ' + report.backend.adapter_name",
            "runtime.driver": "report.backend.adapter_vendor",
            "platform_corroboration": "none in production report; supplement only",
        })
    return dict(sorted(common.items()))


def serialise_manifest(manifest: Mapping[str, Any]) -> bytes:
    return (json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=False,
                       allow_nan=False) + "\n").encode("utf-8")


def _validate_with_comparator(manifest_bytes: bytes, image_path: pathlib.Path, image_name: str,
                              contract: dict[str, Any], contract_hash: str, role: str) -> None:
    """Run the comparator's own manifest loader against a private staged copy."""

    with tempfile.TemporaryDirectory(prefix="eawr-manifest-") as staging:
        staged = pathlib.Path(staging)
        shutil.copyfile(image_path, staged / image_name)
        staged_manifest = staged / "manifest.json"
        staged_manifest.write_bytes(manifest_bytes)
        try:
            capture_compare.load_capture(staged_manifest, contract, contract_hash, role)
        except capture_compare.HarnessError as error:
            raise EvidenceError(f"comparator manifest validation: {error}") from error


def _exclusive_write(path: pathlib.Path, data: bytes) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
    try:
        descriptor = os.open(path, flags, 0o644)
    except FileExistsError as error:
        raise UsageError("--output: refusing to overwrite an existing manifest") from error
    except OSError as error:
        raise UsageError(f"--output: cannot create manifest: {error}") from error
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
    except OSError as error:
        try:
            path.unlink()
        except OSError:
            pass
        raise UsageError(f"--output: cannot write manifest: {error}") from error


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = _Parser(prog="build_manifest.py", description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True, parser_class=_Parser)
    build = commands.add_parser("build", help="assemble and validate one capture manifest")
    build.add_argument("--host", required=True, choices=("prototype", "production"))
    build.add_argument("--contract", required=True)
    build.add_argument("--report", required=True)
    build.add_argument("--scene", required=True)
    build.add_argument("--replay", required=True)
    build.add_argument("--image", required=True)
    build.add_argument("--supplement", required=True)
    build.add_argument("--build-receipt", required=True)
    build.add_argument("--runtime-receipt", required=True)
    build.add_argument("--prototype-capture-derivation")
    build.add_argument("--role", required=True, choices=("baseline", "candidate"))
    build.add_argument("--output", required=True)
    return parser.parse_args(argv)


def build(args: argparse.Namespace, receipt: dict[str, Any]) -> None:
    host = args.host
    receipt["host"] = host
    receipt["role"] = args.role
    receipt["field_sources"] = _field_sources(host)

    if host == "prototype" and args.prototype_capture_derivation is None:
        raise UsageError("--prototype-capture-derivation: required for --host prototype")
    if host == "production" and args.prototype_capture_derivation is not None:
        raise UsageError("--prototype-capture-derivation: only valid for --host prototype")

    options = {
        "contract": args.contract,
        "report": args.report,
        "scene": args.scene,
        "replay": args.replay,
        "image": args.image,
        "supplement": args.supplement,
        "build_receipt": args.build_receipt,
        "runtime_receipt": args.runtime_receipt,
    }
    if args.prototype_capture_derivation is not None:
        options["prototype_capture_derivation"] = args.prototype_capture_derivation

    # Path policy for the output first: it is a usage error regardless of evidence.
    _check_cli_path_text(args.output, "--output")
    output = pathlib.Path(os.path.abspath(args.output))
    if not output.name.lower().endswith(".json"):
        raise UsageError("--output: manifest file name must end with .json")
    if os.path.lexists(output):
        raise UsageError("--output: refusing to overwrite an existing manifest")
    _check_no_link_chain(output.parent, "--output")

    paths: dict[str, pathlib.Path] = {}
    for name, text in options.items():
        paths[name] = _input_path(text, "--" + name.replace("_", "-"))

    # No two inputs may be the same file (including hard links / case aliases),
    # and the output must not alias any input path.
    seen: dict[tuple[Any, ...], str] = {}
    for name, path in paths.items():
        key = _file_key(path)
        if key in seen:
            raise UsageError(f"--{name.replace('_', '-')}: aliases --{seen[key].replace('_', '-')}")
        seen[key] = name
    output_norm = os.path.normcase(str(output))
    for name, path in paths.items():
        if os.path.normcase(str(path)) == output_norm:
            raise UsageError(f"--output: aliases --{name.replace('_', '-')}")

    image_path = paths["image"]
    try:
        same_directory = os.path.samefile(output.parent, image_path.parent)
    except OSError as error:
        raise UsageError(f"--output: cannot inspect output directory: {error}") from error
    if not same_directory:
        raise UsageError("--output: manifest must be created in the same directory as --image")
    if os.path.normcase(output.name) == os.path.normcase(image_path.name):
        raise UsageError("--output: aliases --image")

    raw = {name: _read(path, "--" + name.replace("_", "-")) for name, path in paths.items()}
    hashes = {name: _sha256(data) for name, data in raw.items()}
    receipt["inputs"] = {
        name: {"file_name": paths[name].name, "sha256": hashes[name], "bytes": len(raw[name])}
        for name in sorted(paths)
    }
    for name in ("build_receipt", "runtime_receipt", "prototype_capture_derivation"):
        if name in raw and not raw[name]:
            raise EvidenceError(f"--{name.replace('_', '-')}: receipt file is empty")
    receipt_hashes = [hashes[name] for name in ("build_receipt", "runtime_receipt",
                                                "prototype_capture_derivation") if name in hashes]
    if len(set(receipt_hashes)) != len(receipt_hashes):
        raise EvidenceError("receipts: build, runtime and derivation receipts must be distinct evidence")

    contract, contract_hash = _validate_contract(raw["contract"])
    receipt["contract"] = {"contract_id": contract["contract_id"], "sha256": contract_hash}

    _expect(hashes["scene"], SCENE_SHA256, "--scene SHA-256")
    _expect(hashes["replay"], REPLAY_SHA256, "--replay SHA-256")
    image_sha256 = hashes["image"]
    receipt["image"] = _validate_image(raw["image"], image_path)

    report = _object(load_strict_json(raw["report"], "report"), "report")
    if host == "prototype":
        observed = _validate_prototype_report(report)
    else:
        observed = _validate_production_report(report, image_sha256, hashes["replay"])

    supplement = _validate_supplement(load_strict_json(raw["supplement"], "supplement"), host)
    _expect(supplement["build_receipt_sha256"], hashes["build_receipt"],
            "supplement.build_receipt_sha256 (vs --build-receipt)")
    _expect(supplement["runtime_receipt_sha256"], hashes["runtime_receipt"],
            "supplement.runtime_receipt_sha256 (vs --runtime-receipt)")
    if host == "prototype":
        _expect(supplement["capture_derivation_receipt_sha256"], hashes["prototype_capture_derivation"],
                "supplement.capture_derivation_receipt_sha256 (vs --prototype-capture-derivation)")
        _validate_derivation(
            load_strict_json(raw["prototype_capture_derivation"], "capture_derivation"),
            supplement["build_revision"], image_sha256, hashes["replay"],
        )

    runtime = {
        "platform": supplement["platform"],
        "architecture": supplement["architecture"],
        "engine": observed["engine"],
        "engine_version": ENGINE_VERSION,
        "backend": BACKEND,
        "graphics_api": observed["graphics_api"],
        "device": f"{observed['vendor']} {observed['adapter_name']}",
        "driver": observed["vendor"],
        "driver_version": supplement["driver_version"],
        "build_revision": supplement["build_revision"],
    }
    manifest = {
        "schema_version": 1,
        "contract_sha256": contract_hash,
        "role": args.role,
        "image": {"path": image_path.name, "sha256": image_sha256},
        "identity": copy.deepcopy(FROZEN_IDENTITY),
        "runtime": runtime,
    }
    manifest_bytes = serialise_manifest(manifest)
    _validate_with_comparator(manifest_bytes, image_path, image_path.name,
                              contract, contract_hash, args.role)

    _exclusive_write(output, manifest_bytes)
    try:
        capture_compare.load_capture(output, contract, contract_hash, args.role)
    except capture_compare.HarnessError as error:
        # The image or directory changed between validation and creation.
        try:
            output.unlink()
        except OSError:
            pass
        raise EvidenceError(f"post-write comparator validation failed; manifest removed: {error}") from error

    receipt["manifest"] = {
        "file_name": output.name,
        "sha256": _sha256(manifest_bytes),
        "written": True,
        "runtime": runtime,
    }
    receipt["status"] = "assembled"


def run(argv: list[str] | None = None) -> int:
    receipt: dict[str, Any] = {
        "schema_version": 1,
        "tool": TOOL,
        "command": "build",
        "status": "error",
        "visual_comparison": False,
        "acceptance": False,
        "scope": "manifest-assembly-only",
        "human_review_gaps": list(HUMAN_REVIEW_GAPS),
    }
    exit_code = 2
    try:
        args = parse_args(list(sys.argv[1:] if argv is None else argv))
        build(args, receipt)
        exit_code = 0
    except EvidenceError as error:
        receipt["status"] = "failed"
        receipt["errors"] = [str(error)]
        exit_code = 1
    except UsageError as error:
        receipt["status"] = "error"
        receipt["errors"] = [str(error)]
        exit_code = 2
    except (OSError, ValueError) as error:
        receipt["status"] = "error"
        receipt["errors"] = [f"tool error: {error}"]
        exit_code = 2
    except Exception as error:  # noqa: BLE001 - stdout must stay one receipt
        receipt["status"] = "error"
        receipt["errors"] = [f"tool error: {type(error).__name__}: {error}"]
        exit_code = 2
    receipt["exit_code"] = exit_code
    if exit_code != 0:
        receipt.setdefault("manifest", {"written": False})
    print(json.dumps(receipt, indent=2, sort_keys=True, ensure_ascii=False))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(run())
