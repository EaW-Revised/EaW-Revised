#!/usr/bin/env python3
"""Package one platform's release archives from a finished build (docs/releasing.md).

Writes two zips into --out:
  eaw-revised-<version>-<platform>-tools.zip   the headless tools (sim_headless, the scanners, benchmarks)
  eaw-revised-<version>-<platform>-viewer.zip  the Godot viewer project with its built GDExtension
Each carries LICENSE, THIRD_PARTY_NOTICES.md and README.md at its top. No game data,
fonts or Godot binaries go in: players supply their own game installation and the pinned Godot 4.7.2.

With --godot (the pinned standard Godot 4.7.2 Windows GUI executable) a Windows build also writes
  eaw-revised-<version>-windows-x64-player.zip  the portable player folder: EAWR.exe (--launcher), the Godot
                                                 runtime (sha256 and identity verified), the viewer project
                                                 pre-imported so a read-only folder works, licences, the player
                                                 guide and a SHA256SUMS of every file in the folder
No retail bytes go in: the player's fonts are extracted into Godot user data at first start.
With --debug (after the Release archives, see docs/releasing.md) it writes only
  eaw-revised-<version>-windows-x64-debug.zip   the same folder built from the Debug configuration: the viewer
                                                 extension and the headless tools with their PDBs, asserts on

Usage:
  python tools/release/package_release.py --build-dir out/build/linux-x64-gcc --platform linux-x64 \\
      --version v0.1.5 --out out/release
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Callable, Optional

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "apps" / "viewer" / "tools"))
import export_package  # noqa: E402  (the pinned Godot identity and licence bundling)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/common"))
from eawr_python_helpers import sha256_file  # noqa: E402

TOOLS = ("sim_headless", "sim_bench", "path_bench", "asset_scan", "asset_validate", "xml_scan", "scene_scan",
         "sky_scan", "unit_scan", "script_smoke", "hud_movie_prepare")
DOCS = ("LICENSE", "THIRD_PARTY_NOTICES.md", "README.md")
VIEWER_PROJECT = ROOT / "apps" / "viewer" / "project"
PLATFORMS = {
    "linux-x64": {"exe": "", "extension": "libeawr_viewer.linux.template_release.x86_64.so"},
    "linux-arm64": {"exe": "", "extension": "libeawr_viewer.linux.template_release.arm64.so"},
    "windows-x64": {"exe": ".exe", "extension": "libeawr_viewer.windows.template_release.x86_64.dll"},
}


# The player folder (#3961): the launcher exe at its root, the pinned Godot beside it, the viewer project.
PLAYER_LAUNCHER = "EAWR.exe"
PLAYER_GODOT = "godot/Godot_v4.7.2-stable_win64.exe"
PLAYER_SUMS = "SHA256SUMS"
# The bundled runtime must be byte-identical to the pinned official build, not only report its version.
# (The maintainer-only remote-run tooling pins the same bytes; tests/ci/test_package_release.py compares them
# where that tooling exists.)
PINNED_GODOT_SHA256 = "ab1824f85bfd8e0e4128182c000c4003a3e042245b2967848d089b2a04b22424"
DEBUG_GUIDE = ROOT / "docs" / "debug-build.md"
PLAYER_GUIDE = ROOT / "docs" / "player-guide.md"
IMPORT_TIMEOUT_SECONDS = 180
# What a pre-imported project must contain so starting it never writes beside the project.
PREIMPORT_FILES = (".godot/extension_list.cfg", ".godot/global_script_class_cache.cfg", ".godot/uid_cache.bin")
# Nothing the game owns may ride along in the player folder.
FORBIDDEN_PLAYER_SUFFIXES = export_package.FORBIDDEN_PCK_SUFFIXES | {".ttf", ".otf", ".meg", ".tga"}


def find_tool(build_dir: Path, name: str, suffix: str, configuration: str = "Release") -> Path:
    for candidate in (build_dir / "apps" / name / (name + suffix),
                      build_dir / "apps" / name / configuration / (name + suffix)):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"{name}{suffix} not built under {build_dir}/apps/{name}")


def add_docs(archive: zipfile.ZipFile, top: str) -> None:
    for name in DOCS:
        archive.write(ROOT / name, f"{top}/{name}")


def godot_import(godot: Path, project: Path) -> Optional[str]:
    """Let the pinned Godot write the project's .godot metadata once, headless, at packaging time.

    The exit code proves nothing (the viewer's host node may stop the run itself); preimport_project checks
    the files. Returns the tail of the output for its error message.
    """
    result = subprocess.run([str(godot), "--headless", "--path", str(project), "--import"], capture_output=True,
                            text=True, timeout=IMPORT_TIMEOUT_SECONDS, check=False)
    return f"exit {result.returncode}: {(result.stdout + result.stderr)[-400:]}"


def preimport_project(godot: Path, project: Path, importer: Callable[[Path, Path], Optional[str]] = godot_import) -> None:
    """Generate .godot beside a staged project, so a read-only player folder starts without writing.

    The editor layout the import also creates is dropped: only the files a run reads stay.
    """
    output = importer(godot, project)
    shutil.rmtree(project / ".godot" / "editor", ignore_errors=True)
    missing = [name for name in PREIMPORT_FILES if not (project / name).is_file()]
    if missing:
        raise RuntimeError(f"the pre-import did not produce {', '.join(missing)}" + (f" ({output})" if output else ""))


def build_player(stem: str, spec: dict, out: Path, godot: Path, launcher: Optional[Path],
                 importer: Callable[[Path, Path], Optional[str]] = godot_import,
                 debug_build_dir: Optional[Path] = None) -> Path:
    """The portable Windows player folder as a zip: launcher slot, pinned Godot GUI runtime, pre-imported project.

    With debug_build_dir (the Debug configuration of the root build) the same folder is written as the debug
    variant: the viewer project's current extension must be the Debug one and carry its PDB, and the headless tools
    of that build ride along in tools/ with their PDBs (docs/debug-build.md ships as DEBUG-BUILD.md).
    """
    debug = debug_build_dir is not None
    if godot.name.casefold() != Path(PLAYER_GODOT).name.casefold():
        raise ValueError(f"--godot must be the standard GUI executable {Path(PLAYER_GODOT).name} (no console companion)")
    observed = sha256_file(godot)
    if observed != PINNED_GODOT_SHA256:
        raise ValueError(f"{godot.name} sha256 {observed} is not the pinned official build {PINNED_GODOT_SHA256}")
    identity, _ = export_package.validate_godot_identity(godot)
    extension = VIEWER_PROJECT / "bin" / spec["extension"]
    if not extension.is_file():
        raise FileNotFoundError(f"viewer extension not built: {extension}")
    variant = "debug" if debug else "player"
    top = f"{stem}-{variant}"
    extension_pdb = extension.with_suffix(".pdb")
    if debug and not extension_pdb.is_file():
        raise FileNotFoundError(f"the debug viewer extension has no PDB beside it: {extension_pdb}")
    with tempfile.TemporaryDirectory(prefix=f"eawr-{variant}-") as temporary:
        folder = Path(temporary) / top
        project = folder / "project"
        for path in sorted(VIEWER_PROJECT.rglob("*")):
            relative = path.relative_to(VIEWER_PROJECT)
            if not path.is_file() or ".godot" in relative.parts:
                continue
            if relative.parts[0] == "bin" and path.name != spec["extension"] and not (debug and path == extension_pdb):
                continue  # another platform's extension, or build leftovers
            (project / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, project / relative)
        (folder / "godot").mkdir()
        shutil.copyfile(godot, folder / PLAYER_GODOT)
        preimport_project(folder / PLAYER_GODOT, project, importer)
        launcher_sha = None
        if launcher is not None:
            shutil.copyfile(launcher, folder / PLAYER_LAUNCHER)
            launcher_sha = sha256_file(launcher)
        else:
            print(f"package_release: no --launcher: the player zip has no {PLAYER_LAUNCHER} yet", file=sys.stderr)
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            shutil.copyfile(ROOT / name, folder / name)
        shutil.copyfile(PLAYER_GUIDE, folder / "README.md")
        tool_sha256: dict[str, str] = {}
        if debug:
            shutil.copyfile(DEBUG_GUIDE, folder / "DEBUG-BUILD.md")
            (folder / "tools").mkdir()
            for name in TOOLS:
                binary = find_tool(debug_build_dir, name, spec["exe"], "Debug")
                symbols = binary.with_suffix(".pdb")
                if not symbols.is_file():
                    raise FileNotFoundError(f"the debug tool has no PDB beside it: {symbols}")
                shutil.copyfile(binary, folder / "tools" / binary.name)
                shutil.copyfile(symbols, folder / "tools" / symbols.name)
                tool_sha256[binary.name] = sha256_file(binary)
        export_package.bundle_licenses(folder)
        runtime = {
            "schema": "eawr-player-runtime/1",
            "variant": variant,
            "godot_identity": identity,
            "godot_sha256": sha256_file(godot),
            "launcher_sha256": launcher_sha,
            "extension_sha256": sha256_file(extension),
        }
        if debug:
            runtime["configuration"] = "Debug"
            runtime["tools_sha256"] = tool_sha256
        (folder / "runtime.json").write_text(json.dumps(runtime, indent=2) + "\n", encoding="utf-8", newline="\n")
        files = sorted(path for path in folder.rglob("*") if path.is_file())
        forbidden = [str(path.relative_to(folder)) for path in files
                     if path.suffix.lower() in FORBIDDEN_PLAYER_SUFFIXES or path.name.casefold() == "starwarsg.exe"]
        if forbidden:
            raise ValueError(f"the player folder would carry game or generated files: {forbidden}")
        sums = "".join(f"{sha256_file(path)}  {path.relative_to(folder).as_posix()}\n" for path in files)
        (folder / PLAYER_SUMS).write_text(sums, encoding="utf-8", newline="\n")
        files.append(folder / PLAYER_SUMS)
        player_zip = out / f"{stem}-{variant}.zip"
        with zipfile.ZipFile(player_zip, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in files:
                archive.write(path, f"{top}/{path.relative_to(folder).as_posix()}")
    return player_zip


def package(build_dir: Path, platform: str, version: str, out: Path, godot: Optional[Path] = None,
            launcher: Optional[Path] = None,
            importer: Callable[[Path, Path], Optional[str]] = godot_import) -> list[Path]:
    spec = PLATFORMS[platform]
    if godot is not None and platform != "windows-x64":
        raise ValueError("the player folder is Windows x64 only for now")
    out.mkdir(parents=True, exist_ok=True)
    stem = f"eaw-revised-{version}-{platform}"

    tools_zip = out / f"{stem}-tools.zip"
    with zipfile.ZipFile(tools_zip, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in TOOLS:
            binary = find_tool(build_dir, name, spec["exe"])
            info = zipfile.ZipInfo.from_file(binary, f"{stem}-tools/bin/{binary.name}")
            info.external_attr = 0o755 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, binary.read_bytes())
        add_docs(archive, f"{stem}-tools")

    extension = VIEWER_PROJECT / "bin" / spec["extension"]
    if not extension.is_file():
        raise FileNotFoundError(f"viewer extension not built: {extension}")
    viewer_zip = out / f"{stem}-viewer.zip"
    with zipfile.ZipFile(viewer_zip, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(VIEWER_PROJECT.rglob("*")):
            if not path.is_file() or ".godot" in path.relative_to(VIEWER_PROJECT).parts:
                continue
            relative = path.relative_to(VIEWER_PROJECT).as_posix()
            if relative.startswith("bin/") and path.name != spec["extension"]:
                continue  # another platform's extension, or build leftovers
            archive.write(path, f"{stem}-viewer/project/{relative}")
        add_docs(archive, f"{stem}-viewer")
        for name in ("play-demo.cmd", "play-demo.sh", "demo.py"):
            path = ROOT / "tools" / "release" / name
            info = zipfile.ZipInfo.from_file(path, f"{stem}-viewer/{name}")
            info.external_attr = (0o755 if name.endswith(".sh") else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, path.read_bytes())
        archive.write(ROOT / "tools/fonts/extract_eaw_fonts.py", f"{stem}-viewer/extract_eaw_fonts.py")
    files = [tools_zip, viewer_zip]
    if godot is not None:
        files.append(build_player(stem, spec, out, godot, launcher, importer))
    return files


def package_debug(build_dir: Path, version: str, out: Path, godot: Path, launcher: Optional[Path],
                  importer: Callable[[Path, Path], Optional[str]] = godot_import) -> Path:
    """The Windows x64 debug variant (eaw-revised-<version>-windows-x64-debug.zip).

    build_dir is the root build's Debug configuration; the viewer project's bin/ must hold the Debug extension
    and its PDB when this runs (build the Release archives first: both builds write to the same bin/).
    """
    spec = PLATFORMS["windows-x64"]
    out.mkdir(parents=True, exist_ok=True)
    return build_player(f"eaw-revised-{version}-windows-x64", spec, out, godot, launcher, importer, build_dir)


def write_sums(files: list[Path], out: Path) -> Path:
    sums = out / "SHA256SUMS.txt"
    lines = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}" for path in sorted(files)]
    sums.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return sums


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, help="the root CMake build (out/build/<preset>)")
    parser.add_argument("--platform", choices=sorted(PLATFORMS))
    parser.add_argument("--version")
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "release")
    parser.add_argument("--godot", type=Path,
                        help="pinned standard Godot 4.7.2 Windows GUI executable: also write the player zip")
    parser.add_argument("--launcher", type=Path, help="the launcher exe to place at the player folder's root as EAWR.exe")
    parser.add_argument("--allow-missing-launcher", action="store_true",
                        help="write a player zip without EAWR.exe (only while the launcher is not built)")
    parser.add_argument("--sums-only", action="store_true", help="only write SHA256SUMS.txt for the zips in --out")
    parser.add_argument("--debug", action="store_true",
                        help="write only the Windows x64 debug archive: --build-dir is the root build's Debug "
                             "configuration, the viewer project's bin/ holds the Debug extension and its PDB "
                             "(needs --godot and --launcher); SHA256SUMS.txt is left to --sums-only")
    args = parser.parse_args(argv)
    sums = None
    try:
        if args.sums_only:
            files = sorted(args.out.glob("*.zip"))
            if not files:
                raise FileNotFoundError(f"no zips in {args.out}")
        else:
            if not (args.build_dir and args.platform and args.version):
                parser.error("--build-dir, --platform and --version are required")
            if args.godot and not args.launcher and not args.allow_missing_launcher:
                parser.error("--godot needs --launcher (or --allow-missing-launcher)")
            if args.debug:
                if args.platform != "windows-x64" or not args.godot:
                    parser.error("--debug needs --platform windows-x64 and --godot")
                files = [package_debug(args.build_dir, args.version, args.out, args.godot, args.launcher)]
            else:
                files = package(args.build_dir, args.platform, args.version, args.out, args.godot, args.launcher)
        if not args.debug:
            sums = write_sums(files, args.out)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"package_release: {error}", file=sys.stderr)
        return 2
    for path in files + ([sums] if sums else []):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
