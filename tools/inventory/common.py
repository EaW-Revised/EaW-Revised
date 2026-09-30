from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

TOOL_VERSION = "1.0.0"


def canonical_bytes(value: Any) -> bytes:
    """Phase-0 canonical JSON: sorted keys, UTF-8, LF and one final newline."""
    text = json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        indent=2,
        sort_keys=True,
    )
    return (text + "\n").encode("utf-8")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def manifest_id(payload: dict[str, Any]) -> str:
    copy = dict(payload)
    copy.pop("manifest_id", None)
    return sha256(canonical_bytes(copy))


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical_bytes(value))


def source_hashes(directory: Path, names: list[str]) -> list[dict[str, str]]:
    rows = []
    for name in names:
        path = directory / name
        rows.append({"path": f"tools/inventory/{name}", "sha256": sha256(path.read_bytes())})
    return rows


def ascii_fold(value: str) -> str:
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in value)


def canonical_path(value: str) -> str:
    if not value or value.startswith(("/", "\\")) or (len(value) >= 2 and value[1] == ":"):
        raise ValueError("absolute or empty logical path")
    parts: list[str] = []
    for raw in value.replace("\\", "/").split("/"):
        if raw in ("", "."):
            continue
        if raw == "..":
            if not parts:
                raise ValueError("logical path escapes mount")
            parts.pop()
            continue
        if "\x00" in raw:
            raise ValueError("logical path contains NUL")
        parts.append(ascii_fold(raw))
    if not parts:
        raise ValueError("logical path has no components")
    return "/".join(parts)
