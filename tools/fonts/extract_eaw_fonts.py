#!/usr/bin/env python3
"""Extract the four EmpireAtWar TrueType faces from the player's own FoC executable.

Forces of Corruption ships no font files: EmpireAtWar-Bold, -Light, -Medium and
-Stencil are embedded in corruption/StarWarsG.exe and registered in memory at
start-up (docs/ui/ui-layer.md, owner decision D1 in #164, ticket UI-05 #191).
They are the foundry's work, so the project never commits or ships them. This
script copies them out of the player's own install into an ignored local cache
(default out/fonts/), which the viewer reads at run time.

The scan is structural. Every TrueType table directory in the file is checked:
table records, every table checksum, the whole-font checksum adjustment and the
required tables; the name table then names the face. The executable must be a
pinned FoC build. Any other file fails with exit code 3 unless
--allow-unknown-build is given, and even then all four faces must be found and
valid.

The script prints names, sizes and SHA-256 digests only, never font bytes.
Standard library only. Exit codes: 0 extracted, 2 usage or unreadable input,
3 unknown executable, 4 faces missing, invalid or ambiguous, 5 output refused
or not written.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_OUT = ROOT / "out" / "fonts"
MANIFEST = "fonts.json"
MANIFEST_SCHEMA = "eawr-font-cache/1"

FACES = ("EmpireAtWar-Bold", "EmpireAtWar-Light", "EmpireAtWar-Medium", "EmpireAtWar-Stencil")

# SHA-256 of the executable -> label. Only these builds extract without
# --allow-unknown-build.
KNOWN_BUILDS: Dict[str, str] = {
    "07daeeaec9d1e751383b1db3f5847b6c66f68785440a6c52c87b85f723d018a4":
        "FoC Steam x64 (corruption/StarWarsG.exe, 12,428,800 bytes, file version 1.0.0.0)",
}

# Face -> (bytes, SHA-256) as extracted from the pinned build. A pinned build
# must reproduce these exactly; an unknown build that differs is reported.
PINNED_FACES: Dict[str, Tuple[int, str]] = {
    "EmpireAtWar-Bold": (65684, "3df279cf0063098dcd54156878bc7dc0a3c1010a30c790ffc55695cd799c5aec"),
    "EmpireAtWar-Light": (67148, "6ea18fea12c6536f7ce151ea42bda0a03a03451919fa239af2bb8a31012ce0aa"),
    "EmpireAtWar-Medium": (66580, "24a5c0bad551c136f2d0ef045ae319104cee0bc9df411acb088ad3c25459ae3c"),
    "EmpireAtWar-Stencil": (67632, "3086766b3daee145a7d5c3f9f39ec9dbd49ac66bfe739201424af919553251fc"),
}

EXIT_USAGE = 2
EXIT_UNKNOWN_BUILD = 3
EXIT_FACES = 4
EXIT_OUTPUT = 5

SFNT_VERSIONS = (b"\x00\x01\x00\x00", b"true")
REQUIRED_TABLES = (b"cmap", b"glyf", b"head", b"hhea", b"hmtx", b"loca", b"maxp", b"name", b"post")
MAX_TABLES = 128
MAX_FACE_BYTES = 4 * 1024 * 1024
HEAD_MAGIC = 0x5F0F3CF5
FONT_CHECKSUM = 0xB1B0AFBA

NAME_FAMILY = 1
NAME_SUBFAMILY = 2
NAME_FULL = 4
NAME_POSTSCRIPT = 6


class ExtractionError(Exception):
    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code


class SfntError(Exception):
    """A table directory that looked like a font but failed validation."""


@dataclass(frozen=True)
class Face:
    full_name: str
    family: str
    subfamily: str
    postscript_name: str
    data: bytes

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()


@dataclass(frozen=True)
class Scan:
    faces: List[Face]
    rejected: List[str]


def _checksum(data: bytes) -> int:
    padded = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(padded) // 4}I", padded)) & 0xFFFFFFFF


def _directory(buffer: bytes, start: int) -> Optional[List[Tuple[bytes, int, int, int]]]:
    """The table records of a well-formed table directory at `start`, else None."""
    if start + 12 > len(buffer) or buffer[start:start + 4] not in SFNT_VERSIONS:
        return None
    count, search_range, entry_selector, range_shift = struct.unpack_from(">4H", buffer, start + 4)
    if not 1 <= count <= MAX_TABLES:
        return None
    selector = count.bit_length() - 1
    if entry_selector != selector or search_range != 16 << selector or range_shift != count * 16 - search_range:
        return None
    if start + 12 + 16 * count > len(buffer):
        return None
    records = [struct.unpack_from(">4sIII", buffer, start + 12 + 16 * index) for index in range(count)]
    tags = [record[0] for record in records]
    if any(not all(0x20 <= byte <= 0x7E for byte in tag) for tag in tags):
        return None
    if any(left >= right for left, right in zip(tags, tags[1:])):
        return None
    return records


def _decode_name(platform: int, encoding: int, raw: bytes) -> Optional[str]:
    try:
        if platform in (0, 3):
            return raw.decode("utf-16-be")
        if platform == 1 and encoding == 0:
            return raw.decode("mac_roman")
    except UnicodeDecodeError:
        return None
    return None


def _names(table: bytes) -> Dict[int, str]:
    if len(table) < 6:
        raise SfntError("name table is shorter than its header")
    version, count, string_offset = struct.unpack_from(">3H", table, 0)
    if version > 1 or 6 + 12 * count > len(table) or string_offset > len(table):
        raise SfntError("name table header is out of range")
    # Windows English first, then any Windows, Unicode, Macintosh.
    ranked: Dict[int, Tuple[int, str]] = {}
    for index in range(count):
        platform, encoding, language, name_id, length, offset = struct.unpack_from(">6H", table, 6 + 12 * index)
        begin = string_offset + offset
        if begin + length > len(table):
            raise SfntError(f"name record {name_id} is out of range")
        text = _decode_name(platform, encoding, table[begin:begin + length])
        if text is None:
            continue
        rank = 0 if (platform, language) == (3, 0x409) else 1 if platform == 3 else 2 if platform == 0 else 3
        if name_id not in ranked or rank < ranked[name_id][0]:
            ranked[name_id] = (rank, text)
    return {name_id: text for name_id, (_, text) in ranked.items()}


def validate_face(buffer: bytes, start: int = 0) -> Face:
    """Validate the TrueType font at `start` and return it; SfntError if it is not one."""
    records = _directory(buffer, start)
    if records is None:
        raise SfntError("no TrueType table directory")
    directory_end = 12 + 16 * len(records)
    spans = []
    for tag, _, offset, length in records:
        name = tag.decode("ascii")
        if offset % 4 or offset < directory_end or offset + length > MAX_FACE_BYTES:
            raise SfntError(f"table {name!r} is misplaced")
        spans.append((offset, offset + length, name))
    spans.sort()
    for (_, end, left), (begin, _, right) in zip(spans, spans[1:]):
        if begin < end:
            raise SfntError(f"tables {left!r} and {right!r} overlap")
    end = max(stop for _, stop, _ in spans)
    if start + end > len(buffer):
        raise SfntError("the font runs past the end of the file")
    # sfnt tables are 4-byte aligned; the file carries zero padding.
    font = bytearray(buffer[start:start + end] + b"\0" * (-end % 4))
    tables = {tag: (checksum, offset, length) for tag, checksum, offset, length in records}
    missing = [tag.decode("ascii") for tag in REQUIRED_TABLES if tag not in tables]
    if missing:
        raise SfntError(f"required tables missing: {', '.join(missing)}")
    _, head_offset, head_length = tables[b"head"]
    if head_length < 54 or struct.unpack_from(">I", font, head_offset + 12)[0] != HEAD_MAGIC:
        raise SfntError("head table has no magic number")
    adjustment = struct.unpack_from(">I", font, head_offset + 8)[0]
    struct.pack_into(">I", font, head_offset + 8, 0)
    for tag, (checksum, offset, length) in tables.items():
        if _checksum(bytes(font[offset:offset + length])) != checksum:
            raise SfntError(f"table {tag.decode('ascii')!r} checksum mismatch")
    if (FONT_CHECKSUM - _checksum(bytes(font))) & 0xFFFFFFFF != adjustment:
        raise SfntError("whole-font checksum adjustment mismatch")
    struct.pack_into(">I", font, head_offset + 8, adjustment)
    _, name_offset, name_length = tables[b"name"]
    names = _names(bytes(font[name_offset:name_offset + name_length]))
    full_name = names.get(NAME_FULL, "").strip()
    if not full_name:
        raise SfntError("name table has no full name")
    return Face(
        full_name=full_name,
        family=names.get(NAME_FAMILY, "").strip(),
        subfamily=names.get(NAME_SUBFAMILY, "").strip(),
        postscript_name=names.get(NAME_POSTSCRIPT, "").strip(),
        data=bytes(font),
    )


def scan(buffer: bytes) -> Scan:
    """Every valid TrueType font in `buffer`, and why other table directories failed."""
    faces: List[Face] = []
    rejected: List[str] = []
    for signature in SFNT_VERSIONS:
        position = buffer.find(signature)
        while position >= 0:
            if _directory(buffer, position) is not None:
                try:
                    faces.append(validate_face(buffer, position))
                except SfntError as error:
                    rejected.append(str(error))
            position = buffer.find(signature, position + 1)
    return Scan(faces, rejected)


def _is_pe(data: bytes) -> bool:
    if len(data) < 0x40 or data[:2] != b"MZ":
        return False
    header = struct.unpack_from("<I", data, 0x3C)[0]
    return header + 4 <= len(data) and data[header:header + 4] == b"PE\0\0"


def _child(directory: pathlib.Path, name: str) -> Optional[pathlib.Path]:
    exact = directory / name
    if exact.exists():
        return exact
    if directory.is_dir():
        for entry in directory.iterdir():
            if entry.name.lower() == name.lower():
                return entry
    return None


def locate_executable(game_root: pathlib.Path) -> pathlib.Path:
    """corruption/StarWarsG.exe under a game root (or the corruption folder itself)."""
    corruption = game_root if game_root.name.lower() == "corruption" else _child(game_root, "corruption")
    executable = _child(corruption, "StarWarsG.exe") if corruption is not None else None
    if executable is None or not executable.is_file():
        raise ExtractionError(EXIT_USAGE, f"no corruption/StarWarsG.exe under {game_root}; "
                              "EAWR supports Forces of Corruption only")
    return executable


def select_faces(found: Scan, pinned_build: bool) -> Tuple[Dict[str, Face], List[str]]:
    """The four expected faces keyed by name, and warnings. ExtractionError when one is missing."""
    chosen: Dict[str, Face] = {}
    warnings: List[str] = []
    for wanted in FACES:
        matches = {face.data: face for face in found.faces
                   if wanted.lower() in (face.full_name.lower(), face.postscript_name.lower())}
        if len(matches) > 1:
            raise ExtractionError(EXIT_FACES, f"{len(matches)} different fonts are named {wanted}")
        if matches:
            chosen[wanted] = next(iter(matches.values()))
    missing = [name for name in FACES if name not in chosen]
    if missing:
        names = sorted({face.full_name for face in found.faces}) or ["none"]
        detail = f"; {len(found.rejected)} other table directories failed validation" if found.rejected else ""
        raise ExtractionError(EXIT_FACES, f"missing faces: {', '.join(missing)} (valid fonts found: "
                              f"{', '.join(names)}{detail})")
    for name, face in chosen.items():
        if (face.size, face.sha256) == PINNED_FACES[name]:
            continue
        message = f"{name} ({face.size} bytes, sha256 {face.sha256}) differs from the pinned face"
        if pinned_build:
            raise ExtractionError(EXIT_FACES, message)
        warnings.append(message)
    return chosen, warnings


def _ignored(out_dir: pathlib.Path, names: Sequence[str]) -> bool:
    """True when every output path is outside this repository or git-ignored in it."""
    try:
        relative = out_dir.resolve().relative_to(ROOT)
    except ValueError:
        return True
    if not (ROOT / ".git").exists():
        return True
    paths = [(relative / name).as_posix() for name in names]
    try:
        # Prints each ignored path; a tracked path never counts as ignored.
        result = subprocess.run(["git", "-C", str(ROOT), "check-ignore", "-z", "--stdin"],
                                input="\0".join(paths).encode() + b"\0", capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        result = None
    if result is None or result.returncode not in (0, 1):
        # Without a working git only the ignored out/ tree is trusted.
        return relative.parts[:1] == ("out",)
    return set(result.stdout.decode().split("\0")) >= set(paths)


def manifest(executable_digest: str, executable_size: int, faces: Dict[str, Face]) -> dict:
    return {
        "schema": MANIFEST_SCHEMA,
        "tool": "tools/fonts/extract_eaw_fonts.py",
        "source": {
            "file": "StarWarsG.exe",
            "sha256": executable_digest,
            "bytes": executable_size,
            "build": KNOWN_BUILDS.get(executable_digest),
        },
        "faces": [{
            "face": name,
            "file": f"{name}.ttf",
            "family": faces[name].family,
            "subfamily": faces[name].subfamily,
            "bytes": faces[name].size,
            "sha256": faces[name].sha256,
            "pinned": (faces[name].size, faces[name].sha256) == PINNED_FACES[name],
        } for name in FACES],
    }


def _publish(target: pathlib.Path, data: bytes) -> None:
    """Write through a new, uniquely named file (exclusive create, so a planted file or symlink is
    never followed or overwritten) and move it over the target."""
    handle, name = tempfile.mkstemp(prefix=f".{target.name}.", suffix=".tmp", dir=target.parent)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
        os.replace(name, target)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(name)
        raise


def write_cache(out_dir: pathlib.Path, faces: Dict[str, Face], summary: dict) -> None:
    names = [f"{name}.ttf" for name in FACES] + [MANIFEST]
    if not _ignored(out_dir, names):
        raise ExtractionError(EXIT_OUTPUT, f"refusing to write fonts to {out_dir}: the path is inside this "
                              "repository but not git-ignored (the faces must never be committed)")
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
        for name in FACES:
            _publish(out_dir / f"{name}.ttf", faces[name].data)
        _publish(out_dir / MANIFEST, (json.dumps(summary, indent=2) + "\n").encode("utf-8"))
    except OSError as error:
        raise ExtractionError(EXIT_OUTPUT, f"could not write the font cache in {out_dir}: {error}") from error


def extract(executable: pathlib.Path, out_dir: pathlib.Path, allow_unknown_build: bool,
            dry_run: bool) -> Tuple[dict, List[str]]:
    try:
        data = executable.read_bytes()
    except OSError as error:
        raise ExtractionError(EXIT_USAGE, f"cannot read {executable}: {error}") from error
    if not _is_pe(data):
        raise ExtractionError(EXIT_USAGE, f"{executable} is not a Windows executable")
    digest = hashlib.sha256(data).hexdigest()
    pinned_build = digest in KNOWN_BUILDS
    if not pinned_build and not allow_unknown_build:
        known = "; ".join(KNOWN_BUILDS.values())
        raise ExtractionError(EXIT_UNKNOWN_BUILD, f"unknown executable {executable} (sha256 {digest}, "
                              f"{len(data)} bytes). EAWR is pinned to: {known}. Point --game-root at a "
                              "Forces of Corruption install, or pass --allow-unknown-build to extract "
                              "from this build anyway.")
    faces, warnings = select_faces(scan(data), pinned_build)
    if not pinned_build:
        warnings.insert(0, f"unknown executable (sha256 {digest}); extracted because of --allow-unknown-build")
    summary = manifest(digest, len(data), faces)
    if not dry_run:
        write_cache(out_dir, faces, summary)
    return summary, warnings


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--game-root", type=pathlib.Path,
                        help="EaW install root holding corruption/ (default: EAWR_EAW_GAME_ROOT)")
    source.add_argument("--exe", type=pathlib.Path, help="the FoC StarWarsG.exe itself")
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT,
                        help=f"font cache directory (default: {DEFAULT_OUT.relative_to(ROOT).as_posix()})")
    parser.add_argument("--allow-unknown-build", action="store_true",
                        help="extract from an executable that is not a pinned FoC build")
    parser.add_argument("--dry-run", action="store_true", help="scan and report without writing")
    parser.add_argument("--json", action="store_true", help="print the cache manifest as JSON")
    arguments = parser.parse_args(argv)
    try:
        if arguments.exe is not None:
            executable = arguments.exe
        else:
            root = arguments.game_root or (pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
                                           if os.environ.get("EAWR_EAW_GAME_ROOT") else None)
            if root is None:
                raise ExtractionError(EXIT_USAGE, "pass --game-root or --exe (or set EAWR_EAW_GAME_ROOT)")
            executable = locate_executable(root)
        summary, warnings = extract(executable, arguments.out, arguments.allow_unknown_build, arguments.dry_run)
    except ExtractionError as error:
        print(f"extract_eaw_fonts: error: {error}", file=sys.stderr)
        return error.code
    for warning in warnings:
        print(f"extract_eaw_fonts: warning: {warning}", file=sys.stderr)
    if arguments.json:
        print(json.dumps(summary, indent=2))
        return 0
    build = summary["source"]["build"] or "unknown build"
    print(f"EAWR fonts from {build} (sha256 {summary['source']['sha256']})")
    for face in summary["faces"]:
        print(f"  {face['face']:<20} {face['bytes']:>7} bytes  sha256 {face['sha256']}")
    if arguments.dry_run:
        print("dry run: nothing written")
    else:
        print(f"wrote {len(summary['faces'])} faces to {arguments.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
