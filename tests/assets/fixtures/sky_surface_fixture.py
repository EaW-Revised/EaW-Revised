"""Wholly original synthetic sky-surface fixture for the P1-06 sky_scan ledger.

Every byte is generated here from invented values: two space TED maps and one
land TED map whose environments name invented sky objects, an XML catalog for
those objects, three sky ALO models and a handful of tiny textures. Nothing is
copied from, derived from or shaped like an installed asset beyond the public
chunk layouts the loaders document. Names carry an ``EAWR_SKY`` /
``eawr_sky`` prefix. Shader names are either the invented
``EawrSyntheticOpaqueDiffuse.fx`` (the planner's only qualification row) or
bare effect names, used only so the descriptor-family lookup has something to
match; no shader body or parameter value is modelled.

Maps (logical paths under data/art/maps):

* ``eawr_sky_space_alpha.ted`` (space): environment 0 primary ``EAWR_SKY_A``,
  secondary ``EAWR_SKY_B``; environment 1 primary ``EAWR_SKY_NOMODEL`` (object
  declares no model), secondary ``EAWR_SKY_MISSING_MODEL`` (model file absent);
  environment 2 primary ``EAWR_SKY_UNLISTED`` (not in the catalog), no
  secondary.
* ``eawr_sky_space_beta.ted`` (space): environment 0 primary ``EAWR_SKY_A``.
* ``eawr_sky_land.ted`` (land): environment 0 primary ``EAWR_SKY_C``,
  secondary ``EAWR_SKY_B``.

Models:

* ``eawr_sky_a.alo``: Dome (identity, EawrSyntheticOpaqueDiffuse.fx), Veil
  (identity, Nebula.fx, NU2C format), Sun (bone stored with a 0x206 chunk,
  billboard mode 7, MeshAdditive.fx), Ring (translated rigid child,
  MeshGloss.fx, BaseTexture authored .tga but shipped .dds).
* ``eawr_sky_b.alo``: Haze (hidden mesh, skinned, MeshAlpha.fx, NU2C), Shade
  (stored-hidden bone), Glow (scaled, non-rigid bone).
* ``eawr_sky_c.alo``: one mesh with two submeshes, MeshGloss.fx with a present
  BaseTexture and Skydome.fx with an absent BaseTexture plus a CloudTexture.

``write_fixture_root(root, variant)`` materialises ``root/GameData/Data``.
``stripped_catalog`` omits the game-object registry so the catalog is
unavailable.
"""

from __future__ import annotations

import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from scene_fixture import _chunk, _cstring, _mini, _vertex, dds_bytes  # noqa: E402

IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)
TRANSLATED = (1, 0, 0, 30, 0, 1, 0, -12, 0, 0, 1, 5)
SCALED = (2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0)

# (name, parent, visible, billboard or None for a 0x205 bone, transform)
BONES_A = [("Root", -1, True, None, IDENTITY), ("RingArm", 0, True, None, TRANSLATED),
           ("SunCard", 0, True, 7, IDENTITY)]
BONES_B = [("Root", -1, True, None, IDENTITY), ("Shut", 0, False, None, IDENTITY),
           ("Swell", 0, True, None, SCALED)]
BONES_C = [("Root", -1, True, None, IDENTITY)]

# mesh name -> (bone, visible, [submesh]); submesh = (shader, vertex format,
# [(parameter, texture)], skinned)
MODELS = {
    "eawr_sky_a.alo": (BONES_A, [
        ("Dome", 0, True, [("EawrSyntheticOpaqueDiffuse.fx", "alD3dVertNU2",
                            [("BaseTexture", "eawr_sky_dome.tga")], False)]),
        ("Veil", 0, True, [("Nebula.fx", "alD3dVertNU2C", [("BaseTexture", "eawr_sky_veil.tga")], False)]),
        ("Sun", 2, True, [("MeshAdditive.fx", "alD3dVertNU2", [("BaseTexture", "eawr_sky_sun.tga")], False)]),
        ("Ring", 1, True, [("MeshGloss.fx", "alD3dVertNU2", [("BaseTexture", "eawr_sky_ring.tga")], False)]),
    ]),
    "eawr_sky_b.alo": (BONES_B, [
        ("Haze", 0, False, [("MeshAlpha.fx", "alD3dVertNU2C", [("BaseTexture", "eawr_sky_haze.dds")], True)]),
        ("Shade", 1, True, [("MeshAlpha.fx", "alD3dVertNU2", [("BaseTexture", "eawr_sky_haze.dds")], False)]),
        ("Glow", 2, True, [("MeshAlpha.fx", "alD3dVertNU2", [("BaseTexture", "eawr_sky_haze.dds")], False)]),
    ]),
    "eawr_sky_c.alo": (BONES_C, [
        ("Dome", 0, True, [
            ("MeshGloss.fx", "alD3dVertNU2", [("BaseTexture", "eawr_sky_land_present.tga")], False),
            ("Skydome.fx", "alD3dVertNU2",
             [("BaseTexture", "eawr_sky_land_absent.tga"), ("CloudTexture", "eawr_sky_land_cloud.tga")], False),
        ]),
    ]),
}

# Texture files actually shipped (logical names under data/art/textures).
# eawr_sky_ring.tga is authored but shipped as .dds; eawr_sky_land_absent.tga
# is authored and shipped under no name.
TEXTURE_FILES = ["eawr_sky_dome.tga", "eawr_sky_veil.tga", "eawr_sky_sun.tga", "eawr_sky_ring.dds",
                 "eawr_sky_haze.dds", "eawr_sky_land_present.tga", "eawr_sky_land_cloud.tga"]

# object -> (model tag, model name) or None for an object with no model tag.
OBJECTS = {
    "EAWR_SKY_A": ("Space_Model_Name", "eawr_sky_a.alo"),
    "EAWR_SKY_B": ("Model_Name", "EAWR_SKY_B.ALO"),
    "EAWR_SKY_C": ("Land_Model_Name", "eawr_sky_c"),
    "EAWR_SKY_NOMODEL": None,
    "EAWR_SKY_MISSING_MODEL": ("Space_Model_Name", "eawr_sky_missing.alo"),
}

# map file -> (kind, [(primary, secondary)])
MAPS = {
    "EAWR_SKY_SPACE_ALPHA.TED": (2, [("EAWR_SKY_A", "EAWR_SKY_B"),
                                     ("EAWR_SKY_NOMODEL", "EAWR_SKY_MISSING_MODEL"),
                                     ("EAWR_SKY_UNLISTED", None)]),
    "EAWR_SKY_SPACE_BETA.TED": (2, [("EAWR_SKY_A", None)]),
    "EAWR_SKY_LAND.TED": (1, [("EAWR_SKY_C", "EAWR_SKY_B")]),
}

VARIANTS = {"baseline", "stripped_catalog"}


# --- TED ------------------------------------------------------------------------

def _environments(skies) -> bytes:
    records = b""
    for index, (primary, secondary) in enumerate(skies):
        minis = _mini(0x14, _cstring(f"EAWR_SKY_ENVIRONMENT_{index}")) + _mini(0x19, _cstring(primary))
        if secondary is not None:
            minis += _mini(0x1A, _cstring(secondary))
        records += _chunk(6, minis)
    return _chunk(256, _chunk(4, records, True), True)


def ted_bytes(kind: int, skies) -> bytes:
    root = _mini(0, struct.pack("<I", 0x0201)) + _mini(1, struct.pack("<I", kind))
    main = _environments(skies)
    if kind == 1:
        width = height = 3
        header = (_mini(0, struct.pack("<I", width)) + _mini(1, struct.pack("<I", height))
                  + _mini(4, struct.pack("<I", width * height)) + _mini(5, struct.pack("<I", 1)))
        material = _chunk(3, _mini(0x0C, _cstring("eawr_sky_land_present.tga")))
        plane = struct.pack("<hBB", 0, 0, 255) * (width * height)
        terrain = _chunk(0, header) + _chunk(2, material, True) + _chunk(5, plane)
        main += _chunk(257, terrain, True) + _chunk(266, plane) + _chunk(267, b"", True)
    main += _chunk(258, _chunk(1, b"", True), True)
    return struct.pack("<II", 0, len(root)) + root + _chunk(1, main, True)


# --- ALO ------------------------------------------------------------------------

QUAD = [((-1.0, 0.0, -1.0), (0.0, -1.0, 0.0), (0.0, 1.0)), ((1.0, 0.0, -1.0), (0.0, -1.0, 0.0), (1.0, 1.0)),
        ((1.0, 0.0, 1.0), (0.0, -1.0, 0.0), (1.0, 0.0)), ((-1.0, 0.0, 1.0), (0.0, -1.0, 0.0), (0.0, 0.0))]
QUAD_INDICES = [0, 1, 2, 0, 2, 3]


def _bone(name, parent, visible, billboard, transform) -> bytes:
    if billboard is None:
        data = _chunk(0x205, struct.pack("<iI", parent, 1 if visible else 0) + struct.pack("<12f", *transform))
    else:
        data = _chunk(0x206, struct.pack("<iII", parent, 1 if visible else 0, billboard)
                      + struct.pack("<12f", *transform))
    return _chunk(0x202, _chunk(0x203, _cstring(name)) + data, True)


def _submesh(shader, vertex_format, textures, skinned) -> bytes:
    parameters = _chunk(0x10101, _cstring(shader))
    for parameter, texture in textures:
        parameters += _chunk(0x10105, _mini(1, _cstring(parameter)) + _mini(2, _cstring(texture)))
    geometry = (_chunk(0x10001, struct.pack("<II", len(QUAD), len(QUAD_INDICES) // 3))
                + _chunk(0x10002, _cstring(vertex_format))
                + _chunk(0x10007, b"".join(_vertex(*vertex) for vertex in QUAD))
                + _chunk(0x10004, struct.pack(f"<{len(QUAD_INDICES)}H", *QUAD_INDICES)))
    if skinned:
        geometry += _chunk(0x10006, struct.pack("<I", 0))
    return _chunk(0x10100, parameters, True) + _chunk(0x10000, geometry, True)


def alo_bytes(bones, meshes) -> bytes:
    skeleton = _chunk(0x200, _chunk(0x201, struct.pack("<I", len(bones))) + b"".join(_bone(*b) for b in bones), True)
    body = b""
    for name, _bone_index, visible, submeshes in meshes:
        # The stored flag is "hidden": 0 means visible.
        info = struct.pack("<I3f3fIII", len(submeshes), -1.0, 0.0, -1.0, 1.0, 0.0, 1.0, 0, 0 if visible else 1, 0)
        mesh = _chunk(0x401, _cstring(name)) + _chunk(0x402, info)
        for submesh in submeshes:
            mesh += _submesh(*submesh)
        body += _chunk(0x400, mesh, True)
    connections = _chunk(0x601, _mini(1, struct.pack("<I", len(meshes))) + _mini(4, struct.pack("<I", 0)))
    for index, (_name, bone_index, _visible, _submeshes) in enumerate(meshes):
        connections += _chunk(0x602, _mini(2, struct.pack("<I", index)) + _mini(3, struct.pack("<I", bone_index)))
    return skeleton + body + _chunk(0x600, connections, True)


def tga_bytes() -> bytes:
    """A 2x2 uncompressed 32-bit TGA of one invented colour."""
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 2, 2, 32, 8)
    return header + bytes((40, 80, 160, 255)) * 4


# --- XML ------------------------------------------------------------------------

def objects_xml() -> str:
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<SpaceProps>"]
    for name, model in OBJECTS.items():
        lines.append(f'  <SpaceProp Name="{name}">')
        if model is None:
            lines.append("    <Scale_Factor>1.0</Scale_Factor>")
        else:
            lines.append(f"    <{model[0]}>{model[1]}</{model[0]}>")
        lines.append("  </SpaceProp>")
    lines.append("</SpaceProps>")
    return "\n".join(lines) + "\n"


def write_fixture_root(root: pathlib.Path, variant: str = "baseline") -> pathlib.Path:
    assert variant in VARIANTS, variant
    data = root / "GameData" / "Data"
    for folder in ("Art/Maps", "Art/Models", "Art/Textures", "XML"):
        (data / folder).mkdir(parents=True, exist_ok=True)
    for name, (kind, skies) in MAPS.items():
        (data / "Art" / "Maps" / name).write_bytes(ted_bytes(kind, skies))
    for name, (bones, meshes) in MODELS.items():
        (data / "Art" / "Models" / name).write_bytes(alo_bytes(bones, meshes))
    for name in TEXTURE_FILES:
        payload = dds_bytes((40, 80, 160, 255)) if name.endswith(".dds") else tga_bytes()
        (data / "Art" / "Textures" / name).write_bytes(payload)
    xml = data / "XML"
    if variant != "stripped_catalog":
        (xml / "GameObjectFiles.xml").write_text(
            "<Game_Object_Files><File>eawr_sky_objects.xml</File></Game_Object_Files>\n", encoding="utf-8")
        (xml / "eawr_sky_objects.xml").write_text(objects_xml(), encoding="utf-8")
    (xml / "HardpointDataFiles.xml").write_text("<Hard_Point_Files></Hard_Point_Files>\n", encoding="utf-8")
    (xml / "FactionFiles.xml").write_text("<Faction_Files></Faction_Files>\n", encoding="utf-8")
    (xml / "CampaignFiles.xml").write_text("<Campaign_Files></Campaign_Files>\n", encoding="utf-8")
    (xml / "SFXEventFiles.xml").write_text("<SFXEvent_Files></SFXEvent_Files>\n", encoding="utf-8")
    (data / "MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
    return root
