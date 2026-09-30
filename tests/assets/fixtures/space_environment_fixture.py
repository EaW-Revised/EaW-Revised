"""Wholly original synthetic space-environment fixture for E-space-primary-sky-v1.

Every byte is generated here from invented values: a kind-2 (space) TED map
with one environment, an XML catalog declaring its invented primary sky object,
a two-surface sky ALO and two quadrant-coloured textures (a bottom-left TGA and
a BGRA DDS). Nothing is copied from, derived from or shaped like an installed
asset beyond the public chunk layouts the loaders document. Names carry an
``EAWR_SPACE`` / ``eawr_space`` prefix; the one shader identity,
``EawrSyntheticOpaqueDiffuse.fx``, is invented and is the only row the slice's
qualification table admits.

Geometry is asymmetric on purpose. Surface 0 ("SkyA") is a wall facing the
camera in the source plane y=+400, right of and above centre. Surface 1
("SkyB") is a wall in the source plane x=-300, seen obliquely on the left and
below centre. With the render camera at the origin looking along render -Z
(source +Y), a missing or doubled (x, y, z) -> (x, z, -y) conversion moves
both surfaces off their predeclared regions.

``write_fixture_root(root, variant)`` materialises ``root/GameData/Data``.
Variants: ``baseline``; ``swap`` (only surface 1's texture pixels differ);
``missing_texture`` (surface 1's texture file is absent); ``unqualified_shader``
(surface 1 names an unreviewed shader); ``hidden_bone`` (the identity bone both
meshes attach to is stored hidden); ``hidden_ancestor`` (the meshes attach to a
visible identity bone whose identity parent is stored hidden). The two hidden
variants are negative controls: the slice has no reviewed bone-visibility rule,
so neither may become an accepted visible upload. ``rigid`` adds an asymmetric
translated root and a rotated, translated child for graphical bake evidence.

MeshGloss variants (``meshgloss*``) author both surfaces as ``MeshGloss.fx``
with invented Emissive, Diffuse, Specular and Shininess values (``MESHGLOSS``)
ahead of BaseTexture, the field order the sky ledger records for the star
spheres. ``meshgloss_shininess`` changes only Shininess, ``meshgloss_nospecular``
zeroes only Specular, ``meshgloss_emissive`` changes only surface 1's Emissive,
``meshgloss_rigid`` uses the ``rigid`` bones. Negative controls:
``meshgloss_nonfinite`` (a NaN in surface 1's Emissive), ``meshgloss_missing``
(surface 1 has no Diffuse) and ``meshgloss_fxo`` (surface 1 names
``MeshGloss.fxo``, which the route never admits). The values are test inputs,
not measurements of any installed asset.

MeshAdditive variants (``meshadditive*``, P1 #27) use their own three-surface
layout (``ADDITIVE_SURFACES``), all attached to rigid non-billboard bones:
surface 0 ("Sun", ``MeshAdditive.fx``, float4 Color and UVScrollRate) is a
nearer quad that half overlaps surface 1 ("SkyA", the qualified opaque diffuse
backdrop), so the overlap measures ONE/ONE onto a non-zero destination; its
texture ``eawr_space_add.dds`` carries a different alpha per quadrant, which
must never scale rgb. Surface 2 ("Veil", ``MeshAdditive.fx``, float3 Color,
zero scroll) is two layers with identical screen projection (the far layer is
the near layer scaled by 1.25 about the eye at the origin), near layer first
in the index buffer, so its pixels are twice the fragment only when the pass
writes no depth. ``meshadditive_opaque_alpha`` makes every texel alpha 255,
``meshadditive_unused`` changes only Color.w and UVScrollRate.zw. Negative
controls: ``meshadditive_billboard`` (the Sun bone is billboard mode 7),
``meshadditive_missing`` (the Sun has no UVScrollRate) and ``meshadditive_fxo``
(the Sun names ``MeshAdditive.fxo``). Nothing is shaped like an installed sun.
"""

from __future__ import annotations

import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from scene_fixture import _chunk, _cstring, _mini, _vertex  # noqa: E402

MAP_LOGICAL_PATH = "data/art/maps/eawr_space_synthetic.ted"
SKY_OBJECT = "EAWR_SPACE_PRIMARY_SKY"
SECONDARY_SKY_OBJECT = "EAWR_SPACE_SECONDARY_SKY"
CLOUD_TEXTURE = "eawr_space_cloud.tga"
SKY_MODEL = "eawr_space_sky.alo"
QUALIFIED_SHADER = "EawrSyntheticOpaqueDiffuse.fx"
UNREVIEWED_SHADER = "EawrSyntheticUnreviewed.fx"
MESHGLOSS_SHADER = "MeshGloss.fx"
MESHGLOSS_ROUTE_ID = "eawr-space-sky-meshgloss-v1"

# Render-basis fixed camera: eye, target, up, fov, near, far.
CAMERA = "0,0,0,0,0,-1,0,1,0,60,1,5000"
# Looking the other way: both surfaces are behind the camera.
OFF_CAMERA = "0,0,0,0,0,1,0,1,0,60,1,5000"

TEXTURE_SIZE = 64
# Quadrant colours in UV order: (u<.5,v<.5), (u>=.5,v<.5), (u<.5,v>=.5), (u>=.5,v>=.5).
# v=0 is the image's top row.
QUADRANTS = {
    "eawr_space_a.tga": [(220, 40, 40), (40, 200, 60), (40, 70, 220), (230, 210, 40)],
    "eawr_space_b.dds": [(210, 50, 200), (50, 210, 210), (235, 235, 235), (240, 140, 30)],
}
SWAPPED_B = [(240, 140, 30), (235, 235, 235), (50, 210, 210), (210, 50, 200)]

# (mesh name, texture, corner at UV (0,0), u axis, v axis), all source basis.
SURFACES = [
    ("SkyA", "eawr_space_a.tga", (60.0, 400.0, 160.0), (200.0, 0.0, 0.0), (0.0, 0.0, -200.0)),
    ("SkyB", "eawr_space_b.dds", (-300.0, 300.0, 40.0), (0.0, 300.0, 0.0), (0.0, 0.0, -200.0)),
]
GRID = 4  # cells per side; even, so every triangle lies in one UV quadrant

EXPECTED = {
    "surfaces": 2,
    "declared_placements": 1,
    "not_rendered": {"secondary_sky": "declared_not_rendered", "cloud_texture": "declared_not_rendered",
                     "planet": "unexamined_blocked", "nebula": "unexamined_blocked"},
}


def type_crc(name: str) -> int:
    import zlib
    return zlib.crc32(name.upper().encode("ascii")) & 0xFFFFFFFF


# --- TED ------------------------------------------------------------------------

def ted_bytes() -> bytes:
    root = _mini(0, struct.pack("<I", 0x0201)) + _mini(1, struct.pack("<I", 2))
    environment = (_mini(0x14, _cstring("EAWR_SPACE_ENVIRONMENT")) + _mini(0x19, _cstring(SKY_OBJECT))
                   + _mini(0x1A, _cstring(SECONDARY_SKY_OBJECT)) + _mini(0x2F, _cstring(CLOUD_TEXTURE)))
    bundle = _chunk(4, _chunk(6, environment), True)
    # One gameplay-style placement of an invented, uncatalogued object: the
    # environment-only mode must count it and compose none.
    minis = (_mini(0, struct.pack("<I", 700)) + _mini(1, struct.pack("<I", type_crc("EAWR_SPACE_MARKER")))
             + _mini(4, struct.pack("<3f", 10.0, 20.0, 0.0)) + _mini(5, struct.pack("<3f", 0.0, 0.0, 0.0)))
    objects = _chunk(1100, _chunk(1113, _chunk(1200, minis), True), True)
    main = _chunk(256, bundle, True) + _chunk(258, _chunk(1, objects, True), True)
    return struct.pack("<II", 0, len(root)) + root + _chunk(1, main, True)


# --- ALO ------------------------------------------------------------------------

def _grid(origin, u_axis, v_axis):
    vertices = []
    indices = []
    normal = tuple(u_axis[(i + 1) % 3] * v_axis[(i + 2) % 3] - u_axis[(i + 2) % 3] * v_axis[(i + 1) % 3]
                   for i in range(3))
    length = sum(c * c for c in normal) ** 0.5
    normal = tuple(c / length for c in normal)
    for row in range(GRID + 1):
        for column in range(GRID + 1):
            u = column / GRID
            v = row / GRID
            position = tuple(origin[i] + u_axis[i] * u + v_axis[i] * v for i in range(3))
            vertices.append((position, normal, (u, v)))
    for row in range(GRID):
        for column in range(GRID):
            corner = row * (GRID + 1) + column
            indices += [corner, corner + 1, corner + GRID + 2, corner, corner + GRID + 2, corner + GRID + 1]
    return vertices, indices


IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)
RIGID_ROOT = (1, 0, 0, 17, 0, 1, 0, 11, 0, 0, 1, 7)
RIGID_CHILD = (0.9961947, -0.08715574, 0, -9,
               0.08715574, 0.9961947, 0, 13, 0, 0, 1, -5)


def _bone(name: str, parent: int, visible: bool, transform=IDENTITY) -> bytes:
    data = struct.pack("<iI", parent, 1 if visible else 0) + struct.pack("<12f", *transform)
    return _chunk(0x202, _chunk(0x203, _cstring(name)) + _chunk(0x205, data), True)


def _material_chunk(name: str, value) -> bytes:
    """One ALO material parameter: a float (0x10103), a float3 (0x10104) or a
    float4 (0x10106)."""
    if isinstance(value, tuple) and len(value) == 3:
        return _chunk(0x10104, _mini(1, _cstring(name)) + _mini(2, struct.pack("<3f", *value)))
    if isinstance(value, tuple):
        return _chunk(0x10106, _mini(1, _cstring(name)) + _mini(2, struct.pack("<4f", *value)))
    return _chunk(0x10103, _mini(1, _cstring(name)) + _mini(2, struct.pack("<f", value)))


def alo_bytes(shaders, bones=(("Root", -1, True),), materials=None) -> bytes:
    """``bones`` is (name, parent, visible[, transform]); meshes attach to the last.
    ``materials`` optionally gives each surface's (name, value) fields, written
    before its BaseTexture."""
    skeleton = _chunk(0x200, _chunk(0x201, struct.pack("<I", len(bones)))
                      + b"".join(_bone(*bone) for bone in bones), True)
    attach = len(bones) - 1
    meshes = b""
    for index, ((name, texture, origin, u_axis, v_axis), shader) in enumerate(zip(SURFACES, shaders)):
        vertices, indices = _grid(origin, u_axis, v_axis)
        parameters = _chunk(0x10101, _cstring(shader))
        if materials is not None:
            parameters += b"".join(_material_chunk(field, value) for field, value in materials[index])
        parameters += _chunk(0x10105, _mini(1, _cstring("BaseTexture")) + _mini(2, _cstring(texture)))
        geometry = (_chunk(0x10001, struct.pack("<II", len(vertices), len(indices) // 3))
                    + _chunk(0x10002, _cstring("alD3dVertNU2"))
                    + _chunk(0x10007, b"".join(_vertex(*vertex) for vertex in vertices))
                    + _chunk(0x10004, struct.pack(f"<{len(indices)}H", *indices)))
        points = [vertex[0] for vertex in vertices]
        low = [min(point[i] for point in points) for i in range(3)]
        high = [max(point[i] for point in points) for i in range(3)]
        info = struct.pack("<I3f3fIII", 1, *low, *high, 0, 0, 0)
        mesh = (_chunk(0x401, _cstring(name)) + _chunk(0x402, info)
                + _chunk(0x10100, parameters, True) + _chunk(0x10000, geometry, True))
        meshes += _chunk(0x400, mesh, True)
    connections = _chunk(0x601, _mini(1, struct.pack("<I", len(SURFACES))) + _mini(4, struct.pack("<I", 0)))
    for index in range(len(SURFACES)):
        connections += _chunk(0x602, _mini(2, struct.pack("<I", index)) + _mini(3, struct.pack("<I", attach)))
    return skeleton + meshes + _chunk(0x600, connections, True)


# --- textures -------------------------------------------------------------------

def quadrant_rows(colours):
    """Top-to-bottom rows of RGB tuples."""
    half = TEXTURE_SIZE // 2
    rows = []
    for y in range(TEXTURE_SIZE):
        row = []
        for x in range(TEXTURE_SIZE):
            row.append(colours[(1 if x >= half else 0) + (2 if y >= half else 0)])
        rows.append(row)
    return rows


def tga_bytes(colours) -> bytes:
    """Uncompressed 32-bit BGRA TGA with the default bottom-left origin."""
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, TEXTURE_SIZE, TEXTURE_SIZE, 32, 8)
    body = bytearray()
    for row in reversed(quadrant_rows(colours)):
        for r, g, b in row:
            body += bytes((b, g, r, 255))
    return header + bytes(body)


def dds_bgra_bytes(colours, alphas=None) -> bytes:
    """Uncompressed 32-bit DDS with BGRA channel masks (top-left origin).
    ``alphas`` optionally gives each quadrant's alpha (default 255)."""
    size = TEXTURE_SIZE
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, 0x100F, size, size, size * 4, 0, 1)
    header += b"\0" * 44
    header += struct.pack("<IIIIIIII", 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    header += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    assert len(header) == 128
    body = bytearray()
    alpha_rows = quadrant_rows([(alpha,) for alpha in alphas]) if alphas else None
    for y, row in enumerate(quadrant_rows(colours)):
        for x, (r, g, b) in enumerate(row):
            body += bytes((b, g, r, alpha_rows[y][x][0] if alpha_rows else 255))
    return header + bytes(body)


# --- XML ------------------------------------------------------------------------

def objects_xml() -> str:
    return ('<?xml version="1.0" encoding="utf-8"?>\n<SpaceProps>\n'
            f'  <SpaceProp Name="{SKY_OBJECT}">\n    <Space_Model_Name>{SKY_MODEL}</Space_Model_Name>\n'
            '  </SpaceProp>\n</SpaceProps>\n')


# Invented MeshGloss fields per surface, in the ledger's authored order.
# Values keep 2 * (Diffuse * probe irradiance + Emissive) <= 1 so neither the
# unlit nor the probe expectation clips.
MESHGLOSS = [
    [("Emissive", (0.375, 0.25, 0.25, 1.0)), ("Diffuse", (0.25, 0.5, 0.5, 1.0)),
     ("Specular", (0.75, 0.75, 0.75, 1.0)), ("Shininess", 16.0)],
    [("Emissive", (0.25, 0.25, 0.375, 1.0)), ("Diffuse", (0.5, 0.5, 1.0, 1.0)),
     ("Specular", (0.5, 0.5, 0.5, 1.0)), ("Shininess", 32.0)],
]
# meshgloss-light-probe-control (apps/viewer/src/space_environment.cpp).
PROBE_IRRADIANCE = (0.5, 0.25, 0.125)


def _replace(fields, name, value):
    return [(field, value if field == name else current) for field, current in fields]


def _without(fields, name):
    return [(field, current) for field, current in fields if field != name]


MATERIALS = {
    "meshgloss": MESHGLOSS,
    "meshgloss_rigid": MESHGLOSS,
    "meshgloss_shininess": [_replace(MESHGLOSS[0], "Shininess", 2.0), _replace(MESHGLOSS[1], "Shininess", 100.0)],
    "meshgloss_nospecular": [_replace(fields, "Specular", (0.0, 0.0, 0.0, 1.0)) for fields in MESHGLOSS],
    "meshgloss_emissive": [MESHGLOSS[0], _replace(MESHGLOSS[1], "Emissive", (0.125, 0.375, 0.25, 1.0))],
    "meshgloss_nonfinite": [MESHGLOSS[0], _replace(MESHGLOSS[1], "Emissive", (0.25, float("nan"), 0.375, 1.0))],
    "meshgloss_missing": [MESHGLOSS[0], _without(MESHGLOSS[1], "Diffuse")],
    "meshgloss_fxo": MESHGLOSS,
}


def srgb_to_linear(value: float) -> float:
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def linear_to_srgb(value: float) -> float:
    value = max(value, 0.0)
    return 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055


def meshgloss_expected(colour, fields, irradiance=(0.0, 0.0, 0.0)):
    """8-bit expectation of a specular-free MeshGloss texel under a constant
    irradiance: 2 * (Diffuse * irradiance + Emissive) * linear texel."""
    values = dict(fields)
    emissive, diffuse = values["Emissive"], values["Diffuse"]
    return tuple(255.0 * linear_to_srgb(2.0 * (diffuse[i] * irradiance[i] + emissive[i])
                                        * srgb_to_linear(colour[i] / 255.0)) for i in range(3))


VISIBLE_ROOT = (("Root", -1, True),)
BONES = {
    "baseline": VISIBLE_ROOT,
    "swap": VISIBLE_ROOT,
    "missing_texture": VISIBLE_ROOT,
    "unqualified_shader": VISIBLE_ROOT,
    "hidden_bone": (("Root", -1, False),),
    "hidden_ancestor": (("Root", -1, False), ("Sky", 0, True)),
    "rigid": (("Root", -1, True, RIGID_ROOT), ("Sky", 0, True, RIGID_CHILD)),
}
for _variant in MATERIALS:
    BONES[_variant] = BONES["rigid"] if _variant == "meshgloss_rigid" else VISIBLE_ROOT


# --- MeshAdditive (P1 #27) --------------------------------------------------------

MESHADDITIVE_SHADER = "MeshAdditive.fx"
MESHADDITIVE_ROUTE_ID = "eawr-space-sky-meshadditive-t0-v1"
MESHADDITIVE_CONTROL = "meshadditive-synthetic"
ADDITIVE_TEXTURE = "eawr_space_add.dds"
ADDITIVE_QUADRANTS = [(200, 120, 40), (40, 160, 220), (120, 220, 80), (240, 60, 180)]
# Distinct texel alpha per quadrant; with ONE/ONE none may change rgb.
ADDITIVE_ALPHAS = [255, 0, 128, 32]
SUN_COLOR = (0.5, 0.75, 1.0, 0.2)
SUN_SCROLL = (0.25, -0.5, 7.0, 9.0)
VEIL_COLOR = (0.25, 0.375, 0.5)
VEIL_SCROLL = (0.0, 0.0, 0.0, 0.0)
# (mesh name, texture, corner at UV (0,0), u axis, v axis, bone, layer scales), source basis.
ADDITIVE_SURFACES = [
    ("Sun", ADDITIVE_TEXTURE, (-60.0, 300.0, 60.0), (150.0, 0.0, 0.0), (0.0, 0.0, -150.0), 1, (1.0,)),
    ("SkyA", "eawr_space_a.tga", (60.0, 400.0, 160.0), (200.0, 0.0, 0.0), (0.0, 0.0, -200.0), 0, (1.0,)),
    ("Veil", "eawr_space_b.dds", (-300.0, 300.0, 40.0), (0.0, 300.0, 0.0), (0.0, 0.0, -200.0), 0, (1.0, 1.25)),
]
ADDITIVE_MATERIALS = {
    "meshadditive": [[("Color", SUN_COLOR), ("UVScrollRate", SUN_SCROLL)], None,
                     [("Color", VEIL_COLOR), ("UVScrollRate", VEIL_SCROLL)]],
}
ADDITIVE_MATERIALS["meshadditive_opaque_alpha"] = ADDITIVE_MATERIALS["meshadditive"]
ADDITIVE_MATERIALS["meshadditive_billboard"] = ADDITIVE_MATERIALS["meshadditive"]
ADDITIVE_MATERIALS["meshadditive_fxo"] = ADDITIVE_MATERIALS["meshadditive"]
ADDITIVE_MATERIALS["meshadditive_unused"] = [
    [("Color", (0.5, 0.75, 1.0, 0.9)), ("UVScrollRate", (0.25, -0.5, -3.0, 0.5))], None,
    ADDITIVE_MATERIALS["meshadditive"][2]]
ADDITIVE_MATERIALS["meshadditive_missing"] = [
    [("Color", SUN_COLOR)], None, ADDITIVE_MATERIALS["meshadditive"][2]]
ADDITIVE_VARIANTS = set(ADDITIVE_MATERIALS)


def _additive_bone(name: str, parent: int, billboard: int) -> bytes:
    """A visible identity bone in the version-2 (0x206) record, which carries
    the billboard mode."""
    data = struct.pack("<iII", parent, 1, billboard) + struct.pack("<12f", *IDENTITY)
    return _chunk(0x202, _chunk(0x203, _cstring(name)) + _chunk(0x206, data), True)


def additive_alo_bytes(variant: str) -> bytes:
    shaders = [MESHADDITIVE_SHADER, QUALIFIED_SHADER, MESHADDITIVE_SHADER]
    if variant == "meshadditive_fxo":
        shaders[0] = "MeshAdditive.fxo"
    billboard = 7 if variant == "meshadditive_billboard" else 0
    bones = [_additive_bone("Root", -1, 0), _additive_bone("SunBone", 0, billboard)]
    skeleton = _chunk(0x200, _chunk(0x201, struct.pack("<I", len(bones))) + b"".join(bones), True)
    meshes = b""
    for index, (name, texture, origin, u_axis, v_axis, _bone_index, layers) in enumerate(ADDITIVE_SURFACES):
        vertices = []
        indices = []
        for scale in layers:
            grid_vertices, grid_indices = _grid(tuple(c * scale for c in origin), tuple(c * scale for c in u_axis),
                                                tuple(c * scale for c in v_axis))
            indices += [len(vertices) + i for i in grid_indices]
            vertices += grid_vertices
        parameters = _chunk(0x10101, _cstring(shaders[index]))
        fields = ADDITIVE_MATERIALS[variant][index]
        if fields is not None:
            parameters += b"".join(_material_chunk(field, value) for field, value in fields)
        parameters += _chunk(0x10105, _mini(1, _cstring("BaseTexture")) + _mini(2, _cstring(texture)))
        geometry = (_chunk(0x10001, struct.pack("<II", len(vertices), len(indices) // 3))
                    + _chunk(0x10002, _cstring("alD3dVertNU2"))
                    + _chunk(0x10007, b"".join(_vertex(*vertex) for vertex in vertices))
                    + _chunk(0x10004, struct.pack(f"<{len(indices)}H", *indices)))
        points = [vertex[0] for vertex in vertices]
        low = [min(point[i] for point in points) for i in range(3)]
        high = [max(point[i] for point in points) for i in range(3)]
        info = struct.pack("<I3f3fIII", 1, *low, *high, 0, 0, 0)
        mesh = (_chunk(0x401, _cstring(name)) + _chunk(0x402, info)
                + _chunk(0x10100, parameters, True) + _chunk(0x10000, geometry, True))
        meshes += _chunk(0x400, mesh, True)
    connections = _chunk(0x601, _mini(1, struct.pack("<I", len(ADDITIVE_SURFACES))) + _mini(4, struct.pack("<I", 0)))
    for index, surface in enumerate(ADDITIVE_SURFACES):
        connections += _chunk(0x602, _mini(2, struct.pack("<I", index)) + _mini(3, struct.pack("<I", surface[5])))
    return skeleton + meshes + _chunk(0x600, connections, True)


def additive_texel_fragment(colour, color, light_scale=(1.0, 1.0, 1.0, 1.0)):
    """8-bit stored-value fragment of one MeshAdditive texel: the texel times
    saturate(Color.rgb * LIGHT_SCALE.rgb * LIGHT_SCALE.a), no sRGB decode."""
    return tuple(colour[i] * min(max(color[i] * light_scale[i] * light_scale[3], 0.0), 1.0) for i in range(3))


VARIANTS = set(BONES) | ADDITIVE_VARIANTS


def write_fixture_root(root: pathlib.Path, variant: str = "baseline") -> pathlib.Path:
    assert variant in VARIANTS, variant
    data = root / "GameData" / "Data"
    for folder in ("Art/Maps", "Art/Models", "Art/Textures", "XML"):
        (data / folder).mkdir(parents=True, exist_ok=True)
    (data / "Art" / "Maps" / "EAWR_SPACE_SYNTHETIC.TED").write_bytes(ted_bytes())
    shaders = [QUALIFIED_SHADER, UNREVIEWED_SHADER if variant == "unqualified_shader" else QUALIFIED_SHADER]
    if variant in MATERIALS:
        shaders = [MESHGLOSS_SHADER, "MeshGloss.fxo" if variant == "meshgloss_fxo" else MESHGLOSS_SHADER]
    if variant in ADDITIVE_VARIANTS:
        (data / "Art" / "Models" / SKY_MODEL).write_bytes(additive_alo_bytes(variant))
        alphas = None if variant == "meshadditive_opaque_alpha" else ADDITIVE_ALPHAS
        (data / "Art" / "Textures" / ADDITIVE_TEXTURE).write_bytes(dds_bgra_bytes(ADDITIVE_QUADRANTS, alphas))
    else:
        (data / "Art" / "Models" / SKY_MODEL).write_bytes(alo_bytes(shaders, BONES[variant], MATERIALS.get(variant)))
    (data / "Art" / "Textures" / "eawr_space_a.tga").write_bytes(tga_bytes(QUADRANTS["eawr_space_a.tga"]))
    if variant != "missing_texture":
        colours = SWAPPED_B if variant == "swap" else QUADRANTS["eawr_space_b.dds"]
        (data / "Art" / "Textures" / "eawr_space_b.dds").write_bytes(dds_bgra_bytes(colours))
    xml = data / "XML"
    (xml / "GameObjectFiles.xml").write_text(
        "<Game_Object_Files><File>eawr_space_objects.xml</File></Game_Object_Files>\n", encoding="utf-8")
    (xml / "HardpointDataFiles.xml").write_text("<Hard_Point_Files></Hard_Point_Files>\n", encoding="utf-8")
    (xml / "FactionFiles.xml").write_text("<Faction_Files></Faction_Files>\n", encoding="utf-8")
    (xml / "CampaignFiles.xml").write_text("<Campaign_Files></Campaign_Files>\n", encoding="utf-8")
    (xml / "SFXEventFiles.xml").write_text("<SFXEvent_Files></SFXEvent_Files>\n", encoding="utf-8")
    (xml / "eawr_space_objects.xml").write_text(objects_xml(), encoding="utf-8")
    (data / "MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
    return root
