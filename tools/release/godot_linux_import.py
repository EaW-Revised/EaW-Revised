#!/usr/bin/env python3
"""The packaging pre-import on a Linux host, with the official Linux Godot 4.7.2 editor.

A player folder ships the patched Windows Godot exe as a file and a project whose `.godot` metadata was written beforehand so a
read-only folder starts without writing (package_release.preimport_project). The only thing the import does for this project is
write three metadata files (`extension_list.cfg`, `global_script_class_cache.cfg`, `uid_cache.bin`; the viewer project holds no
imported resources, and the uid of `eawr_viewer.gdextension` is derived from its path). They do not depend on the patched
renderer or on the operating system, so a Linux host can write them with the pinned official Linux editor instead of running the
Windows exe through Wine. Proof 2026-11-14: the three files and `.godot/.gdignore` were byte-identical to the ones the patched
Windows exe wrote for the same project (kit e706), with and without the Linux viewer extension present.

The shipped Windows exe is never run on Linux: package_release identifies it by its pinned SHA-256 and the fork commit embedded in
its bytes.

  python tools/release/godot_linux_import.py fetch <dir>      download + verify the pinned editor into <dir>, print its path
  python tools/release/godot_linux_import.py verify <exe>     check the exe against the pin and report its version
"""
from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Optional

LINUX_GODOT_NAME = "Godot_v4.7.2-stable_linux.x86_64"
LINUX_GODOT_ZIP = LINUX_GODOT_NAME + ".zip"
LINUX_GODOT_URL = "https://github.com/godotengine/godot-builds/releases/download/4.7.2-stable/" + LINUX_GODOT_ZIP
# The official release's own SHA512-SUMS.txt line for the zip, and the SHA-256 of the exe inside it.
LINUX_GODOT_ZIP_SHA512 = ("9aa00f7a605200940bce3027a567b782f49bd8e940dd06ae9e987bd65aee1b1467edd56ed84fcdcbdd44354bf613bdbb4e"
                          "5d2913e925850368e150c59ed54c65")
LINUX_GODOT_SHA256 = "8d106cbe6144c2dc7e881d61d2429c1a8a76e6b22ef48bd5e48dcf934953f71e"
LINUX_GODOT_VERSION = "4.7.2.stable.official.ed1daf0bf"
IMPORT_TIMEOUT_SECONDS = 180
# No console window on a Windows desktop (tools/ci/console_window_guard.py); 0 on the Linux host.
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def check_pinned(godot: Path) -> str:
    """The exe is the pinned official Linux editor, byte for byte; returns its SHA-256."""
    godot = Path(godot)
    if not godot.is_file():
        raise ValueError(f"the Linux import Godot is missing: {godot}")
    observed = sha256_file(godot)
    if observed != LINUX_GODOT_SHA256:
        raise ValueError(f"{godot.name} sha256 {observed} is not the pinned official Linux Godot {LINUX_GODOT_SHA256}")
    return observed


def _isolated_environment(home: Path) -> dict[str, str]:
    """A scrubbed environment: no editor settings, cache or fontconfig state of the host's user leaks in or out."""
    return {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(home), "XDG_CONFIG_HOME": str(home / "config"),
            "XDG_DATA_HOME": str(home / "data"), "XDG_CACHE_HOME": str(home / "cache"), "LANG": "C", "LC_ALL": "C"}


def import_project(godot: Path, project: Path) -> str:
    """Run the pre-import on a staged project (the same call the Windows route makes: `--headless --path <project> --import`).

    The exit code proves nothing (the viewer's host node may stop the run itself); package_release.preimport_project checks the
    files. Returns the tail of the output for its error message."""
    check_pinned(godot)
    with tempfile.TemporaryDirectory(prefix="eawr-linux-import-") as home:
        result = subprocess.run([str(godot), "--headless", "--path", str(project), "--import"], capture_output=True, text=True,
                                timeout=IMPORT_TIMEOUT_SECONDS, check=False, env=_isolated_environment(Path(home)),
                                creationflags=NO_WINDOW)
    return f"exit {result.returncode}: {(result.stdout + result.stderr)[-400:]}"


def importer(godot: Path):
    """The importer callable package_release takes: (shipped Windows exe, project) -> output tail. The shipped exe is only a
    path here; the Linux editor does the work."""
    godot = Path(godot)
    check_pinned(godot)

    def run(_shipped: Path, project: Path) -> Optional[str]:
        return import_project(godot, project)
    return run


def fetch(directory: Path) -> Path:
    """Download the official zip, check its SHA-512 and the exe's SHA-256, and unpack the exe into `directory`."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / LINUX_GODOT_NAME
    if target.is_file() and sha256_file(target) == LINUX_GODOT_SHA256:
        return target
    with tempfile.TemporaryDirectory(prefix="eawr-linux-godot-") as scratch:
        archive = Path(scratch) / LINUX_GODOT_ZIP
        with urllib.request.urlopen(LINUX_GODOT_URL, timeout=300) as response, archive.open("wb") as stream:
            for block in iter(lambda: response.read(1024 * 1024), b""):
                stream.write(block)
        digest = hashlib.sha512(archive.read_bytes()).hexdigest()
        if digest != LINUX_GODOT_ZIP_SHA512:
            raise ValueError(f"{LINUX_GODOT_ZIP} sha512 {digest} is not the official release's {LINUX_GODOT_ZIP_SHA512}")
        with zipfile.ZipFile(archive) as zipped:
            if LINUX_GODOT_NAME not in zipped.namelist():
                raise ValueError(f"{LINUX_GODOT_ZIP} holds no {LINUX_GODOT_NAME}")
            part = directory / (LINUX_GODOT_NAME + ".part")
            part.write_bytes(zipped.read(LINUX_GODOT_NAME))
    part.chmod(0o755)
    check_pinned(part)
    part.replace(target)
    return target


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("fetch").add_argument("directory", type=Path)
    sub.add_parser("verify").add_argument("exe", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "fetch":
            print(fetch(args.directory))
        else:
            check_pinned(args.exe)
            version = subprocess.run([str(args.exe), "--version"], capture_output=True, text=True, timeout=30,
                                     creationflags=NO_WINDOW).stdout.strip()
            if version != LINUX_GODOT_VERSION:
                raise ValueError(f"--version reports {version!r}, expected {LINUX_GODOT_VERSION!r}")
            print(f"{args.exe}: {version} sha256 {LINUX_GODOT_SHA256}")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"godot_linux_import: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
