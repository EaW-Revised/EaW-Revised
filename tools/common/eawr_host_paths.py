"""Python twin of tools/common/EawrHostPaths.ps1 (see docs/host-paths.md).

Precedence, highest first: an explicit value, the setting's environment
variable, the file named by EAWR_HOST_PATHS (else config/host-paths.local.json),
then the committed config/host-paths.json. The settings table must match the
PowerShell resolver's; tests/ci/test_linux_build.py checks that.
"""

from __future__ import annotations

import json
import os
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SETTINGS = {
    "rigToolsRoot": "EAWR_RIG_TOOLS_ROOT",
    "rigPython": "EAWR_RIG_PYTHON",
    "airtestPython": "EAWR_AIRTEST_PYTHON",
    "rigAirtestRunnerRoot": "EAWR_RIG_AIRTEST_RUNNER_ROOT",
    "rigFeasibilityRoot": "EAWR_RIG_FEASIBILITY_ROOT",
    "focMenuTemplates": "EAWR_FOC_MENU_TEMPLATES",
    "linuxBuildHost": "EAWR_LINUX_BUILD_HOST",
    "linuxBuildUser": "EAWR_LINUX_BUILD_USER",
    "linuxBuildIdentity": "EAWR_LINUX_BUILD_IDENTITY",
    "linuxBuildSlots": "EAWR_LINUX_BUILD_SLOTS",
    "windowsBuildHost": "EAWR_WINDOWS_BUILD_HOST",
    "windowsBuildUser": "EAWR_WINDOWS_BUILD_USER",
    "windowsBuildIdentity": "EAWR_WINDOWS_BUILD_IDENTITY",
    "localGpuSlotTool": "EAWR_LOCAL_GPU_SLOT_TOOL",
    "localLabRoot": "EAWR_LOCAL_LAB_ROOT",
}


class HostPathError(Exception):
    pass


def _read(path: pathlib.Path) -> dict | None:
    if not path.is_file():
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schemaVersion") != 1:
        raise HostPathError(f"Unsupported host-paths schemaVersion in {path}: {data.get('schemaVersion')}")
    for key in data:
        if key not in ("schemaVersion", "_comment") and key not in SETTINGS:
            raise HostPathError(f"Unknown host-paths setting '{key}' in {path}")
    return data


def resolve(name: str, explicit: str | None = None, optional: bool = False,
            environ: os._Environ | dict | None = None, config_dir: pathlib.Path | None = None) -> str | None:
    if name not in SETTINGS:
        raise HostPathError(f"Unknown host-paths setting '{name}'")
    if explicit:
        return explicit
    environ = os.environ if environ is None else environ
    if environ.get(SETTINGS[name]):
        return environ[SETTINGS[name]]
    config_dir = config_dir or ROOT / "config"
    override = environ.get("EAWR_HOST_PATHS")
    if override and not pathlib.Path(override).is_file():
        raise HostPathError(f"EAWR_HOST_PATHS names a missing file: {override}")
    for path in (pathlib.Path(override) if override else config_dir / "host-paths.local.json",
                 config_dir / "host-paths.json"):
        data = _read(path)
        if data and data.get(name):
            return str(data[name])
    if optional:
        return None
    raise HostPathError(f"Host path '{name}' is not configured: pass it explicitly, set {SETTINGS[name]}, "
                        "or add it to config/host-paths.local.json")
