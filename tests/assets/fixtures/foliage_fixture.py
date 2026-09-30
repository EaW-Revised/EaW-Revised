"""Wholly original synthetic foliage fixture for the #147 wind sway.

The scene fixture's flat land map (tests/assets/fixtures/scene_fixture.py)
with one environment record that carries a strong wind, one Tree.fx model (a
tall box standing on z = 0) and one Grass.fx model (a low box whose v runs
from 1 at its base to 0 at its top). Boxes, not cards, so the top-down fixed
capture sees their tops move. Every value is invented
here; object and file names carry an ``EAWR_FOLIAGE`` / ``eawr_foliage`` prefix.

``write_fixture_root(root)`` materialises ``root/GameData/Data`` for
``--eawr-game-root root``. The map keeps the scene fixture's logical path, so
the synthetic map camera config binds to it once its hash is rewritten.
"""

from __future__ import annotations

import pathlib
import struct

import scene_fixture as base

MAP_LOGICAL_PATH = base.MAP_LOGICAL_PATH

# R-WX-01: heading 0 blows along +X. Strong, so the sway is many pixels wide.
WIND_HEADING_DEGREES = 0.0
WIND_SPEED = 20.0

TREE_HEIGHT = 120.0
TREE_BEND_SCALE = 1.0
GRASS_HEIGHT = 40.0
GRASS_BEND_SCALE = 0.5

OBJECTS = {
    "EAWR_FOLIAGE_TREE": "eawr_foliage_tree.alo",
    "EAWR_FOLIAGE_GRASS": "eawr_foliage_grass.alo",
}

# (object id, source position, (roll, pitch, yaw) degrees)
PLACEMENTS = [
    ("EAWR_FOLIAGE_TREE", (220.0, 240.0, 0.0), (0.0, 0.0, 0.0)),
    ("EAWR_FOLIAGE_GRASS", (420.0, 240.0, 0.0), (0.0, 0.0, 0.0)),
]

TEXTURES = {
    "eawr_foliage_leaf.dds": (40, 200, 60, 255),
    "eawr_foliage_blade.dds": (250, 150, 30, 255),
    "eawr_scene_ground.dds": base.TEXTURES["eawr_scene_ground.dds"],
}


def environment_minis() -> bytes:
    """One environment record: the fourteen lighting minis the candidate
    mapping reads (colours, intensities, headings and tilts in radians), a
    name, and the wind heading (0x2b, degrees) and speed (0x2c)."""
    colours = [(1.0, 0.95, 0.9), (0.3, 0.3, 0.4), (0.2, 0.25, 0.2), (1.0, 1.0, 1.0), (0.35, 0.35, 0.35)]
    scalars = [1.0, 0.4, 0.3, 0.6, 2.2, 4.0, 0.9, 0.3, 0.2]
    minis = b""
    for index, colour in enumerate(colours):
        minis += base._mini(index, struct.pack("<3f", *colour))
    for index, value in enumerate(scalars, start=5):
        minis += base._mini(index, struct.pack("<f", value))
    minis += base._mini(0x14, base._cstring("EAWR_Foliage_Windy"))
    minis += base._mini(0x2B, struct.pack("<f", WIND_HEADING_DEGREES))
    minis += base._mini(0x2C, struct.pack("<f", WIND_SPEED))
    return minis


def ted_bytes() -> bytes:
    """The scene fixture's flat map and chunk layout with this fixture's
    placements and one 1/256/4/6 environment record."""
    width, height = base.TERRAIN_WIDTH, base.TERRAIN_HEIGHT
    root = base._mini(0, struct.pack("<I", 0x0201)) + base._mini(1, struct.pack("<I", 1))
    header = (base._mini(0, struct.pack("<I", width)) + base._mini(1, struct.pack("<I", height))
              + base._mini(4, struct.pack("<I", width * height)) + base._mini(5, struct.pack("<I", 1)))
    material = base._chunk(3, base._mini(0x0C, base._cstring("eawr_scene_ground.tga")))
    plane = struct.pack("<hBB", 0, 0, 255) * (width * height)
    terrain = base._chunk(0, header) + base._chunk(2, material, True) + base._chunk(5, plane)
    objects = b""
    for serial, (name, position, orientation) in enumerate(PLACEMENTS, start=500):
        minis = (base._mini(0, struct.pack("<I", serial)) + base._mini(1, struct.pack("<I", base.type_crc(name)))
                 + base._mini(4, struct.pack("<3f", *position)) + base._mini(5, struct.pack("<3f", *orientation)))
        objects += base._chunk(1100, base._chunk(1113, base._chunk(1200, minis), True), True)
    environments = base._chunk(256, base._chunk(4, base._chunk(6, environment_minis()), True), True)
    main = (environments + base._chunk(257, terrain, True) + base._chunk(266, plane)
            + base._chunk(267, b"", True) + base._chunk(258, base._chunk(1, objects, True), True))
    return struct.pack("<II", 0, len(root)) + root + base._chunk(1, main, True)


def _prism(half_x: float, half_y: float, height: float):
    """A closed box standing on z = 0, each face in both windings, with v = 1
    at z = 0 and v = 0 at z = height (the top face is all v = 0)."""
    bottom = [(-half_x, -half_y), (half_x, -half_y), (half_x, half_y), (-half_x, half_y)]
    faces = []
    for index in range(4):
        (x0, y0), (x1, y1) = bottom[index], bottom[(index + 1) % 4]
        length = ((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5
        normal = ((y1 - y0) / length, (x0 - x1) / length, 0.0)
        faces.append(([(x0, y0, 0.0), (x1, y1, 0.0), (x1, y1, height), (x0, y0, height)],
                      [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)], normal))
    faces.append(([(x, y, height) for x, y in bottom], [(0.0, 0.0)] * 4, (0.0, 0.0, 1.0)))
    faces.append(([(x, y, 0.0) for x, y in bottom], [(0.0, 1.0)] * 4, (0.0, 0.0, -1.0)))
    vertices, indices = [], []
    for points, uvs, normal in faces:
        base_index = len(vertices)
        vertices += [(point, normal, uv) for point, uv in zip(points, uvs)]
        indices += [base_index, base_index + 1, base_index + 2, base_index, base_index + 2, base_index + 3]
        indices += [base_index, base_index + 2, base_index + 1, base_index, base_index + 3, base_index + 2]
    return vertices, indices


def alo_bytes(shader: str, texture: str, bend_scale: float, half_x: float, half_y: float, height: float) -> bytes:
    bone_data = struct.pack("<iI", -1, 1) + struct.pack("<12f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)
    bone = base._chunk(0x202, base._chunk(0x203, base._cstring("Root")) + base._chunk(0x205, bone_data), True)
    skeleton = base._chunk(0x200, base._chunk(0x201, struct.pack("<I", 1)) + bone, True)
    vertices, indices = _prism(half_x, half_y, height)
    parameters = (base._chunk(0x10101, base._cstring(shader))
                  + base._chunk(0x10103, base._mini(1, base._cstring("BendScale"))
                                + base._mini(2, struct.pack("<f", bend_scale)))
                  + base._chunk(0x10105, base._mini(1, base._cstring("BaseTexture"))
                                + base._mini(2, base._cstring(texture))))
    geometry = (base._chunk(0x10001, struct.pack("<II", len(vertices), len(indices) // 3))
                + base._chunk(0x10002, base._cstring("alD3dVertNU2"))
                + base._chunk(0x10007, b"".join(base._vertex(*vertex) for vertex in vertices))
                + base._chunk(0x10004, struct.pack(f"<{len(indices)}H", *indices)))
    info = struct.pack("<I3f3fIII", 1, -half_x, -half_y, 0.0, half_x, half_y, height, 0, 0, 0)
    mesh = (base._chunk(0x401, base._cstring("Foliage")) + base._chunk(0x402, info)
            + base._chunk(0x10100, parameters, True) + base._chunk(0x10000, geometry, True))
    connections = base._chunk(0x601, base._mini(1, struct.pack("<I", 1)) + base._mini(4, struct.pack("<I", 0)))
    connections += base._chunk(0x602, base._mini(2, struct.pack("<I", 0)) + base._mini(3, struct.pack("<I", 0)))
    return skeleton + base._chunk(0x400, mesh, True) + base._chunk(0x600, connections, True)


def objects_xml() -> str:
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<GroundStructures>"]
    for name, model in OBJECTS.items():
        lines.append(f'  <GroundBuildable Name="{name}">')
        lines.append(f"    <Land_Model_Name>{model}</Land_Model_Name>")
        lines.append("  </GroundBuildable>")
    lines.append("</GroundStructures>")
    return "\n".join(lines) + "\n"


def write_fixture_root(root: pathlib.Path) -> pathlib.Path:
    base.write_fixture_root(root)
    data = root / "GameData" / "Data"
    (data / "Art" / "Maps" / "EAWR_SCENE_SYNTHETIC.TED").write_bytes(ted_bytes())
    models = data / "Art" / "Models"
    (models / OBJECTS["EAWR_FOLIAGE_TREE"]).write_bytes(
        alo_bytes("Tree.fx", "eawr_foliage_leaf.tga", TREE_BEND_SCALE, 8.0, 8.0, TREE_HEIGHT))
    (models / OBJECTS["EAWR_FOLIAGE_GRASS"]).write_bytes(
        alo_bytes("Grass.fx", "eawr_foliage_blade.tga", GRASS_BEND_SCALE, 30.0, 10.0, GRASS_HEIGHT))
    for name, rgba in TEXTURES.items():
        (data / "Art" / "Textures" / name).write_bytes(base.dds_bytes(rgba))
    (data / "XML" / "eawr_scene_objects.xml").write_text(objects_xml(), encoding="utf-8")
    return root
