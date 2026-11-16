"""Wholly original synthetic land map whose sky is a star-tile dome (#6802).

The scene fixture's flat land map (tests/assets/fixtures/scene_fixture.py) with
one environment record whose primary sky names an invented object. That
object's model is one box around the model origin carrying a single
MeshGlossForStarfields.fxo surface (a MeshGloss-family tile surface, the shape
the star-tile sky models have), a BaseTexture and authored Emissive, Diffuse
and Specular values. The texture is a 4 x 4 checker of a light and a dark colour: a
dome that is drawn is plain to see around the terrain, and the map mode's own
evidence (which compares pixels with the capture's corner colour) can tell it
from the clear colour, which a one-colour dome would defeat. Every value is invented here; names carry an ``EAWR_STAR_SKY`` /
``eawr_star_sky`` prefix. The map keeps the scene fixture's logical path.

``write_fixture_root(root)`` materialises ``root/GameData/Data`` for
``--eawr-game-root root``.
"""

from __future__ import annotations

import pathlib
import struct

import foliage_fixture
import scene_fixture as base

MAP_LOGICAL_PATH = base.MAP_LOGICAL_PATH

SKY_OBJECT = "EAWR_STAR_SKY"
SKY_MODEL = "eawr_star_sky.alo"
SKY_TEXTURE = "eawr_star_sky_tile.dds"
SKY_SHADER = "MeshGlossForStarfields.fxo"
# A light and a dark texel: with Emissive 0.5 the unlit gain is exactly 1, so the
# dome shows them as stored.
SKY_RGBA = (200, 220, 255, 255)
SKY_DARK_RGBA = (20, 20, 40, 255)
EMISSIVE = (0.5, 0.5, 0.5, 1.0)
HALF_EXTENT = 1000.0
# #6811: the additive variant's one surface is MeshAdditive.fxo (not a sun
# bone): the same tile texture added over the clear colour with an authored
# Color and an unscrolled UVScrollRate.
ADDITIVE_SHADER = "MeshAdditive.fxo"
ADDITIVE_COLOR = (1.0, 1.0, 1.0, 1.0)
ADDITIVE_SCROLL = (0.0, 0.0, 0.0, 0.0)


def environment_minis() -> bytes:
    """The foliage fixture's lighting record (it is the one the map mode's
    ``--eawr-environment map`` decodes), with the star sky as primary sky and
    no wind."""
    minis = foliage_fixture.environment_minis()
    wind = base._mini(0x2B, struct.pack("<f", foliage_fixture.WIND_HEADING_DEGREES)) \
        + base._mini(0x2C, struct.pack("<f", foliage_fixture.WIND_SPEED))
    assert minis.endswith(wind)
    minis = minis[:-len(wind)]
    minis = minis.replace(base._mini(0x14, base._cstring("EAWR_Foliage_Windy")),
                          base._mini(0x14, base._cstring("EAWR_Star_Sky")))
    return minis + base._mini(0x19, base._cstring(SKY_OBJECT))


def ted_bytes() -> bytes:
    """The scene fixture's flat map with no placements and one 1/256/4/6
    environment record."""
    width, height = base.TERRAIN_WIDTH, base.TERRAIN_HEIGHT
    root = base._mini(0, struct.pack("<I", 0x0201)) + base._mini(1, struct.pack("<I", 1))
    header = (base._mini(0, struct.pack("<I", width)) + base._mini(1, struct.pack("<I", height))
              + base._mini(4, struct.pack("<I", width * height)) + base._mini(5, struct.pack("<I", 1)))
    material = base._chunk(3, base._mini(0x0C, base._cstring("eawr_scene_ground.tga")))
    plane = struct.pack("<hBB", 0, 0, 255) * (width * height)
    terrain = base._chunk(0, header) + base._chunk(2, material, True) + base._chunk(5, plane)
    environments = base._chunk(256, base._chunk(4, base._chunk(6, environment_minis()), True), True)
    main = (environments + base._chunk(257, terrain, True) + base._chunk(266, plane)
            + base._chunk(267, b"", True) + base._chunk(258, base._chunk(1, b"", True), True))
    return struct.pack("<II", 0, len(root)) + root + base._chunk(1, main, True)


def _vector4(name: str, value) -> bytes:
    return base._chunk(0x10106, base._mini(1, base._cstring(name)) + base._mini(2, struct.pack("<4f", *value)))


def alo_bytes(additive: bool = False) -> bytes:
    bone_data = struct.pack("<iI", -1, 1) + struct.pack("<12f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)
    bone = base._chunk(0x202, base._chunk(0x203, base._cstring("Root")) + base._chunk(0x205, bone_data), True)
    skeleton = base._chunk(0x200, base._chunk(0x201, struct.pack("<I", 1)) + bone, True)
    vertices, indices = base._box(HALF_EXTENT, HALF_EXTENT, HALF_EXTENT)
    texture = base._chunk(0x10105, base._mini(1, base._cstring("BaseTexture"))
                          + base._mini(2, base._cstring("eawr_star_sky_tile.tga")))
    if additive:
        parameters = (base._chunk(0x10101, base._cstring(ADDITIVE_SHADER))
                      + _vector4("Color", ADDITIVE_COLOR) + _vector4("UVScrollRate", ADDITIVE_SCROLL) + texture)
    else:
        parameters = (base._chunk(0x10101, base._cstring(SKY_SHADER))
                      + _vector4("Emissive", EMISSIVE) + _vector4("Diffuse", (1.0, 1.0, 1.0, 1.0))
                      + _vector4("Specular", (0.0, 0.0, 0.0, 1.0)) + texture)
    geometry = (base._chunk(0x10001, struct.pack("<II", len(vertices), len(indices) // 3))
                + base._chunk(0x10002, base._cstring("alD3dVertNU2"))
                + base._chunk(0x10007, b"".join(base._vertex(*vertex) for vertex in vertices))
                + base._chunk(0x10004, struct.pack(f"<{len(indices)}H", *indices)))
    info = struct.pack("<I3f3fIII", 1, -HALF_EXTENT, -HALF_EXTENT, 0.0, HALF_EXTENT, HALF_EXTENT, HALF_EXTENT, 0, 0, 0)
    mesh = (base._chunk(0x401, base._cstring("Universe")) + base._chunk(0x402, info)
            + base._chunk(0x10100, parameters, True) + base._chunk(0x10000, geometry, True))
    connections = base._chunk(0x601, base._mini(1, struct.pack("<I", 1)) + base._mini(4, struct.pack("<I", 0)))
    connections += base._chunk(0x602, base._mini(2, struct.pack("<I", 0)) + base._mini(3, struct.pack("<I", 0)))
    return skeleton + base._chunk(0x400, mesh, True) + base._chunk(0x600, connections, True)


def sky_dds_bytes() -> bytes:
    """The scene fixture's 4 x 4 DDS header with a checker of the two colours."""
    header = base.dds_bytes(SKY_RGBA)[:128]
    texels = [SKY_RGBA if (x + y) % 2 == 0 else SKY_DARK_RGBA for y in range(4) for x in range(4)]
    return header + b"".join(bytes(texel) for texel in texels)


def objects_xml() -> str:
    return "\n".join(['<?xml version="1.0" encoding="utf-8"?>', "<GroundStructures>",
                      f'  <GroundBuildable Name="{SKY_OBJECT}">',
                      f"    <Land_Model_Name>{SKY_MODEL}</Land_Model_Name>",
                      "  </GroundBuildable>", "</GroundStructures>"]) + "\n"


def write_fixture_root(root: pathlib.Path, additive: bool = False) -> pathlib.Path:
    base.write_fixture_root(root)
    data = root / "GameData" / "Data"
    (data / "Art" / "Maps" / "EAWR_SCENE_SYNTHETIC.TED").write_bytes(ted_bytes())
    (data / "Art" / "Models" / SKY_MODEL).write_bytes(alo_bytes(additive))
    (data / "Art" / "Textures" / SKY_TEXTURE).write_bytes(sky_dds_bytes())
    base.write_effect_presence(data, (ADDITIVE_SHADER if additive else SKY_SHADER,))
    (data / "XML" / "eawr_scene_objects.xml").write_text(objects_xml(), encoding="utf-8")
    return root
