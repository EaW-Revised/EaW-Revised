"""Kit engine staging and audit (owner rule 11-03: never the official Godot).

A test kit takes its engine ONLY through stage_engine(): the player + console exe come from a pinned source folder
(tools/release/fetch_pinned_godot.ps1 -Console -Out <dir>, or an explicit --source), are checked against
tools/engine_pins.json (the engine marked 'default') with tools/common/engine_guard.py, and are copied to <kit>/godot/.
There is deliberately no parameter that copies godot/* from another kit folder.

CLI:
  python tools/release/kit_engine.py stage  <kit_dir> [--source DIR]   exit 3 = refused / missing / hash mismatch
  python tools/release/kit_engine.py verify <kit_dir|kit.zip>          exit 3 = an exe under godot/ is not a pinned patched engine
  python tools/release/kit_engine.py notes  <kit_dir>                  prints the NOTES.txt engine line, derived from the exe
"""
from __future__ import annotations

import argparse
import importlib.util
import hashlib
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("engine_guard", ROOT / "tools" / "common" / "engine_guard.py")
guard = importlib.util.module_from_spec(_spec)
sys.modules.setdefault("engine_guard", guard)
_spec.loader.exec_module(guard)

DEFAULT_SOURCE = ROOT / "out" / "godot-eawr"
KIT_ENGINE_DIR = "godot"
REFUSED_EXIT = guard.REFUSED_EXIT


class KitEngineError(RuntimeError):
    pass


def _default_files(pins: dict) -> tuple[str, dict]:
    label = pins.get("default", "")
    entry = pins.get("engines", {}).get(label)
    if not entry or not entry.get("files"):
        raise KitEngineError(f"tools/engine_pins.json has no default engine files (default={label!r})")
    return label, entry["files"]


def _refuse_if_unpatched(identity_fn, path, label, pins):
    """identify() the file; raise KitEngineError unless it is a pinned patched engine (the override is NOT honoured here)."""
    identity = guard.identify(path, pins) if identity_fn is None else identity_fn
    if identity["patched"]:
        return identity
    reason = guard.OFFICIAL_MESSAGE if identity["label"] else guard.UNKNOWN_MESSAGE
    raise KitEngineError(f"{reason}: {Path(str(path)).name} sha256 {identity['sha256']}"
                         f"{' (' + identity['label'] + ')' if identity['label'] else ''} "
                         f"expected the pinned {label} engine (tools/engine_pins.json)")


def stage_engine(kit_dir, source_dir=None, pins: dict | None = None) -> list[dict]:
    """Copy the default pinned engine's exe files from source_dir into <kit_dir>/godot/; fail closed otherwise.

    Every file is verified (name listed in the pin, sha256 equal to the pin, identified as a patched engine) BEFORE any
    copy, and the copy is re-verified. Returns the identities of the staged files."""
    pins = pins or guard.load_pins()
    label, files = _default_files(pins)
    source = Path(source_dir) if source_dir else DEFAULT_SOURCE
    kit_dir = Path(kit_dir)
    checked = []
    for name, want in files.items():
        src = source / name
        if not src.is_file():
            raise KitEngineError(f"missing engine file {src} (fetch it with tools/release/fetch_pinned_godot.ps1 -Console "
                                 f"-Out <dir> or pass --source)")
        identity = _refuse_if_unpatched(None, src, label, pins)
        if identity["sha256"] != want or identity["label"] != label:
            raise KitEngineError(f"hash mismatch: {name} is {identity['label']} sha256 {identity['sha256']}, the kit needs "
                                 f"{label} sha256 {want} (tools/engine_pins.json)")
        checked.append((name, src))
    dest = kit_dir / KIT_ENGINE_DIR
    dest.mkdir(parents=True, exist_ok=True)
    for stale in dest.glob("*.exe"):
        if stale.name not in files:
            raise KitEngineError(f"unexpected exe in the kit engine folder: {stale} (remove it; kits carry only the pinned engine)")
    staged = []
    for name, src in checked:
        target = dest / name
        shutil.copyfile(src, target)
        staged.append(_refuse_if_unpatched(None, target, label, pins))
    return staged


def _kit_exes(kit_dir: Path) -> list[Path]:
    folder = Path(kit_dir) / KIT_ENGINE_DIR
    return sorted(folder.glob("*.exe")) if folder.is_dir() else []


def verify_kit(kit, pins: dict | None = None) -> list[dict]:
    """Every exe under <kit>/godot (folder or .zip) must be a pinned patched engine; at least one must exist."""
    pins = pins or guard.load_pins()
    kit = Path(kit)
    identities = []
    if kit.is_file() and zipfile.is_zipfile(kit):
        with zipfile.ZipFile(kit) as archive:
            names = [n for n in archive.namelist()
                     if n.lower().endswith(".exe") and f"/{KIT_ENGINE_DIR}/" in "/" + n.lower().replace("\\", "/")]
            for name in names:
                identities.append(_identity_for_sha(hashlib.sha256(archive.read(name)).hexdigest(), name, pins))
    else:
        for exe in _kit_exes(kit):
            identities.append(guard.identify(exe, pins))
    if not identities:
        raise KitEngineError(f"no engine exe under {KIT_ENGINE_DIR}/ in {kit}")
    for identity in identities:
        if not identity["patched"]:
            reason = guard.OFFICIAL_MESSAGE if identity["label"] else guard.UNKNOWN_MESSAGE
            raise KitEngineError(f"{reason}: {identity['path']} sha256 {identity['sha256']}"
                                 f"{' (' + identity['label'] + ')' if identity['label'] else ''}")
    return identities


def _identity_for_sha(sha: str, name: str, pins: dict) -> dict:
    for patched, table in ((True, pins.get("engines", {})), (False, pins.get("known_unpatched", {}))):
        for label, entry in table.items():
            if sha in entry.get("files", {}).values():
                return {"path": name, "sha256": sha, "label": label, "patched": patched}
    return {"path": name, "sha256": sha, "label": None, "patched": False}


def engine_notes_line(kit_dir, pins: dict | None = None) -> str:
    """The NOTES.txt engine line, recomputed from the GUI exe in <kit_dir>/godot (never from a template or older kit)."""
    pins = pins or guard.load_pins()
    exes = _kit_exes(Path(kit_dir))
    if not exes:
        raise KitEngineError(f"no engine exe under {KIT_ENGINE_DIR}/ in {kit_dir}")
    gui = next((e for e in exes if "console" not in e.name.lower()), exes[0])
    identity = guard.identify(gui, pins)
    if not identity["patched"]:
        raise KitEngineError(f"{guard.OFFICIAL_MESSAGE if identity['label'] else guard.UNKNOWN_MESSAGE}: {gui} "
                             f"sha256 {identity['sha256']}")
    return f"Engine: {identity['label']} (patched Godot fork) {gui.name} sha256 {identity['sha256']}"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    stage = sub.add_parser("stage")
    stage.add_argument("kit_dir", type=Path)
    stage.add_argument("--source", type=Path)
    for name in ("verify", "notes"):
        sub.add_parser(name).add_argument("kit", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "stage":
            for identity in stage_engine(args.kit_dir, args.source):
                print(f"staged {identity['label']} {identity['sha256']} {identity['path']}")
        elif args.cmd == "verify":
            for identity in verify_kit(args.kit):
                print(f"ok {identity['label']} {identity['sha256']} {identity['path']}")
        else:
            print(engine_notes_line(args.kit))
    except (KitEngineError, guard.UnpatchedEngineError) as error:
        print(f"KIT ENGINE: {error}", file=sys.stderr)
        return REFUSED_EXIT
    return 0


if __name__ == "__main__":
    sys.exit(main())
