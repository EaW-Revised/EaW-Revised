"""Reusable bounded loose/MEG corpus reader matching the P0-02 VFS contract.

This is deliberately an offline reader.  It is not a second runtime asset API.
Raw mode includes inactive discovered archives for accounting; effective mode only
uses manifest-active archives and applies the documented VFS precedence.
"""
from __future__ import annotations

import hashlib
import re
import struct
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from collections.abc import Sequence
from os import PathLike
from typing import Iterator

try:
    from .common import ascii_fold, canonical_path, sha256
except ImportError:  # direct script/test import
    from common import ascii_fold, canonical_path, sha256

MAX_MANIFEST = 4 * 1024 * 1024
MAX_ARCHIVE_RECORDS = 1_000_000
MAX_FILENAME_BYTES = 64 * 1024 * 1024
MAX_MEMBER_BYTES = 64 * 1024 * 1024
SFX = (
    ("Audio/SFX/SFX2D_English.meg", "data/audio/sfx"),
    ("Audio/SFX/SFX2D_Non_Localized.meg", "data/audio/sfx"),
    ("Audio/SFX/SFX3D_Non_Localized.meg", "data/audio/sfx"),
)
PATCHES = ("Patch.meg", "Patch2.meg", "64Patch.meg")


class CorpusError(RuntimeError):
    pass


@dataclass(frozen=True)
class MegMember:
    logical_path: str
    original_path: str
    offset: int
    size: int


@dataclass(frozen=True)
class Archive:
    layer_id: str
    path: Path
    relative_path: str
    source_id: str
    active: bool
    active_index: int | None
    prefix: str
    members: tuple[MegMember, ...]
    sha256: str


@dataclass(frozen=True)
class Source:
    logical_path: str
    source_id: str
    origin: str
    layer_id: str
    data: bytes
    sha256: str
    archive_path: str | None = None
    active: bool = True
    winner: bool = False

    def metadata(self) -> dict[str, object]:
        return {
            "active": self.active,
            "archive_path": self.archive_path,
            "layer_id": self.layer_id,
            "logical_path": self.logical_path,
            "origin": self.origin,
            "sha256": self.sha256,
            "size": len(self.data),
            "source_id": self.source_id,
            "winner": self.winner,
        }


@dataclass(frozen=True)
class Layer:
    layer_id: str
    data_root: Path


def _ci_child(parent: Path, relative: str) -> Path | None:
    current = parent
    for part in relative.replace("\\", "/").split("/"):
        if not part:
            continue
        if not current.is_dir():
            return None
        matches = [p for p in current.iterdir() if ascii_fold(p.name) == ascii_fold(part)]
        if len(matches) > 1:
            raise CorpusError(f"case-insensitive collision below {current}: {part}")
        if not matches:
            return None
        current = matches[0]
    return current


def _parse_manifest(layer: Layer) -> tuple[list[tuple[Path, str, str]], list[str]]:
    manifest = _ci_child(layer.data_root, "MegaFiles.xml")
    if manifest is None:
        raise CorpusError(f"{layer.layer_id}: missing Data/MegaFiles.xml")
    raw = manifest.read_bytes()
    if len(raw) > MAX_MANIFEST:
        raise CorpusError(f"{layer.layer_id}: MegaFiles.xml exceeds 4 MiB")
    # Mirror the public VFS Unicode-family gate rather than letting the host's
    # locale or ElementTree's legacy-codepage support choose semantics.
    signatures = (
        (b"\x00\x00\xfe\xff", "utf-32-be", "utf-32"), (b"\xff\xfe\x00\x00", "utf-32-le", "utf-32"),
        (b"\xfe\xff", "utf-16-be", "utf-16"), (b"\xff\xfe", "utf-16-le", "utf-16"),
        (b"\xef\xbb\xbf", "utf-8-sig", "utf-8"), (b"\x00\x00\x00<", "utf-32-be", "utf-32"),
        (b"<\x00\x00\x00", "utf-32-le", "utf-32"), (b"\x00<\x00?", "utf-16-be", "utf-16"),
        (b"<\x00?\x00", "utf-16-le", "utf-16"),
    )
    codec, family = "utf-8", "utf-8"
    for signature, candidate_codec, candidate_family in signatures:
        if raw.startswith(signature): codec, family = candidate_codec, candidate_family; break
    try: decoded_manifest = raw.decode(codec)
    except UnicodeDecodeError as exc: raise CorpusError(f"{layer.layer_id}: invalid manifest Unicode: {exc}") from exc
    declaration = re.match(r"^\ufeff?\s*<\?xml\s+[^?]*encoding\s*=\s*(['\"])([^'\"]+)\1", decoded_manifest, re.I)
    if declaration:
        label = ascii_fold(declaration.group(2))
        declared_family = "utf-8" if label == "utf-8" else "utf-16" if label == "utf-16" else "utf-32" if label == "utf-32" else "legacy"
        if declared_family != family:
            raise CorpusError(f"{layer.layer_id}: unsupported or mismatched manifest encoding")
    if "<!DOCTYPE" in decoded_manifest.upper():
        raise CorpusError(f"{layer.layer_id}: MegaFiles.xml contains a doctype")
    try:
        root = ET.fromstring(raw)
    except (ET.ParseError, ValueError) as exc:
        raise CorpusError(f"{layer.layer_id}: invalid MegaFiles.xml: {exc}") from exc
    if root.tag != "Mega_Files":
        raise CorpusError(f"{layer.layer_id}: MegaFiles.xml root is not Mega_Files")
    declarations: list[str] = []
    for child in list(root):
        if child.tag != "File":
            continue
        if list(child):
            raise CorpusError(f"{layer.layer_id}: manifest File has child elements")
        value = (child.text or "").strip().replace("\\", "/")
        while ascii_fold(value).startswith("data/"):
            value = value[5:]
        if not value:
            raise CorpusError(f"{layer.layer_id}: empty manifest File")
        canonical_path(value)
        declarations.append(value)
    if not declarations:
        raise CorpusError(f"{layer.layer_id}: manifest contains no File entries")

    requested: list[tuple[str, str, bool]] = [(v, "", True) for v in declarations]
    requested += [(p, prefix, False) for p, prefix in SFX]
    requested += [(p, "", False) for p in PATCHES]
    found: list[tuple[Path, str, str]] = []
    missing: list[str] = []
    seen: set[str] = set()
    for rel, prefix, report_missing in requested:
        key = canonical_path(rel)
        if key in seen:
            continue
        seen.add(key)
        path = _ci_child(layer.data_root, rel)
        if path is None:
            if report_missing:
                missing.append("Data/" + rel)
            continue
        found.append((path, "Data/" + rel.replace("\\", "/"), prefix))
    return found, missing


def _read_meg(path: Path, prefix: str = "", hash_archive: bool = True) -> tuple[tuple[MegMember, ...], str]:
    """The archive's members and the SHA-256 of its bytes ("" when hash_archive is off)."""
    size = path.stat().st_size
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        header = stream.read(8)
        if len(header) != 8:
            raise CorpusError(f"{path.name}: truncated MEG header")
        digest.update(header)
        name_count, entry_count = struct.unpack("<II", header)
        if name_count > MAX_ARCHIVE_RECORDS or entry_count > MAX_ARCHIVE_RECORDS:
            raise CorpusError(f"{path.name}: MEG record limit exceeded")
        names: list[str] = []
        name_bytes = 0
        for index in range(name_count):
            length_raw = stream.read(2)
            if len(length_raw) != 2:
                raise CorpusError(f"{path.name}: truncated filename length {index}")
            digest.update(length_raw)
            length = struct.unpack("<H", length_raw)[0]
            name_bytes += length
            if name_bytes > MAX_FILENAME_BYTES:
                raise CorpusError(f"{path.name}: filename table limit exceeded")
            value = stream.read(length)
            if len(value) != length:
                raise CorpusError(f"{path.name}: truncated filename {index}")
            digest.update(value)
            if b"\0" in value:
                raise CorpusError(f"{path.name}: filename {index} contains NUL")
            names.append(value.decode("latin-1"))
        rows: list[tuple[int, int, int]] = []
        for index in range(entry_count):
            raw = stream.read(20)
            if len(raw) != 20:
                raise CorpusError(f"{path.name}: truncated entry {index}")
            digest.update(raw)
            _crc, _flags, length, offset, name_index = struct.unpack("<IIIII", raw)
            if name_index >= len(names):
                raise CorpusError(f"{path.name}: invalid filename index {name_index}")
            if offset > size or length > size - offset:
                raise CorpusError(f"{path.name}: member outside archive bounds")
            if length > MAX_MEMBER_BYTES:
                raise CorpusError(f"{path.name}: member exceeds 64 MiB policy")
            rows.append((name_index, offset, length))
        # Hash exact archive bytes without trusting offsets or rereading through members.
        for block in iter(lambda: stream.read(1024 * 1024), b"") if hash_archive else ():
            digest.update(block)
    members: list[MegMember] = []
    keys: set[str] = set()
    for name_index, offset, length in rows:
        original = names[name_index].replace("\\", "/")
        if prefix:
            original = prefix.rstrip("/") + "/" + original.lstrip("/")
        logical = canonical_path(original)
        if logical in keys:
            raise CorpusError(f"{path.name}: duplicate logical member {logical}")
        keys.add(logical)
        members.append(MegMember(logical, original, offset, length))
    return tuple(members), digest.hexdigest() if hash_archive else ""


class Corpus:
    def __init__(self, game_root: Path | str,
                 mod_root: Path | str | PathLike[str] | Sequence[Path | str | PathLike[str]] | None = None):
        self.game_root = Path(game_root)
        if mod_root is None:
            mod_roots: tuple[Path, ...] = ()
        elif isinstance(mod_root, (str, PathLike)):
            mod_roots = (Path(mod_root),)
        else:
            mod_roots = tuple(Path(root) for root in mod_root)
        resolved_data_roots = {
            str((root if ascii_fold(root.name) == "data" else root / "Data").resolve()).casefold()
            for root in mod_roots
        }
        if len(resolved_data_roots) != len(mod_roots):
            raise CorpusError("duplicate --mod-root data root after path resolution")
        self.mod_roots = mod_roots
        # Retained for existing one-root callers that inspect this attribute.
        self.mod_root = mod_roots[0] if mod_roots else None
        self._archive_cache: dict[tuple[str, str], Archive] = {}
        self.last_manifest: dict[str, object] = {}

    def _layers(self, profile: str) -> list[Layer]:
        base = self.game_root / "GameData" / "Data"
        expansion = self.game_root / "corruption" / "Data"
        if profile == "eaw":
            layers = [Layer("base", base)]
        elif profile == "foc":
            layers = [Layer("expansion", expansion), Layer("base", base)]
        elif profile == "remake":
            if not self.mod_roots:
                raise CorpusError("remake profile requires --mod-root")
            mod_layers = []
            for index, root in enumerate(self.mod_roots):
                mod_data = root if ascii_fold(root.name) == "data" else root / "Data"
                # Preserve all historical one-root/leaf provenance identifiers.
                layer_id = "mod" if index == 0 else f"mod[{index}]"
                mod_layers.append(Layer(layer_id, mod_data))
            layers = mod_layers + [Layer("expansion", expansion), Layer("base", base)]
        else:
            raise CorpusError(f"unknown profile: {profile}")
        for layer in layers:
            if not layer.data_root.is_dir():
                raise CorpusError(f"{layer.layer_id}: missing data root")
        return layers

    def _archives(self, layer: Layer) -> tuple[list[Archive], list[str], list[str]]:
        active_rows, missing = _parse_manifest(layer)
        active_keys = {str(path.resolve()).casefold(): (i, rel, prefix) for i, (path, rel, prefix) in enumerate(active_rows)}
        discovered = sorted((p for p in layer.data_root.rglob("*") if p.is_file() and ascii_fold(p.suffix) == ".meg"),
                            key=lambda p: ascii_fold(p.relative_to(layer.data_root).as_posix()))
        archives: list[Archive] = []
        for path in discovered:
            native_key = str(path.resolve()).casefold()
            active = active_keys.get(native_key)
            relative = active[1] if active else "Data/" + path.relative_to(layer.data_root).as_posix()
            source_id = f"{layer.layer_id}:{relative}"
            prefix = active[2] if active else ""
            cache_key = (native_key, prefix)
            cached = self._archive_cache.get(cache_key)
            if cached is None:
                members, archive_hash = _read_meg(path, prefix)
                cached = Archive(layer.layer_id, path, relative, source_id, active is not None,
                                 active[0] if active else None, prefix, members, archive_hash)
                self._archive_cache[cache_key] = cached
            archives.append(Archive(cached.layer_id, cached.path, cached.relative_path, cached.source_id,
                                    active is not None, active[0] if active else None, prefix,
                                    cached.members, cached.sha256))
        missing_native = [rel for path, rel, _prefix in active_rows if str(path.resolve()).casefold() not in {str(p.resolve()).casefold() for p in discovered}]
        return archives, missing, missing_native

    @staticmethod
    def _read_member(archive: Archive, member: MegMember) -> bytes:
        with archive.path.open("rb") as stream:
            stream.seek(member.offset)
            data = stream.read(member.size)
        if len(data) != member.size:
            raise CorpusError(f"{archive.source_id}: short member read {member.logical_path}")
        return data

    def read_effective(self, profile: str, logical_path: str) -> Source | None:
        """The one source the effective view resolves for a logical path, or None.

        Same precedence as iter_sources(mode="effective"), reading only archive
        indexes and the winning member, so one lookup does not hash every archive.
        """
        logical = canonical_path(logical_path)
        if not logical.startswith("data/"):
            raise CorpusError(f"not a Data/ logical path: {logical_path}")
        for layer in self._layers(profile):
            path = _ci_child(layer.data_root, logical[len("data/"):])
            if path is not None and path.is_file():
                data = path.read_bytes()
                original = "Data/" + path.relative_to(layer.data_root).as_posix()
                return Source(logical, f"{layer.layer_id}:loose:{original}", "loose", layer.layer_id,
                              data, sha256(data), None, True, True)
            rows, _missing = _parse_manifest(layer)
            for index, (archive_path, relative, prefix) in reversed(list(enumerate(rows))):
                members, _ = _read_meg(archive_path, prefix, hash_archive=False)
                member = next((m for m in members if m.logical_path == logical), None)
                if member is None:
                    continue
                archive = Archive(layer.layer_id, archive_path, relative, f"{layer.layer_id}:{relative}", True, index,
                                  prefix, members, "")
                data = self._read_member(archive, member)
                return Source(logical, archive.source_id, "archive", layer.layer_id, data, sha256(data),
                              relative, True, True)
        return None

    def iter_sources(self, profile: str, extension: str, mode: str) -> Iterator[Source]:
        if mode not in ("raw", "effective"):
            raise CorpusError("mode must be raw or effective")
        suffix = extension.lower() if extension.startswith(".") else "." + extension.lower()
        ordered: list[Source] = []
        manifest_layers: list[dict[str, object]] = []
        for layer in self._layers(profile):
            archives, missing, _ = self._archives(layer)
            loose: list[Source] = []
            loose_keys: dict[str, str] = {}
            for path in sorted(layer.data_root.rglob("*"), key=lambda p: ascii_fold(p.relative_to(layer.data_root).as_posix())):
                if not path.is_file() or ascii_fold(path.suffix) != suffix:
                    continue
                original = "Data/" + path.relative_to(layer.data_root).as_posix()
                logical = canonical_path(original)
                if logical in loose_keys:
                    raise CorpusError(f"{layer.layer_id}: case-insensitive loose collision: {loose_keys[logical]} and {original}")
                loose_keys[logical] = original
                data = path.read_bytes()
                loose.append(Source(logical, f"{layer.layer_id}:loose:{original}", "loose",
                                    layer.layer_id, data, sha256(data), None, True, False))
            active = sorted((a for a in archives if a.active), key=lambda a: a.active_index or 0, reverse=True)
            inactive = sorted((a for a in archives if not a.active), key=lambda a: ascii_fold(a.relative_path))
            archive_order = active if mode == "effective" else active + inactive
            members: list[Source] = []
            for archive in archive_order:
                for member in sorted(archive.members, key=lambda m: m.logical_path):
                    if not member.logical_path.endswith(suffix):
                        continue
                    data = self._read_member(archive, member)
                    members.append(Source(member.logical_path, archive.source_id, "archive", layer.layer_id,
                                          data, sha256(data), archive.relative_path, archive.active, False))
            ordered.extend(loose)
            ordered.extend(members)
            manifest_layers.append({
                "active_archives_low_to_high": [a.relative_path for a in sorted(active, key=lambda a: a.active_index or 0)],
                "discovered_archives": [{"active": a.active, "sha256": a.sha256, "source_id": a.source_id} for a in archives],
                "layer_id": layer.layer_id,
                "missing_declared_archives": missing,
            })
        winners: dict[str, int] = {}
        for index, source in enumerate(ordered):
            if source.active and source.logical_path not in winners:
                winners[source.logical_path] = index
        result: list[Source] = []
        for index, source in enumerate(ordered):
            winner = winners.get(source.logical_path) == index
            if mode == "effective" and not winner:
                continue
            result.append(Source(**{**source.__dict__, "winner": winner}))
        result.sort(key=lambda s: (s.logical_path, s.source_id, s.archive_path or ""))
        self.last_manifest = {"mode": mode, "profile": profile, "layers": manifest_layers,
                              "raw_or_effective_count": len(result)}
        yield from result


def iter_sources(profile: str, extension: str, mode: str, *, game_root: Path | str,
                 mod_root: Path | str | PathLike[str] |
                 Sequence[Path | str | PathLike[str]] | None = None) -> Iterator[Source]:
    yield from Corpus(game_root, mod_root).iter_sources(profile, extension, mode)
