"""Report loading and result checks for run_supplemental.py."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path


class CheckFailure(RuntimeError):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)

def report_for(asset_scan: Path, profile: str, game_root: Path, report: Path, repo: Path, mod_root: Path | None = None) -> subprocess.CompletedProcess[str]:
    command = [str(asset_scan), "--profile", profile, "--game-root", str(game_root)]
    if mod_root is not None:
        command += ["--mod-root", str(mod_root)]
    command += ["--report", str(report)]
    return run(command, repo)


def load_report(path: Path) -> dict:
    check(path.exists(), f"asset_scan did not create report: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise CheckFailure(f"invalid JSON report {path}: {error}") from error


def codes(report: dict) -> list[str]:
    return [str(item.get("code")) for item in report.get("errors", [])]


def assert_report_shape(report: dict) -> None:
    check(report.get("schema_version") == 1, "report schema_version is not 1")
    check(isinstance(report.get("layers"), list), "report layers is not a list")
    check(isinstance(report.get("archive_probes"), list), "report archive_probes is not a list")
    check(isinstance(report.get("counts"), dict), "report counts is not an object")
    check(isinstance(report.get("winners"), list), "report winners is not a list")
    check(isinstance(report.get("shadowed"), list), "report shadowed is not a list")
    check(isinstance(report.get("errors"), list), "report errors is not a list")
    winner_paths = [item["path"] for item in report["winners"]]
    check(winner_paths == sorted(winner_paths), "winner paths are not sorted")
    check(len(winner_paths) == len(set(winner_paths)), "winner paths are not unique")
    check(report["counts"]["effective_records"] == len(report["winners"]), "effective count disagrees with winners")
