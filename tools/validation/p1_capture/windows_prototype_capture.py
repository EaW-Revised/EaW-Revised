#!/usr/bin/env python3
"""Observed Windows prototype baseline repeats for P1-01 (issue #22).

Windows only.  Two subcommands:

``dry-run``
    Validates the audited prior evidence, the source checkout, every pinned
    hash and the rewritten argument arrays, probes the host/adapters and
    self-checks the process observer against this Python process.  It writes
    nothing and launches nothing.

``run --allow-live-launch``
    Creates a new unique git-ignored evidence directory, stages a copy of the
    audited prototype project (without ``.godot``/``out``), and runs two
    sequential repeats with distinct outputs.  Each launch reuses the audited
    argument array with only the ``--path``, ``--eawr-capture`` and
    ``--eawr-report`` values rewritten, uses an argument-list API with a hidden
    window, and observes the real engine process tree and its loaded extension
    module (path and on-disk hash) before and after the capture PNG first
    appears.  Afterwards it re-hashes every fixed input and writes
    ``artifact-manifest.json``.

Provenance is claimed only when every corroboration is present; anything
missing or inconsistent fails closed (exit 1) and the manifest records why.
The tool never writes into the prior evidence, the installed game or mod roots
or tracked files, never compares pixels, and never approves a contract:
``visual_comparison`` and ``acceptance`` are always false.  Standard output is
one JSON receipt with private absolute paths replaced by placeholders.

Exit status: 0 corroborated; 1 evidence missing, mismatched or uncorroborated;
2 command-line, platform or tool error.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import re
import secrets
import shutil
import stat
import subprocess
import sys
from typing import Any, Callable, Mapping, NoReturn

_TOOL_DIRECTORY = pathlib.Path(__file__).resolve().parent
if str(_TOOL_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(_TOOL_DIRECTORY))

import build_manifest  # noqa: E402  (strict JSON and prototype report schema)
import win_process_observer as observer  # noqa: E402

TOOL = "windows_prototype_capture"
KIND = "p1-01-windows-prototype-observed-repeat"
REPO_ROOT = _TOOL_DIRECTORY.parents[2]

# Hashes of the audited retained receipts, from
# docs/rendering.md#prototype-capture-timing (p1-completion).
AUDITED_STAGING_RECEIPT_SHA256 = "39ca1c77e531b052c6e5c7af602b5306b90bc00499012243a9ace162e0f38712"
AUDITED_READINESS_SHA256 = "abc6b0ec47c6f934031938667443c72fa61156b735222a1182116e894341278b"
OLD_RECEIPT = "staging-receipt.json"
OLD_READINESS = "windows-p0-repeat-readiness.json"
OLD_ARGV = ("p0-1/argv.json", "p0-2/argv.json")

RUN_NAMES = ("p0-1", "p0-2")
EXCLUDED_TOP_LEVEL = frozenset({".godot", "out"})
PROTOTYPE_SOURCE_PATHS = ("prototypes", "tests/replay/fixtures")
EVIDENCE_PREFIX = "windows-p0-observed-"
GDEXTENSION_FILE = "eawr_godot.gdextension"
GDEXTENSION_WINDOWS_KEYS = ("windows.debug.x86_64", "windows.release.x86_64")
VIEWPORT = (1280, 720)

ENGINE_OPTIONS = frozenset({"--rendering-method", "--path"})
USER_VALUE_OPTIONS = frozenset({
    "--eawr-game-root", "--eawr-mod-root", "--eawr-scene", "--eawr-replay", "--eawr-capture", "--eawr-report",
})
USER_FLAGS = frozenset({"--eawr-benchmark"})
REQUIRED_OPTIONS = ENGINE_OPTIONS | USER_VALUE_OPTIONS | USER_FLAGS
REWRITTEN_OPTIONS = ("--path", "--eawr-capture", "--eawr-report")
RENDERING_METHOD = "gl_compatibility"
DEVICE_LINE = re.compile(r"Using Device: (?P<vendor>.+?) - (?P<name>.+?)\s*$", re.MULTILINE)
HEX40 = re.compile(r"[0-9a-f]{40}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")

LIMITATIONS = [
    "Module observation is polling: it shows the extension mapped in the engine at sampled instants that bracket the "
    "first PNG sighting, not continuously.",
    "The loaded-extension hash is of the on-disk file at the mapped path while the engine was alive; Windows refuses "
    "writes to a mapped image, but the in-memory image itself is not read.  The loaded module's base address and "
    "SizeOfImage come from the loader list (GetModuleInformation) and are only checked against the file's PE "
    "header and across the key snapshots.",
    "On timeout or observer failure only process instances seen in a snapshot, or current descendants of them, can "
    "be terminated; a descendant created and orphaned entirely between two polls is not visible to the observer.",
    "The hidden-window request is recorded; whether the engine honoured it is recorded as visible_top_level_windows.",
    "Game and mod roots are only passed through to the engine; this tool neither hashes nor writes them.",
    "No pixels are compared or judged; the PNG is only hashed and decoded for its size.",
    "The prototype DLL is untracked; its provenance is the audited hash pinned from the prior staging receipt, not "
    "the current helper revision.",
]


class EvidenceError(ValueError):
    """Evidence missing, mismatched or uncorroborated (exit 1)."""


class UsageError(ValueError):
    """Command-line, platform or tool failure (exit 2)."""


class _Parser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        raise UsageError(f"command line: {message}")


# ---------------------------------------------------------------- utilities

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: pathlib.Path | str) -> str:
    return observer.sha256_file(str(path))


def try_sha256(path: pathlib.Path | str) -> tuple[str | None, str | None]:
    """``(digest, None)``, or ``(None, reason)`` naming no path when the file cannot be hashed."""

    try:
        if not os.path.isfile(path):
            return None, "missing"
        return sha256_file(path), None
    except OSError as error:
        return None, f"{type(error).__name__}: {error.strerror or 'unreadable'}"


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def serialise(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def exclusive_write(path: pathlib.Path, data: bytes) -> str:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
    descriptor = os.open(path, flags, 0o644)
    with os.fdopen(descriptor, "wb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    return sha256_bytes(data)


def read_json(path: pathlib.Path, context: str) -> Any:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise EvidenceError(f"{context}: cannot read ({error.strerror})") from error
    try:
        return build_manifest.load_strict_json(raw, context)
    except build_manifest.EvidenceError as error:
        raise EvidenceError(str(error)) from error


def is_link_or_reparse(path: pathlib.Path) -> bool:
    try:
        info = os.lstat(path)
    except OSError:
        return False
    attributes = getattr(info, "st_file_attributes", 0)
    return stat.S_ISLNK(info.st_mode) or bool(attributes & build_manifest.FILE_ATTRIBUTE_REPARSE_POINT)


def is_within(child: str | pathlib.Path, parent: str | pathlib.Path) -> bool:
    # realpath expands 8.3 short names and links so aliases cannot escape.
    child_text = observer.norm_path(os.path.realpath(str(child)))
    parent_text = observer.norm_path(os.path.realpath(str(parent))).rstrip("\\/")
    return child_text == parent_text or child_text.startswith(parent_text + os.sep)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def text_field(container: Mapping[str, Any], key: str, context: str) -> str:
    value = container.get(key) if isinstance(container, Mapping) else None
    require(isinstance(value, str) and value != "", f"{context}.{key}: expected a non-empty string")
    return value


def hash_field(container: Mapping[str, Any], key: str, context: str) -> str:
    value = text_field(container, key, context)
    require(bool(HEX64.match(value)), f"{context}.{key}: expected a lowercase SHA-256")
    return value


# --------------------------------------------------------------- argv policy

def parse_argv(argv: Any, context: str) -> dict[str, int]:
    """Validate the audited argument array and map each option to its index.

    Before ``--`` only ``--rendering-method gl_compatibility`` and ``--path``
    are allowed; after it only the prototype's baseline capture options.
    Probes, close-up capture, headless and editor flags are refused.
    """

    require(isinstance(argv, list) and len(argv) > 1, f"{context}: expected a non-empty argument array")
    require(all(isinstance(item, str) and item != "" for item in argv), f"{context}: every argument must be a "
            "non-empty string")
    require(argv.count("--") == 1, f"{context}: expected exactly one '--' separator")
    separator = argv.index("--")
    indices: dict[str, int] = {}
    index = 1
    while index < separator:
        option = argv[index]
        require(option in ENGINE_OPTIONS, f"{context}: engine argument {option!r} is not allowed")
        require(option not in indices, f"{context}: duplicate {option}")
        require(index + 1 < separator, f"{context}: {option} has no value")
        indices[option] = index + 1
        index += 2
    index = separator + 1
    while index < len(argv):
        option = argv[index]
        require(option not in indices, f"{context}: duplicate {option}")
        if option in USER_FLAGS:
            indices[option] = index
            index += 1
        elif option in USER_VALUE_OPTIONS:
            require(index + 1 < len(argv) and not argv[index + 1].startswith("--"), f"{context}: {option} has no value")
            indices[option] = index + 1
            index += 2
        else:
            raise EvidenceError(f"{context}: user argument {option!r} is not allowed")
    missing = sorted(REQUIRED_OPTIONS - set(indices))
    require(not missing, f"{context}: missing {', '.join(missing)}")
    require(argv[indices["--rendering-method"]] == RENDERING_METHOD,
            f"{context}: --rendering-method must be {RENDERING_METHOD}")
    return indices


def rewrite_argv(old: list[str], project: str, capture: str, report: str) -> list[str]:
    indices = parse_argv(old, "audited argv")
    new = list(old)
    new[indices["--path"]] = project
    new[indices["--eawr-capture"]] = capture
    new[indices["--eawr-report"]] = report
    changed = {index for index, (left, right) in enumerate(zip(old, new)) if left != right}
    allowed = {indices[name] for name in REWRITTEN_OPTIONS}
    require(len(new) == len(old) and changed <= allowed, "argv rewrite touched an argument other than "
            "--path/--eawr-capture/--eawr-report")
    require(parse_argv(new, "rewritten argv") == indices, "rewritten argv changed its option layout")
    return new


# ----------------------------------------------------------------- git/source

def git(repo: pathlib.Path, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(repo), *arguments], capture_output=True, text=True, timeout=60,
                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def source_state(repo: pathlib.Path, prototype_revision: str) -> dict[str, Any]:
    head = git(repo, "rev-parse", "HEAD")
    require(head.returncode == 0, "source: git rev-parse HEAD failed")
    status = git(repo, "status", "--porcelain=v1", "--untracked-files=all")
    require(status.returncode == 0, "source: git status failed")
    require(status.stdout.strip() == "", "source: the checkout has uncommitted or untracked changes")
    ancestor = git(repo, "merge-base", "--is-ancestor", prototype_revision, "HEAD")
    require(ancestor.returncode == 0, "source: the audited prototype revision is not an ancestor of HEAD")
    diff = git(repo, "diff", "--quiet", prototype_revision, "HEAD", "--", *PROTOTYPE_SOURCE_PATHS)
    require(diff.returncode == 0, "source: prototype sources or replay fixtures changed since the audited revision")
    return {
        "helper_revision": head.stdout.strip(),
        "prototype_revision": prototype_revision,
        "checkout_clean": True,
        "prototype_paths_unchanged_since_prototype_revision": list(PROTOTYPE_SOURCE_PATHS),
    }


def git_ignored(repo: pathlib.Path, path: pathlib.Path) -> bool:
    probe = path / "p1-capture-ignore-probe"
    result = git(repo, "check-ignore", "-q", "--no-index", str(probe))
    return result.returncode == 0


# -------------------------------------------------------------- project files

def project_files(root: pathlib.Path) -> dict[str, str]:
    """Relative POSIX path -> SHA-256 of every file to stage, refusing links."""

    require(root.is_dir() and not is_link_or_reparse(root), "project: source project directory is missing or a link")
    files: dict[str, str] = {}
    for directory, subdirectories, names in os.walk(root):
        current = pathlib.Path(directory)
        relative_directory = current.relative_to(root)
        if relative_directory == pathlib.Path("."):
            subdirectories[:] = [name for name in subdirectories if name not in EXCLUDED_TOP_LEVEL]
        for name in list(subdirectories):
            require(not is_link_or_reparse(current / name), "project: linked directory in source project")
        subdirectories.sort()
        for name in sorted(names):
            path = current / name
            require(not is_link_or_reparse(path) and path.is_file(), "project: link or special file in source project")
            files[(relative_directory / name).as_posix()] = sha256_file(path)
    require(bool(files), "project: source project is empty")
    return files


def gdextension_windows_library(project: pathlib.Path) -> str:
    descriptor = project / GDEXTENSION_FILE
    try:
        text = descriptor.read_text(encoding="utf-8")
    except OSError as error:
        raise EvidenceError("project: cannot read the GDExtension descriptor") from error
    values: dict[str, str] = {}
    for line in text.splitlines():
        match = re.match(r'\s*([A-Za-z0-9_.]+)\s*=\s*"([^"]*)"\s*$', line)
        if match:
            values[match.group(1)] = match.group(2)
    libraries = {values.get(key) for key in GDEXTENSION_WINDOWS_KEYS}
    require(len(libraries) == 1 and None not in libraries, "project: Windows GDExtension entries are missing or differ")
    library = libraries.pop()
    require(library.startswith("res://bin/") and "/" not in library[len("res://bin/"):],
            "project: Windows GDExtension library must be res://bin/<file>")
    return library[len("res://bin/"):]


# -------------------------------------------------------------------- preflight

def load_prior_evidence(old: pathlib.Path, expect_receipt: str, expect_readiness: str) -> dict[str, Any]:
    receipt_path, readiness_path = old / OLD_RECEIPT, old / OLD_READINESS
    hashes: dict[str, str] = {}
    for relative in (OLD_RECEIPT, OLD_READINESS, *OLD_ARGV):
        path = old / relative
        require(path.is_file() and not is_link_or_reparse(path), f"prior evidence: {relative} is missing")
        hashes[relative] = sha256_file(path)
    require(hashes[OLD_RECEIPT] == expect_receipt, "prior evidence: staging receipt hash differs from the audited hash")
    require(hashes[OLD_READINESS] == expect_readiness, "prior evidence: readiness hash differs from the audited hash")
    receipt = read_json(receipt_path, "prior staging receipt")
    readiness = read_json(readiness_path, "prior readiness")
    argv_1 = read_json(old / OLD_ARGV[0], "prior p0-1 argv")
    argv_2 = read_json(old / OLD_ARGV[1], "prior p0-2 argv")
    indices = parse_argv(argv_1, "prior p0-1 argv")
    require(parse_argv(argv_2, "prior p0-2 argv") == indices, "prior p0-2 argv has a different layout")
    differing = {index for index, (left, right) in enumerate(zip(argv_1, argv_2)) if left != right}
    require(differing <= {indices["--eawr-capture"], indices["--eawr-report"]},
            "prior p0-1 and p0-2 argv differ outside --eawr-capture/--eawr-report")
    runs = readiness.get("runs") if isinstance(readiness, dict) else None
    require(isinstance(runs, list) and len(runs) == 2, "prior readiness: expected two runs")
    require(runs[0].get("argv") == argv_1 and runs[1].get("argv") == argv_2,
            "prior readiness argv differs from the retained argv.json files")
    return {"receipt": receipt, "readiness": readiness, "argv": argv_1, "indices": indices, "hashes": hashes}


def preflight(args: argparse.Namespace) -> dict[str, Any]:
    if not observer.IS_WINDOWS:
        raise UsageError("this capture helper runs on Windows only")
    repo = pathlib.Path(args.repo).resolve()
    old = pathlib.Path(args.prior_evidence).resolve()
    require(old.is_dir(), "prior evidence directory is missing")
    prior = load_prior_evidence(old, args.expect_staging_receipt_sha256, args.expect_readiness_sha256)
    receipt, readiness, argv, indices = prior["receipt"], prior["readiness"], prior["argv"], prior["indices"]

    pins = {
        "prototype_revision": text_field(receipt, "source_head", "prior staging receipt"),
        "extension_sha256": hash_field(receipt, "prototype_dll_sha256", "prior staging receipt"),
        "launcher_sha256": hash_field(receipt, "godot_console_sha256", "prior staging receipt"),
        "engine_version": text_field(receipt, "godot_version", "prior staging receipt"),
        "scene_sha256": hash_field(receipt, "scene_sha256", "prior staging receipt"),
        "replay_sha256": hash_field(receipt, "replay_sha256", "prior staging receipt"),
    }
    require(bool(HEX40.match(pins["prototype_revision"])), "prior staging receipt: source_head is not a revision")
    require(receipt.get("source_clean") is True, "prior staging receipt: source was not clean")
    runtime = readiness.get("runtime") if isinstance(readiness, dict) else None
    require(isinstance(runtime, dict), "prior readiness: runtime section missing")
    pins["engine_sha256"] = hash_field(runtime, "godot_engine_sibling_sha256", "prior readiness.runtime")
    engine_path = text_field(runtime, "godot_engine_sibling", "prior readiness.runtime")
    launcher_path = argv[0]
    require(launcher_path == receipt.get("godot_console") == runtime.get("godot_console"),
            "prior evidence: launcher path differs between argv, staging receipt and readiness")
    require(runtime.get("godot_console_sha256") == pins["launcher_sha256"],
            "prior evidence: launcher hash differs between staging receipt and readiness")
    require(os.path.dirname(observer.norm_path(engine_path)) == os.path.dirname(observer.norm_path(launcher_path)),
            "prior evidence: engine executable is not beside the launcher")
    source = readiness.get("source") if isinstance(readiness, dict) else None
    require(isinstance(source, dict) and source.get("head") == pins["prototype_revision"],
            "prior readiness: source head differs from the staging receipt")
    game_root, mod_root = argv[indices["--eawr-game-root"]], argv[indices["--eawr-mod-root"]]
    require(game_root == receipt.get("game_root") and mod_root == receipt.get("mod_root"),
            "prior evidence: argv roots differ from the staging receipt")

    require(observer.same_path(str(repo), text_field(receipt, "source_repo", "prior staging receipt")),
            "this checkout is not the audited source checkout")
    project = repo / "prototypes/godot/project"
    require(observer.same_path(str(project), text_field(receipt, "project_source", "prior staging receipt")),
            "prior staging receipt: project source is not this checkout's prototype project")
    require(set(receipt.get("excluded") or []) == set(EXCLUDED_TOP_LEVEL),
            "prior staging receipt: excluded directories differ")

    source_info = source_state(repo, pins["prototype_revision"])
    files = project_files(project)
    library = gdextension_windows_library(project)
    require(f"bin/{library}" in files, "project: the GDExtension library is not in the source project")
    require(files[f"bin/{library}"] == pins["extension_sha256"],
            "project: source prototype DLL hash differs from the audited hash")

    fixed_inputs = {
        "launcher": launcher_path,
        "engine": engine_path,
        "scene": argv[indices["--eawr-scene"]],
        "replay": argv[indices["--eawr-replay"]],
        "source_extension": str(project / "bin" / library),
    }
    expected = {
        "launcher": pins["launcher_sha256"],
        "engine": pins["engine_sha256"],
        "scene": pins["scene_sha256"],
        "replay": pins["replay_sha256"],
        "source_extension": pins["extension_sha256"],
    }
    for name in ("scene", "replay"):
        require(is_within(fixed_inputs[name], repo), f"{name}: argv path is outside the source checkout")
    pre_hashes = hash_fixed_inputs(fixed_inputs)
    for name, digest in expected.items():
        require(pre_hashes[name] == digest, f"{name}: pre-launch hash differs from the pinned hash")
    for name, root in (("game root", game_root), ("mod root", mod_root)):
        require(os.path.isdir(root), f"{name}: directory is missing")

    parent = pathlib.Path(args.evidence_parent).resolve() if args.evidence_parent else (
        repo / "out/renderer-acceptance")
    require(is_within(parent, repo) and parent != repo, "evidence parent must be inside the source checkout")
    for name, root in (("game root", game_root), ("mod root", mod_root), ("prior evidence", old),
                       ("engine directory", os.path.dirname(launcher_path))):
        require(not is_within(parent, root), f"evidence parent must not be inside the {name}")
    require(git_ignored(repo, parent), "evidence parent is not git-ignored")

    tool_hashes = {name: sha256_file(_TOOL_DIRECTORY / name) for name in (
        "windows_prototype_capture.py", "win_process_observer.py", "build_manifest.py")}
    return {
        "repo": repo,
        "prior_evidence": old,
        "prior_hashes": prior["hashes"],
        "old_argv": argv,
        "indices": indices,
        "pins": pins,
        "source": source_info,
        "project": project,
        "project_files": files,
        "extension_library": library,
        "fixed_inputs": fixed_inputs,
        "pre_hashes": pre_hashes,
        "game_root": game_root,
        "mod_root": mod_root,
        "evidence_parent": parent,
        "tool_hashes": tool_hashes,
    }


def hash_fixed_inputs(paths: Mapping[str, str]) -> dict[str, str]:
    hashes: dict[str, str] = {}
    for name, path in paths.items():
        require(os.path.isfile(path) and not is_link_or_reparse(pathlib.Path(path)), f"{name}: file is missing")
        hashes[name] = sha256_file(path)
    return hashes


# ------------------------------------------------------------------ host probe

HOST_POWERSHELL = r"""
$ErrorActionPreference = 'Stop'
$os = Get-CimInstance Win32_OperatingSystem
$identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$adapters = @(Get-CimInstance Win32_VideoController | Sort-Object PNPDeviceID | ForEach-Object {
  [ordered]@{
    Name = $_.Name; AdapterCompatibility = $_.AdapterCompatibility; DriverVersion = $_.DriverVersion
    DriverDate = if ($_.DriverDate) { $_.DriverDate.ToUniversalTime().ToString('o') } else { $null }
    PNPDeviceID = $_.PNPDeviceID; Status = $_.Status; VideoProcessor = $_.VideoProcessor
  } })
[ordered]@{
  os = [ordered]@{ Caption = $os.Caption; Version = $os.Version; BuildNumber = $os.BuildNumber
    OSArchitecture = $os.OSArchitecture
    LastBootUpTimeUtc = $os.LastBootUpTime.ToUniversalTime().ToString('o') }
  user = [ordered]@{ Name = $identity.Name; Sid = $identity.User.Value }
  powershell_session_id = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
  adapters = $adapters
} | ConvertTo-Json -Depth 5 -Compress
"""


def collect_host_receipt() -> dict[str, Any]:
    import winreg  # Windows only

    completed = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", HOST_POWERSHELL],
        capture_output=True, timeout=120, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    require(completed.returncode == 0, "host probe: PowerShell CIM query failed")
    cim = json.loads(completed.stdout.decode("utf-8", errors="replace"))
    registry: dict[str, Any] = {}
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion") as key:
        for name in ("ProductName", "DisplayVersion", "EditionID", "CurrentBuild", "UBR", "InstallationType"):
            try:
                registry[name] = winreg.QueryValueEx(key, name)[0]
            except OSError:
                registry[name] = None
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Cryptography") as key:
        machine_guid = winreg.QueryValueEx(key, "MachineGuid")[0]
    version = sys.getwindowsversion()
    probe = observer.WindowsProcessProbe()
    return {
        "collected_utc": utc_now(),
        "computer_name": os.environ.get("COMPUTERNAME"),
        "machine_guid_sha256": sha256_bytes(str(machine_guid).encode("utf-8")),
        "architecture": platform.machine(),
        "windows_version": {"major": version.major, "minor": version.minor, "build": version.build},
        "registry": registry,
        "os": cim.get("os"),
        "boot_time_from_tick_utc": observer.boot_time_utc_from_tick(),
        "user": cim.get("user"),
        "observer_session_id": probe._session(os.getpid()),
        "powershell_session_id": cim.get("powershell_session_id"),
        "adapters": cim.get("adapters") or [],
    }


HOST_STABLE_KEYS = ("computer_name", "machine_guid_sha256", "architecture", "windows_version", "registry", "os",
                    "user", "observer_session_id", "powershell_session_id", "adapters")


def check_host_receipt(host: Mapping[str, Any]) -> None:
    require(isinstance(host.get("user"), dict) and bool(host["user"].get("Sid")), "host: user SID missing")
    require(isinstance(host.get("os"), dict) and bool(host["os"].get("LastBootUpTimeUtc")), "host: boot time missing")
    require(isinstance(host.get("observer_session_id"), int) and host["observer_session_id"] > 0,
            "host: observer is not in an interactive session")
    require(host.get("observer_session_id") == host.get("powershell_session_id"),
            "host: PowerShell and observer sessions differ")
    require(isinstance(host.get("adapters"), list) and bool(host["adapters"]), "host: no display adapters enumerated")


def compare_hosts(before: Mapping[str, Any], after: Mapping[str, Any]) -> list[str]:
    return [f"host {key} changed between the before and after receipts"
            for key in HOST_STABLE_KEYS if before.get(key) != after.get(key)]


def select_adapter(adapters: list[Mapping[str, Any]], name: str) -> dict[str, Any]:
    matches = [dict(adapter) for adapter in adapters if adapter.get("Name") == name]
    require(len(matches) == 1, f"selected adapter: expected exactly one enumerated adapter named as the engine "
            f"device, found {len(matches)}")
    adapter = matches[0]
    require(bool(adapter.get("DriverVersion")) and bool(adapter.get("PNPDeviceID")),
            "selected adapter: driver version or device ID missing")
    return adapter


# ---------------------------------------------------------------- self check

def observer_self_check() -> dict[str, Any]:
    """Prove the ctypes observer reads this very process correctly."""

    probe = observer.WindowsProcessProbe()
    pid = os.getpid()
    table = probe.process_table()
    identity = probe.identity(pid)
    modules, error = probe.modules(pid)
    python_dll = f"python{sys.version_info.major}{sys.version_info.minor}.dll"
    loaded = [module for module in modules or [] if os.path.basename(module["path"]).lower() == python_dll]
    try:
        image_consistent = (len(loaded) == 1 and loaded[0]["image_error"] is None and loaded[0]["base_address"] > 0
                            and loaded[0]["image_size"] == observer.pe_size_of_image(loaded[0]["path"]))
    except (OSError, ValueError):
        image_consistent = False
    checks = {
        "process_table_has_self": pid in table,
        "image_path_is_executable": identity is not None and observer.same_path(identity["image_path"], sys.executable),
        "command_line_matches": identity is not None and identity["command_line"] == observer.current_command_line(),
        "session_available": identity is not None and isinstance(identity["session_id"], int),
        "modules_listed": modules is not None and error is None,
        "python_dll_listed": bool(loaded),
        "python_dll_base_and_image_size_match_pe_header": image_consistent,
        "argv_round_trip": observer.split_command_line(subprocess.list2cmdline(["a b", 'c"d', "e\\"]))
        == ["a b", 'c"d', "e\\"],
    }
    return {"passed": all(checks.values()), "checks": checks}


# ----------------------------------------------------------------- redaction

def redactor(plan: Mapping[str, Any]) -> Callable[[str], str]:
    pairs = []
    for label, path in (
            ("<evidence-parent>", plan.get("evidence_parent")),
            ("<prior-evidence>", plan.get("prior_evidence")),
            ("<game-root>", plan.get("game_root")),
            ("<mod-root>", plan.get("mod_root")),
            ("<engine-directory>", os.path.dirname(plan["fixed_inputs"]["launcher"]) if plan.get("fixed_inputs")
             else None),
            ("<repo>", plan.get("repo"))):
        if path:
            pairs.append((str(path), label))
            pairs.append((str(path).replace("\\", "/"), label))
    pairs.sort(key=lambda pair: -len(pair[0]))

    def redact(text: str) -> str:
        for path, label in pairs:
            text = re.sub(re.escape(path), label, text, flags=re.IGNORECASE)
        return text

    return redact


def redact_value(value: Any, redact: Callable[[str], str]) -> Any:
    if isinstance(value, str):
        return redact(value)
    if isinstance(value, pathlib.PurePath):
        return redact(str(value))
    if isinstance(value, dict):
        return {key: redact_value(item, redact) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [redact_value(item, redact) for item in value]
    return value


# --------------------------------------------------------------------- staging

def create_evidence_directory(parent: pathlib.Path) -> pathlib.Path:
    parent.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    directory = parent / f"{EVIDENCE_PREFIX}{stamp}-{secrets.token_hex(4)}"
    directory.mkdir()  # exclusive: FileExistsError if it somehow exists
    return directory


def stage_project(plan: Mapping[str, Any], staged: pathlib.Path) -> dict[str, Any]:
    source = plan["project"]
    staged.mkdir()
    for relative in plan["project_files"]:
        target = staged / pathlib.PurePosixPath(relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / pathlib.PurePosixPath(relative), target)
    staged_files = project_files(staged)
    require(staged_files == plan["project_files"], "staging: staged project bytes differ from the source project")
    return staged_files


# ----------------------------------------------------------------------- runs

def default_launcher(argv: list[str], stdout: Any, stderr: Any, cwd: str) -> tuple[Any, list[str], list[str]]:
    """Launch and return (process, launched argv, engine argv tail expected)."""

    return observer.launch_hidden(argv, stdout, stderr, cwd), list(argv), list(argv[1:])


def check_png(path: pathlib.Path) -> dict[str, Any]:
    from PIL import Image

    raw = path.read_bytes()
    require(raw.startswith(build_manifest.PNG_SIGNATURE), "capture: file is not a PNG")
    with Image.open(path) as image:
        image.load()
        size = image.size
        details = {"format": image.format, "mode": image.mode, "size": list(size)}
    require(tuple(size) == VIEWPORT, "capture: PNG is not 1280x720")
    return details


def capture_one(plan: Mapping[str, Any], evidence: pathlib.Path, staged: pathlib.Path, name: str,
                probe: Any, session_id: int, launcher: Callable[..., Any],
                observe_options: Mapping[str, Any]) -> dict[str, Any]:
    run_dir = evidence / name
    run_dir.mkdir()
    capture, report = run_dir / "frame.png", run_dir / "report.json"
    argv = rewrite_argv(plan["old_argv"], str(staged), str(capture), str(report))
    result: dict[str, Any] = {"name": name, "failures": []}
    exclusive_write(run_dir / "argv.json", serialise(argv))
    extension_path = staged / "bin" / plan["extension_library"]
    observation_error = None
    with open(run_dir / "stdout.log", "xb") as out, open(run_dir / "stderr.log", "xb") as err:
        result["launch_requested_utc"] = utc_now()
        process, launched_argv, engine_tail = launcher(argv, out, err, str(run_dir))
        try:
            record = observer.observe_launch(process, probe, str(capture), plan["extension_library"],
                                             **observe_options)
        except observer.ObservationError as error:
            observation_error = error  # the observer already terminated the owned tree
        finally:
            try:
                exit_code = process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()
                exit_code = process.wait()
    if observation_error is not None:
        exclusive_write(run_dir / "exit-code.txt", f"{exit_code}\n".encode("ascii"))
        exclusive_write(run_dir / "observation.json", serialise(observation_error.record))
        raise observation_error
    result["launcher_exit_code"] = exit_code
    result["launched_argv_sha256"] = sha256_bytes(serialise(launched_argv))
    exclusive_write(run_dir / "exit-code.txt", f"{exit_code}\n".encode("ascii"))
    exclusive_write(run_dir / "observation.json", serialise(record))
    expected = {
        "root_pid": int(process.pid),
        "root_image": plan["fixed_inputs"]["launcher"],
        "engine_image": plan["fixed_inputs"]["engine"],
        "extension_path": str(extension_path),
        "extension_sha256": plan["pins"]["extension_sha256"],
        "session_id": session_id,
        "root_command_line": subprocess.list2cmdline(launched_argv),
        "argv_tail": engine_tail,
    }
    assessment = observer.assess_observation(record, expected)
    exclusive_write(run_dir / "process-provenance.json", serialise({"expected": expected, "assessment": assessment}))
    result["process_provenance"] = assessment["status"]
    result["failures"].extend(f"{name}: {message}" for message in assessment["failures"])
    engine_exit = assessment.get("engine_exit") or {}
    result["engine_exit_code"] = engine_exit.get("exit_code")
    if exit_code != 0:
        result["failures"].append(f"{name}: launcher exit code {exit_code}")
    if engine_exit.get("exit_code") != 0:
        result["failures"].append(f"{name}: engine exit code {engine_exit.get('exit_code')}")

    for label, path in (("stdout", run_dir / "stdout.log"), ("stderr", run_dir / "stderr.log"),
                        ("report", report), ("capture", capture)):
        result[f"{label}_sha256"], hash_error = try_sha256(path)
        if hash_error not in (None, "missing"):
            result["failures"].append(f"{name}: {label} cannot be hashed ({hash_error})")
    try:
        report_value = read_json(report, f"{name} report")
        try:
            report_summary = build_manifest._validate_prototype_report(report_value)
        except build_manifest.EvidenceError as error:
            raise EvidenceError(f"{name} report: {error}") from error
        result["report"] = {"status": report_value.get("status"), **report_summary}
    except EvidenceError as error:
        result["failures"].append(str(error))
        report_summary = None
    try:
        result["png"] = check_png(capture)
    except (EvidenceError, OSError, ValueError) as error:
        result["failures"].append(f"{name}: capture PNG invalid or missing ({type(error).__name__})")
    try:
        stdout_text = (run_dir / "stdout.log").read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        stdout_text = ""
        result["failures"].append(f"{name}: stdout log unreadable ({type(error).__name__})")
    devices = DEVICE_LINE.findall(stdout_text)
    if len(devices) != 1:
        result["failures"].append(f"{name}: expected exactly one 'Using Device' line in stdout, found {len(devices)}")
    else:
        vendor, device = devices[0]
        result["engine_log_device"] = {"vendor": vendor, "name": device}
        if report_summary and (report_summary.get("adapter_name"), report_summary.get("vendor")) != (device, vendor):
            result["failures"].append(f"{name}: report adapter differs from the engine log device")
    result["status"] = "corroborated" if not result["failures"] else "failed"
    return result


# ------------------------------------------------------------------ manifest

def artifact_listing(evidence: pathlib.Path, exclude: set[str], failures: list[str]) -> list[dict[str, Any]]:
    """Hash every evidence file; one that cannot be read is listed and recorded as a failure."""

    rows: list[dict[str, Any]] = []
    try:
        paths = sorted(evidence.rglob("*"))
    except OSError as error:
        failures.append(f"artifacts: cannot enumerate the evidence directory ({type(error).__name__})")
        return rows
    for path in paths:
        relative = path.relative_to(evidence).as_posix()
        if relative in exclude:
            continue
        try:
            if not path.is_file():
                continue
            rows.append({"path": relative, "bytes": path.stat().st_size, "sha256": sha256_file(path)})
        except OSError as error:
            rows.append({"path": relative, "bytes": None, "sha256": None, "error": type(error).__name__})
            failures.append(f"artifact {relative}: cannot be hashed ({type(error).__name__})")
    return rows


def run_live(args: argparse.Namespace, plan: dict[str, Any], *, launcher: Callable[..., Any],
             host_probe: Callable[[], dict[str, Any]], probe: Any) -> dict[str, Any]:
    """Stage, capture both repeats and always finish with an artifact manifest.

    Any error after the evidence directory exists is recorded as a failure in
    the manifest rather than escaping, so partial evidence is never orphaned.
    """

    failures: list[str] = []
    runs: list[dict[str, Any]] = []
    state: dict[str, Any] = {}
    evidence = create_evidence_directory(plan["evidence_parent"])
    plan["evidence_directory"] = evidence
    staged = evidence / "project"
    try:
        stage_project(plan, staged)
        exclusive_write(evidence / "staging-receipt.json", serialise({
            "schema_version": 1,
            "kind": f"{KIND}-staging",
            "created_utc": utc_now(),
            "source_project": str(plan["project"]),
            "staged_project": str(staged),
            "excluded_top_level": sorted(EXCLUDED_TOP_LEVEL),
            "files": plan["project_files"],
            "extension_library": f"bin/{plan['extension_library']}",
            "source": plan["source"],
            "pins": plan["pins"],
            "fixed_inputs": plan["fixed_inputs"],
            "pre_launch_hashes": plan["pre_hashes"],
            "prior_evidence": str(plan["prior_evidence"]),
            "prior_evidence_hashes": plan["prior_hashes"],
            "tool_hashes": plan["tool_hashes"],
        }))
        host_before = state["host_before"] = host_probe()
        exclusive_write(evidence / "host-before.json", serialise(host_before))
        check_host_receipt(host_before)
        session_id = state["session_id"] = host_before["observer_session_id"]
        observe_options = {"interval_seconds": args.poll_interval, "timeout_seconds": args.timeout}
        for name in RUN_NAMES:
            result = capture_one(plan, evidence, staged, name, probe, session_id, launcher, observe_options)
            runs.append(result)
            failures.extend(result["failures"])
            if result["failures"]:
                failures.append(f"stopped after {name}; later repeats were not launched")
                break
    except Exception as error:  # noqa: BLE001 - recorded, then the manifest is still written
        failures.append(f"aborted: {type(error).__name__}: {error}")
    finally:
        try:
            state.update(verify_after_runs(plan, staged, failures, host_probe, evidence, state.get("host_before")))
        except Exception as error:  # noqa: BLE001 - recorded, then the manifest is still written
            failures.append(f"post-run verification aborted: {type(error).__name__}")

    selected = None
    host_before, host_after = state.get("host_before") or {}, state.get("host_after") or {}
    try:
        devices = {(run_result.get("engine_log_device") or {}).get("name") for run_result in runs}
        if len(runs) == len(RUN_NAMES) and len(devices) == 1 and None not in devices:
            device = devices.pop()
            try:
                before_adapter = select_adapter(host_before.get("adapters") or [], device)
                after_adapter = select_adapter(host_after.get("adapters") or [], device)
                require(before_adapter == after_adapter, "selected adapter changed between host receipts")
                selected = before_adapter
            except EvidenceError as error:
                failures.append(str(error))
        else:
            failures.append("selected adapter: the repeats did not report one common engine device")
        if len(runs) != len(RUN_NAMES):
            failures.append(f"only {len(runs)} of {len(RUN_NAMES)} repeats completed")
        else:
            captures = [run_result.get("capture_sha256") for run_result in runs]
            reports = [run_result.get("report_sha256") for run_result in runs]
            state["repeat_capture_hashes_equal"] = None not in captures and len(set(captures)) == 1
            state["repeat_report_hashes_equal"] = None not in reports and len(set(reports)) == 1
    except Exception as error:  # noqa: BLE001 - recorded, then the manifest is still written
        failures.append(f"manifest assembly: {type(error).__name__}")
    artifacts = artifact_listing(evidence, {"artifact-manifest.json"}, failures)

    status = "corroborated" if not failures else "failed"
    manifest = {
        "schema_version": 1,
        "kind": KIND,
        "tool": {"name": TOOL, "hashes": plan["tool_hashes"], "helper_revision": plan["source"]["helper_revision"]},
        "created_utc": utc_now(),
        "evidence_directory": evidence.name,
        "status": status,
        "provenance_claimed": status == "corroborated",
        "failures": failures,
        "visual_comparison": False,
        "acceptance": False,
        "source": plan["source"],
        "pins": plan["pins"],
        "pre_launch_hashes": plan["pre_hashes"],
        "post_run_hashes": state.get("post_hashes"),
        "staged_files_unchanged": state.get("staged_files_unchanged"),
        "files_created_in_staged_project": state.get("new_staged"),
        "prior_evidence_unchanged": state.get("prior_evidence_unchanged"),
        "host": {"session_id": state.get("session_id"),
                 "boot_time_utc": (host_before.get("os") or {}).get("LastBootUpTimeUtc"),
                 "user_sid": (host_before.get("user") or {}).get("Sid"), "os": host_before.get("os"),
                 "registry": host_before.get("registry"), "computer_name": host_before.get("computer_name"),
                 "machine_guid_sha256": host_before.get("machine_guid_sha256")},
        "selected_adapter": selected,
        "launch": {"api": "subprocess.Popen(argument list, shell=False)", "window": "STARTF_USESHOWWINDOW+SW_HIDE",
                   "creation_flags": "CREATE_NO_WINDOW", "poll_interval_seconds": args.poll_interval,
                   "rewritten_options": list(REWRITTEN_OPTIONS)},
        "repeat_capture_hashes_equal": state.get("repeat_capture_hashes_equal"),
        "repeat_report_hashes_equal": state.get("repeat_report_hashes_equal"),
        "runs": runs,
        "limitations": LIMITATIONS,
        "artifacts": artifacts,
    }
    manifest_sha = exclusive_write(evidence / "artifact-manifest.json", serialise(manifest))
    return {"status": status, "failures": failures, "evidence": evidence, "manifest_sha256": manifest_sha,
            "runs": runs}


def verify_after_runs(plan: Mapping[str, Any], staged: pathlib.Path, failures: list[str],
                      host_probe: Callable[[], dict[str, Any]], evidence: pathlib.Path,
                      host_before: Mapping[str, Any] | None) -> dict[str, Any]:
    """Post-hash fixed inputs, staged files and prior evidence; re-probe the host.

    Every file is checked on its own: a deleted or unreadable input becomes a
    recorded failure naming no private path instead of an exception, so the
    artifact manifest is still written.
    """

    state: dict[str, Any] = {}
    try:
        host_after = state["host_after"] = host_probe()
        exclusive_write(evidence / "host-after.json", serialise(host_after))
        if host_before is not None:
            failures.extend(compare_hosts(host_before, host_after))
    except Exception as error:  # noqa: BLE001
        failures.append(f"host after-receipt failed: {type(error).__name__}")
    post: dict[str, str | None] = {}
    for name, path in plan["fixed_inputs"].items():
        post[name], error = try_sha256(path)
        if error is not None:
            failures.append(f"{name}: post-run hash unavailable ({error})")
        elif post[name] != plan["pre_hashes"][name]:
            failures.append(f"{name}: post-run hash differs from the pre-launch hash")
    state["post_hashes"] = post
    staged_after: dict[str, str | None] = {}
    for relative, digest in plan["project_files"].items():
        staged_after[relative], error = try_sha256(staged / pathlib.PurePosixPath(relative))
        if staged_after[relative] != digest:
            failures.append(f"staged {relative}: missing or changed during the runs"
                            + (f" ({error})" if error is not None else ""))
    state["staged_files_unchanged"] = staged_after == dict(plan["project_files"])
    try:
        state["new_staged"] = sorted(
            path.relative_to(staged).as_posix() for path in staged.rglob("*")
            if path.is_file() and path.relative_to(staged).as_posix() not in plan["project_files"]
        ) if staged.is_dir() else []
    except OSError as error:
        state["new_staged"] = None
        failures.append(f"staged project: cannot enumerate files created during the runs ({type(error).__name__})")
    prior_after: dict[str, str | None] = {}
    for relative in plan["prior_hashes"]:
        prior_after[relative], error = try_sha256(plan["prior_evidence"] / relative)
        if error is not None:
            failures.append(f"prior evidence {relative}: post-run hash unavailable ({error})")
    state["prior_evidence_unchanged"] = prior_after == dict(plan["prior_hashes"])
    if not state["prior_evidence_unchanged"]:
        failures.append("prior evidence changed during the runs")
    return state


# ------------------------------------------------------------------------ CLI

def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = _Parser(prog="windows_prototype_capture.py", description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True, parser_class=_Parser)
    for name in ("dry-run", "run"):
        command = commands.add_parser(name)
        command.add_argument("--prior-evidence", required=True,
                             help="audited prior evidence directory (read only)")
        command.add_argument("--repo", default=str(REPO_ROOT), help="audited source checkout (default: this one)")
        command.add_argument("--evidence-parent", default=None,
                             help="git-ignored parent for the new evidence directory (default out/renderer-acceptance)")
        command.add_argument("--expect-staging-receipt-sha256", default=AUDITED_STAGING_RECEIPT_SHA256)
        command.add_argument("--expect-readiness-sha256", default=AUDITED_READINESS_SHA256)
        command.add_argument("--poll-interval", type=float, default=0.05)
        command.add_argument("--timeout", type=float, default=300.0, help="per-run observation timeout in seconds")
        if name == "run":
            command.add_argument("--allow-live-launch", action="store_true",
                                 help="required: acknowledges this launches the Godot prototype twice")
    args = parser.parse_args(argv)
    if not 0.005 <= args.poll_interval <= 1.0:
        raise UsageError("--poll-interval must be between 0.005 and 1 second")
    if args.timeout <= 0:
        raise UsageError("--timeout must be positive")
    if args.command == "run" and not args.allow_live_launch:
        raise UsageError("run launches Godot; pass --allow-live-launch to confirm")
    return args


def dry_run_receipt(plan: Mapping[str, Any], host: Mapping[str, Any], self_check: Mapping[str, Any]) -> dict[str, Any]:
    placeholder = "<new-evidence>"
    template = rewrite_argv(plan["old_argv"], f"{placeholder}/project", f"{placeholder}/<run>/frame.png",
                            f"{placeholder}/<run>/report.json")
    return {
        "argv_template": template,
        "argv_option_indices": plan["indices"],
        "pins": plan["pins"],
        "pre_launch_hashes": plan["pre_hashes"],
        "source": plan["source"],
        "project_file_count": len(plan["project_files"]),
        "extension_library": f"bin/{plan['extension_library']}",
        "prior_evidence_hashes": plan["prior_hashes"],
        "tool_hashes": plan["tool_hashes"],
        "host_probe": {
            "user_sid_present": bool((host.get("user") or {}).get("Sid")),
            "boot_time_present": bool((host.get("os") or {}).get("LastBootUpTimeUtc")),
            "observer_session_id": host.get("observer_session_id"),
            "os_build": (host.get("os") or {}).get("BuildNumber"),
            "adapter_names": [adapter.get("Name") for adapter in host.get("adapters") or []],
        },
        "observer_self_check": self_check,
        "runs_planned": list(RUN_NAMES),
        "launched": False,
    }


def run(argv: list[str] | None = None, *, launcher: Callable[..., Any] = default_launcher,
        host_probe: Callable[[], dict[str, Any]] = collect_host_receipt,
        probe_factory: Callable[[], Any] | None = None,
        self_check: Callable[[], dict[str, Any]] = observer_self_check) -> int:
    receipt: dict[str, Any] = {"tool": TOOL, "status": "failed", "visual_comparison": False, "acceptance": False,
                               "provenance_claimed": False}
    plan: dict[str, Any] = {}
    code = 2
    try:
        args = parse_args(sys.argv[1:] if argv is None else argv)
        receipt["command"] = args.command
        plan = preflight(args)
        if args.command == "dry-run":
            host = host_probe()
            check_host_receipt(host)
            checked = self_check()
            require(checked["passed"], "observer self-check failed")
            receipt.update(dry_run_receipt(plan, host, checked))
            receipt["status"] = "ready"
            code = 0
        else:
            probe = probe_factory() if probe_factory else observer.WindowsProcessProbe()
            outcome = run_live(args, plan, launcher=launcher, host_probe=host_probe, probe=probe)
            receipt.update({
                "status": outcome["status"],
                "provenance_claimed": outcome["status"] == "corroborated",
                "failures": outcome["failures"],
                "evidence_directory": outcome["evidence"].relative_to(plan["repo"]).as_posix(),
                "artifact_manifest_sha256": outcome["manifest_sha256"],
                "runs": [{key: run_result.get(key) for key in (
                    "name", "status", "launcher_exit_code", "engine_exit_code", "process_provenance",
                    "capture_sha256", "report_sha256")} for run_result in outcome["runs"]],
            })
            code = 0 if outcome["status"] == "corroborated" else 1
    except EvidenceError as error:
        receipt["error"] = str(error)
        code = 1
    except UsageError as error:
        receipt["error"] = str(error)
        code = 2
    except Exception as error:  # noqa: BLE001 - one structured receipt, never a traceback
        receipt["error"] = f"unexpected {type(error).__name__}: {error}"
        code = 2
    if plan:
        receipt = redact_value(receipt, redactor(plan))
    print(json.dumps(receipt, indent=2, sort_keys=True, default=str))
    return code


if __name__ == "__main__":
    sys.exit(run())
