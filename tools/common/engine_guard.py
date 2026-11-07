"""Engine guard (owner rule 11-03): measurements, gates and previews run only on OUR patched Godot.

tools/engine_pins.json lists the SHA-256 of every patched exe we accept ('engines') and of the unpatched ones we know
('known_unpatched', refused with a clearer message). identify() names an exe; require_patched() refuses anything that is
not in 'engines'. The only bypass is EAWR_ALLOW_UNPATCHED_ENGINE=ab-control (exact value): a labelled control arm of an
engine A/B (official or release-template exe). A bypassed run carries the engine label and the override, never a plain
"patched" claim. The guard's own tests use the same value.

CLI: python tools/common/engine_guard.py <exe> [<exe> ...] [--json]
     exit 0 = every exe is a pinned patched engine (prints label + sha256), 3 = refused.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

PINS_PATH = Path(__file__).resolve().parents[1] / "engine_pins.json"
OVERRIDE_ENV = "EAWR_ALLOW_UNPATCHED_ENGINE"
OVERRIDE_VALUES = ("ab-control",)
OFFICIAL_MESSAGE = "official Godot detected, not our patched engine"
UNKNOWN_MESSAGE = "unknown Godot build, not our patched engine"
REFUSED_EXIT = 3


class UnpatchedEngineError(RuntimeError):
    pass


def load_pins(path: Path = PINS_PATH) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def identify(exe: Path | str, pins: dict | None = None) -> dict:
    """{'path', 'sha256', 'label', 'patched'}; label is None for a build no table names."""
    pins = pins or load_pins()
    exe = Path(exe)
    sha = _sha256(exe)
    for patched, table in ((True, pins.get("engines", {})), (False, pins.get("known_unpatched", {}))):
        for label, entry in table.items():
            if sha in entry.get("files", {}).values():
                return {"path": str(exe), "sha256": sha, "label": label, "patched": patched}
    return {"path": str(exe), "sha256": sha, "label": None, "patched": False}


def expected_sha256(file_name: str, pins: dict | None = None) -> str:
    """The pinned hash a refused exe should have had: the default engine's file of the same name, else its GUI exe."""
    pins = pins or load_pins()
    files = pins.get("engines", {}).get(pins.get("default", ""), {}).get("files", {})
    if not files:
        return "unknown"
    return files.get(file_name) or files.get("Godot_v4.7.2-stable_win64.exe") or next(iter(files.values()))


def override_active(environ=os.environ) -> str | None:
    """The bypass value (ab-control) when set exactly, else None."""
    value = environ.get(OVERRIDE_ENV)
    return value if value in OVERRIDE_VALUES else None


def require_patched(exe: Path | str, purpose: str = "run", pins: dict | None = None, environ=os.environ) -> dict:
    """identify() + refuse an unpatched engine. With the ab-control override it returns the identity (patched False, override set)."""
    exe = Path(exe)
    if not exe.is_file():
        raise UnpatchedEngineError(f"engine not found: {exe}")
    identity = identify(exe, pins)
    if identity["patched"]:
        return identity
    override = override_active(environ)
    if override:
        sys.stderr.write(f"engine guard: {OVERRIDE_ENV}={override}: allowing {identity['label'] or 'unknown'} "
                         f"{exe} ({purpose}); label every result of this run as {override}\n")
        return {**identity, "override": override}
    reason = OFFICIAL_MESSAGE if identity["label"] else UNKNOWN_MESSAGE
    raise UnpatchedEngineError(f"{reason}: UNPATCHED ENGINE: found sha256 {identity['sha256']} expected "
                               f"{expected_sha256(exe.name, pins)} (tools/engine_pins.json); {exe}"
                               f"{' (' + identity['label'] + ')' if identity['label'] else ''}; {purpose} refused.")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("exe", nargs="+", type=Path)
    parser.add_argument("--purpose", default="run")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    identities = []
    try:
        for exe in args.exe:
            identities.append(require_patched(exe, args.purpose))
    except UnpatchedEngineError as error:
        print(f"ENGINE GUARD: {error}", file=sys.stderr)
        return REFUSED_EXIT
    for identity in identities:
        print(json.dumps(identity) if args.json else f"{identity['label']} {identity['sha256']} {identity['path']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
