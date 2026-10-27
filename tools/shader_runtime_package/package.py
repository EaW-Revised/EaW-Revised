"""Verify and stage the optional, separately linked shader compiler package.

The checked-in lock is authoritative. A package never supplies its own trust
policy. The caller explicitly selects a reviewed replacement lock when testing
or rebuilding replacement libraries. No code in this module downloads anything.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "third_party/shader_runtime/lock.json"


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            value.update(block)
    return value.hexdigest()


def logical_path(value: str) -> str:
    path = PurePosixPath(value)
    if (not value or "\\" in value or ":" in value or path.is_absolute()
            or any(part in {"", ".", ".."} or part.startswith("~") for part in value.split("/"))
            or path.suffix.lower() in {".pdb", ".exe", ".obj", ".lib"}):
        raise ValueError("unsafe shader package member")
    return value


def load_lock(path: Path = LOCK) -> dict:
    lock = json.loads(path.read_text(encoding="utf-8"))
    if lock["schema"] != "eawr-shader-runtime/1" or lock["platform"] != "windows-x64":
        raise ValueError("unsupported shader package schema/platform")
    names = set()
    for item in lock["files"]:
        name = logical_path(item["path"])
        if name.casefold() in names:
            raise ValueError("duplicate shader package member")
        names.add(name.casefold())
        if len(item["sha256"]) != 64 or any(c not in "0123456789abcdef" for c in item["sha256"]):
            raise ValueError("invalid shader package digest")
    return lock


def pe_imports(path: Path) -> list[str]:
    """Read PE32+ import names independently of dumpbin/objdump output formatting."""
    data = path.read_bytes()
    def u16(offset):
        return struct.unpack_from("<H", data, offset)[0]
    def u32(offset):
        return struct.unpack_from("<I", data, offset)[0]
    try:
        pe = u32(0x3c)
        if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0" or u16(pe + 4) != 0x8664:
            raise ValueError("shader DLL is not Windows x64 PE")
        optional = pe + 24
        if u16(optional) != 0x20b:
            raise ValueError("shader DLL is not PE32+")
        sections = optional + u16(pe + 20)
        def offset(rva):
            for index in range(u16(pe + 6)):
                section = sections + index * 40
                start, size = u32(section + 12), u32(section + 16)
                if start <= rva < start + size:
                    result = u32(section + 20) + rva - start
                    if result >= len(data):
                        break
                    return result
            raise ValueError("shader DLL RVA outside file")
        # Delay-loaded imports would need their own closure audit. These recipes
        # do not produce any; reject rather than silently ignore a future change.
        if u32(optional + 108) <= 13 or u32(optional + 112 + 13 * 8):
            raise ValueError("shader DLL has unsupported delay imports")
        rva = u32(optional + 120)
        imports = []
        if rva:
            descriptor = offset(rva)
            for _ in range(256):
                if not any(data[descriptor:descriptor + 20]):
                    return sorted(set(imports))
                name = offset(u32(descriptor + 12))
                end = data.index(b"\0", name, min(name + 256, len(data)))
                imports.append(data[name:end].decode("ascii").lower())
                descriptor += 20
            raise ValueError("shader DLL import bound exceeded")
        return []
    except (IndexError, struct.error, UnicodeError) as error:
        raise ValueError("malformed shader DLL import table") from error


def verify(folder: Path, lock_path: Path = LOCK) -> dict:
    if lock_path.resolve() == LOCK.resolve():
        config = json.loads((ROOT / "tools/publish_scan/config.json").read_text(encoding="utf-8"))
        if config["assets"]["external_package_manifests"]["third_party/shader_runtime/lock.json"] != digest(lock_path):
            raise ValueError("shader lock differs from publish manifest pin")
    lock = load_lock(lock_path)
    if folder.is_symlink() or not folder.is_dir():
        raise ValueError("shader package directory missing or linked")
    expected = {item["path"] for item in lock["files"]} | {"manifest.json"}
    actual = set()
    for path in folder.rglob("*"):
        if path.is_symlink():
            raise ValueError("linked shader package member")
        if path.is_file():
            actual.add(path.relative_to(folder).as_posix())
    if actual != expected:
        raise ValueError("shader package missing or unexpected members")
    if (folder / "manifest.json").read_bytes() != lock_path.read_bytes():
        raise ValueError("shader package manifest differs from reviewed lock")
    binaries = {item["path"].casefold() for item in lock["files"] if item["role"] == "dll"}
    for item in lock["files"]:
        path = folder / item["path"]
        if path.stat().st_size != item["size"] or digest(path) != item["sha256"]:
            raise ValueError(f"shader package integrity failure: {item['path']}")
        if item["role"] == "dll":
            imports = pe_imports(path)
            if imports != item["imports"]:
                raise ValueError("shader DLL import table differs from lock")
            if any(name not in binaries and name not in lock["system_dlls"] for name in imports):
                raise ValueError("unaccounted shader DLL dependency")
    return lock


def stage(source: Path, destination: Path, lock_path: Path = LOCK) -> dict:
    lock = verify(source, lock_path)
    if destination.exists():
        raise ValueError("shader package destination already exists")
    destination.mkdir(parents=True)
    for name in [item["path"] for item in lock["files"]] + ["manifest.json"]:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, target)
    verify(destination, lock_path)
    return lock


def assemble(inputs: Path, destination: Path, lock_path: Path = LOCK) -> dict:
    """Create a package from offline inputs, then verify against the reviewed lock."""
    lock = load_lock(lock_path)
    if destination.exists():
        raise ValueError("shader package destination already exists")
    destination.mkdir(parents=True)
    for item in lock["files"]:
        source = inputs / item["path"]
        if source.is_symlink() or not source.is_file() or digest(source) != item["sha256"]:
            raise ValueError("offline shader input differs from reviewed lock")
        target = destination / item["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    shutil.copyfile(lock_path, destination / "manifest.json")
    verify(destination, lock_path)
    return lock


def write_header(lock_path: Path, output: Path) -> None:
    lock = load_lock(lock_path)
    lines = ["// Generated from third_party/shader_runtime/lock.json by package.py --header.",
             "#pragma once", "#include <array>",
             "namespace eawr::presentation::shader_runtime::pinned {",
             f"inline constexpr std::array<RuntimeFile, {len(lock['files'])}> files{{{{"]
    for item in lock["files"]:
        lines.append(f'    {{"{item["path"]}", "{item["sha256"]}", {item["size"]}ULL}},')
    lines.extend(["}};", "} // namespace eawr::presentation::shader_runtime::pinned", ""])
    output.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=LOCK)
    parser.add_argument("--verify", type=Path)
    parser.add_argument("--stage", type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--assemble", type=Path)
    args = parser.parse_args()
    if args.header:
        write_header(args.lock, args.header)
    if args.assemble:
        if not args.stage:
            parser.error("--assemble needs --stage")
        assemble(args.assemble, args.stage, args.lock)
    if args.verify:
        if args.stage:
            stage(args.verify, args.stage, args.lock)
        else:
            verify(args.verify, args.lock)
        print("shader package hashes, notices and DLL closure verified")


if __name__ == "__main__":
    main()
