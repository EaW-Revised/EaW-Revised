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
import update_manifest  # noqa: E402
from shader_runtime_package import package as shader_runtime_package  # noqa: E402

TOOLS = ("sim_headless", "sim_bench", "path_bench", "asset_scan", "asset_validate", "xml_scan", "scene_scan",
         "sky_scan", "unit_scan", "script_smoke", "hud_movie_prepare")
DOCS = ("LICENSE", "THIRD_PARTY_NOTICES.md")
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
REVISED_LAYER_RELPATH = Path("Revised")
REVISED_LAYER_MANIFEST = ROOT / "tools/release/revised-layer-manifest.json"
REVISED_LAYER_README = "The Revised voice and hotkey layer is included and enabled by default."
# The bundled runtime must be byte-identical to the pinned patched build (EAWR fork 5b733a90f, MSVC lto=full;
# third_party/godot/README.md), not only report its version. The maintainer-only remote-run tooling keeps pinning the
# stock official exe for its probes, so the two pins differ on purpose.
PINNED_GODOT_SHA256 = "96c03783011f197aff146e92c1e33e0000e8975b3a873fbfcc52cd86dff12b7f"
DEBUG_GUIDE = ROOT / "docs" / "debug-build.md"
PLAYER_GUIDE = ROOT / "docs" / "player-guide.md"
IMPORT_TIMEOUT_SECONDS = 180
# Through a runner (Wine on a Linux host) the first run in a cold prefix is slow: the version probe alone needs up to
# export_package.GODOT_RUNNER_VERSION_TIMEOUT_SECONDS, and the import does real work after it.
RUNNER_IMPORT_TIMEOUT_SECONDS = 600
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


KIT_LABEL_FILE = ROOT / "tools" / "release" / "kit-label.txt"
KIT_LABEL_RULE = "the kit label must be 1-32 characters from A-Z a-z 0-9 . _ - (no spaces, paths or quotes)"


def read_kit_label(path: Path = KIT_LABEL_FILE) -> str:
    """The kit a release is cut as: the first line of tools/release/kit-label.txt, '' when the cut has no file.

    The offload build scripts pass the same line to CMake as EAWR_KIT_LABEL; this packager builds nothing and only
    checks the binaries against it.
    """
    if not path.is_file():
        return ""
    # utf-8-sig: a file saved by PowerShell may start with a byte order mark, which the offload scripts' reader drops.
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    label = lines[0].strip() if lines else ""
    if label and not re.fullmatch(r"[A-Za-z0-9._-]{1,32}", label):
        raise ValueError(f"kit label {label!r} in {path.name} is invalid: {KIT_LABEL_RULE}")
    return label


def verify_kit_label(label: str, binaries: list[tuple[Path, bool]]) -> None:
    """A cut that carries a kit label must ship binaries whose identity starts with it (a warm tree keeps an old one)."""
    if not label:
        return
    # The identity string is not NUL-delimited: in the real binaries it follows the hex digit table ("...6789abcdef")
    # directly. So the label starts either after a character outside the label alphabet or right after that table;
    # any other label character before it means a longer, different label (10.3.2g is not 0.3.2g).
    # Residual: a label that begins with the table's last character 'f' cannot be told from the table glued to a
    # shorter label. Accepted: the label is the committed kit-label.txt and the identity always starts with it.
    prefix = re.compile(rb"(?:(?<![A-Za-z0-9_.-])|(?<=0123456789abcdef))" + re.escape(label.encode())
                        + rb"\+[0-9a-f]{40}")
    for binary, required in binaries:
        with binary.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            if BUILD_IDENTITY_PATTERN.search(data) and not prefix.search(data):
                raise ValueError(f"{binary.name} embeds a build identity without the kit label {label}: rebuild it "
                                 f"from the cut commit (a warm tree keeps a stale one)")


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


def add_docs(archive: zipfile.ZipFile, top: str, readme: str) -> None:
    for name in DOCS:
        archive.write(ROOT / name, f"{top}/{name}")
    archive.write(ROOT / "tools" / "release" / readme, f"{top}/README.md")


def import_timeout() -> int:
    return RUNNER_IMPORT_TIMEOUT_SECONDS if os.environ.get("EAWR_GODOT_RUNNER") else IMPORT_TIMEOUT_SECONDS


def godot_import(godot: Path, project: Path) -> Optional[str]:
    """Let the pinned Godot write the project's .godot metadata once, headless, at packaging time.

    The exit code proves nothing (the viewer's host node may stop the run itself); preimport_project checks
    the files. Returns the tail of the output for its error message.
    """
    result = subprocess.run(export_package.godot_command(godot, "--headless", "--path", str(project), "--import"), capture_output=True,
                            text=True, timeout=import_timeout(), check=False)
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


def stage_revised_layer(folder: Path, source: Optional[Path], allow_missing: bool, public: bool) -> bool:
    """Stage the layer; public packages require the exact reviewed files and bytes."""
    if source is None or not source.is_dir():
        message = "Revised layer source is missing; set --revised-layer or EAWR_REVISED_LAYER_DIR"
        if public or not allow_missing:
            raise FileNotFoundError(message)
        print(f"package_release: warning: {message}", file=sys.stderr)
        return False
    if not all((source / "Data" / name).is_dir() for name in ("Audio", "XML")):
        raise ValueError("Revised layer source must contain Data/Audio and Data/XML")
    if any(path.is_symlink() for path in source.rglob("*")):
        raise ValueError("Revised layer source must not contain symlinks")
    target = folder / REVISED_LAYER_RELPATH
    before = {p.relative_to(source): (p.stat().st_size, sha256_file(p)) for p in source.rglob("*") if p.is_file()}
    if not before:
        raise ValueError("Revised layer source contains no files")
    if public:
        manifest = json.loads(REVISED_LAYER_MANIFEST.read_text(encoding="utf-8"))
        expected = {Path(name): (entry["size"], entry["sha256"])
                    for name, entry in manifest["files"].items()}
        if manifest.get("schema") != 1 or before != expected:
            raise ValueError("public Revised layer differs from the reviewed file manifest")
    shutil.copytree(source, target)
    after = {p.relative_to(target): (p.stat().st_size, sha256_file(p)) for p in target.rglob("*") if p.is_file()}
    if before != after:
        raise ValueError("Revised layer copy verification failed (file count, sizes or hashes differ)")
    return True


def archive_stem(version: str, platform: str) -> str:
    """Keep archive and top-folder names within the shipped updater's name limit.

    Normal release names stay unchanged. Long kit labels retain a readable
    prefix plus a digest of the full version; manifest/build identities keep
    the complete version and commit.
    """
    budget = update_manifest.NAME_LENGTH - len(f"eaw-revised--{platform}-player.zip")
    if len(version) > budget:
        digest = hashlib.sha256(version.encode("utf-8")).hexdigest()[:12]
        version = version[:budget - len(digest) - 1].rstrip(".-") + "-" + digest
    return f"eaw-revised-{version}-{platform}"


def build_player(stem: str, spec: dict, out: Path, godot: Path, launcher: Optional[Path],
                 importer: Callable[[Path, Path], Optional[str]] = godot_import,
                 debug_build_dir: Optional[Path] = None, mods_service: Optional[Path] = None,
                 manager_artifact: Optional[Path] = None,
                 launcher_pin: Path = launcher_artifact.DEFAULT_PIN, with_mods_manager: bool = False,
                 shader_runtime: Optional[Path] = None, revised_layer: Optional[Path] = None,
                 allow_missing_revised_layer: bool = False, public: bool = False,
                 readme_note: str = "") -> Path:
    """The portable Windows player folder as a zip: launcher slot, pinned Godot GUI runtime, pre-imported project.

    With debug_build_dir (the Debug configuration of the root build) the same folder is written as the debug
    variant: the viewer project's current extension must be the Debug one and carry its PDB, and the headless tools
    of that build ride along in tools/ with their PDBs (docs/debug-build.md ships as DEBUG-BUILD.md).
    """
    if (VIEWER_PROJECT / "cross-build-rig-only.json").exists():
        raise ValueError("cross-built viewer artifacts are for rig runs only")
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
            shutil.copyfile(launcher_artifact.require_start_stub(launcher.parent), folder / "EAWR Mods.exe")
            launcher_artifact.install_manager(folder, mods_service, manager_artifact, launcher_pin)
        preimport_project(folder / PLAYER_GODOT, project, importer)
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            shutil.copyfile(ROOT / name, folder / name)
        shutil.copyfile(PLAYER_GUIDE, folder / "README.md")
        if readme_note:
            with (folder / "README.md").open("a", encoding="utf-8", newline="\n") as readme:
                readme.write(f"\n{readme_note}\n")
        included_layer = stage_revised_layer(folder, revised_layer, allow_missing_revised_layer, public)
        if included_layer:
            with (folder / "README.md").open("a", encoding="utf-8", newline="\n") as readme:
                readme.write(f"\n{REVISED_LAYER_README}\n")
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
        if shader_runtime is not None:
            # Verify the exact third-party file set. These DLLs have upstream
            # identities, and never exempt project binaries from verify_identity.
            shader_runtime_package.stage(shader_runtime, folder / "shader-runtime")
            shutil.copyfile(folder / "shader-runtime" / "SOURCE-OFFER.md", folder / "SHADER_RUNTIME_SOURCE-OFFER.md")
        runtime = {
            "schema": "eawr-player-runtime/1",
            "variant": variant,
            "godot_identity": identity,
            "godot_sha256": sha256_file(godot),
            "launcher_sha256": launcher_sha,
            "extension_sha256": sha256_file(extension),
        }
        if shader_runtime is not None:
            runtime["shader_runtime_manifest_sha256"] = sha256_file(folder / "shader-runtime" / "manifest.json")
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
            expect_identity: Optional[str] = None, shader_runtime: Optional[Path] = None,
            revised_layer: Optional[Path] = None, allow_missing_revised_layer: bool = False,
            public: bool = False, readme_note: str = "") -> list[Path]:
    if (VIEWER_PROJECT / "cross-build-rig-only.json").exists():
        raise ValueError("cross-built viewer artifacts are for rig runs only")
    spec = PLATFORMS[platform]
    if shader_runtime is not None:
        if platform != "windows-x64":
            raise ValueError("shader runtime package is qualified for Windows x64 only")
        shader_runtime_package.verify(shader_runtime)
    if with_mods_manager and godot is None:
        raise ValueError("manager packaging needs the player folder (--godot)")
    if godot is not None and platform != "windows-x64":
        raise ValueError("the player folder is Windows x64 only for now")
    verify_identity(expect_identity, identity_binaries(spec, build_dir, "Release", launcher, mods_service))
    verify_kit_label(read_kit_label(), identity_binaries(spec, build_dir, "Release", launcher, mods_service))
    out.mkdir(parents=True, exist_ok=True)
    stem = archive_stem(version, platform)

    tools_zip = out / f"{stem}-tools.zip"
    with zipfile.ZipFile(tools_zip, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in TOOLS:
            binary = find_tool(build_dir, name, spec["exe"])
            info = zipfile.ZipInfo.from_file(binary, f"{stem}-tools/bin/{binary.name}")
            info.external_attr = 0o755 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, binary.read_bytes())
        add_docs(archive, f"{stem}-tools", "readme-tools.md")

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
        add_docs(archive, f"{stem}-viewer", "readme-viewer.md")
        if shader_runtime is not None:
            with tempfile.TemporaryDirectory(prefix="shader-runtime-", dir=out) as temporary:
                staged = Path(temporary) / "shader-runtime"
                shader_runtime_package.stage(shader_runtime, staged)
                for path in sorted(staged.rglob("*")):
                    if path.is_file():
                        archive.write(path, f"{stem}-viewer/shader-runtime/{path.relative_to(staged).as_posix()}")
                archive.write(staged / "SOURCE-OFFER.md", f"{stem}-viewer/SHADER_RUNTIME_SOURCE-OFFER.md")
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
                                  with_mods_manager=with_mods_manager, shader_runtime=shader_runtime, revised_layer=revised_layer,
                                  allow_missing_revised_layer=allow_missing_revised_layer, public=public,
                                  readme_note=readme_note))
    return files


def package_debug(build_dir: Path, version: str, out: Path, godot: Path, launcher: Optional[Path],
                  importer: Callable[[Path, Path], Optional[str]] = godot_import,
                  mods_service: Optional[Path] = None, manager_artifact: Optional[Path] = None,
                  launcher_pin: Path = launcher_artifact.DEFAULT_PIN, with_mods_manager: bool = False,
                  expect_identity: Optional[str] = None, shader_runtime: Optional[Path] = None,
                  revised_layer: Optional[Path] = None, allow_missing_revised_layer: bool = False,
                  public: bool = False, readme_note: str = "") -> Path:
    """The Windows x64 debug variant (eaw-revised-<version>-windows-x64-debug.zip).

    build_dir is the root build's Debug configuration; the viewer project's bin/ must hold the Debug extension
    and its PDB when this runs (build the Release archives first: both builds write to the same bin/).
    """
    spec = PLATFORMS["windows-x64"]
    verify_identity(expect_identity, identity_binaries(spec, build_dir, "Debug", launcher, mods_service))
    verify_kit_label(read_kit_label(), identity_binaries(spec, build_dir, "Debug", launcher, mods_service))
    out.mkdir(parents=True, exist_ok=True)
    return build_player(archive_stem(version, "windows-x64"), spec, out, godot, launcher, importer, build_dir, mods_service, manager_artifact, launcher_pin, with_mods_manager, shader_runtime, revised_layer, allow_missing_revised_layer, public, readme_note)


def write_sums(files: list[Path], out: Path) -> Path:
    sums = out / "SHA256SUMS.txt"
    lines = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}" for path in sorted(files)]
    sums.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return sums


def retain_release_symbols(build_dir, version, store, debug=False):
    """Private archive is deliberately outside the public assets and sums list."""
    identity = symbol_store.build_identity(Path(build_dir) / "src/core/generated/build_identity.cpp")
    binaries = retained_binaries(build_dir, debug)
    return symbol_store.archive(store, identity, binaries,
                                "release-" + version + ("-Debug" if debug else "-Release"), release=True)


NO_SYMBOLS_NOTE = "symbols: none (test kit)"


def retained_binaries(build_dir, debug=False) -> list[Path]:
    """The PE images whose private symbols a release retains (the viewer extension and the headless tools)."""
    binaries = [VIEWER_PROJECT / "bin" / PLATFORMS["windows-x64"]["extension"]]
    binaries += [find_tool(build_dir, name, ".exe", "Debug" if debug else "Release") for name in TOOLS]
    return binaries


def check_symbol_identities(build_dir, debug=False) -> None:
    """Fail at the start, naming every retained PE without a PDB identity, a matching PDB beside it, or missing, not only the first."""
    problems = []
    names = [VIEWER_PROJECT / "bin" / PLATFORMS["windows-x64"]["extension"]]
    for name in TOOLS:
        try:
            names.append(find_tool(build_dir, name, ".exe", "Debug" if debug else "Release"))
        except FileNotFoundError as error:
            problems.append(f"{name}.exe: {error}")
    for binary in names:
        try:
            pdb_name, key, _ = symbol_store.pe_identity(binary.read_bytes())
        except FileNotFoundError:
            problems.append(f"{binary}: missing")
            continue
        except ValueError as error:
            problems.append(f"{binary}: {error}")
            continue
        symbols = binary.parent / pdb_name
        try:
            if symbol_store.inspect(symbols, symbol_store.pdb_identity) != key:
                problems.append(f"{binary}: {pdb_name} does not match the image")
        except FileNotFoundError:
            problems.append(f"{binary}: {pdb_name} is missing beside it")
        except (OSError, ValueError) as error:
            problems.append(f"{binary}: {pdb_name}: {error}")
    if problems:
        raise ValueError("these binaries cannot be retained with private symbols:\n  " + "\n  ".join(problems)
                         + "\nrebuild with --require-symbols (and --viewer), or pass --no-symbols for an owner test kit")


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
    parser.add_argument("--shader-runtime", type=Path, help="offline verified optional shader DLL package; activation stays off")
    parser.add_argument("--revised-layer", type=Path, default=Path(os.environ["EAWR_REVISED_LAYER_DIR"]) if os.environ.get("EAWR_REVISED_LAYER_DIR") else None)
    parser.add_argument("--allow-missing-revised-layer", action="store_true", help="warn instead of failing for preview packages")
    parser.add_argument("--public", action="store_true", help="require the reviewed Revised layer and release symbols")
    parser.add_argument("--no-symbols", action="store_true",
                        help="owner TEST kit only: skip private symbol retention and note 'symbols: none (test kit)' in "
                             "the README and update manifest; refused with --public")
    parser.add_argument("--channel", choices=update_manifest.CHANNELS,
                        help="update channel written to update-manifest.json (default: preview for a -suffix version, else stable)")
    args = parser.parse_args(argv)
    sums = None
    note = NO_SYMBOLS_NOTE if args.no_symbols else ""
    try:
        with contextlib.ExitStack() as resources:
            if args.sums_only:
                files = sorted(args.out.glob("*.zip"))
                if not files:
                    raise FileNotFoundError(f"no zips in {args.out}")
            else:
                if not (args.build_dir and args.platform and args.version):
                    parser.error("--build-dir, --platform and --version are required")
                if args.no_symbols and args.public:
                    parser.error("--no-symbols is for owner test kits and cannot be combined with --public")
                if args.platform == "windows-x64" and not args.no_symbols:
                    check_symbol_identities(args.build_dir, args.debug)
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
                    if args.no_symbols:
                        print(f"package_release: {NO_SYMBOLS_NOTE}: private symbols are not retained", file=sys.stderr)
                    elif args.symbol_store is None:
                        parser.error("windows-x64 needs --symbol-store or the EAWR_SYMBOL_STORE environment variable")
                    if not args.no_symbols:
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
                                           expect_identity=args.expect_identity, shader_runtime=args.shader_runtime, revised_layer=args.revised_layer,
                                           allow_missing_revised_layer=args.allow_missing_revised_layer, public=args.public,
                                           readme_note=note)]
                else:
                    files = package(args.build_dir, args.platform, args.version, args.out, args.godot, args.launcher,
                                    mods_service=args.mods_service, manager_artifact=args.launcher_artifact,
                                    launcher_pin=args.launcher_pin, with_mods_manager=args.with_mods_manager,
                                    expect_identity=args.expect_identity, shader_runtime=args.shader_runtime, revised_layer=args.revised_layer,
                                    allow_missing_revised_layer=args.allow_missing_revised_layer, public=args.public,
                                    readme_note=note)
            if not args.debug:
                if not args.sums_only:
                    player = next((f for f in files if f.name.endswith("-windows-x64-player.zip")), None)
                    if player is not None:
                        # The updater's manifest (docs/releasing.md): per-file and archive hashes of the zip just written.
                        print(update_manifest.write(player, args.out, args.version, args.expect_identity, args.channel, note))
                manifest_file = args.out / update_manifest.MANIFEST_NAME
                sums = write_sums(files + ([manifest_file] if manifest_file.is_file() else []), args.out)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"package_release: {error}", file=sys.stderr)
        return 2
    for path in files + ([sums] if sums else []):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
