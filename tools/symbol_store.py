#!/usr/bin/env python3
"""Retain matching PE/PDB pairs in a private, offline Windows symbol store.

The PE CodeView record and PDB Info stream must agree on GUID and age.
Layout follows the Windows symbol server convention; build manifests contain
portable names and hashes, never build-machine paths. Nothing is uploaded.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import uuid
import zipfile


def unpack(data, offset, fmt):
    if offset < 0 or offset + struct.calcsize(fmt) > len(data):
        raise ValueError("truncated symbol metadata")
    return struct.unpack_from(fmt, data, offset)


def codeview(data):
    if len(data) < 25 or bytes(data[:4]) != b"RSDS":
        raise ValueError("missing modern CodeView identity")
    guid = uuid.UUID(bytes_le=bytes(data[4:20])).hex.upper()
    age, = unpack(data, 20, "<I")
    raw_name = bytes(data[24:]).split(b"\0", 1)[0]
    name = raw_name.decode("utf-8", errors="replace").replace("\\", "/").rsplit("/", 1)[-1]
    if not name or name in (".", "..") or any(c in name for c in '/\\:*?"<>|'):
        raise ValueError("invalid PDB filename")
    return name, guid + f"{age:X}"


def pe_identity(data):
    if bytes(data[:2]) != b"MZ":
        raise ValueError("not a PE image")
    pe, = unpack(data, 60, "<I")
    if bytes(data[pe:pe + 4]) != b"PE\0\0":
        raise ValueError("missing PE header")
    sections, timestamp = unpack(data, pe + 6, "<HI")
    optional_size, = unpack(data, pe + 20, "<H")
    optional = pe + 24
    magic, = unpack(data, optional, "<H")
    if magic not in (0x10B, 0x20B):
        raise ValueError("unsupported PE optional header")
    image_size, = unpack(data, optional + 56, "<I")
    directories = optional + (112 if magic == 0x20B else 96)
    debug_rva, debug_size = unpack(data, directories + 6 * 8, "<II")
    if not debug_size or debug_size % 28:
        raise ValueError("missing PE debug directory")
    debug_offset = None
    for index in range(sections):
        section = optional + optional_size + index * 40
        virtual_size, rva, raw_size, raw_offset = unpack(data, section + 8, "<IIII")
        if rva <= debug_rva < rva + max(virtual_size, raw_size):
            debug_offset = raw_offset + debug_rva - rva
            break
    if debug_offset is None:
        raise ValueError("unmapped PE debug directory")
    for offset in range(debug_offset, debug_offset + debug_size, 28):
        kind, size, _, pointer = unpack(data, offset + 12, "<IIII")
        if kind == 2:
            if pointer + size > len(data):
                raise ValueError("truncated CodeView record")
            name, key = codeview(data[pointer:pointer + size])
            return name, key, f"{timestamp:08X}{image_size:x}"
    raise ValueError("PE image has no PDB identity")


def pdb_identity(data):
    if bytes(data[:32]) != b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0":
        raise ValueError("not an MSF 7 PDB")
    block_size, _, blocks, directory_size, _, block_map = unpack(data, 32, "<IIIIII")
    if block_size not in (512, 1024, 2048, 4096, 8192, 16384, 32768) or blocks * block_size > len(data):
        raise ValueError("invalid PDB block layout")
    count = (directory_size + block_size - 1) // block_size
    if count * 4 > block_size:
        raise ValueError("unsupported PDB directory map")
    indices = unpack(data, block_map * block_size, "<" + "I" * count)
    if any(index >= blocks for index in indices):
        raise ValueError("invalid PDB directory block")
    directory = b"".join(data[index * block_size:(index + 1) * block_size] for index in indices)[:directory_size]
    streams, = unpack(directory, 0, "<I")
    if streams < 2 or streams > len(directory) // 4:
        raise ValueError("missing PDB info stream")
    sizes = unpack(directory, 4, "<" + "I" * streams)
    info_index = 4 + streams * 4 + (0 if sizes[0] == 0xFFFFFFFF else (sizes[0] + block_size - 1) // block_size * 4)
    block, = unpack(directory, info_index, "<I")
    if sizes[1] < 28 or block >= blocks:
        raise ValueError("invalid PDB info stream")
    age, = unpack(data, block * block_size + 8, "<I")
    guid = uuid.UUID(bytes_le=bytes(data[block * block_size + 12:block * block_size + 28])).hex.upper()
    return guid + f"{age:X}"


def inspect(path, reader):
    with Path(path).open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        return reader(data)


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def build_identity(path):
    match = re.search(r'return "([A-Za-z0-9_.+-]+)";', Path(path).read_text(encoding="utf-8"))
    if not match:
        raise ValueError("missing generated build identity")
    return match[1]


def import_archive(source, store):
    """Validate an untrusted transport archive before publishing private symbols."""
    store = Path(store).resolve()
    with zipfile.ZipFile(source) as zipped, tempfile.TemporaryDirectory(prefix="eawr-symbols-") as temporary:
        manifest = json.loads(zipped.read("manifest.json"))
        if manifest.get("schema") != "eawr-symbols/1":
            raise ValueError("unknown symbol archive schema")
        for value in (manifest["build_id"], manifest["label"]):
            if not re.fullmatch(r"[A-Za-z0-9_.+-]+", value) or value in (".", ".."):
                raise ValueError("invalid symbol archive identity")
        paths = {}
        for entry in manifest["files"]:
            for kind in ("binary", "pdb"):
                name = entry[kind]
                if ("\\" in name or ":" in name or name.startswith("/")
                        or any(part in ("", ".", "..") for part in name.split("/"))):
                    raise ValueError("unsafe symbol archive path")
                target = Path(temporary) / name
                target.parent.mkdir(parents=True, exist_ok=True)
                with zipped.open(name) as input_stream, target.open("wb") as output:
                    shutil.copyfileobj(input_stream, output)
                if digest(target) != entry[kind + "_sha256"]:
                    raise ValueError("symbol archive hash mismatch")
                paths[name] = target
            name, key, image_key = inspect(paths[entry["binary"]], pe_identity)
            binary_name = paths[entry["binary"]].name
            if (entry["pdb"] != f"{name}/{key}/{name}"
                    or entry["binary"] != f"{binary_name}/{image_key}/{binary_name}"
                    or inspect(paths[entry["pdb"]], pdb_identity) != key):
                raise ValueError("symbol archive identity mismatch")
        if not paths:
            raise ValueError("empty symbol archive")
        for name, path in paths.items():
            retain(path, store / name)
        target = store / "builds" / manifest["build_id"] / (manifest["label"] + ".json")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        return target


def retain(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    expected = digest(source)
    if destination.exists():
        if digest(destination) != expected:
            raise ValueError(f"symbol identity collision: {destination.name}")
        return expected
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
        temporary = Path(output.name)
        with Path(source).open("rb") as stream:
            shutil.copyfileobj(stream, output)
    try:
        # Hard-link publishing cannot overwrite an immutable identity another
        # pool process has just retained. Both files are on the store volume.
        try:
            os.link(temporary, destination)
        except FileExistsError:
            if digest(destination) != expected:
                raise ValueError(f"symbol identity collision: {destination.name}")
    finally:
        temporary.unlink(missing_ok=True)
    return expected


def archive(store, build_id, binaries, label="build", release=False):
    if not re.fullmatch(r"[A-Za-z0-9_.+-]+", build_id) or build_id in (".", ".."):
        raise ValueError("invalid build identity")
    if not re.fullmatch(r"[A-Za-z0-9_.+-]+", label) or label in (".", ".."):
        raise ValueError("invalid archive label")
    store = Path(store).resolve()
    entries = []
    for binary in sorted(set(map(Path, binaries))):
        name, key, image_key = inspect(binary, pe_identity)
        symbols = binary.parent / name
        if inspect(symbols, pdb_identity) != key:
            raise ValueError(f"PDB does not match image: {binary.name}")
        pdb_relative = Path(name) / key / name
        image_relative = Path(binary.name) / image_key / binary.name
        entries.append({"binary": image_relative.as_posix(), "pdb": pdb_relative.as_posix(),
                        "binary_sha256": retain(binary, store / image_relative),
                        "pdb_sha256": retain(symbols, store / pdb_relative)})
    if not entries:
        raise ValueError("no symbol pairs to retain")
    manifest = {"schema": "eawr-symbols/1", "build_id": build_id, "label": label,
                "release": release, "files": entries}
    folder = store / "builds" / build_id
    folder.mkdir(parents=True, exist_ok=True)
    # Separate labels keep the root/viewer and Release/Debug producers additive.
    target = folder / f"{label}.json"
    with tempfile.NamedTemporaryFile(mode="w", dir=folder, delete=False, encoding="utf-8") as stream:
        temporary = Path(stream.name)
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    os.replace(temporary, target)
    if release:
        archive_path = folder / f"{label}-symbols.zip"
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as output:
            output.write(target, "manifest.json")
            for name in sorted({entry[kind] for entry in entries for kind in ("binary", "pdb")}):
                output.write(store / name, name)
    return target


def bundle(target, build_id, binaries, label="build"):
    """Host staging keeps one compressed copy, without a second symbol tree."""
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".symbols-", dir=target.parent) as temporary:
        manifest = archive(Path(temporary), build_id, binaries, label, release=True)
        os.replace(manifest.with_name(label + "-symbols.zip"), target)
    return target


def prune_bundles(root, active_root, keep=10):
    """Keep ten newest job bundles plus any job named by a build-slot owner."""
    root = Path(root).resolve()
    if root.parent == root:
        raise ValueError("symbol bundle cleanup refuses a filesystem root")
    active = set()
    for owner in Path(active_root).glob("build*.owner"):
        try:
            active.add(owner.read_text(encoding="utf-8-sig").split()[0])
        except (OSError, IndexError):
            # An unreadable owner means cleanup is unverifiable; leave it alone.
            return
    # Only pool or CI job namespaces belong to this producer. Unrelated
    # directories are never eligible for retention cleanup.
    job_name = re.compile(r"(?:\d{8}-\d{6}-[0-9a-f]{8}-[0-9a-f]{4}|\d+-\d+-[A-Za-z0-9_.+-]+)")
    folders = sorted((path for path in root.iterdir() if path.is_dir() and job_name.fullmatch(path.name)),
                     key=lambda path: path.stat().st_mtime, reverse=True)
    for folder in folders[keep:]:
        if folder.name in active or folder.is_symlink() or folder.resolve().parent != root:
            continue
        shutil.rmtree(folder)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", type=Path, required=True)
    identity = parser.add_mutually_exclusive_group(required=True)
    identity.add_argument("--build-id")
    identity.add_argument("--identity-file", type=Path)
    parser.add_argument("--label", default="build")
    parser.add_argument("--input", type=Path, action="append", default=[])
    parser.add_argument("--scan", type=Path, action="append", default=[])
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--active-root", type=Path)
    args = parser.parse_args()
    binaries = args.input + [path for root in args.scan for path in root.rglob("*")
                             if path.suffix.lower() in (".exe", ".dll") and path.is_file()]
    try:
        identity = args.build_id or build_identity(args.identity_file)
        if args.bundle:
            if args.active_root and args.bundle.parent.parent.resolve() != args.store.resolve():
                raise ValueError("job bundle must be directly inside its declared staging store")
            print(bundle(args.bundle, identity, binaries, args.label))
            if args.active_root:
                prune_bundles(args.store, args.active_root)
        else:
            print(archive(args.store, identity, binaries, args.label, args.release))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"symbol_store: {error}\n")


if __name__ == "__main__":
    main()
