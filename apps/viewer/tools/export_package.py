#!/usr/bin/env python3
"""Export and optionally smoke-test one production viewer package.

The helper deliberately consumes the checked-in export preset and template
manifest. It never copies game data into the project and writes all receipts
under the ignored ``out/`` tree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import sys
import urllib.request
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
PROJECT = ROOT / "apps" / "viewer" / "project"
TEMPLATES = ROOT / "apps" / "viewer" / "export-templates.json"
DEPENDENCIES = ROOT / "apps" / "viewer" / "dependencies.json"
OUT_ROOT = (ROOT / "out").resolve()
LICENSE_CACHE = ROOT / "out" / "godot" / "licenses"
# Every package carries this project's licence and notices, and the engine's and
# godot-cpp's licence texts (Godot's COPYRIGHT.txt lists its bundled third-party code).
LICENSES_DIR = "licenses"
PROJECT_LICENSE_FILES = {"LICENSE": ROOT / "LICENSE", "THIRD_PARTY_NOTICES.md": ROOT / "THIRD_PARTY_NOTICES.md"}

PINNED_GODOT_VERSION = "4.7.2-stable"
PINNED_GODOT_BUILD = "ed1daf0bf"
PINNED_GODOT_IDENTITY = f"4.7.2.stable.official.{PINNED_GODOT_BUILD}"
GODOT_VERSION_TIMEOUT_SECONDS = 10
MAX_GODOT_VERSION_OUTPUT = 4096

PLATFORMS: dict[str, dict[str, str]] = {
    "windows-x86_64": {
        "preset": "Windows x86_64",
        "template": "Windows x86_64",
        "executable": "eawr-viewer.exe",
        "extension": "libeawr_viewer.windows.template_release.x86_64.dll",
    },
    "linux-x86_64": {
        "preset": "Linux x86_64",
        "template": "Linux x86_64",
        "executable": "eawr-viewer.x86_64",
        "extension": "libeawr_viewer.linux.template_release.x86_64.so",
    },
    "linux-arm64": {
        "preset": "Linux ARM64",
        "template": "Linux ARM64",
        "executable": "eawr-viewer.arm64",
        "extension": "libeawr_viewer.linux.template_release.arm64.so",
    },
}

REQUIRED_PCK_ENTRIES = {
    ".godot/extension_list.cfg",
    ".godot/global_script_class_cache.cfg",
    ".godot/uid_cache.bin",
    "common/original-v1.eawr-replay",
    "common/scene.json",
    # Project-authored, non-retail camera bindings (P1-09) packed by the
    # preset's config/*.json include filter.
    "config/camera-bindings.json",
    "eawr_viewer.gdextension",
    "main.tscn.remap",
    "project.binary",
}
FORBIDDEN_PCK_SUFFIXES = {
    ".ala",
    ".alo",
    ".dds",
    ".fx",
    ".glsl",
    ".hlsl",
    ".png",
    ".spv",
    ".tga",
    ".wgsl",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_text(argv: list[str]) -> str:
    return shlex.join([str(item) for item in argv])


def resolve_output_path(path: Path, label: str) -> Path:
    """Resolve an output root and reject paths outside the ignored repository tree."""
    try:
        resolved = path.resolve()
        OUT_ROOT.relative_to(ROOT)
        resolved.relative_to(OUT_ROOT)
    except (OSError, ValueError) as error:
        raise ValueError(
            f"{label} must resolve within the repository out/ tree: {path}"
        ) from error
    return resolved


def _version_from_identity(identity: str) -> str:
    parts = identity.split(".")
    if len(parts) < 4:
        raise ValueError(f"unparseable Godot identity: {identity!r}")
    return ".".join(parts[:3]) + f"-{parts[3]}"


def validate_godot_identity(godot: Path) -> tuple[str, str]:
    """Run a bounded version probe and return (build identity, release version)."""
    if not godot.is_file():
        raise FileNotFoundError(f"Godot executable not found: {godot}")
    try:
        result = subprocess.run(
            [str(godot), "--version"],
            text=True,
            capture_output=True,
            check=False,
            timeout=GODOT_VERSION_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError(f"Godot --version probe failed for {godot}: {error}") from error
    output = (result.stdout or "") + (result.stderr or "")
    if len(output) > MAX_GODOT_VERSION_OUTPUT:
        raise ValueError(
            f"Godot --version output exceeded {MAX_GODOT_VERSION_OUTPUT} characters"
        )
    if result.returncode:
        raise ValueError(f"Godot --version exited {result.returncode} for {godot}")
    identities = [line.strip() for line in output.splitlines() if line.strip()]
    if PINNED_GODOT_IDENTITY not in identities:
        observed = identities[0] if identities else "<no version output>"
        raise ValueError(
            f"Godot executable does not report pinned identity "
            f"{PINNED_GODOT_IDENTITY!r}; observed {observed!r}"
        )
    identity = next(line for line in identities if line == PINNED_GODOT_IDENTITY)
    version = _version_from_identity(identity)
    if version != PINNED_GODOT_VERSION:
        raise ValueError(
            f"pinned Godot identity/version contract is inconsistent: "
            f"{identity!r} -> {version!r}, expected {PINNED_GODOT_VERSION!r}"
        )
    return identity, version


def verify_smoke_engine_version(smoke: dict[str, Any], expected_version: str) -> None:
    """Reject a smoke receipt that reports a different Godot engine version."""
    reported: list[tuple[str, str]] = []
    for key in ("godot_version", "engine_version"):
        if key in smoke:
            reported.append((key, str(smoke[key])))
    versions = smoke.get("versions")
    if isinstance(versions, dict) and "godot" in versions:
        reported.append(("versions.godot", str(versions["godot"])))
    backend = smoke.get("backend")
    if isinstance(backend, dict) and "engine" in backend:
        reported.append(("backend.engine", str(backend["engine"])))
    for field, value in reported:
        normalized = value.strip()
        if normalized.startswith("Godot "):
            normalized = normalized[len("Godot "):].strip()
        if normalized not in {expected_version, PINNED_GODOT_IDENTITY}:
            raise ValueError(
                f"smoke receipt {field} does not match pinned Godot version "
                f"{expected_version!r}: observed {value!r}"
            )


def run(argv: list[str], cwd: Path, log: Path) -> subprocess.CompletedProcess[str]:
    log.parent.mkdir(parents=True, exist_ok=True)
    print(f"$ {command_text(argv)}")
    result = subprocess.run(
        [str(item) for item in argv],
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    log.write_text(result.stdout, encoding="utf-8")
    print(result.stdout, end="")
    print(f"exit={result.returncode}")
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}); see {log}")
    return result


def pck_entries(path: Path) -> list[str]:
    with path.open("rb") as stream:
        header = stream.read(40)
        if len(header) != 40 or header[:4] != b"GDPC":
            raise ValueError(f"{path}: unsupported or invalid Godot PCK header")
        if struct.unpack_from("<I", header, 4)[0] != 4:
            raise ValueError(f"{path}: expected Godot PCK format 4")
        stream.seek(struct.unpack_from("<Q", header, 32)[0])
        count_raw = stream.read(4)
        if len(count_raw) != 4:
            raise ValueError(f"{path}: truncated PCK directory")
        count = struct.unpack("<I", count_raw)[0]
        if count > 100_000:
            raise ValueError(f"{path}: unreasonable PCK file count {count}")
        entries: list[str] = []
        for _ in range(count):
            length_raw = stream.read(4)
            if len(length_raw) != 4:
                raise ValueError(f"{path}: truncated PCK path length")
            length = struct.unpack("<I", length_raw)[0]
            raw_path = stream.read(length)
            if len(raw_path) != length:
                raise ValueError(f"{path}: truncated PCK path")
            entries.append(raw_path.rstrip(b"\0").decode("utf-8"))
            if len(stream.read(36)) != 36:
                raise ValueError(f"{path}: truncated PCK entry")
        return entries


def pinned_license_files() -> list[dict[str, str]]:
    return json.loads(DEPENDENCIES.read_text(encoding="utf-8"))["license_files"]


def expected_license_names() -> set[str]:
    return set(PROJECT_LICENSE_FILES) | {item["name"] for item in pinned_license_files()}


def cached_license(item: dict[str, str]) -> Path:
    """The pinned licence file from the ignored cache, fetched once and checked by SHA-256."""
    path = LICENSE_CACHE / item["name"]
    if not path.is_file() or sha256(path) != item["sha256"]:
        LICENSE_CACHE.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(item["url"], timeout=60) as response:  # noqa: S310 (pinned https URL)
            data = response.read()
        if hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise ValueError(f"licence file {item['name']} does not match its pinned SHA-256")
        path.write_bytes(data)
    return path


def bundle_licenses(package: Path) -> None:
    target = package / LICENSES_DIR
    if target.exists():
        shutil.rmtree(target)
    target.mkdir()
    for name, source in PROJECT_LICENSE_FILES.items():
        shutil.copyfile(source, target / name)
    for item in pinned_license_files():
        shutil.copyfile(cached_license(item), target / item["name"])


def validate_package(package: Path, spec: dict[str, str], template: Path) -> dict[str, Any]:
    executable = package / spec["executable"]
    extension = package / spec["extension"]
    pck = package / f"{executable.stem}.pck"
    expected_files = {executable.name, extension.name, pck.name}
    actual_files = {path.name for path in package.iterdir() if path.is_file()}
    actual_dirs = {path.name for path in package.iterdir() if path.is_dir()}
    if actual_files != expected_files or actual_dirs != {LICENSES_DIR}:
        raise ValueError(
            f"package file set mismatch: files={sorted(actual_files)}, dirs={sorted(actual_dirs)}"
        )
    licenses = package / LICENSES_DIR
    actual_licenses = {path.name for path in licenses.iterdir()}
    if actual_licenses != expected_license_names():
        raise ValueError(f"package licence set mismatch: {sorted(actual_licenses)}")
    entries = pck_entries(pck)
    missing = sorted(REQUIRED_PCK_ENTRIES - set(entries))
    if missing:
        raise ValueError(f"PCK is missing required public resources: {missing}")
    scene_entries = [
        entry
        for entry in entries
        if entry.startswith(".godot/exported/") and entry.endswith("-main.scn")
    ]
    if len(scene_entries) != 1:
        raise ValueError(f"PCK expected one exported main scene, found {scene_entries}")
    expected_entries = REQUIRED_PCK_ENTRIES | set(scene_entries)
    if len(entries) != len(expected_entries) or set(entries) != expected_entries:
        raise ValueError(
            f"PCK entry allowlist mismatch: expected={sorted(expected_entries)}, "
            f"actual={entries}"
        )
    forbidden = sorted(
        entry for entry in entries if Path(entry).suffix.lower() in FORBIDDEN_PCK_SUFFIXES
    )
    if forbidden:
        raise ValueError(f"PCK contains forbidden generated/private candidates: {forbidden}")
    return {
        "package_files": sorted(actual_files),
        "pck_entries": entries,
        "template_sha256": sha256(template),
        "executable_sha256": sha256(executable),
        "extension_sha256": sha256(extension),
        "pck_sha256": sha256(pck),
        "package_bytes": {path.name: path.stat().st_size for path in package.iterdir() if path.is_file()},
        "license_files": sorted(actual_licenses),
    }


def default_godot(platform: str) -> Path:
    if platform.startswith("windows"):
        return ROOT / "out" / "godot" / "bin" / "windows" / "Godot_v4.7.2-stable_win64_console.exe"
    return ROOT / "out" / "godot" / "bin" / "linux" / "Godot_v4.7.2-stable_linux.x86_64"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    parser.add_argument("--godot", type=Path, help="pinned Godot 4.7.2 console/editor binary")
    parser.add_argument("--package-root", type=Path)
    parser.add_argument("--receipt-root", type=Path)
    parser.add_argument("--smoke", action="store_true", help="launch the exported package headlessly")
    args = parser.parse_args()

    spec = PLATFORMS[args.platform]
    godot = (args.godot or Path(os.environ.get("EAWR_GODOT_EXECUTABLE", default_godot(args.platform)))).resolve()
    package = resolve_output_path(
        args.package_root or ROOT / "out" / "viewer-packages" / args.platform,
        "package-root",
    )
    receipt = resolve_output_path(
        args.receipt_root or ROOT / "out" / "viewer-receipts" / args.platform,
        "receipt-root",
    )
    template_manifest = json.loads(TEMPLATES.read_text(encoding="utf-8"))
    if template_manifest.get("godot_version") != PINNED_GODOT_VERSION:
        raise ValueError(
            "template manifest godot_version does not match pinned Godot version "
            f"{PINNED_GODOT_VERSION!r}: {template_manifest.get('godot_version')!r}"
        )
    template_spec = template_manifest["templates"][spec["template"]]
    template = (ROOT / template_spec["path"]).resolve()
    godot_identity, godot_version = validate_godot_identity(godot)
    if not template.is_file():
        raise FileNotFoundError(f"export template not found: {template}")
    observed_template_sha = sha256(template)
    if observed_template_sha != template_spec["sha256"]:
        raise ValueError(
            f"template SHA-256 mismatch for {template}: {observed_template_sha} != {template_spec['sha256']}"
        )
    if args.smoke and args.platform == "linux-arm64":
        raise ValueError("refusing native smoke for the ARM64 package on a non-ARM host")

    package.mkdir(parents=True, exist_ok=True)
    receipt.mkdir(parents=True, exist_ok=True)
    executable = package / spec["executable"]
    export_log = receipt / "export.log"
    export_command = [
        godot,
        "--headless",
        "--path",
        PROJECT,
        "--export-release",
        spec["preset"],
        executable,
    ]
    run(export_command, ROOT, export_log)
    bundle_licenses(package)
    validation = validate_package(package, spec, template)

    smoke: dict[str, Any] | None = None
    if args.smoke:
        smoke_report = receipt / "standalone-headless.json"
        smoke_log = receipt / "smoke.log"
        smoke_command = [
            executable,
            "--headless",
            "--",
            "--eawr-headless-probe",
            "--eawr-report",
            smoke_report,
        ]
        run(smoke_command, package, smoke_log)
        smoke = json.loads(smoke_report.read_text(encoding="utf-8"))
        if smoke.get("status") != "headless_startup_and_core_replay_passed":
            raise ValueError(f"standalone smoke did not pass: {smoke.get('status')!r}")
        verify_smoke_engine_version(smoke, godot_version)

    result = {
        "schema_version": 1,
        "platform": args.platform,
        "preset": spec["preset"],
        "godot": str(godot),
        "godot_version": godot_version,
        "godot_identity": godot_identity,
        "godot_cpp_version": "10.0.0-stable",
        "template": str(template),
        "template_manifest_sha256": template_spec["sha256"],
        "package": str(package),
        "export_command": command_text(export_command),
        "export_log": str(export_log),
        "validation": validation,
        "smoke": smoke,
    }
    manifest = receipt / "package-receipt.json"
    manifest.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"export_package.py: {error}", file=sys.stderr)
        raise SystemExit(2)
