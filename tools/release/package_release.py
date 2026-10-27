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
import contextlib
import os
import hashlib
import json
import mmap
import re
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
sys.path.insert(0, str(ROOT / "tools"))
import symbol_store  # noqa: E402
sys.path.insert(0, str(ROOT / "tools" / "release"))
import launcher_artifact  # noqa: E402

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
PLAYER_SAFE_START = "EAWR (Safe render thread).cmd"
# The bundled runtime must be byte-identical to the pinned patched build (EAWR fork fad06b579, MSVC lto=full;
# third_party/godot/README.md), not only report its version. The maintainer-only remote-run tooling keeps pinning the
# stock official exe for its probes, so the two pins differ on purpose.
PINNED_GODOT_SHA256 = "453b4ad56a8f5c64b39c1d75e77468c5227475b2483922dee44ade51cff43442"
DEBUG_GUIDE = ROOT / "docs" / "debug-build.md"
PLAYER_GUIDE = ROOT / "docs" / "player-guide.md"
IMPORT_TIMEOUT_SECONDS = 180
# What a pre-imported project must contain so starting it never writes beside the project.
PREIMPORT_FILES = (".godot/extension_list.cfg", ".godot/global_script_class_cache.cfg", ".godot/uid_cache.bin")
# Nothing the game owns may ride along in the player folder.
FORBIDDEN_PLAYER_SUFFIXES = export_package.FORBIDDEN_PCK_SUFFIXES | {".ttf", ".otf", ".meg", ".tga"}


# The build identity string the CMake build-identity module generates: <version>+<40-hex commit>[+dirty]+gcsync<N>.
BUILD_IDENTITY_PATTERN = re.compile(rb"[0-9][A-Za-z0-9_.-]*\+([0-9a-f]{40})((?:\+[A-Za-z0-9_.-]+)*)")


def embedded_identities(binary: Path) -> list[tuple[str, bool]]:
    """Every (commit, dirty) build identity embedded in a binary."""
    with binary.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        return sorted({(match[1].decode(), b"+dirty" in match[2]) for match in BUILD_IDENTITY_PATTERN.finditer(data)})


def verify_identity(expected: Optional[str], binaries: list[tuple[Path, bool]]) -> None:
    """Refuse binaries built from another commit than the one being packaged (a warm pool tree can carry a stale one).

    binaries is (path, required): a required binary must embed an identity, an optional one only has to match
    when it embeds one. expected None skips the check (library callers); the command line always passes it.
    """
    if expected is None:
        return
    expected = expected.strip().lower()
    if not re.fullmatch(r"[0-9a-f]{40}", expected):
        raise ValueError(f"--expect-identity must be a full 40-character commit hash, not {expected!r}")
    for binary, required in binaries:
        found = embedded_identities(binary)
        if required and not found:
            raise ValueError(f"{binary.name} embeds no build identity (expected commit {expected})")
        for commit, dirty in found:
            if commit != expected or dirty:
                raise ValueError(f"{binary.name} embeds build identity {commit}{'+dirty' if dirty else ''}, "
                                 f"not the packaged commit {expected}: rebuild it (a warm tree keeps a stale identity)")


def identity_binaries(spec: dict, build_dir: Path, configuration: str, launcher: Optional[Path],
                      mods_service: Optional[Path]) -> list[tuple[Path, bool]]:
    binaries = [(VIEWER_PROJECT / "bin" / spec["extension"], True)]
    if mods_service is not None:
        binaries.append((mods_service, True))
        loadcheck = mods_service.parent / ("eawr_loadcheck" + spec["exe"])
        if loadcheck.is_file():
            binaries.append((loadcheck, False))
    if launcher is not None:
        binaries.append((launcher, False))
    binaries += [(find_tool(build_dir, name, spec["exe"], configuration), False) for name in TOOLS]
    return binaries


def reject_payload_leftovers(folder: Path, allowed_pdbs: frozenset[str] = frozenset()) -> None:
    """No import leftover (~*) and no PDB beyond the allowed ones may ride in an archive payload."""
    for path in sorted(folder.rglob("*")):
        relative = path.relative_to(folder).as_posix()
        if path.is_file() and (path.name.startswith("~") or (path.suffix.lower() == ".pdb" and relative not in allowed_pdbs)):
            raise ValueError(f"the package payload would carry a build leftover or private symbols: {relative}")


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
    for leftover in (project / "bin").glob("~*"):  # the patched Godot's import copies the extension and its PDB there
        leftover.unlink()
    missing = [name for name in PREIMPORT_FILES if not (project / name).is_file()]
    if missing:
        raise RuntimeError(f"the pre-import did not produce {', '.join(missing)}" + (f" ({output})" if output else ""))


def build_player(stem: str, spec: dict, out: Path, godot: Path, launcher: Optional[Path],
                 importer: Callable[[Path, Path], Optional[str]] = godot_import,
                 debug_build_dir: Optional[Path] = None, mods_service: Optional[Path] = None,
                 manager_artifact: Optional[Path] = None,
                 launcher_pin: Path = launcher_artifact.DEFAULT_PIN, with_mods_manager: bool = False) -> Path:
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
        raise ValueError(f"{godot.name} sha256 {observed} is not the pinned patched build {PINNED_GODOT_SHA256}")
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
        if with_mods_manager:
            cache = Path(temporary) / "manager-cache"
            cache.mkdir()
            manager_artifact = launcher_artifact.prepare(mods_service, launcher, manager_artifact, launcher_pin, cache)
        folder = Path(temporary) / top
        project = folder / "project"
        for path in sorted(VIEWER_PROJECT.rglob("*")):
            relative = path.relative_to(VIEWER_PROJECT)
            if not path.is_file() or ".godot" in relative.parts:
                continue
            if not debug and path.suffix.lower() == ".pdb":
                continue
            if relative.parts[0] == "bin" and path.name != spec["extension"] and not (debug and path == extension_pdb):
                continue  # another platform's extension, or build leftovers
            (project / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, project / relative)
        (folder / "godot").mkdir()
        shutil.copyfile(godot, folder / PLAYER_GODOT)
        launcher_sha = None
        if launcher is not None:
            shutil.copyfile(launcher, folder / PLAYER_LAUNCHER)
            launcher_sha = sha256_file(launcher)
        else:
            print(f"package_release: no --launcher: the player zip has no {PLAYER_LAUNCHER} yet", file=sys.stderr)
        if with_mods_manager:
            launcher_artifact.install_manager(folder, mods_service, manager_artifact, launcher_pin)
        preimport_project(folder / PLAYER_GODOT, project, importer)
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            shutil.copyfile(ROOT / name, folder / name)
        shutil.copyfile(PLAYER_GUIDE, folder / "README.md")
        # Opt-out from the default Separate render thread: the same launcher with the Safe engine mode.
        (folder / PLAYER_SAFE_START).write_text(
            f'@echo off\r\nstart "" "%~dp0{PLAYER_LAUNCHER}" --safe-render-thread %*\r\n', encoding="utf-8", newline="")
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
        reject_payload_leftovers(folder, frozenset(
            {f"project/bin/{extension_pdb.name}",
             *(f"tools/{Path(name).with_suffix('.pdb').name}" for name in tool_sha256)} if debug else ()))
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
            importer: Callable[[Path, Path], Optional[str]] = godot_import,
            mods_service: Optional[Path] = None, manager_artifact: Optional[Path] = None,
            launcher_pin: Path = launcher_artifact.DEFAULT_PIN, with_mods_manager: bool = False,
            expect_identity: Optional[str] = None) -> list[Path]:
    spec = PLATFORMS[platform]
    if with_mods_manager and godot is None:
        raise ValueError("manager packaging needs the player folder (--godot)")
    if godot is not None and platform != "windows-x64":
        raise ValueError("the player folder is Windows x64 only for now")
    verify_identity(expect_identity, identity_binaries(spec, build_dir, "Release", launcher, mods_service))
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
            if path.suffix.lower() == ".pdb":
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
        files.append(build_player(stem, spec, out, godot, launcher, importer, mods_service=mods_service,
                                  manager_artifact=manager_artifact, launcher_pin=launcher_pin,
                                  with_mods_manager=with_mods_manager))
    return files


def package_debug(build_dir: Path, version: str, out: Path, godot: Path, launcher: Optional[Path],
                  importer: Callable[[Path, Path], Optional[str]] = godot_import,
                  mods_service: Optional[Path] = None, manager_artifact: Optional[Path] = None,
                  launcher_pin: Path = launcher_artifact.DEFAULT_PIN, with_mods_manager: bool = False,
                  expect_identity: Optional[str] = None) -> Path:
    """The Windows x64 debug variant (eaw-revised-<version>-windows-x64-debug.zip).

    build_dir is the root build's Debug configuration; the viewer project's bin/ must hold the Debug extension
    and its PDB when this runs (build the Release archives first: both builds write to the same bin/).
    """
    spec = PLATFORMS["windows-x64"]
    verify_identity(expect_identity, identity_binaries(spec, build_dir, "Debug", launcher, mods_service))
    out.mkdir(parents=True, exist_ok=True)
    return build_player(f"eaw-revised-{version}-windows-x64", spec, out, godot, launcher, importer, build_dir, mods_service, manager_artifact, launcher_pin, with_mods_manager)


def write_sums(files: list[Path], out: Path) -> Path:
    sums = out / "SHA256SUMS.txt"
    lines = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}" for path in sorted(files)]
    sums.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return sums


def retain_release_symbols(build_dir, version, store, debug=False):
    """Private archive is deliberately outside the public assets and sums list."""
    identity = symbol_store.build_identity(Path(build_dir) / "src/core/generated/build_identity.cpp")
    binaries = [VIEWER_PROJECT / "bin" / PLATFORMS["windows-x64"]["extension"]]
    binaries += [find_tool(build_dir, name, ".exe", "Debug" if debug else "Release") for name in TOOLS]
    return symbol_store.archive(store, identity, binaries,
                                "release-" + version + ("-Debug" if debug else "-Release"), release=True)


def git_head() -> str:
    result = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise ValueError("cannot read the packaged commit from git: pass --expect-identity <commit>")
    return result.stdout.strip()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, help="the root CMake build (out/build/<preset>)")
    parser.add_argument("--platform", choices=sorted(PLATFORMS))
    parser.add_argument("--version")
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "release")
    parser.add_argument("--symbol-store", type=Path,
                        default=Path(os.environ["EAWR_SYMBOL_STORE"]) if os.environ.get("EAWR_SYMBOL_STORE") else None,
                        help="private Windows symbol archive location (outside public release output); "
                             "defaults to $EAWR_SYMBOL_STORE, required for windows-x64")
    parser.add_argument("--godot", type=Path,
                        help="pinned standard Godot 4.7.2 Windows GUI executable: also write the player zip")
    parser.add_argument("--launcher", type=Path, help="the launcher exe to place at the player folder's root as EAWR.exe")
    parser.add_argument("--expect-identity", metavar="SHA",
                        help="the full commit hash every shipped binary must embed as its build identity "
                             "(default: git HEAD of this checkout); packaging is refused on any other")
    parser.add_argument("--sums-only", action="store_true", help="only write SHA256SUMS.txt for the zips in --out")
    parser.add_argument("--debug", action="store_true",
                        help="write only the Windows x64 debug archive: --build-dir is the root build's Debug "
                             "configuration, the viewer project's bin/ holds the Debug extension and its PDB "
                             "(needs --godot and --launcher); SHA256SUMS.txt is left to --sums-only")
    parser.add_argument("--mods-service", type=Path, help="pool-built static-CRT mods service")
    parser.add_argument("--launcher-artifact", type=Path, help="local pinned manager zip; otherwise GitHub release")
    parser.add_argument("--launcher-pin", type=Path, default=launcher_artifact.DEFAULT_PIN)
    parser.add_argument("--with-mods-manager", action="store_true", help="include the pinned manager and mods service")
    args = parser.parse_args(argv)
    sums = None
    try:
        with contextlib.ExitStack() as resources:
            if args.sums_only:
                files = sorted(args.out.glob("*.zip"))
                if not files:
                    raise FileNotFoundError(f"no zips in {args.out}")
            else:
                if not (args.build_dir and args.platform and args.version):
                    parser.error("--build-dir, --platform and --version are required")
                if args.expect_identity is None:
                    args.expect_identity = git_head()
                if args.with_mods_manager:
                    if not args.godot or args.platform != "windows-x64":
                        parser.error("--with-mods-manager needs --godot and --platform windows-x64")
                    if not (args.launcher and args.mods_service):
                        parser.error("--with-mods-manager needs --launcher and --mods-service")
                    cache = Path(resources.enter_context(tempfile.TemporaryDirectory(prefix="eawr-manager-pin-")))
                    args.launcher_artifact = launcher_artifact.prepare(args.mods_service, args.launcher,
                        args.launcher_artifact, args.launcher_pin, cache)
                elif args.godot and not args.launcher:
                    parser.error("--godot needs --launcher")
                if args.platform == "windows-x64":
                    if args.symbol_store is None:
                        parser.error("windows-x64 needs --symbol-store or the EAWR_SYMBOL_STORE environment variable")
                    if args.symbol_store.resolve().is_relative_to(args.out.resolve()):
                        raise ValueError("private symbols must be outside the public release output")
                    retained = retain_release_symbols(args.build_dir, args.version, args.symbol_store, args.debug)
                    print(f"Private symbols retained: {retained}", file=sys.stderr)
                if args.debug:
                    if args.platform != "windows-x64" or not args.godot:
                        parser.error("--debug needs --platform windows-x64 and --godot")
                    files = [package_debug(args.build_dir, args.version, args.out, args.godot, args.launcher,
                                           mods_service=args.mods_service, manager_artifact=args.launcher_artifact,
                                           launcher_pin=args.launcher_pin, with_mods_manager=args.with_mods_manager,
                                           expect_identity=args.expect_identity)]
                else:
                    files = package(args.build_dir, args.platform, args.version, args.out, args.godot, args.launcher,
                                    mods_service=args.mods_service, manager_artifact=args.launcher_artifact,
                                    launcher_pin=args.launcher_pin, with_mods_manager=args.with_mods_manager,
                                    expect_identity=args.expect_identity)
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
