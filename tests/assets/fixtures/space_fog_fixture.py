"""Wholly original synthetic fixture for the opt-in space fog join (P1-07 #28).

It extends the E-space-primary-sky-v1 fixture (``space_environment_fixture``)
with invented placements on its kind-2 map: three ``SpaceUnit`` boxes drawn
through ``BatchMeshGloss.fx``, one ``SpaceProp`` box the fog run must not admit,
and the base fixture's uncatalogued marker. Every byte is generated here from
invented values; names carry an ``EAWR_SPACE_FOG`` prefix.

Layout (TED source basis, right-handed, X right, Y forward, Z up). The fixture
camera sits at the origin looking along source +Y, so screen x follows source X
and every unit is below the horizon at z = -40..-20. The proposed synthetic
grid (``GRID``) is 3 x 2 cells of 100 x 100 from source (-150, 100):

* unit 0 at (-50, 150): x -90..-10 crosses the x = -50 edge in row 0;
* unit 1 at (50, 250): x 10..90 crosses the x = 50 edge in row 1;
* unit 2 at (200, 290): x 160..240 is outside the grid, so it samples dark;
* the prop at (-20, 180, 20) is above the horizon and must stay undrawn.

Variant ``unsupported`` gives the unit model a second ``MeshShadowVolume.fx``
surface, which no legacy selector admits, so an admitted unit cannot be
composed whole and the fog run must fail. Variant ``no_fog_material`` draws the
unit through ``MeshGloss.fx``: a legacy selector the scene accepts, but not a
fog-stub-v1 consumer, so the renderer refuses the fog declaration. Variant
``prop_no_fog_material`` does the same to the prop only: a run that also admits
``SpaceProp`` composes the three units first and then fails on the prop.
"""

from __future__ import annotations

import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import scene_fixture  # noqa: E402
import space_environment_fixture as base  # noqa: E402
from scene_fixture import _chunk, _cstring, _mini  # noqa: E402

MAP_LOGICAL_PATH = base.MAP_LOGICAL_PATH
CAMERA = base.CAMERA
UNIT_OBJECT = "EAWR_SPACE_FOG_UNIT"
PROP_OBJECT = "EAWR_SPACE_FOG_PROP"
UNIT_MODEL = "eawr_space_fog_unit.alo"
PROP_MODEL = "eawr_space_fog_prop.alo"
UNIT_TEXTURE = "eawr_space_fog_blue.tga"
PROP_TEXTURE = "eawr_space_fog_red.tga"
UNIT_HALF = (40.0, 10.0, 20.0)  # half x, half y, height

# (object id, source position, (roll, pitch, yaw) degrees). The units' yaw -90
# cancels the fixed +90 degree model turn (R-ROT-01, #288), so each unit's long
# model axis stays along source X as the layout above describes.
PLACEMENTS = [
    (UNIT_OBJECT, (-50.0, 150.0, -40.0), (0.0, 0.0, -90.0)),
    (UNIT_OBJECT, (50.0, 250.0, -40.0), (0.0, 0.0, -90.0)),
    (UNIT_OBJECT, (200.0, 290.0, -40.0), (0.0, 0.0, -90.0)),
    (PROP_OBJECT, (-20.0, 180.0, 20.0), (0.0, 0.0, 0.0)),
    ("EAWR_SPACE_MARKER", (10.0, 20.0, 0.0), (0.0, 0.0, 0.0)),
]

# Synthetic grid geometry: origin and cell size in source units.
GRID = {"origin": (-150, 100), "cell": (100, 100), "width": 3, "height": 2}

VARIANTS = {"baseline", "unsupported", "no_fog_material", "prop_no_fog_material"}


def ted_bytes() -> bytes:
    root = _mini(0, struct.pack("<I", 0x0201)) + _mini(1, struct.pack("<I", 2))
    environment = (_mini(0x14, _cstring("EAWR_SPACE_ENVIRONMENT")) + _mini(0x19, _cstring(base.SKY_OBJECT))
                   + _mini(0x1A, _cstring(base.SECONDARY_SKY_OBJECT)) + _mini(0x2F, _cstring(base.CLOUD_TEXTURE)))
    bundle = _chunk(4, _chunk(6, environment), True)
    objects = b""
    for serial, (name, position, orientation) in enumerate(PLACEMENTS, start=700):
        minis = (_mini(0, struct.pack("<I", serial)) + _mini(1, struct.pack("<I", base.type_crc(name)))
                 + _mini(4, struct.pack("<3f", *position)) + _mini(5, struct.pack("<3f", *orientation)))
        objects += _chunk(1100, _chunk(1113, _chunk(1200, minis), True), True)
    main = _chunk(256, bundle, True) + _chunk(258, _chunk(1, objects, True), True)
    return struct.pack("<II", 0, len(root)) + root + _chunk(1, main, True)


def objects_xml() -> str:
    return ('<?xml version="1.0" encoding="utf-8"?>\n<SpaceObjects>\n'
            f'  <SpaceProp Name="{base.SKY_OBJECT}">\n    <Space_Model_Name>{base.SKY_MODEL}</Space_Model_Name>\n'
            '  </SpaceProp>\n'
            f'  <SpaceUnit Name="{UNIT_OBJECT}">\n    <Space_Model_Name>{UNIT_MODEL}</Space_Model_Name>\n'
            '  </SpaceUnit>\n'
            f'  <SpaceProp Name="{PROP_OBJECT}">\n    <Space_Model_Name>{PROP_MODEL}</Space_Model_Name>\n'
            '  </SpaceProp>\n</SpaceObjects>\n')


def write_fixture_root(root: pathlib.Path, variant: str = "baseline") -> pathlib.Path:
    assert variant in VARIANTS, variant
    # The old synthetic opaque shader is deliberately unsupported by the
    # production environment route. Use the reviewed MeshGloss sky fixture.
    base.write_fixture_root(root, "meshgloss")
    data = root / "GameData" / "Data"
    (data / "Art" / "Maps" / "EAWR_SPACE_SYNTHETIC.TED").write_bytes(ted_bytes())
    shader = "MeshGloss.fx" if variant == "no_fog_material" else "BatchMeshGloss.fx"
    unit = [(shader, UNIT_TEXTURE, *UNIT_HALF)]
    if variant == "unsupported":
        unit.append(("MeshShadowVolume.fx", "", *UNIT_HALF))
    (data / "Art" / "Models" / UNIT_MODEL).write_bytes(scene_fixture.alo_bytes(unit))
    prop_shader = "MeshGloss.fx" if variant == "prop_no_fog_material" else "BatchMeshGloss.fx"
    (data / "Art" / "Models" / PROP_MODEL).write_bytes(
        scene_fixture.alo_bytes([(prop_shader, PROP_TEXTURE, 40.0, 10.0, 20.0)]))
    (data / "Art" / "Textures" / "eawr_space_fog_blue.dds").write_bytes(scene_fixture.dds_bytes((30, 60, 230, 255)))
    (data / "Art" / "Textures" / "eawr_space_fog_red.dds").write_bytes(scene_fixture.dds_bytes((230, 30, 30, 255)))
    (data / "XML" / "eawr_space_objects.xml").write_text(objects_xml(), encoding="utf-8")
    return root
