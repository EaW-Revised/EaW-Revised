"""Wholly original synthetic MTD + TGA atlas fixture for the viewer overlay.

Every byte produced here is generated from the constants in this module. No
game bytes, no extracted asset and no installation path is involved. The page
is a 32-bit uncompressed TGA; both top-left and bottom-left source-origin
variants are emitted so the viewer's one documented origin conversion is
exercised.

The directory holds exactly two records in the documented 81-byte layout:

    char name[64]; u32 x; u32 y; u32 width; u32 height; u8 has_alpha

``I_ALPHA_ON.TGA`` is the alpha-on entry: its left half is fully opaque and its
right half is fully transparent, so a viewer capture proves both that the
rectangle was sampled and that alpha was honoured. ``I_ALPHA_OFF.TGA`` is the
alpha-off entry: every texel is opaque.
"""

from __future__ import annotations

import pathlib
import struct

PAGE_WIDTH = 64
PAGE_HEIGHT = 64

MTD_LOGICAL_PATH = "data/art/textures/eawr_overlay_fixture.mtd"
PAGE_LOGICAL_PATH = "data/art/textures/eawr_overlay_fixture.tga"
BOTTOM_LEFT_MTD_LOGICAL_PATH = "data/art/textures/eawr_overlay_fixture_bottom_left.mtd"
BOTTOM_LEFT_PAGE_LOGICAL_PATH = "data/art/textures/eawr_overlay_fixture_bottom_left.tga"

# The same directory over a DDS backing page, so the overlay is exercised on
# both supported page containers. The DDS page uses the A8R8G8B8 channel masks
# the decoder reports as bgra8, which is the overlay's channel-restore path.
DDS_MTD_LOGICAL_PATH = "data/art/textures/eawr_overlay_fixture_dds.mtd"
DDS_PAGE_LOGICAL_PATH = "data/art/textures/eawr_overlay_fixture_dds.dds"

ALPHA_ON_NAME = "I_ALPHA_ON.TGA"
ALPHA_OFF_NAME = "I_ALPHA_OFF.TGA"

# (name, x, y, width, height, has_alpha)
ENTRIES = (
    (ALPHA_ON_NAME, 8, 8, 16, 16, 1),
    (ALPHA_OFF_NAME, 32, 8, 16, 16, 0),
)

# Deliberately saturated, mutually distinct and far from the viewer's near
# black clear colour, so a drawn texel is distinguishable without tolerance
# guesswork. Each rectangle is asymmetric on both axes: the alpha-on entry is
# opaque on its left and transparent on its right, and carries a marker band
# across its top rows only. A flipped or transposed sampling therefore cannot
# reproduce the same source-colour-to-drawn-colour mapping.
ALPHA_ON_RGB = (255, 128, 0)
ALPHA_ON_MARKER_RGB = (255, 0, 255)
ALPHA_ON_MARKER_ROWS = 4
ALPHA_OFF_RGB = (0, 224, 255)
ALPHA_OFF_MARKER_RGB = (0, 64, 160)
ALPHA_OFF_MARKER_ROWS = 4
ALPHA_OFF_EDGE_RGB = (255, 255, 255)
PAGE_FILL_RGBA = (0, 0, 0, 0)

NAME_FIELD_BYTES = 64
RECORD_BYTES = 81


def _name_field(name: str) -> bytes:
    encoded = name.encode("ascii")
    if len(encoded) >= NAME_FIELD_BYTES:
        raise ValueError(f"fixture name must be NUL-terminated within 64 bytes: {name}")
    return encoded + b"\x00" * (NAME_FIELD_BYTES - len(encoded))


def mtd_bytes() -> bytes:
    payload = bytearray(struct.pack("<I", len(ENTRIES)))
    for name, x, y, width, height, has_alpha in ENTRIES:
        payload += _name_field(name)
        payload += struct.pack("<IIII", x, y, width, height)
        payload += struct.pack("<B", has_alpha)
    expected = 4 + len(ENTRIES) * RECORD_BYTES
    if len(payload) != expected:
        raise AssertionError(f"MTD fixture is {len(payload)} bytes, expected {expected}")
    return bytes(payload)


def _page_pixels() -> list[list[tuple[int, int, int, int]]]:
    rows = [[PAGE_FILL_RGBA for _ in range(PAGE_WIDTH)] for _ in range(PAGE_HEIGHT)]
    on_x, on_y, on_w, on_h = ENTRIES[0][1:5]
    for row in range(on_h):
        for column in range(on_w):
            if column >= on_w // 2:
                rows[on_y + row][on_x + column] = (0, 0, 0, 0)
                continue
            colour = ALPHA_ON_MARKER_RGB if row < ALPHA_ON_MARKER_ROWS else ALPHA_ON_RGB
            rows[on_y + row][on_x + column] = (*colour, 255)
    off_x, off_y, off_w, off_h = ENTRIES[1][1:5]
    for row in range(off_h):
        for column in range(off_w):
            if column == off_w - 1:
                colour = ALPHA_OFF_EDGE_RGB
            elif row < ALPHA_OFF_MARKER_ROWS:
                colour = ALPHA_OFF_MARKER_RGB
            else:
                colour = ALPHA_OFF_RGB
            rows[off_y + row][off_x + column] = (*colour, 255)
    return rows


def _tga_bytes(source_origin: str) -> bytes:
    if source_origin not in {"top-left", "bottom-left"}:
        raise ValueError(f"unsupported TGA source origin: {source_origin}")
    # Uncompressed true-colour, 32 bits per pixel, 8 alpha bits. Stored channel
    # order is B, G, R, A; a bottom-left source stores rows in reverse order.
    header = struct.pack(
        "<BBBHHBHHHHBB",
        0,  # id length
        0,  # colour map type
        2,  # uncompressed true-colour
        0, 0, 0,  # colour map specification
        0, 0,  # x/y origin
        PAGE_WIDTH,
        PAGE_HEIGHT,
        32,
        0x28 if source_origin == "top-left" else 0x08,
    )
    body = bytearray()
    rows = _page_pixels()
    if source_origin == "bottom-left":
        rows = list(reversed(rows))
    for row in rows:
        for red, green, blue, alpha in row:
            body += bytes((blue, green, red, alpha))
    return header + bytes(body)


def tga_bytes() -> bytes:
    return _tga_bytes("top-left")


def bottom_left_tga_bytes() -> bytes:
    return _tga_bytes("bottom-left")


def dds_bytes() -> bytes:
    # Uncompressed 32-bit DDS with DDPF_RGB | DDPF_ALPHAPIXELS and A8R8G8B8
    # channel masks; DDS scanlines are top-down, matching MTD's convention.
    header = struct.pack(
        "<4sIIIIIII44xIIIIIIIIIIIII",
        b"DDS ",
        124,
        0x1 | 0x2 | 0x4 | 0x8 | 0x1000,  # CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
        PAGE_HEIGHT,
        PAGE_WIDTH,
        PAGE_WIDTH * 4,
        0,  # depth
        1,  # mip count
        32,  # pixel format size
        0x1 | 0x40,  # DDPF_ALPHAPIXELS | DDPF_RGB
        0,  # four CC
        32,  # bits per pixel
        0x00FF0000,  # red mask
        0x0000FF00,  # green mask
        0x000000FF,  # blue mask
        0xFF000000,  # alpha mask
        0x1000,  # DDSCAPS_TEXTURE
        0, 0, 0, 0,
    )
    if len(header) != 128:
        raise AssertionError(f"DDS header is {len(header)} bytes, expected 128")
    body = bytearray()
    for row in _page_pixels():
        for red, green, blue, alpha in row:
            body += bytes((blue, green, red, alpha))
    return bytes(header) + bytes(body)


def alpha_on_texel_counts() -> tuple[int, int]:
    """Return (opaque texels, transparent texels) inside the alpha-on rectangle."""
    _name, _x, _y, width, height, _has_alpha = ENTRIES[0]
    opaque = (width // 2) * height
    return opaque, width * height - opaque


def distinct_opaque_colours(entry_index: int) -> int:
    """Count the distinct opaque source colours inside one entry rectangle."""
    _name, x, y, width, height, _has_alpha = ENTRIES[entry_index]
    rows = _page_pixels()
    return len({
        rows[y + row][x + column][:3]
        for row in range(height)
        for column in range(width)
        if rows[y + row][x + column][3] == 255
    })


def write_fixture_root(root: pathlib.Path) -> pathlib.Path:
    """Materialise a minimal single-layer VFS data root and return it."""
    data = root / "Data"
    textures = data / "art" / "textures"
    textures.mkdir(parents=True, exist_ok=True)
    # The mount contract requires at least one declared archive. This fixture
    # is loose-only, so it declares one archive that is deliberately absent;
    # the resolver records that as missing and mounts the loose layer.
    (data / "MegaFiles.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<Mega_Files>\n  <File>EawrOverlayFixture.meg</File>\n</Mega_Files>\n",
        encoding="utf-8",
    )
    (textures / "eawr_overlay_fixture.mtd").write_bytes(mtd_bytes())
    (textures / "eawr_overlay_fixture.tga").write_bytes(tga_bytes())
    (textures / "eawr_overlay_fixture_bottom_left.mtd").write_bytes(mtd_bytes())
    (textures / "eawr_overlay_fixture_bottom_left.tga").write_bytes(bottom_left_tga_bytes())
    (textures / "eawr_overlay_fixture_dds.mtd").write_bytes(mtd_bytes())
    (textures / "eawr_overlay_fixture_dds.dds").write_bytes(dds_bytes())
    return root


if __name__ == "__main__":
    import sys

    target = write_fixture_root(pathlib.Path(sys.argv[1]))
    print(target)
