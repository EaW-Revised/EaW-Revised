"""Fetch and verify the paired manager project before installing it in a player kit."""
from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PIN = ROOT / "config/launcher-pin.json"


def read_pin(path: Path = DEFAULT_PIN) -> dict:
    pin = json.loads(path.read_text(encoding="utf-8"))
    version = pin.get("launcher_version", "")
    artifact = pin.get("artifact", {})
    protocol = pin.get("protocol", {})
    if (not re.fullmatch(r"\d+\.\d+\.\d+", version)
            or artifact.get("file") != f"eawr-launcher-{version}.zip"
            or not re.fullmatch(r"[0-9a-f]{64}", artifact.get("sha256", ""))
            or protocol.get("major") != 1
            or type(protocol.get("min_minor")) is not int or protocol["min_minor"] < 0):
        raise ValueError(f"invalid launcher pin: {path}")
    return pin


def verify_local(source: Path, pin: dict) -> None:
    if not source.is_file():
        raise FileNotFoundError(f"launcher artifact missing: {source}")
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != pin["artifact"]["sha256"]:
        raise ValueError(f"launcher artifact SHA-256 mismatch: {source} (expected {pin['artifact']['sha256']}, got {digest})")


def obtain(artifact: Path | None, pin: dict, cache: Path) -> Path:
    source = artifact
    if source is None:
        try:
            subprocess.run(["gh", "release", "download", "v" + pin["launcher_version"], "-R",
                            "EaW-Revised/eawr-launcher", "--pattern", pin["artifact"]["file"],
                            "--dir", str(cache)], check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as error:
            raise RuntimeError(f"cannot download pinned launcher artifact: {error.stderr.strip()}") from error
        except FileNotFoundError as error:
            raise RuntimeError("cannot download pinned launcher artifact: install GitHub CLI or pass --launcher-artifact") from error
        source = cache / pin["artifact"]["file"]
    verify_local(source, pin)
    return source


def prepare(service: Path | None, launcher: Path | None, artifact: Path | None,
            pin_path: Path, cache: Path) -> Path:
    """Validate an opted-in pair and obtain its verified zip before expensive build/import work."""
    for name, path in (("mods service", service), ("native launcher", launcher)):
        if path is None or not path.is_file():
            raise FileNotFoundError(f"{name} missing: {path}")
    if not (service.parent / "eawr_loadcheck.exe").is_file():
        raise FileNotFoundError("load check helper missing beside mods service")
    return obtain(artifact, read_pin(pin_path), cache)


def install(destination: Path, artifact: Path | None = None, pin_path: Path = DEFAULT_PIN) -> dict:
    pin = read_pin(pin_path)
    with tempfile.TemporaryDirectory(prefix="eawr-launcher-") as scratch:
        source = obtain(artifact, pin, Path(scratch))
        staged = Path(scratch) / "launcher"
        staged.mkdir()
        with zipfile.ZipFile(source) as archive:
            for entry in archive.infolist():
                relative = PurePosixPath(entry.filename)
                if (relative.is_absolute() or ".." in relative.parts or "\\" in entry.filename
                        or ":" in entry.filename or (entry.external_attr >> 16) & 0o170000 == 0o120000):
                    raise ValueError(f"unsafe launcher archive entry: {entry.filename}")
            archive.extractall(staged)
        if not (staged / "project.godot").is_file():
            raise ValueError("launcher artifact has no project.godot at its root")
        if destination.exists():
            raise ValueError(f"launcher destination already exists: {destination}")
        shutil.copytree(staged, destination)
    return pin


def install_manager(kit: Path, service: Path, artifact: Path | None = None,
                    pin_path: Path = DEFAULT_PIN) -> dict:
    if not service.is_file():
        raise FileNotFoundError(f"mods service missing: {service}")
    helper = service.parent / "eawr_loadcheck.exe"
    if not helper.is_file():
        raise FileNotFoundError(f"load check helper missing: {helper}")
    if not (kit / "EAWR.exe").is_file():
        raise FileNotFoundError(f"EAWR.exe missing from kit: {kit}")
    pin = install(kit / "launcher", artifact, pin_path)
    shutil.copyfile(service, kit / "eawr_mods_service.exe")
    shutil.copyfile(helper, kit / "eawr_loadcheck.exe")
    # WSH starts the native GUI entry point without even a transient cmd.exe console.
    (kit / "EAWR Mods.vbs").write_text(
        'Set shell = CreateObject("WScript.Shell")\n'
        'Set fso = CreateObject("Scripting.FileSystemObject")\n'
        'exe = fso.BuildPath(fso.GetParentFolderName(WScript.ScriptFullName), "EAWR.exe")\n'
        'shell.Run Chr(34) & exe & Chr(34) & " --mods", 1, False\n', encoding="ascii")
    # Optional desktop shortcut installer; generated only for manager-equipped kits.
    (kit / "Create EAWR Mods desktop shortcut.vbs").write_text(
        'Set shell = CreateObject("WScript.Shell")\n'
        'Set fso = CreateObject("Scripting.FileSystemObject")\n'
        'kit = fso.GetParentFolderName(WScript.ScriptFullName)\n'
        'exe = fso.BuildPath(kit, "EAWR.exe")\n'
        'If Not fso.FileExists(exe) Or Not fso.FileExists(fso.BuildPath(kit, "launcher\\project.godot")) Or Not fso.FileExists(fso.BuildPath(kit, "eawr_mods_service.exe")) Then\n'
        '  MsgBox "EAWR Mods is missing or incomplete. Extract the whole EAWR download and try again.", vbOKOnly + vbCritical, "EAWR Mods"\n'
        '  WScript.Quit 2\n'
        'End If\n'
        'shortcut = fso.BuildPath(shell.SpecialFolders("Desktop"), "EAWR Mods.lnk")\n'
        'If fso.FileExists(shortcut) Then\n'
        '  If MsgBox("An EAWR Mods desktop shortcut already exists. Replace it?", vbYesNo + vbQuestion + vbDefaultButton2, "EAWR Mods") <> vbYes Then WScript.Quit 0\n'
        'End If\n'
        'Set link = shell.CreateShortcut(shortcut)\n'
        'link.TargetPath = exe\n'
        'link.Arguments = "--mods"\n'
        'link.WorkingDirectory = kit\n'
        'link.Description = "Choose mods and play Empire at War Revised"\n'
        'link.IconLocation = exe & ",0"\n'
        'link.Save\n', encoding="ascii")
    (kit / "launcher-pin.json").write_text(json.dumps(pin, indent=2) + "\n", encoding="utf-8")
    return pin
