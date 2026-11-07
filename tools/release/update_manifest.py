"""update-manifest.json (schema 1) for the launcher's over-the-air updater.

Built from the finished player zip, so the hashes describe the bytes that are uploaded. The format is defined by the
launcher's updater design note (manifest section); the launcher client parses it strictly, and `validate` below
applies the same rules so a bad manifest is refused here, before it reaches a release.

Integrity is HTTPS plus these hashes. There is no signing: `signature` is the reserved empty string.
The output is deterministic (no timestamps, files sorted by path, fixed key order).
"""

from __future__ import annotations

import hashlib
import json
import re
import zipfile
from pathlib import Path
from typing import Any

SCHEMA = 1
PROJECT = "eaw-revised"
MIN_UPDATER = 1
MANIFEST_NAME = "update-manifest.json"
COMPONENTS = ("game", "launcher", "service", "layers")
CHANNELS = ("stable", "preview")
MAX_BYTES = 2 * 1024 * 1024
MAX_FILES = 20000
MAX_EXPANDED = 4 * 1024 ** 3
NAME_LENGTH = 64
SERVICE_FILES = frozenset({"eawr_mods_service.exe", "eawr_loadcheck.exe"})
LAUNCHER_FILES = frozenset({"EAWR Mods.exe", "launcher-pin.json"})
# <major>.<minor>.<patch>[<letters>]<-|+><label> (a bare 0.3.2m is not a version): owner test kits use 0.3.2m-f8bf25ed or 0.1.0+4e2da633 (label [0-9A-Za-z.+-]).
_VERSION = re.compile(r"v?(\d{1,6})\.(\d{1,6})\.(\d{1,6})(?:[0-9A-Za-z]*[-+]([0-9A-Za-z.+-]+))?")
_NAME = re.compile(r"[A-Za-z0-9_-][A-Za-z0-9_.-]*[A-Za-z0-9_-]|[A-Za-z0-9_-]")
_HEX = re.compile(r"[0-9a-f]{64}")


def split_version(text: str) -> tuple[str, str, bool]:
    """(tag, version, is_prerelease) of a release name such as v0.3.2 or 0.3.2-preview.1."""
    match = _VERSION.fullmatch(text.strip())
    if match is None or len(text.strip()) > 40:
        raise ValueError(f"update manifest: {text!r} is not a release version (v0.3.2, 0.3.2-preview.1 or 0.3.2m-f8bf25ed)")
    pre = match.group(4)
    if pre is not None and (pre.startswith(".") or pre.endswith(".") or ".." in pre):
        raise ValueError(f"update manifest: {text!r} has an invalid pre-release suffix")
    version = text.strip().removeprefix("v")
    return "v" + version, version, pre is not None


def component_of(path: str) -> str:
    """Which component a player-folder file belongs to (launcher, service, layers, otherwise game)."""
    if path.startswith("launcher/") or path in LAUNCHER_FILES:
        return "launcher"
    if path in SERVICE_FILES:
        return "service"
    if path.startswith("Revised/"):
        return "layers"
    return "game"


def safe_relative(path: str) -> bool:
    """Same rules as the client: relative, forward slashes, no empty/./.. segment, no stream or drive, no control chars."""
    if not path or len(path) > 260 or path.startswith("/") or "\\" in path or ":" in path:
        return False
    for segment in path.split("/"):
        if segment in ("", ".", "..") or segment.endswith((".", " ")):
            return False
        if any(ord(c) < 32 or c in '<>"|?*' for c in segment):
            return False
    return True


def _hash_zip_member(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> str:
    digest = hashlib.sha256()
    with archive.open(info) as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build(player_zip: Path, version: str, build_id: str, channel: str | None = None, notes: str = "") -> dict[str, Any]:
    """The manifest dictionary for one player zip (single top folder; every file listed, SHA256SUMS included)."""
    tag, plain, prerelease = split_version(version)
    if channel is None:
        channel = "preview" if prerelease else "stable"
    if channel not in CHANNELS:
        raise ValueError(f"update manifest: unknown channel {channel!r}")
    if channel == "stable" and prerelease:
        raise ValueError(f"update manifest: {tag} is a pre-release and cannot be on the stable channel")
    files: list[dict[str, Any]] = []
    with zipfile.ZipFile(player_zip) as archive:
        infos = [info for info in archive.infolist() if not info.is_dir()]
        tops = {info.filename.split("/", 1)[0] for info in infos}
        if len(tops) != 1 or any("/" not in info.filename for info in infos):
            raise ValueError(f"update manifest: {player_zip.name} must hold exactly one top-level folder")
        top = tops.pop() + "/"
        for info in infos:
            if not safe_relative(info.filename) or not _NAME.fullmatch(top[:-1]):
                raise ValueError(f"update manifest: unsafe entry name in {player_zip.name}: {info.filename[:80]!r}")
        for info in infos:
            files.append({"path": info.filename[len(top):], "size": info.file_size,
                          "sha256": _hash_zip_member(archive, info), "component": ""})
    for entry in files:
        entry["component"] = component_of(entry["path"])
    files.sort(key=lambda entry: entry["path"])
    archive_bytes = player_zip.read_bytes()
    manifest = {
        "schema": SCHEMA,
        "project": PROJECT,
        "tag": tag,
        "version": plain,
        "channel": channel,
        "build_id": build_id,
        "min_updater": MIN_UPDATER,
        "archive": {"name": player_zip.name, "size": len(archive_bytes),
                    "sha256": hashlib.sha256(archive_bytes).hexdigest()},
        "files": files,
        "signature": "",
        "notes": notes,
    }
    validate(manifest)
    return manifest


def validate(manifest: dict[str, Any], expected_tag: str = "", expected_channel: str = "") -> None:
    """Raise ValueError unless the client would accept this manifest (the same checks as its parser)."""
    def need(ok: bool, message: str) -> None:
        if not ok:
            raise ValueError(f"update manifest: {message}")

    def integer(value: Any) -> bool:
        return isinstance(value, int) and not isinstance(value, bool)

    need(manifest.get("schema") == SCHEMA and integer(manifest.get("schema")), "unsupported schema")
    need(manifest.get("project") == PROJECT, "wrong project")
    for key in ("tag", "version", "channel", "build_id", "notes"):
        need(isinstance(manifest.get(key), str), f"field {key} is missing")
    tag, plain, _ = split_version(manifest["tag"])
    need(manifest["tag"] == tag and plain == manifest["version"], "tag and version do not agree")
    for key in ("version", "build_id"):
        value = manifest[key]
        need(len(value) <= NAME_LENGTH and _NAME.fullmatch(value) is not None and not value.startswith("."),
             f"{key} is not a plain name")
    need(manifest["channel"] in CHANNELS, "channel is unknown")
    need(not expected_tag or expected_tag == manifest["tag"], "tag does not match the release")
    need(not expected_channel or expected_channel == manifest["channel"], "channel does not match the release")
    need(integer(manifest.get("min_updater")) and manifest["min_updater"] >= 1, "min_updater is missing")
    need(manifest.get("signature") == "", "signature must be the reserved empty string")
    archive = manifest.get("archive")
    need(isinstance(archive, dict), "archive is missing")
    name = archive.get("name")
    need(isinstance(name, str) and name.endswith(".zip") and _NAME.fullmatch(name) is not None
         and len(name) <= NAME_LENGTH, "archive name is not valid")
    need(integer(archive.get("size")) and 0 < archive["size"] <= MAX_EXPANDED, "archive size is not valid")
    need(isinstance(archive.get("sha256"), str) and _HEX.fullmatch(archive["sha256"]) is not None,
         "archive hash is not valid")
    files = manifest.get("files")
    need(isinstance(files, list) and 0 < len(files) <= MAX_FILES, "file list is missing or too long")
    seen: set[str] = set()
    total = 0
    for entry in files:
        need(isinstance(entry, dict), "file entry is not an object")
        path = entry.get("path")
        need(isinstance(path, str) and safe_relative(path), f"file path is not safe: {str(path)[:80]}")
        need(path.lower() not in seen, f"repeats a file path: {path[:80]}")
        seen.add(path.lower())
        need(integer(entry.get("size")) and entry["size"] >= 0, f"file size is not valid: {path[:80]}")
        total += entry["size"]
        need(isinstance(entry.get("sha256"), str) and _HEX.fullmatch(entry["sha256"]) is not None,
             f"file hash is not valid: {path[:80]}")
        need(entry.get("component") in COMPONENTS, f"file component is not valid: {path[:80]}")
    need(total <= MAX_EXPANDED, "expanded size is too large")
    need(len(render(manifest).encode("utf-8")) <= MAX_BYTES, "manifest is too large")


def render(manifest: dict[str, Any]) -> str:
    """Deterministic text: fixed key order as built, two-space indent, LF, trailing newline."""
    return json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"


def write(player_zip: Path, out: Path, version: str, build_id: str, channel: str | None = None, notes: str = "") -> Path:
    manifest = build(player_zip, version, build_id, channel, notes)
    path = out / MANIFEST_NAME
    path.write_text(render(manifest), encoding="utf-8", newline="\n")
    return path
