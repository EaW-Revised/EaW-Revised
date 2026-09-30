"""A small synthetic TrueType font, built in code, for the font tests (#191).

The retail EmpireAtWar faces are never committed, so tests that need a font
build this one: square glyphs for space, digits and ASCII letters, one
contour each, under any name. FreeType loads it, so the viewer can draw it.
Every table checksum and the whole-font checksum adjustment are correct.
"""

from __future__ import annotations

import struct
from typing import Dict, List, Tuple

UNITS_PER_EM = 1000
ASCENT = 800
DESCENT = 200
ADVANCE = 600
RANGES = ((0x30, 0x39), (0x41, 0x5A), (0x61, 0x7A))


def checksum(data: bytes) -> int:
    padded = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(padded) // 4}I", padded)) & 0xFFFFFFFF


def _square(left: int, bottom: int, right: int, top: int) -> bytes:
    # One clockwise contour of four on-curve points; coordinates are deltas.
    points = ((left, bottom), (left, top), (right, top), (right, bottom))
    header = struct.pack(">hhhhh", 1, left, bottom, right, top)
    body = struct.pack(">HH", 3, 0) + bytes([0x01] * 4)
    previous_x = previous_y = 0
    xs = b""
    ys = b""
    for x, y in points:
        xs += struct.pack(">h", x - previous_x)
        ys += struct.pack(">h", y - previous_y)
        previous_x, previous_y = x, y
    glyph = header + body + xs + ys
    return glyph + b"\0" * (-len(glyph) % 4)


def _glyphs() -> Tuple[List[bytes], Dict[int, int]]:
    # Glyph 0 .notdef (a box), 1 space (empty), then one glyph per character;
    # glyph heights vary a little so different characters differ on screen.
    glyphs = [_square(50, 0, 550, 700), b""]
    cmap: Dict[int, int] = {0x20: 1}
    for first, last in RANGES:
        for code in range(first, last + 1):
            cmap[code] = len(glyphs)
            glyphs.append(_square(80, 0, 520, 400 + (code % 7) * 50))
    return glyphs, cmap


def _cmap(mapping: Dict[int, int]) -> bytes:
    segments = [(0x20, 0x20)] + list(RANGES) + [(0xFFFF, 0xFFFF)]
    count = len(segments)
    selector = count.bit_length() - 1
    search = 2 << selector
    ends = b"".join(struct.pack(">H", end) for _, end in segments)
    starts = b"".join(struct.pack(">H", start) for start, _ in segments)
    deltas = b"".join(struct.pack(">h", ((mapping[start] - start + 0x8000) % 0x10000) - 0x8000
                                  if start != 0xFFFF else 1) for start, _ in segments)
    offsets = b"\0\0" * count
    body = struct.pack(">HH", count * 2, search) + struct.pack(">HH", selector, count * 2 - search)
    body += ends + b"\0\0" + starts + deltas + offsets
    subtable = struct.pack(">HHH", 4, 6 + len(body), 0) + body
    return struct.pack(">HHHHI", 0, 1, 3, 1, 12) + subtable


def _name(records: Dict[int, str]) -> bytes:
    strings = b""
    entries = b""
    for name_id in sorted(records):
        encoded = records[name_id].encode("utf-16-be")
        entries += struct.pack(">6H", 3, 1, 0x409, name_id, len(encoded), len(strings))
        strings += encoded
    return struct.pack(">3H", 0, len(records), 6 + len(entries)) + entries + strings


def build_font(full_name: str, family: str = "") -> bytes:
    """A valid TrueType font whose full and PostScript names are `full_name`."""
    glyphs, mapping = _glyphs()
    offsets = [0]
    for glyph in glyphs:
        offsets.append(offsets[-1] + len(glyph))
    loca = b"".join(struct.pack(">H", offset // 2) for offset in offsets)
    head = struct.pack(">IIIIHHqqhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x000B, UNITS_PER_EM,
                       0, 0, 0, 0, ADVANCE, 700, 0, 8, 2, 0, 0)
    hhea = struct.pack(">IhhhHhhhhhhhhhhhH", 0x00010000, ASCENT, -DESCENT, 0, ADVANCE, 0, 50, 550, 1, 0, 0,
                       0, 0, 0, 0, 0, len(glyphs))
    hmtx = b"".join(struct.pack(">Hh", ADVANCE, 50 if index == 0 else 0 if index == 1 else 80)
                    for index in range(len(glyphs)))
    maxp = struct.pack(">IHHHHHHHHHHHHHH", 0x00010000, len(glyphs), 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)
    post = struct.pack(">IIhhIIIII", 0x00030000, 0, -100, 50, 0, 0, 0, 0, 0)
    name = _name({1: family or full_name, 2: "Regular", 4: full_name, 6: full_name.replace(" ", "")})
    tables = {
        b"cmap": _cmap(mapping), b"glyf": b"".join(glyphs), b"head": head, b"hhea": hhea,
        b"hmtx": hmtx, b"loca": loca, b"maxp": maxp, b"name": name, b"post": post,
    }
    return assemble(tables)


def assemble(tables: Dict[bytes, bytes]) -> bytes:
    """An sfnt from raw tables, with the checksums and the head adjustment filled in."""
    count = len(tables)
    selector = count.bit_length() - 1
    search = 16 << selector
    header = struct.pack(">IHHHH", 0x00010000, count, search, selector, count * 16 - search)
    offset = 12 + 16 * count
    records = b""
    data = b""
    head_at = None
    for tag in sorted(tables):
        table = tables[tag]
        if tag == b"head":
            head_at = offset + len(data)
            table = table[:8] + b"\0\0\0\0" + table[12:]
        records += struct.pack(">4sIII", tag, checksum(table), offset + len(data), len(table))
        data += table + b"\0" * (-len(table) % 4)
    font = bytearray(header + records + data)
    if head_at is not None:
        struct.pack_into(">I", font, head_at + 8, (0xB1B0AFBA - checksum(bytes(font))) & 0xFFFFFFFF)
    return bytes(font)


def fake_executable(fonts: List[bytes], filler: int = 4096) -> bytes:
    """A minimal PE-shaped file with each font embedded between filler bytes."""
    header = bytearray(0x80)
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x40)
    header[0x40:0x44] = b"PE\0\0"
    body = bytes(header)
    for index, font in enumerate(fonts):
        # Filler that also holds a decoy sfnt version with a bad directory.
        body += bytes((index * 37 + position) % 251 for position in range(filler)) + b"\x00\x01\x00\x00\x00\x09"
        body += font
    return body + b"\0" * filler
