#!/usr/bin/env python3
"""Write the committed small skirmish battle save from a test log.

`skirmish_save_fixture` (tests/skirmish/CMakeLists.txt) prints a fresh save when it differs from the committed
tests/skirmish/fixtures/saves/tactical-v2-host.eawr-ckp:

    SKIRMISH-SAVE-FIXTURE-BYTES,sha256=<hex>,base64=<bytes>

After a format change (no back-compat before 1.0) run the test on a pool build with --ctest-verbose and:

    python tools/replay/skirmish_save_fixture.py --log <windows-msvc-ninja.test.log> [--out <save>]

The bytes are written only when the log names exactly one save and its SHA-256 matches. Then update the pin in
tools/publish_scan/config.json (binary_allowlist).
"""
from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_OUT = ROOT / "tests/skirmish/fixtures/saves/tactical-v2-host.eawr-ckp"
LINE = re.compile(r"SKIRMISH-SAVE-FIXTURE-BYTES,sha256=([0-9a-f]{64}),base64=([A-Za-z0-9+/=]+)")


def fresh_save(log: str) -> bytes:
    """The one fresh save a log prints; ValueError when there is none, several different ones, or a bad digest."""
    found = {(digest, text) for digest, text in LINE.findall(log)}
    if len(found) != 1:
        raise ValueError(f"the log names {len(found)} different fresh saves, expected one")
    digest, text = found.pop()
    try:
        data = base64.b64decode(text, validate=True)
    except binascii.Error as error:
        raise ValueError(f"the fresh save is not base64: {error}") from error
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError("the fresh save does not match its SHA-256")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", type=pathlib.Path, required=True, help="a verbose CTest log of skirmish_save_fixture")
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    try:
        data = fresh_save(args.log.read_text(encoding="utf-8", errors="replace"))
    except ValueError as error:
        print(f"skirmish_save_fixture: {error}", file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    print(f"wrote {args.out} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
