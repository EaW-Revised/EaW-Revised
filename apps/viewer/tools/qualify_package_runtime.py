#!/usr/bin/env python3
"""Qualify an already exported viewer package with its graphical runtime exercise.

The harness never exports, builds or copies anything into the package. It
launches the packaged executable from the package directory on an explicitly
requested graphical backend, bounded by a timeout, and retains every piece of
evidence (command, stdout, stderr, exit code, report, PNG, package hashes
before and after) in a fresh directory under the ignored ``out/`` tree. Any
missing, stale or malformed evidence, false lifecycle field, hash change,
nonzero exit, timeout or unexpected engine diagnostic fails the run.

Only a real launch of an exported package produces package runtime evidence.
The receipt records the host operating system (including WSL) so Windows and
Linux evidence are never conflated; synthetic unit runs of this module prove
the harness, not a package.
"""

# No postponed annotations: the dataclasses below must also work when this
# module is loaded by path (importlib) without a sys.modules entry.
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import platform
import re
import signal
import struct
import subprocess
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple


def _load_export_package() -> Any:
    path = Path(__file__).resolve().with_name("export_package.py")
    spec = importlib.util.spec_from_file_location("eawr_export_package", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


export_package = _load_export_package()

ROOT = export_package.ROOT
PLATFORMS: Dict[str, Dict[str, str]] = export_package.PLATFORMS
PINNED_GODOT_IDENTITY: str = export_package.PINNED_GODOT_IDENTITY
PINNED_GODOT_VERSION: str = export_package.PINNED_GODOT_VERSION
PINNED_GODOT_BUILD: str = export_package.PINNED_GODOT_BUILD
sha256_file = export_package.sha256
command_text = export_package.command_text
resolve_output_path = export_package.resolve_output_path

RECEIPT_SCHEMA_VERSION = 1
RECEIPT_KIND = "eawr-viewer-package-runtime-receipt"
EVIDENCE_CLASS = "exported-package-graphical-runtime"
DEFAULT_TIMEOUT_SECONDS = 180.0
MAX_TIMEOUT_SECONDS = 1800.0
# Seconds of filesystem timestamp slack when proving the report is fresh.
FRESHNESS_SLACK_SECONDS = 2.0
MAX_REPORT_BYTES = 1024 * 1024
MAX_CAPTURE_BYTES = 64 * 1024 * 1024
# 8-bit PNG color types the viewer capture may use, mapped to channel counts.
PNG_CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}
MAX_DECODED_CAPTURE_BYTES = 256 * 1024 * 1024

# The editor and the Windows template are the official build; the pinned Linux
# templates are the locally built templates of the same commit accepted in P0,
# which identify as custom builds. Each package must report its own identity.
CUSTOM_GODOT_IDENTITY = f"{PINNED_GODOT_VERSION.replace('-', '.')}.custom_build.{PINNED_GODOT_BUILD}"
ENGINE_IDENTITIES: Dict[str, str] = {
    "windows-x86_64": PINNED_GODOT_IDENTITY,
    "linux-x86_64": CUSTOM_GODOT_IDENTITY,
    "linux-arm64": CUSTOM_GODOT_IDENTITY,
}
ENGINE_BANNER = f"Godot Engine v{PINNED_GODOT_IDENTITY} - https://godotengine.org"
REPORT_ENGINE = f"Godot {PINNED_GODOT_VERSION}"
REPORT_VERSIONS = {"godot": PINNED_GODOT_VERSION, "godot_cpp": "10.0.0-stable", "material_schema": 1}
RUNTIME_STATUS = "renderer_runtime_exercise_passed"
EXPECTED_PASS_ORDER = ["opaque", "alpha-tested", "transparent", "post"]
MIN_SCENE_SWITCHES = 40
RUNTIME_VIEWPORT = {"width": 1280, "height": 720}
LIFECYCLE_TRUE_FIELDS = (
    "renderer_runtime_exercise",
    "runtime_capture_verified",
    "runtime_overlap_verified",
    "runtime_post_dependency_verified",
    # Also covers the live unknown material route/pass rejections.
    "runtime_failed_upload_registry_clean",
    "runtime_shutdown_resources_empty",
)
OTHER_MODE_FALSE_FIELDS = ("tactical_camera_verified", "atlas_overlay_verified")
RUNTIME_REPORT_KEYS = frozenset({
    "schema_version", "status", "failure", "backend", "versions", "scene_sha256", "replay",
    "snapshot_count", "capture_sha256", "capture_identity", "model", "texture",
    "material_program", "animation", "animation_time_seconds", "skin_palette_bound",
    "animation_capture_verified", "renderer_runtime_exercise", "runtime_capture_verified",
    "runtime_overlap_verified", "runtime_post_dependency_verified",
    "runtime_failed_upload_registry_clean", "runtime_scene_switch_count",
    "runtime_shutdown_resources_empty", "tactical_camera_verified", "atlas_overlay_verified",
    "pass_order",
})
# Written by viewers since #184/#220; packaged viewers built before it omit it.
RUNTIME_REPORT_OPTIONAL_KEYS = frozenset({"render_profile"})
BACKEND_KEYS = frozenset({"engine", "rendering_method", "adapter_vendor", "adapter_name", "driver_api"})
SHA256_HEX = re.compile(r"[0-9a-f]{64}")
# viewer_host.cpp hashes this fixed identity as the runtime exercise's scene.
RUNTIME_SCENE_IDENTITY = "eawr-renderer-overlap-v2:opaque,alpha-tested,transparent,post:lifecycle-v1"
RUNTIME_SCENE_SHA256 = hashlib.sha256(RUNTIME_SCENE_IDENTITY.encode("ascii")).hexdigest()
# Stdout markers printed by the exercise. The first proves the live unknown
# route/pass rejections ran (their report field predates them); the second
# proves every check passed before the report write was attempted.
REJECTION_MARKER = "EAWR runtime: unknown material route and pass rejected; registry unchanged"
PASSED_MARKER = "EAWR runtime: all exercise checks passed; persisting report"
RUNTIME_MARKERS = (REJECTION_MARKER, PASSED_MARKER)

# The runtime exercise deliberately submits one uncompilable shader. These are
# the only engine diagnostics a passing run may print; each must appear.
EXPECTED_ENGINE_DIAGNOSTICS = (
    "SHADER ERROR: Unknown identifier in expression: 'unknown_runtime_identifier'.",
    "ERROR: Shader compilation failed.",
)
ENGINE_DIAGNOSTIC_PREFIXES = (
    "ERROR:", "WARNING:", "SCRIPT ERROR:", "SHADER ERROR:", "USER ERROR:", "USER WARNING:",
    "USER SCRIPT ERROR:", "USER SHADER ERROR:",
)
SHUTDOWN_FAILURE_PATTERNS = (
    re.compile(r"leaked", re.IGNORECASE),
    re.compile(r"orphan", re.IGNORECASE),
    re.compile(r"ObjectDB instances"),
    re.compile(r"Program crashed|CrashHandler|Segmentation fault|signal \d+", re.IGNORECASE),
    re.compile(r"^EAWR viewer "),
)

# Graphical backends this harness can verify end to end, with the platforms
# each driver exists on. The project selects Forward+, which Godot runs on the
# RenderingDevice drivers (vulkan, d3d12). The engine's own device banner is matched
# against the report: RenderingDevice prints "<api> <version> - Forward+ -
# Using Device #<index>: <vendor> - <name>" (servers/rendering/rendering_device.cpp).
RENDERING_DRIVERS: Dict[str, Dict[str, Any]] = {
    "vulkan": {
        "rendering_method": "forward_plus",
        "platforms": ("windows-x86_64", "linux-x86_64", "linux-arm64"),
        "banner": re.compile(
            r"^Vulkan (?P<api>\S+) - Forward\+ - Using Device #\d+: "
            r"(?P<vendor>.+?) - (?P<adapter>.+)$"),
    },
    "d3d12": {
        "rendering_method": "forward_plus",
        "platforms": ("windows-x86_64",),
        "banner": re.compile(
            r"^D3D12 (?P<api>\S+) - Forward\+ - Using Device #\d+: "
            r"(?P<vendor>.+?) - (?P<adapter>.+)$"),
    },
}
DISPLAY_DRIVERS: Dict[str, Tuple[str, ...]] = {
    "windows-x86_64": ("windows",),
    "linux-x86_64": ("x11", "wayland"),
    "linux-arm64": ("x11", "wayland"),
}
HOST_REQUIREMENTS: Dict[str, Tuple[str, Tuple[str, ...]]] = {
    "windows-x86_64": ("Windows", ("amd64", "x86_64")),
    "linux-x86_64": ("Linux", ("x86_64", "amd64")),
    "linux-arm64": ("Linux", ("aarch64", "arm64")),
}
REPORT_PERSISTENCE_MESSAGE = "EAWR viewer report output could not be opened"


class QualificationError(Exception):
    """A precondition failure detected before any package process was launched."""


# ----------------------------------------------------------- shared checks

def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def verify_runtime_report(report: Any) -> List[str]:
    """Return every contract violation in a runtime-exercise report.

    Shared by the package harness and the source-project runtime test so both
    hold the report to the same lifecycle contract.
    """
    if not isinstance(report, dict):
        return ["report: expected a JSON object"]
    failures: List[str] = []
    keys = set(report)
    missing = sorted(RUNTIME_REPORT_KEYS - keys)
    unexpected = sorted(keys - RUNTIME_REPORT_KEYS - RUNTIME_REPORT_OPTIONAL_KEYS)
    if missing:
        failures.append(f"report: missing keys {missing}")
    if unexpected:
        failures.append(f"report: unexpected keys {unexpected}")
    if report.get("schema_version") != 1 or not _is_int(report.get("schema_version")):
        failures.append(f"report.schema_version: expected 1, observed {report.get('schema_version')!r}")
    if report.get("status") != RUNTIME_STATUS:
        failures.append(f"report.status: expected {RUNTIME_STATUS!r}, observed {report.get('status')!r}")
    if report.get("failure") != "":
        failures.append(f"report.failure: expected empty, observed {report.get('failure')!r}")
    for key in LIFECYCLE_TRUE_FIELDS:
        if report.get(key) is not True:
            failures.append(f"report.{key}: expected true, observed {report.get(key)!r}")
    for key in OTHER_MODE_FALSE_FIELDS:
        if report.get(key) is not False:
            failures.append(f"report.{key}: expected false, observed {report.get(key)!r}")
    switches = report.get("runtime_scene_switch_count")
    if not _is_int(switches) or switches < MIN_SCENE_SWITCHES:
        failures.append(
            f"report.runtime_scene_switch_count: expected integer >= {MIN_SCENE_SWITCHES}, "
            f"observed {switches!r}")
    if report.get("pass_order") != EXPECTED_PASS_ORDER:
        failures.append(
            f"report.pass_order: expected {EXPECTED_PASS_ORDER}, observed {report.get('pass_order')!r}")
    capture = report.get("capture_sha256")
    if not isinstance(capture, str) or not SHA256_HEX.fullmatch(capture):
        failures.append(f"report.capture_sha256: expected lowercase SHA-256 hex, observed {capture!r}")
    if report.get("scene_sha256") != RUNTIME_SCENE_SHA256:
        failures.append(f"report.scene_sha256: expected runtime identity {RUNTIME_SCENE_SHA256}, "
                        f"observed {report.get('scene_sha256')!r}")
    if report.get("versions") != REPORT_VERSIONS:
        failures.append(f"report.versions: expected {REPORT_VERSIONS}, observed {report.get('versions')!r}")
    backend = report.get("backend")
    if not isinstance(backend, dict) or set(backend) != BACKEND_KEYS:
        failures.append(f"report.backend: expected keys {sorted(BACKEND_KEYS)}")
    else:
        if backend.get("engine") != REPORT_ENGINE:
            failures.append(f"report.backend.engine: expected {REPORT_ENGINE!r}, observed {backend.get('engine')!r}")
        for key in sorted(BACKEND_KEYS):
            if not isinstance(backend.get(key), str) or not backend.get(key):
                failures.append(f"report.backend.{key}: expected non-empty text")
        method = backend.get("rendering_method")
        if isinstance(method, str) and method in {"headless", "unavailable", "unknown"}:
            failures.append("report.backend.rendering_method: not a graphical backend")
    identity = report.get("capture_identity")
    viewport = identity.get("viewport") if isinstance(identity, dict) else None
    if viewport != RUNTIME_VIEWPORT:
        failures.append(f"report.capture_identity.viewport: expected {RUNTIME_VIEWPORT}, observed {viewport!r}")
    return failures


def is_fatal_diagnostic(line: str) -> bool:
    """True for a leak, crash or shutdown diagnostic, which no waiver can cover."""
    return any(pattern.search(line) for pattern in SHUTDOWN_FAILURE_PATTERNS)


def is_waivable_warning(line: str) -> bool:
    return line.startswith("WARNING:") and not is_fatal_diagnostic(line)


def scan_engine_output(text: str, allowed_warnings: Sequence[str] = (),
                       observed_allowed: Optional[Dict[str, int]] = None) -> Tuple[List[str], List[str]]:
    """Return (missing expected diagnostics, unexpected diagnostic lines).

    ``allowed_warnings`` are exact ``WARNING:`` lines explicitly waived for a
    host (for example a driver that cannot change V-Sync); their counts are
    written to ``observed_allowed``. Errors and leak, crash or shutdown
    diagnostics can never be waived.
    """
    seen: Dict[str, int] = {line: 0 for line in EXPECTED_ENGINE_DIAGNOSTICS}
    waived = {line: 0 for line in allowed_warnings if is_waivable_warning(line)}
    unexpected: List[str] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if is_fatal_diagnostic(line):
            unexpected.append(line)
            continue
        if line in seen:
            seen[line] += 1
            continue
        if line in waived:
            waived[line] += 1
            continue
        if line.startswith(ENGINE_DIAGNOSTIC_PREFIXES):
            unexpected.append(line)
    missing = [line for line, count in seen.items() if count == 0]
    if observed_allowed is not None:
        observed_allowed.update(waived)
    return missing, unexpected


def missing_runtime_markers(stdout: str, markers: Sequence[str] = RUNTIME_MARKERS) -> List[str]:
    """Return the exercise markers that do not appear exactly once in stdout."""
    lines = [line.strip() for line in stdout.splitlines()]
    return [marker for marker in markers if lines.count(marker) != 1]


def engine_banner(stdout: str, identity: str) -> Optional[str]:
    """Return the single engine banner for ``identity``, or None.

    Custom builds append their build timestamp before the URL.
    """
    pattern = re.compile(r"^Godot Engine v" + re.escape(identity)
                         + r"(?: \(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} UTC\))?"
                         + r" - https://godotengine\.org$")
    banners = [line.strip() for line in stdout.splitlines() if line.startswith("Godot Engine v")]
    if len(banners) != 1 or not pattern.match(banners[0]):
        return None
    return banners[0]


def parse_backend_banner(stdout: str, rendering_driver: str) -> Optional[Dict[str, str]]:
    pattern = RENDERING_DRIVERS[rendering_driver]["banner"]
    matches = [pattern.match(line.strip()) for line in stdout.splitlines()]
    found = [match for match in matches if match]
    if len(found) != 1:
        return None
    return {
        "api": found[0].group("api"),
        "vendor": found[0].group("vendor"),
        "adapter": found[0].group("adapter"),
        "line": found[0].group(0),
    }


def verify_backend(stdout: str, report: Mapping[str, Any], rendering_driver: str,
                   engine_identity: str = PINNED_GODOT_IDENTITY) -> List[str]:
    """Match the engine banner and the device banner against the report."""
    failures: List[str] = []
    if engine_banner(stdout, engine_identity) is None:
        failures.append(f"stdout: expected exactly one engine banner for {engine_identity!r}")
    banner = parse_backend_banner(stdout, rendering_driver)
    if banner is None:
        failures.append(f"stdout: expected exactly one {rendering_driver} device banner")
        return failures
    backend = report.get("backend")
    if not isinstance(backend, dict):
        return failures + ["report.backend: missing; cannot match device banner"]
    expected_method = RENDERING_DRIVERS[rendering_driver]["rendering_method"]
    comparisons = (
        ("rendering_method", expected_method),
        ("driver_api", banner["api"]),
        ("adapter_vendor", banner["vendor"]),
        ("adapter_name", banner["adapter"]),
    )
    for key, expected in comparisons:
        if backend.get(key) != expected:
            failures.append(f"report.backend.{key}: expected {expected!r} from engine output, "
                            f"observed {backend.get(key)!r}")
    return failures


def _check_png_image_data(idat: bytes, width: int, height: int, channels: int) -> None:
    """Require the IDAT stream to decode to exactly one filtered scanline per row."""
    stride = 1 + width * channels
    expected = stride * height
    if expected > MAX_DECODED_CAPTURE_BYTES:
        raise ValueError(f"capture: PNG dimensions {width}x{height} exceed the decoded size limit")
    decoder = zlib.decompressobj()
    try:
        # Bounded so a hostile stream cannot inflate beyond one extra byte.
        pixels = decoder.decompress(idat, expected + 1)
    except zlib.error as error:
        raise ValueError(f"capture: PNG image data is not a valid zlib stream ({error})") from error
    if len(pixels) > expected or decoder.unconsumed_tail:
        raise ValueError(f"capture: PNG image data decodes to more than {expected} bytes")
    if not decoder.eof:
        raise ValueError("capture: PNG image data zlib stream is truncated")
    if decoder.unused_data:
        raise ValueError("capture: PNG image data has trailing bytes after the zlib stream")
    if len(pixels) != expected:
        raise ValueError(f"capture: PNG image data decodes to {len(pixels)} bytes, expected {expected}")
    for row in range(height):
        kind = pixels[row * stride]
        if kind > 4:
            raise ValueError(f"capture: PNG scanline {row} has filter type {kind}")


def read_png_header(data: bytes) -> Tuple[int, int]:
    """Validate PNG structure, chunk CRCs and decoded image data; return (width, height).

    Only the format the viewer writes is supported: 8-bit grayscale, gray+alpha,
    RGB or RGBA, non-interlaced.
    """
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("capture: missing PNG signature")
    offset = 8
    chunks: List[bytes] = []
    idat: List[bytes] = []
    width = height = channels = 0
    while offset < len(data):
        if offset + 12 > len(data):
            raise ValueError("capture: truncated PNG chunk")
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        end = offset + 12 + length
        if end > len(data):
            raise ValueError("capture: truncated PNG chunk payload")
        payload = data[offset + 8:offset + 8 + length]
        crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if zlib.crc32(kind + payload) & 0xFFFFFFFF != crc:
            raise ValueError(f"capture: PNG chunk {kind!r} CRC mismatch")
        if not chunks:
            if kind != b"IHDR" or length != 13:
                raise ValueError("capture: first PNG chunk is not IHDR")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload)
            if not (0 < width <= 0x7FFFFFFF and 0 < height <= 0x7FFFFFFF):
                raise ValueError(f"capture: invalid PNG dimensions {width}x{height}")
            channels = PNG_CHANNELS.get(color, 0)
            if depth != 8 or not channels or (compression, filtering, interlace) != (0, 0, 0):
                raise ValueError(
                    f"capture: unsupported PNG format (bit depth {depth}, color type {color}, "
                    f"compression {compression}, filter {filtering}, interlace {interlace})")
        if kind == b"IDAT":
            if idat and chunks[-1] != b"IDAT":
                raise ValueError("capture: PNG IDAT chunks are not consecutive")
            idat.append(payload)
        chunks.append(kind)
        offset = end
        if kind == b"IEND":
            break
    if not chunks or chunks[-1] != b"IEND" or offset != len(data):
        raise ValueError("capture: PNG does not end with IEND")
    if not idat:
        raise ValueError("capture: PNG has no image data")
    _check_png_image_data(b"".join(idat), width, height, channels)
    return width, height


# ----------------------------------------------------------- process runner

@dataclass
class ProcessResult:
    argv: List[str]
    cwd: str
    started_utc: str
    finished_utc: str
    started_ns: int
    duration_seconds: float
    exit_code: Optional[int]
    timed_out: bool
    stdout_path: Path
    stderr_path: Path


def _utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def _kill_tree(process: "subprocess.Popen[bytes]") -> None:
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    try:
        process.kill()
    except OSError:
        pass
    process.wait()


def launch(argv: Sequence[str], cwd: Path, stdout_path: Path, stderr_path: Path,
           timeout: float) -> ProcessResult:
    """Run argv from cwd with separate stdout/stderr files and a hard timeout."""
    started_utc = _utc_now()
    started_ns = time.time_ns()
    clock = time.monotonic()
    timed_out = False
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        kwargs: Dict[str, Any] = {}
        if os.name != "nt":
            kwargs["start_new_session"] = True
        process = subprocess.Popen([str(item) for item in argv], cwd=str(cwd), stdin=subprocess.DEVNULL,
                                   stdout=stdout, stderr=stderr, **kwargs)
        try:
            exit_code: Optional[int] = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            _kill_tree(process)
            exit_code = None
    return ProcessResult(
        argv=[str(item) for item in argv],
        cwd=str(cwd),
        started_utc=started_utc,
        finished_utc=_utc_now(),
        started_ns=started_ns,
        duration_seconds=round(time.monotonic() - clock, 3),
        exit_code=exit_code,
        timed_out=timed_out,
        stdout_path=stdout_path,
        stderr_path=stderr_path,
    )


def _read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8", errors="replace")


def _file_record(path: Path) -> Dict[str, Any]:
    return {"path": str(path), "sha256": sha256_file(path), "bytes": path.stat().st_size}


def _process_record(result: ProcessResult) -> Dict[str, Any]:
    return {
        "argv": result.argv,
        "text": command_text(result.argv),
        "cwd": result.cwd,
        "started_utc": result.started_utc,
        "finished_utc": result.finished_utc,
        "duration_seconds": result.duration_seconds,
        "exit_code": result.exit_code,
        "timed_out": result.timed_out,
        "stdout": _file_record(result.stdout_path),
        "stderr": _file_record(result.stderr_path),
    }


# ----------------------------------------------------------- package checks

def package_paths(package: Path, platform_name: str) -> Dict[str, Path]:
    spec = PLATFORMS[platform_name]
    executable = package / spec["executable"]
    return {
        "executable": executable,
        "extension": package / spec["extension"],
        "pck": package / f"{executable.stem}.pck",
    }


def package_hashes(package: Path, platform_name: str) -> Dict[str, Any]:
    paths = package_paths(package, platform_name)
    missing = [name for name, path in paths.items() if not path.is_file()]
    if missing:
        raise QualificationError(f"package is missing {missing} in {package}")
    entries = sorted(path.name + ("/" if path.is_dir() else "") for path in package.iterdir())
    return {
        "files": entries,
        "sha256": {name: sha256_file(path) for name, path in paths.items()},
    }


def verify_package_receipt(receipt_path: Path, platform_name: str,
                           hashes: Mapping[str, str]) -> Dict[str, Any]:
    """Link the run to the export receipt that produced this package."""
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise QualificationError(f"package receipt unreadable: {error}") from error
    validation = receipt.get("validation") if isinstance(receipt, dict) else None
    if not isinstance(validation, dict) or receipt.get("platform") != platform_name:
        raise QualificationError(f"package receipt {receipt_path} is not a {platform_name} export receipt")
    if receipt.get("godot_identity") != PINNED_GODOT_IDENTITY:
        raise QualificationError(
            f"package receipt engine {receipt.get('godot_identity')!r} is not {PINNED_GODOT_IDENTITY!r}")
    for name in ("executable", "extension", "pck"):
        if validation.get(f"{name}_sha256") != hashes[name]:
            raise QualificationError(
                f"package {name} SHA-256 {hashes[name]} does not match export receipt "
                f"{validation.get(f'{name}_sha256')!r}")
    return {"path": str(receipt_path), "sha256": sha256_file(receipt_path), "matched": True}


def host_identity() -> Dict[str, Any]:
    release = platform.release()
    return {
        "system": platform.system(),
        "release": release,
        "version": platform.version(),
        "machine": platform.machine(),
        "wsl": platform.system() == "Linux" and "microsoft" in release.lower(),
        "python": platform.python_version(),
    }


def check_host(platform_name: str, host: Mapping[str, Any]) -> None:
    system, machines = HOST_REQUIREMENTS[platform_name]
    if host["system"] != system or str(host["machine"]).lower() not in machines:
        raise QualificationError(
            f"{platform_name} package requires a native {system} {machines[0]} host; "
            f"observed {host['system']} {host['machine']}")


def prepare_evidence_dir(path: Path) -> Path:
    resolved = resolve_output_path(path, "evidence-dir")
    if resolved.exists():
        if not resolved.is_dir() or any(resolved.iterdir()):
            raise QualificationError(f"evidence directory is not fresh (exists and is not empty): {resolved}")
    resolved.mkdir(parents=True, exist_ok=True)
    return resolved


def check_report_fresh(report: Path, started_ns: int) -> Optional[str]:
    if not report.is_file():
        return f"report missing: {report}"
    modified = report.stat().st_mtime_ns
    if modified + int(FRESHNESS_SLACK_SECONDS * 1e9) < started_ns:
        return f"report is stale: modified before launch ({report})"
    return None


def load_report(path: Path) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    size = path.stat().st_size
    if size > MAX_REPORT_BYTES:
        return None, f"report exceeds {MAX_REPORT_BYTES} bytes"
    try:
        value = json.loads(path.read_bytes().decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as error:
        return None, f"report is malformed JSON: {error}"
    if not isinstance(value, dict):
        return None, "report is not a JSON object"
    return value, None


# ----------------------------------------------------------- qualification

@dataclass
class Qualification:
    platform: str
    package: Path
    evidence_dir: Path
    rendering_driver: str
    display_driver: str
    timeout: float = DEFAULT_TIMEOUT_SECONDS
    package_receipt: Optional[Path] = None
    probe_report_persistence: bool = False
    # Exact WARNING: lines waived for this host; recorded in the receipt and
    # rejected if not observed.
    allowed_warnings: Sequence[str] = field(default_factory=tuple)
    # Test-only launcher prefix (for example a Python interpreter and a fake
    # viewer script). Real qualification leaves it empty.
    command_prefix: Sequence[str] = field(default_factory=tuple)
    check_host_platform: bool = True


def _viewer_argv(config: Qualification, executable: Path, report: Path,
                 capture: Optional[Path]) -> List[str]:
    argv = [*config.command_prefix, str(executable),
            "--rendering-driver", config.rendering_driver,
            "--display-driver", config.display_driver,
            "--", "--eawr-renderer-runtime-test", "--eawr-report", str(report)]
    if capture is not None:
        argv += ["--eawr-capture", str(capture)]
    return argv


def _verify_runtime_run(result: ProcessResult, report_path: Path, capture_path: Path,
                        config: Qualification, receipt: Dict[str, Any]) -> List[str]:
    failures: List[str] = []
    if result.timed_out:
        failures.append(f"process timed out after {config.timeout} seconds")
    elif result.exit_code != 0:
        failures.append(f"process exited {result.exit_code}")
    stdout = _read_text(result.stdout_path)
    stderr = _read_text(result.stderr_path)
    observed_allowed: Dict[str, int] = {}
    missing, unexpected = scan_engine_output(stdout + "\n" + stderr, config.allowed_warnings,
                                             observed_allowed)
    receipt["allowed_engine_warnings"] = observed_allowed
    unused = [line for line, count in observed_allowed.items() if count == 0]
    if unused:
        failures.append(f"allowed engine warnings were not observed (stale waiver) {unused}")
    if missing:
        failures.append(f"engine output lacks expected compiler diagnostics {missing}")
    if unexpected:
        failures.append(f"unexpected engine/shutdown diagnostics {unexpected[:20]}")
    banner = parse_backend_banner(stdout, config.rendering_driver)
    identity = ENGINE_IDENTITIES[config.platform]
    receipt["engine"] = {"banner": engine_banner(stdout, identity), "identity": identity}
    receipt["device_banner"] = banner
    markers = missing_runtime_markers(stdout)
    if markers:
        failures.append(f"stdout lacks runtime exercise markers {markers}")

    stale = check_report_fresh(report_path, result.started_ns)
    if stale:
        return failures + [stale]
    report, error = load_report(report_path)
    receipt["report"] = _file_record(report_path)
    if report is None:
        return failures + [error or "report unreadable"]
    receipt["report"].update({
        "status": report.get("status"),
        "failure": report.get("failure"),
        "backend": report.get("backend"),
        "capture_sha256": report.get("capture_sha256"),
        "scene_sha256": report.get("scene_sha256"),
        "runtime_scene_switch_count": report.get("runtime_scene_switch_count"),
        "pass_order": report.get("pass_order"),
    })
    failures += verify_runtime_report(report)
    failures += verify_backend(stdout, report, config.rendering_driver, identity)

    if not capture_path.is_file():
        return failures + [f"capture missing: {capture_path}"]
    if check_report_fresh(capture_path, result.started_ns):
        failures.append(f"capture is stale: {capture_path}")
    if capture_path.stat().st_size > MAX_CAPTURE_BYTES:
        return failures + [f"capture exceeds {MAX_CAPTURE_BYTES} bytes"]
    data = capture_path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    try:
        width, height = read_png_header(data)
    except ValueError as error:
        return failures + [str(error)]
    receipt["capture"] = {"path": str(capture_path), "sha256": digest, "bytes": len(data),
                          "width": width, "height": height}
    if digest != report.get("capture_sha256"):
        failures.append(f"capture SHA-256 {digest} does not match report {report.get('capture_sha256')!r}")
    if {"width": width, "height": height} != RUNTIME_VIEWPORT:
        failures.append(f"capture is {width}x{height}, expected "
                        f"{RUNTIME_VIEWPORT['width']}x{RUNTIME_VIEWPORT['height']}")
    return failures


def _probe_report_persistence(config: Qualification, executable: Path,
                              receipt: Dict[str, Any]) -> List[str]:
    """Relaunch with an unwritable report target; the package must exit 2."""
    probe_dir = config.evidence_dir / "report-persistence-probe"
    target = probe_dir / "report-target"
    target.mkdir(parents=True)
    try:
        result = launch(_viewer_argv(config, executable, target, None), config.package,
                        probe_dir / "stdout.txt", probe_dir / "stderr.txt", config.timeout)
    except OSError as error:
        receipt["report_persistence_probe"] = {"launch_error": str(error)}
        return [f"report persistence probe could not be launched: {error}"]
    receipt["report_persistence_probe"] = _process_record(result)
    failures: List[str] = []
    # Every failing exercise path also exits 2 after a failed report write;
    # only the passed marker proves the passing path's persistence check ran.
    if missing_runtime_markers(_read_text(result.stdout_path)):
        failures.append("report persistence probe did not reach the passing path")
    if result.timed_out:
        failures.append("report persistence probe timed out")
    elif result.exit_code != 2:
        failures.append(f"report persistence probe exited {result.exit_code}, expected 2")
    stderr = _read_text(result.stderr_path) + _read_text(result.stdout_path)
    if REPORT_PERSISTENCE_MESSAGE not in stderr:
        failures.append("report persistence probe did not print the report open failure")
    if not target.is_dir() or any(target.iterdir()):
        failures.append("report persistence probe changed the unwritable report target")
    return failures


def qualify(config: Qualification) -> Tuple[bool, Dict[str, Any], Path]:
    """Run the qualification. Returns (passed, receipt, receipt path)."""
    if config.rendering_driver not in RENDERING_DRIVERS:
        raise QualificationError(f"unsupported rendering driver {config.rendering_driver!r}")
    if config.platform not in RENDERING_DRIVERS[config.rendering_driver]["platforms"]:
        raise QualificationError(
            f"rendering driver {config.rendering_driver!r} is not valid for {config.platform}")
    if config.display_driver not in DISPLAY_DRIVERS[config.platform]:
        raise QualificationError(
            f"display driver {config.display_driver!r} is not valid for {config.platform}")
    bad_waivers = [line for line in config.allowed_warnings if not line.startswith("WARNING:")]
    if bad_waivers:
        raise QualificationError(f"only exact WARNING: lines can be allowed, not {bad_waivers}")
    fatal_waivers = [line for line in config.allowed_warnings if not is_waivable_warning(line)]
    if fatal_waivers:
        raise QualificationError(f"leak, crash or shutdown diagnostics cannot be waived: {fatal_waivers}")
    if not 0 < config.timeout <= MAX_TIMEOUT_SECONDS:
        raise QualificationError(f"timeout must be in (0, {MAX_TIMEOUT_SECONDS}] seconds")
    host = host_identity()
    if config.check_host_platform:
        check_host(config.platform, host)
    package = config.package.resolve()
    if not package.is_dir():
        raise QualificationError(f"package directory not found: {package}")
    config.package = package
    before = package_hashes(package, config.platform)
    linked = (verify_package_receipt(config.package_receipt.resolve(), config.platform, before["sha256"])
              if config.package_receipt else None)
    evidence = prepare_evidence_dir(config.evidence_dir)
    config.evidence_dir = evidence
    report_path = evidence / "runtime-report.json"
    capture_path = evidence / "runtime-capture.png"
    receipt_path = evidence / "receipt.json"
    executable = package_paths(package, config.platform)["executable"]

    receipt: Dict[str, Any] = {
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "kind": RECEIPT_KIND,
        "evidence_class": EVIDENCE_CLASS if not config.command_prefix else "synthetic-harness-test",
        "verdict": "failed",
        "failures": [],
        "host": host,
        "platform": config.platform,
        "package": str(package),
        "backend_request": {"rendering_driver": config.rendering_driver,
                            "display_driver": config.display_driver},
        "timeout_seconds": config.timeout,
        "engine_warning_waivers": list(config.allowed_warnings),
        "package_receipt": linked,
        "hashes": {"before": before},
        "report_persistence_probe": None,
    }
    argv = _viewer_argv(config, executable, report_path, capture_path)
    try:
        result: Optional[ProcessResult] = launch(argv, package, evidence / "stdout.txt",
                                                 evidence / "stderr.txt", config.timeout)
    except OSError as error:
        result = None
        receipt["command"] = {"argv": argv, "text": command_text(argv), "cwd": str(package),
                              "launch_error": str(error)}
        failures = [f"package could not be launched: {error}"]
    if result is not None:
        receipt["command"] = _process_record(result)
        try:
            failures = _verify_runtime_run(result, report_path, capture_path, config, receipt)
        except (TypeError, ValueError, KeyError, AttributeError) as error:
            # Evidence the checks did not anticipate fails closed with a receipt.
            failures = [f"run evidence could not be verified: {type(error).__name__}: {error}"]
    if config.probe_report_persistence and result is not None:
        failures += _probe_report_persistence(config, executable, receipt)
    try:
        after = package_hashes(package, config.platform)
    except QualificationError as error:
        after = None
        failures.append(f"package changed during the run: {error}")
    receipt["hashes"]["after"] = after
    receipt["hashes"]["unchanged"] = after == before
    if after is not None and after != before:
        failures.append("package executable/extension/PCK hashes or file set changed during the run")
    receipt["failures"] = failures
    receipt["verdict"] = "passed" if not failures else "failed"
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return not failures, receipt, receipt_path


def default_evidence_dir(platform_name: str, rendering_driver: str) -> Path:
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return ROOT / "out" / "viewer-package-runtime" / platform_name / f"{stamp}-{rendering_driver}"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    parser.add_argument("--package", type=Path, required=True, help="exported package directory")
    parser.add_argument("--rendering-driver", choices=sorted(RENDERING_DRIVERS), required=True)
    parser.add_argument("--display-driver", required=True,
                        help="windows on Windows; x11 or wayland on Linux")
    parser.add_argument("--evidence-dir", type=Path,
                        help="fresh directory under out/ (default: timestamped under out/viewer-package-runtime)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--package-receipt", type=Path,
                        help="export_package.py package-receipt.json whose hashes must match")
    parser.add_argument("--probe-report-persistence", action="store_true",
                        help="also relaunch with an unwritable report target and require exit 2")
    parser.add_argument("--allow-engine-warning", action="append", default=[], metavar="LINE",
                        help="exact WARNING: line to waive on this host (repeatable; recorded "
                             "in the receipt; fails if not observed)")
    args = parser.parse_args(argv)
    config = Qualification(
        platform=args.platform,
        package=args.package,
        evidence_dir=args.evidence_dir or default_evidence_dir(args.platform, args.rendering_driver),
        rendering_driver=args.rendering_driver,
        display_driver=args.display_driver,
        timeout=args.timeout,
        package_receipt=args.package_receipt,
        probe_report_persistence=args.probe_report_persistence,
        allowed_warnings=tuple(args.allow_engine_warning),
    )
    try:
        passed, receipt, receipt_path = qualify(config)
    except (QualificationError, ValueError, OSError) as error:
        print(f"qualify_package_runtime.py: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"verdict": receipt["verdict"], "failures": receipt["failures"],
                      "receipt": str(receipt_path)}, indent=2))
    return 0 if passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
