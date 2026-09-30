"""Synthetic MEG-v1 and manifest fixture writers for run_supplemental.py."""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Iterable


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def meg(entries: Iterable[tuple[str, bytes]]) -> bytes:
    """Build a valid MEG-v1 fixture without using the implementation."""

    rows = list(entries)
    names = b"".join(u16(len(name.encode("latin1"))) + name.encode("latin1") for name, _ in rows)
    payload_offset = 8 + len(names) + 20 * len(rows)
    table = bytearray()
    payload = bytearray()
    for index, (name, data) in enumerate(rows):
        table += u32(0x12345678)  # opaque CRC
        table += u32(index)  # opaque engine index
        table += u32(len(data))
        table += u32(payload_offset + len(payload))
        table += u32(index)
        payload += data
    return u32(len(rows)) + u32(len(rows)) + names + table + payload


def malformed_header() -> bytes:
    return b"\x00" * 3


def malformed_name_index() -> bytes:
    data = bytearray(meg([("DATA\\XML\\BAD.XML", b"x")]))
    data[8 + 2 + len("DATA\\XML\\BAD.XML".encode("latin1")) + 16 : 8 + 2 + len("DATA\\XML\\BAD.XML".encode("latin1")) + 20] = u32(1)
    return bytes(data)


def malformed_bounds() -> bytes:
    data = bytearray(meg([("DATA\\XML\\BAD.XML", b"x")]))
    entry_offset = 8 + 2 + len("DATA\\XML\\BAD.XML".encode("latin1"))
    data[entry_offset + 12 : entry_offset + 16] = u32(0xFFFFFF00)
    return bytes(data)


def malformed_duplicate() -> bytes:
    return meg([("DATA\\XML\\Same.XML", b"a"), ("data/xml/same.xml", b"b")])


def malformed_path(path: str) -> bytes:
    return meg([(path, b"x")])


def malformed_count() -> bytes:
    return u32(4_000_001) + u32(0)


def malformed_name_table() -> bytes:
    return u32(1) + u32(0) + u16(32) + b"short"


def malformed_entry_table() -> bytes:
    return u32(0) + u32(1) + b"\x00" * 5


def write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        output.write(value)


def manifest(root: Path, files: list[str]) -> None:
    body = "<Mega_Files>" + "".join(f"<File>{item}</File>" for item in files) + "</Mega_Files>"
    text(root / "MegaFiles.xml", body)
