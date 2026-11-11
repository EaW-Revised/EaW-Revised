#!/usr/bin/env python3
"""The one host-side step of a kit cut, run by the Windows pool and by the Linux cross host alike.

It runs from the kit commit's own tree on the host, after the plan's bootstrap check (the streamed script compares the git
blob id of this file with the plan's payload entry before it runs it). Standard library plus kit_engine only.

  verify   check the tree is the kit commit with clean tracked files, check every staged input against the plan, unpack the
           Revised layer, and stage the pinned engine
  package  run the packager (package_release.py) with one fixed command line; both routes call this code
  receipt  after packaging: check the tree is still clean, hash the outputs, record the toolchain, and write the host receipt

Nothing here copies a script over the tree: a script that is not the kit commit's byte for byte shows up as a dirty file.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "common"))
import hidden_console  # noqa: E402
hidden_console.install()
sys.path.insert(0, str(HERE))
import kit_engine  # noqa: E402

HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")
PLAN_NAME = "plan.json"
SHA_NAME = "sha256.json"
RECEIPT_SCHEMA = "eawr-host-receipt/1"
RECEIPT_LINE = "EAWR-HOST-RECEIPT "
LAYER_NAME = "revised.zip"
FIXED_INPUTS = ("launcher.zip", PLAN_NAME)
DEBUG_INPUTS = ("release-launcher.exe", "release-mods-start.exe", "release-mods-service.exe", "release-loadcheck.exe")


class HostPackageError(ValueError):
    pass


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def plan_digest(plan: dict) -> str:
    return hashlib.sha256(canonical(plan)).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git(tree: Path, *args: str) -> str:
    # core.autocrlf=true makes git compare the clean-filtered bytes: the same blob whether the checkout holds LF or CRLF.
    # core.fileMode=false: the exec bit is not content, and a Linux host may chmod what a Windows checkout cannot.
    result = subprocess.run(["git", "-C", str(tree), "-c", "core.autocrlf=true", "-c", "core.fileMode=false", *args],
                            capture_output=True, text=True, timeout=300)
    if result.returncode:
        raise HostPackageError(f"git {' '.join(args)} failed: {result.stderr.strip() or result.stdout.strip()}")
    return result.stdout


def check_tree(tree: Path, kit_sha: str) -> dict:
    """The tree is the kit commit, and no tracked file differs from its blob. Untracked build output is not looked at."""
    if not HEX40.fullmatch(kit_sha):
        raise HostPackageError("kit sha must be a full lowercase hash")
    head = git(tree, "rev-parse", "HEAD").strip()
    if head != kit_sha:
        raise HostPackageError(f"tree {tree} is at {head}, the plan names {kit_sha}")
    # diff against HEAD compares content through git's own clean filter, so a CRLF checkout of the same blob is not a
    # difference, while `status` would list every file whose size changed. Untracked build output is not looked at.
    dirty = [line for line in git(tree, "diff", "--name-only", "--no-renames", "HEAD").splitlines() if line.strip()]
    if dirty:
        raise HostPackageError("tracked files differ from the kit commit: " + "; ".join(dirty[:8]) +
                               (f" (+{len(dirty) - 8} more)" if len(dirty) > 8 else ""))
    return {"head": head, "tracked_clean": True}


def read_plan(inputs: Path, expected_plan_sha256: str) -> dict:
    if not HEX64.fullmatch(expected_plan_sha256):
        raise HostPackageError("plan sha256 must be 64 lowercase hex digits")
    try:
        plan = json.loads((inputs / PLAN_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise HostPackageError(f"cannot read the staged plan: {error}") from error
    if not isinstance(plan, dict) or plan_digest(plan) != expected_plan_sha256:
        raise HostPackageError("the staged plan does not hash to the plan sha256 of this cut")
    return plan


def engine_names(plan: dict) -> list[str]:
    files = plan.get("inputs", {}).get("engine", {}).get("files", {})
    if not isinstance(files, dict) or not files:
        raise HostPackageError("the plan pins no engine files")
    return sorted(files)


def expected_names(inputs: Path, plan: dict, debug: bool) -> set[str]:
    names = set(FIXED_INPUTS) | set(engine_names(plan))
    if (inputs / LAYER_NAME).exists():
        names.add(LAYER_NAME)
    if debug:
        names.update(DEBUG_INPUTS)
    return names


def check_inputs(inputs: Path, plan: dict, *, debug: bool = False) -> dict:
    """Every staged file hashes to its transfer digest, and the engine and launcher also equal what the plan pinned."""
    try:
        sums = json.loads((inputs / SHA_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise HostPackageError(f"cannot read the input digests: {error}") from error
    names = expected_names(inputs, plan, debug)
    if not isinstance(sums, dict) or set(sums) != names:
        raise HostPackageError(f"invalid input manifest: expected {sorted(names)}, found {sorted(sums) if isinstance(sums, dict) else sums!r}")
    seen = {}
    for name in sorted(names):
        path = inputs / name
        if not path.is_file():
            raise HostPackageError(f"package input missing: {name}")
        actual = sha256_file(path)
        if actual != sums[name]:
            raise HostPackageError(f"package input hash mismatch: {name}")
        seen[name] = actual
    pinned = plan.get("inputs", {})
    engine = pinned.get("engine", {}).get("files", {})
    for name in engine_names(plan):
        if engine.get(name) != seen[name]:
            raise HostPackageError(f"{name} is not the engine the plan pinned")
    if pinned.get("launcher", {}).get("sha256") != seen["launcher.zip"]:
        raise HostPackageError("launcher.zip is not the artifact the plan pinned")
    return seen


def unsafe_entry(entry: zipfile.ZipInfo) -> bool:
    name = entry.filename
    parts = Path(name.replace("\\", "/")).parts
    return (name.startswith("/") or "\\" in name or ":" in name or ".." in parts or
            (entry.external_attr >> 16) & 0o170000 == 0o120000)


def extract_layer(inputs: Path) -> Path | None:
    archive = inputs / LAYER_NAME
    if not archive.is_file():
        return None
    root = inputs / "revised-layer"
    if root.exists():
        shutil.rmtree(root)
    with zipfile.ZipFile(archive) as z:
        if any(unsafe_entry(entry) for entry in z.infolist()):
            raise HostPackageError("unsafe Revised layer archive entry")
        z.extractall(root)
    return root


def cmake_value(text: str, key: str) -> str | None:
    match = re.search(r'^set\(' + re.escape(key) + r' "([^"]*)"\)', text, re.M)
    return match.group(1) if match and match.group(1) and not match.group(1).endswith("-NOTFOUND") else None


def compiler_record(build_dir: Path | None) -> tuple[str | None, str | None]:
    """The compiler and linker CMake detected for this build tree (CMakeFiles/<version>/CMakeCXXCompiler.cmake and the
    cache): id, version and path as CMake recorded them, not whatever happens to be first on PATH."""
    if build_dir is None:
        return None, None
    build_dir = Path(build_dir)
    files = sorted((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"), key=lambda f: f.stat().st_mtime)
    if not files:
        return None, None
    try:
        text = files[-1].read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None, None
    path = cmake_value(text, "CMAKE_CXX_COMPILER")
    compiler = " ".join(x for x in (cmake_value(text, "CMAKE_CXX_COMPILER_ID"), cmake_value(text, "CMAKE_CXX_COMPILER_VERSION")) if x)
    compiler = f"{compiler} ({path})" if compiler and path else (path or compiler or None)
    linker = cmake_value(text, "CMAKE_LINKER")
    if linker is None:
        try:
            for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("CMAKE_LINKER:"):
                    value = line.partition("=")[2].strip()
                    linker = value if value and not value.endswith("-NOTFOUND") else None
        except OSError:
            pass
    return compiler, linker


def toolchain(build_dir: Path | None = None) -> dict:
    """What built and packaged this kit. Best effort: a tool that is not there is recorded as null, never guessed. The
    compiler and linker are the ones CMake recorded for the build tree; PATH is only the fallback."""
    def first_line(command, timeout=60):
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        except (OSError, subprocess.SubprocessError):
            return None
        text = (result.stdout or result.stderr or "").strip()
        return text.splitlines()[0].strip() if result.returncode == 0 and text else None

    compiler, linker = compiler_record(build_dir)
    wine = os.environ.get("EAWR_WINE")
    shell = shutil.which("pwsh") or shutil.which("powershell")
    return {
        "python": platform.python_version(), "os": platform.platform(), "machine": platform.machine(),
        "clang": compiler or first_line([shutil.which("clang") or shutil.which("clang-cl") or "clang", "--version"]),
        "lld": (first_line([linker, "--version"]) or linker) if linker else first_line([shutil.which("lld-link") or shutil.which("ld.lld") or "lld-link", "--version"]),
        "wine": first_line([wine, "--version"], 180) if wine else None,
        "powershell": first_line([shell, "-NoProfile", "-Command", "$PSVersionTable.PSVersion.ToString()"]) if shell else None,
        "git": first_line(["git", "--version"]),
        "windows_sdk": os.environ.get("WindowsSDKVersion") or os.environ.get("WINDOWSSDKVERSION"),
    }


def packager_command(packager, *, expect_identity: str, build_dir, version: str, out, symbol_store, godot, launcher,
                     mods_service, launcher_artifact, revised_layer=None, allow_missing_revised_layer=False,
                     public=False, no_symbols=False, debug=False, python=None) -> list[str]:
    """The one command line of the player packager. The routes differ in where they find the built binaries, how they stage
    the tree and which runner runs the engine; none of them builds this line itself."""
    if no_symbols and public:
        raise HostPackageError("--no-symbols is for owner test kits and cannot be combined with --public")
    command = [python or sys.executable, str(packager), "--expect-identity", expect_identity, "--build-dir", str(build_dir),
               "--platform", "windows-x64", "--version", version, "--out", str(out), "--symbol-store", str(symbol_store),
               "--godot", str(godot), "--launcher", str(launcher), "--with-mods-manager", "--mods-service", str(mods_service),
               "--launcher-artifact", str(launcher_artifact)]
    if revised_layer:
        command += ["--revised-layer", str(revised_layer)]
    for flag, enabled in (("--allow-missing-revised-layer", allow_missing_revised_layer), ("--public", public),
                          ("--no-symbols", no_symbols), ("--debug", debug)):
        if enabled:
            command.append(flag)
    return command


def release_file_lines(out) -> list[str]:
    """`EAWR-RELEASE FILE <path> <bytes> <sha256>` for every file the packager wrote: what the cut parses on either route."""
    return [f"EAWR-RELEASE FILE {f} {f.stat().st_size} {sha256_file(f)}" for f in sorted(Path(out).iterdir()) if f.is_file()]


def run_packager(command: list[str], out) -> int:
    """Run the packager, then print the file lines. Its own output passes through unchanged."""
    code = subprocess.run(command).returncode
    if code == 0:
        for line in release_file_lines(out):
            print(line, flush=True)
    return code


def package(args) -> int:
    packager = Path(args.packager) if args.packager else Path(args.tree) / "tools" / "release" / "package_release.py"
    command = packager_command(
        packager, expect_identity=args.kit_sha, build_dir=args.build_dir, version=args.version, out=args.out,
        symbol_store=args.symbol_store, godot=args.godot, launcher=args.launcher, mods_service=args.mods_service,
        launcher_artifact=args.launcher_artifact, revised_layer=args.revised_layer,
        allow_missing_revised_layer=args.allow_missing_revised_layer, public=args.public, no_symbols=args.no_symbols,
        debug=args.debug)
    print("python " + " ".join(command[1:]), flush=True)
    return run_packager(command, args.out)


def verify(args) -> int:
    tree = Path(args.tree)
    inputs = Path(args.inputs)
    tree_state = check_tree(tree, args.kit_sha)
    plan = read_plan(inputs, args.plan_sha256)
    if plan.get("source", {}).get("kit_sha") != args.kit_sha:
        raise HostPackageError("the plan was made for a different kit commit")
    seen = check_inputs(inputs, plan, debug=args.debug)
    extract_layer(inputs)
    if args.engine_kit:
        engine_kit = Path(args.engine_kit)
        if engine_kit.exists():
            shutil.rmtree(engine_kit)
        engine_kit.mkdir(parents=True)
        kit_engine.stage_engine(engine_kit, inputs)
    print(json.dumps({"tree": tree_state, "inputs": seen}, sort_keys=True))
    return 0


def outputs_of(out: Path) -> list[dict]:
    return [{"name": f.name, "size": f.stat().st_size, "sha256": sha256_file(f)} for f in sorted(Path(out).iterdir()) if f.is_file()]


def receipt(args) -> int:
    tree = Path(args.tree)
    inputs = Path(args.inputs) if args.inputs else None
    after = check_tree(tree, args.kit_sha)  # packaging must not have changed a tracked file
    seen = {}
    if inputs is not None and (inputs / SHA_NAME).is_file():
        # Hash the staged files again: the receipt records what is there now, and refuses if anything changed since `verify`.
        listed = json.loads((inputs / SHA_NAME).read_text(encoding="utf-8"))
        for name, want in sorted(listed.items()):
            path = inputs / name
            if not path.is_file() or sha256_file(path) != want:
                raise HostPackageError(f"staged input changed after it was verified: {name}")
            seen[name] = want
    document = {"schema": RECEIPT_SCHEMA, "plan_sha256": args.plan_sha256, "kit_sha": args.kit_sha,
                "host": args.host, "public": bool(args.public), "tree": after, "inputs": seen,
                "outputs": outputs_of(Path(args.out)),
                "toolchain": toolchain(Path(args.build_dir) if args.build_dir else None)}
    text = json.dumps(document, indent=2, sort_keys=True) + "\n"
    if args.receipt:
        Path(args.receipt).write_text(text, encoding="utf-8", newline="\n")
    print(RECEIPT_LINE + json.dumps(document, sort_keys=True, separators=(",", ":")))
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    pack = sub.add_parser("package")
    pack.add_argument("--tree", required=True)
    pack.add_argument("--packager", help="package_release.py to run (default: the one in --tree)")
    pack.add_argument("--kit-sha", required=True)
    for name in ("build-dir", "version", "out", "symbol-store", "godot", "launcher", "mods-service", "launcher-artifact"):
        pack.add_argument("--" + name, required=True)
    pack.add_argument("--revised-layer")
    for flag in ("allow-missing-revised-layer", "public", "no-symbols", "debug"):
        pack.add_argument("--" + flag, action="store_true")
    for name in ("verify", "receipt"):
        p = sub.add_parser(name)
        p.add_argument("--tree", required=True)
        p.add_argument("--kit-sha", required=True)
        p.add_argument("--plan-sha256", required=True)
        p.add_argument("--inputs", required=name == "verify")
        if name == "verify":
            p.add_argument("--engine-kit", help="stage the pinned engine into this folder")
            p.add_argument("--debug", action="store_true")
        else:
            p.add_argument("--out", required=True)
            p.add_argument("--host", default="")
            p.add_argument("--receipt")
            p.add_argument("--public", action="store_true", help="the package was made with the packager's --public")
            p.add_argument("--build-dir", help="the build tree; its CMake cache names the compiler and linker to record")
    args = parser.parse_args(argv)
    try:
        return {"verify": verify, "receipt": receipt, "package": package}[args.command](args)
    except (HostPackageError, kit_engine.KitEngineError, OSError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print(f"host-package: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
