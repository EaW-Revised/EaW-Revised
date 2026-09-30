#!/usr/bin/env python3
"""Verify that a Godot Linux package and its GDExtension are genuinely AArch64."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path


ELF_MACHINE_AARCH64 = 183
FORBIDDEN_SUFFIXES = {
    ".alo",
    ".dds",
    ".fx",
    ".glsl",
    ".hlsl",
    ".meg",
    ".mtd",
    ".png",
    ".spv",
    ".tga",
}
EXPECTED_PUBLIC_RESOURCES = {
    "common/original-v1.eawr-replay",
    "common/scene.json",
    "eawr_godot.gdextension",
}
ALLOWED_NEEDED = {
    "libc.so.6",
    "libdl.so.2",
    "libgcc_s.so.1",
    "libm.so.6",
    "libpthread.so.0",
    "librt.so.1",
    "libstdc++.so.6",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_needed(sysroot: Path, name: str) -> Path:
    for directory in (
        "lib64",
        "usr/lib64",
        "lib",
        "usr/lib",
        "lib/aarch64-linux-gnu",
        "usr/lib/aarch64-linux-gnu",
    ):
        candidate = sysroot / directory / name
        if candidate.is_file():
            return candidate
    raise ValueError(f"target sysroot does not provide {name}")


def verify_elf(path: Path, readelf: Path, sysroot: Path, *, executable: bool) -> dict[str, object]:
    data = path.read_bytes()[:64]
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError(f"{path}: not a little-endian ELF64 file")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != ELF_MACHINE_AARCH64:
        raise ValueError(f"{path}: ELF e_machine={machine}, expected AArch64 ({ELF_MACHINE_AARCH64})")

    header = subprocess.run(
        [str(readelf), "-h", str(path)], check=True, text=True, capture_output=True
    ).stdout
    if "Machine:                           AArch64" not in header:
        raise ValueError(f"{path}: target readelf did not report AArch64")

    dynamic = subprocess.run(
        [str(readelf), "-d", str(path)], check=True, text=True, capture_output=True
    ).stdout
    needed = sorted(re.findall(r"\(NEEDED\).*\[(.+?)\]", dynamic))
    unexpected = sorted(set(needed) - ALLOWED_NEEDED)
    if unexpected:
        raise ValueError(f"{path}: unexpected dynamic dependencies: {unexpected}")

    program_headers = subprocess.run(
        [str(readelf), "-l", str(path)], check=True, text=True, capture_output=True
    ).stdout
    interpreter_match = re.search(r"Requesting program interpreter: ([^\]]+)", program_headers)
    interpreter = interpreter_match.group(1) if interpreter_match else None
    if executable and interpreter != "/lib/ld-linux-aarch64.so.1":
        raise ValueError(f"{path}: unexpected or absent AArch64 program interpreter: {interpreter}")
    if not executable and interpreter is not None:
        raise ValueError(f"{path}: shared library unexpectedly has a program interpreter: {interpreter}")
    if interpreter is not None and not (sysroot / interpreter.lstrip("/")).is_file():
        raise ValueError(f"{path}: target sysroot does not provide interpreter {interpreter}")
    resolved_needed = {
        name: str(resolve_needed(sysroot, name).relative_to(sysroot)) for name in needed
    }

    return {
        "path": str(path),
        "sha256": sha256(path),
        "bytes": path.stat().st_size,
        "interpreter": interpreter,
        "needed": needed,
        "resolved_needed": resolved_needed,
    }


def pck_entries(path: Path) -> list[str]:
    with path.open("rb") as stream:
        header = stream.read(40)
        if len(header) != 40 or header[:4] != b"GDPC":
            raise ValueError(f"{path}: unsupported or invalid Godot PCK header")
        pack_format = struct.unpack_from("<I", header, 4)[0]
        if pack_format != 4:
            raise ValueError(f"{path}: expected Godot PCK format 4, got {pack_format}")
        directory_offset = struct.unpack_from("<Q", header, 32)[0]
        stream.seek(directory_offset)
        file_count_raw = stream.read(4)
        if len(file_count_raw) != 4:
            raise ValueError(f"{path}: truncated PCK directory")
        file_count = struct.unpack("<I", file_count_raw)[0]
        if file_count > 100_000:
            raise ValueError(f"{path}: unreasonable PCK file count {file_count}")
        result: list[str] = []
        for _ in range(file_count):
            path_length = struct.unpack("<I", stream.read(4))[0]
            raw_path = stream.read(path_length)
            if len(raw_path) != path_length:
                raise ValueError(f"{path}: truncated PCK path")
            result.append(raw_path.rstrip(b"\0").decode("utf-8"))
            metadata = stream.read(8 + 8 + 16 + 4)
            if len(metadata) != 36:
                raise ValueError(f"{path}: truncated PCK entry")
        return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--pck", type=Path, required=True)
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--sysroot", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()

    for candidate in (args.template, args.extension, args.executable, args.pck, args.readelf):
        if not candidate.is_file():
            raise FileNotFoundError(candidate)
    if not args.sysroot.is_dir():
        raise NotADirectoryError(args.sysroot)

    elf_results = [
        verify_elf(args.template, args.readelf, args.sysroot, executable=True),
        verify_elf(args.extension, args.readelf, args.sysroot, executable=False),
        verify_elf(args.executable, args.readelf, args.sysroot, executable=True),
    ]
    if sha256(args.template) != sha256(args.executable):
        raise ValueError("exported executable is not byte-identical to the verified custom ARM64 template")

    entries = pck_entries(args.pck)
    missing = sorted(EXPECTED_PUBLIC_RESOURCES - set(entries))
    if missing:
        raise ValueError(f"PCK is missing required public resources: {missing}")
    forbidden = sorted(name for name in entries if Path(name).suffix.lower() in FORBIDDEN_SUFFIXES)
    if forbidden:
        raise ValueError(f"PCK contains forbidden private/generated shader or image candidates: {forbidden}")

    sidecars = sorted(path.name for path in args.executable.parent.iterdir() if path.is_file())
    expected_sidecars = sorted([args.executable.name, args.pck.name, args.extension.name])
    if sidecars != expected_sidecars:
        raise ValueError(f"package file set differs from strict allowlist: {sidecars} != {expected_sidecars}")

    result = {
        "schema_version": 1,
        "architecture": "AArch64",
        "execution_status": "not_run_cross_compiled_package",
        "target_sysroot": str(args.sysroot),
        "elfs": elf_results,
        "pck": {
            "path": str(args.pck),
            "sha256": sha256(args.pck),
            "bytes": args.pck.stat().st_size,
            "entries": entries,
        },
        "package_files": sidecars,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
