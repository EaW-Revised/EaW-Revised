#!/usr/bin/env python3
"""Translate a bounded legacy Direct3D effect into validated SPIR-V stages.

Private inputs and every source-derived output belong under ignored ``out/shaders``.
Checked-in code contains only the independently written parser/lowering and synthetic
fixtures.  Run with ``--help`` for the reproducible corpus command.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
import platform
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Optional

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.shaders.fx_parser import (Effect, FxError, IncludeResolver, Pass,
                                         Technique, parameter_dict, parse_effect,
                                         prune_unreachable_functions,
                                         source_without_effect_wrappers)
else:
    from .fx_parser import (Effect, FxError, IncludeResolver, Pass, Technique,
                            parameter_dict, parse_effect,
                            prune_unreachable_functions,
                            source_without_effect_wrappers)


TOOL_VERSION = "1.1.0"


ARCHIVE_SHA256 = "88b9cb03322aab9be7451968ca514e9d2c810973d83f65459fd8cb52e7f96f8d"


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


def _write_text_lf(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


class ShaderPolicyError(RuntimeError):
    """A stable non-source diagnostic raised before protected processing."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2,
                       allow_nan=False) + "\n").encode("utf-8")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _resolve_output_path(output: Path) -> Path:
    """Resolve and confine every source-derived artifact below ignored out/shaders."""
    allowed_lexical = REPOSITORY_ROOT / "out" / "shaders"
    allowed_resolved = allowed_lexical.resolve(strict=False)
    # A reparse point on the protected root itself would make the apparently
    # ignored tree write elsewhere.  Fail instead of blessing its target.
    if allowed_resolved != allowed_lexical:
        raise ShaderPolicyError(
            "SHD_OUTPUT_OUTSIDE_CONFINEMENT",
            "repository out/shaders resolves through a symlink or junction",
        )
    candidate_input = output if output.is_absolute() else Path.cwd() / output
    candidate = candidate_input.resolve(strict=False)
    try:
        relative = candidate.relative_to(allowed_resolved)
    except ValueError as error:
        raise ShaderPolicyError(
            "SHD_OUTPUT_OUTSIDE_CONFINEMENT",
            "output must resolve below repository out/shaders",
        ) from error
    if not relative.parts:
        raise ShaderPolicyError(
            "SHD_OUTPUT_OUTSIDE_CONFINEMENT",
            "output must be a child of repository out/shaders",
        )
    existing = candidate
    while not existing.exists() and existing != allowed_resolved:
        existing = existing.parent
    if existing.exists() and not existing.is_dir():
        raise ShaderPolicyError(
            "SHD_OUTPUT_OUTSIDE_CONFINEMENT",
            "output has a non-directory existing parent",
        )
    return candidate


def _platform_identity() -> str:
    system = platform.system().casefold()
    machine = platform.machine().casefold().replace("amd64", "x86_64")
    return f"{system}-{machine}"


def _toolchain_contract() -> dict:
    path = Path(__file__).with_name("toolchain.json")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ShaderPolicyError("SHD_TOOLCHAIN_CONTRACT_INVALID", str(error)) from error
    if value.get("schema_version") != 1:
        raise ShaderPolicyError("SHD_TOOLCHAIN_CONTRACT_INVALID", "expected schema_version 1")
    return value


def _validate_tool_identity(executable: Path, kind: str) -> str:
    candidate = executable.resolve()
    if not candidate.is_file():
        raise ShaderPolicyError("SHD_TOOL_NOT_FOUND", f"{kind} executable does not exist")
    contract = _toolchain_contract().get(kind)
    if not isinstance(contract, dict):
        raise ShaderPolicyError("SHD_TOOLCHAIN_CONTRACT_INVALID", f"missing {kind} contract")
    expected_hash = contract.get("executable_sha256", {}).get(_platform_identity())
    if expected_hash is not None:
        actual_hash = sha256_file(candidate)
        if actual_hash != expected_hash:
            raise ShaderPolicyError(
                "SHD_TOOL_IDENTITY_MISMATCH",
                f"{kind} SHA-256 mismatch for {_platform_identity()}: expected {expected_hash}, got {actual_hash}",
            )
    try:
        result = subprocess.run(
            [str(candidate), "--version"], text=True, capture_output=True,
            check=False, timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ShaderPolicyError(
            "SHD_TOOL_IDENTITY_MISMATCH", f"{kind} --version failed: {error}",
        ) from error
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        raise ShaderPolicyError(
            "SHD_TOOL_IDENTITY_MISMATCH",
            f"{kind} --version exited {result.returncode}",
        )
    if kind == "glslang":
        expected = re.escape(str(contract["version"]))
        matched = re.search(rf"(?im)^Glslang Version:\s+\d+:{expected}\s*$", output)
        requirement = f"Glslang Version ...:{contract['version']}"
    else:
        expected_version = re.escape(str(contract["version"]))
        commit = str(contract["source_commit"])
        matched = re.search(
            rf"(?im)^SPIRV-Tools\s+{expected_version}\s+{expected_version}-\d+-g{re.escape(commit[:7])}(?:\s|$)",
            output,
        )
        requirement = f"SPIRV-Tools {contract['version']} commit {commit[:7]}"
    if matched is None:
        raise ShaderPolicyError(
            "SHD_TOOL_IDENTITY_MISMATCH", f"{kind} does not report pinned identity {requirement}",
        )
    lines = output.splitlines()
    return lines[0].strip() if lines else requirement


def _find_tool(explicit: Optional[Path], toolchain_root: Optional[Path], relative: str,
               fallback: str, kind: Optional[str] = None) -> Path:
    """Select a tool and enforce its pinned --version and SHA-256 identity."""
    if explicit is not None:
        candidate = explicit.resolve()
    elif toolchain_root is not None:
        candidate = (toolchain_root / relative).resolve()
    else:
        found = shutil.which(fallback)
        if not found:
            raise ShaderPolicyError(
                "SHD_TOOL_NOT_FOUND",
                f"{fallback} was not found; pass an explicit path or --toolchain-root",
            )
        candidate = Path(found).resolve()
    identity = kind or ("spirv_tools" if "spirv-val" in fallback else "glslang")
    _validate_tool_identity(candidate, identity)
    return candidate
