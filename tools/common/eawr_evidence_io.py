#!/usr/bin/env python3
"""Read freshly produced CI evidence, retrying past a transient Windows file lock.

A hash or trace comparator must never be flaky (#527). On Windows, antivirus or the
search indexer can briefly keep a just-closed file open, turning an ordinary read of
evidence another process just finished writing into a transient PermissionError under
parallel load; a genuinely missing file (FileNotFoundError) is a different condition
and is never retried.
"""

from __future__ import annotations

import pathlib
import time
from typing import Callable, TypeVar

RETRY_ATTEMPTS = 5
RETRY_DELAY_SECONDS = 0.05

T = TypeVar("T")


def retrying(read: Callable[[], T]) -> T:
    """Call `read()`, retrying a bounded number of times past a transient
    PermissionError before giving up. `read` must be idempotent: it is retried
    from scratch (including re-opening any file), not resumed."""
    delay = RETRY_DELAY_SECONDS
    for attempt in range(RETRY_ATTEMPTS):
        try:
            return read()
        except PermissionError:
            if attempt == RETRY_ATTEMPTS - 1:
                raise
            time.sleep(delay)
            delay *= 2
    raise AssertionError("unreachable")  # pragma: no cover


def read_bytes(path: pathlib.Path) -> bytes:
    """`path.read_bytes()`, retrying a bounded number of times past a transient
    PermissionError before giving up."""
    return retrying(path.read_bytes)


def read_text(path: pathlib.Path, encoding: str = "utf-8") -> str:
    """`path.read_text()`, retrying a bounded number of times past a transient
    PermissionError before giving up."""
    return read_bytes(path).decode(encoding)
