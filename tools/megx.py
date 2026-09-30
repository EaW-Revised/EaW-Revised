"""Read the bounded MEG-v1 index; retain the original one-member inspection CLI."""

from __future__ import annotations

import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class MegEntry:
    name: str
    size: int
    offset: int


def read_index(path: Path) -> list[MegEntry]:
    archive_size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(8)
        if len(header) != 8:
            raise ValueError(f"{path}: truncated MEG header")
        name_count, entry_count = struct.unpack("<II", header)
        if name_count > 1_000_000 or entry_count > 1_000_000:
            raise ValueError(f"{path}: implausible MEG counts")
        names: list[str] = []
        total_name_bytes = 0
        for index in range(name_count):
            length_bytes = stream.read(2)
            if len(length_bytes) != 2:
                raise ValueError(f"{path}: truncated name length {index}")
            length = struct.unpack("<H", length_bytes)[0]
            total_name_bytes += length
            if total_name_bytes > 64 * 1024 * 1024:
                raise ValueError(f"{path}: filename table exceeds 64 MiB")
            value = stream.read(length)
            if len(value) != length or b"\0" in value:
                raise ValueError(f"{path}: invalid name {index}")
            names.append(value.decode("latin-1"))
        entries: list[MegEntry] = []
        for index in range(entry_count):
            row = stream.read(20)
            if len(row) != 20:
                raise ValueError(f"{path}: truncated entry {index}")
            _crc, _flags, size, offset, name_index = struct.unpack("<IIIII", row)
            if name_index >= len(names) or offset > archive_size or size > archive_size - offset:
                raise ValueError(f"{path}: invalid entry {index}")
            entries.append(MegEntry(names[name_index], size, offset))
        return entries


def inspect(path: Path, want: str) -> None:
    for entry in read_index(path):
        if entry.name.upper().endswith(want.upper()):
            with path.open("rb") as stream:
                stream.seek(entry.offset)
                blob = stream.read(entry.size)
            print(entry.name, entry.size, "bytes; header:", blob[:4])
            strings = re.findall(rb"[\x20-\x7e]{4,}", blob)
            print(b"\n".join(strings[:120]).decode())
            return


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: python tools/megx.py ARCHIVE.meg MEMBER_SUFFIX")
    inspect(Path(sys.argv[1]), sys.argv[2])
