"""Shared Python I/O and process primitives; callers retain format and error policy."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess


def sha256_file(path: Path | str) -> str:
    """Hash a file in bounded chunks without changing its bytes."""
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path | str, *, encoding: str = "utf-8"):
    return json.loads(Path(path).read_text(encoding=encoding))


def json_text(value, **options) -> str:
    """Format JSON with the caller's policy and one final newline."""
    options.setdefault("indent", 2)
    return json.dumps(value, **options) + "\n"


def json_bytes(value, **options) -> bytes:
    """UTF-8 JSON with a final LF, for formats that require byte output."""
    return json_text(value, **options).encode("utf-8")


def write_json(path: Path | str, value, *, create_parents=False, binary=False, **options) -> None:
    """Write JSON with the caller's existing formatting and directory policy.

    Text output retains native newline translation; binary output always uses LF.
    """
    path = Path(path)
    if create_parents:
        path.parent.mkdir(parents=True, exist_ok=True)
    text = json_text(value, **options)
    if binary:
        path.write_bytes(text.encode("utf-8"))
    else:
        path.write_text(text, encoding="utf-8")


def run_viewer(command, *, cwd, timeout):
    """Run a prepared viewer command, retaining its combined diagnostic stream."""
    return subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, check=False, timeout=timeout)
