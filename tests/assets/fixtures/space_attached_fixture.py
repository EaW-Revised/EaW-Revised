"""Wholly synthetic populated space map with one host whose attached effects are controlled.

The space-environment fixture supplies the kind-2 map, its sky and the tactical
camera inputs. This module adds one invented space prop (the land attached
fixture's host model, with its six named proxies) and the particle systems the
proxies name, so a populated space run plans real attached-effect records.
``effect_texture`` makes every effect's colour texture a name that never
resolves: the planned record is admitted but its effect cannot be prepared.
"""

from __future__ import annotations

import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "presentation" / "renderer"))
import map_attached_fixture as attached  # noqa: E402
import map_effect_fixture  # noqa: E402
import scene_fixture  # noqa: E402
import space_environment_fixture as space  # noqa: E402
import test_effect_mode as effect_fixture  # noqa: E402

MAP_LOGICAL_PATH = space.MAP_LOGICAL_PATH
HOST_OBJECT = "EAWR_SPACE_ATTACHED_HOST"
HOST_MODEL = "eawr_attached_host.alo"
# Selecting ALT 0 / LOD 0 admits the first proxy only (the others are hidden,
# unresolved or over the aggregate budget, as in the land fixture).
SELECTION = ("--eawr-map-effect-alt", "0", "--eawr-map-effect-lod", "0")


def ted_bytes() -> bytes:
    """The space-environment map plus one placement of the host prop."""
    root = scene_fixture._mini(0, struct.pack("<I", 0x0201)) + scene_fixture._mini(1, struct.pack("<I", 2))
    environment = (scene_fixture._mini(0x14, scene_fixture._cstring("EAWR_SPACE_ENVIRONMENT"))
                   + scene_fixture._mini(0x19, scene_fixture._cstring(space.SKY_OBJECT))
                   + scene_fixture._mini(0x1A, scene_fixture._cstring(space.SECONDARY_SKY_OBJECT))
                   + scene_fixture._mini(0x2F, scene_fixture._cstring(space.CLOUD_TEXTURE)))
    bundle = scene_fixture._chunk(4, scene_fixture._chunk(6, environment), True)
    host = (scene_fixture._mini(0, struct.pack("<I", 701))
            + scene_fixture._mini(1, struct.pack("<I", scene_fixture.type_crc(HOST_OBJECT)))
            + scene_fixture._mini(4, struct.pack("<3f", 0.0, 400.0, 0.0))
            + scene_fixture._mini(5, struct.pack("<3f", 0.0, 0.0, 0.0)))
    objects = scene_fixture._chunk(1100, scene_fixture._chunk(1113, scene_fixture._chunk(1200, host), True), True)
    main = scene_fixture._chunk(256, bundle, True) + scene_fixture._chunk(258, scene_fixture._chunk(1, objects, True), True)
    return struct.pack("<II", 0, len(root)) + root + scene_fixture._chunk(1, main, True)


def write_fixture_root(root: pathlib.Path, *, effect_texture=None) -> pathlib.Path:
    space.write_fixture_root(root, "baseline")
    data = root / "GameData" / "Data"
    models, textures, xml = data / "Art" / "Models", data / "Art" / "Textures", data / "XML"
    (data / "Art" / "Maps" / "EAWR_SPACE_SYNTHETIC.TED").write_bytes(ted_bytes())
    scene_fixture.write_effect_presence(data, ["BatchMeshGloss.fx"])
    (models / HOST_MODEL).write_bytes(attached.host_alo())
    options = {"texture": effect_texture} if effect_texture is not None else {}
    effect = map_effect_fixture.particle_bytes("eawr-space-attached-spark", (1.0, 0.7, 0.2), **options)
    for name, _, _ in attached.PROXIES:
        if name != "eawr_attached_unresolved":
            (models / f"{name}.alo").write_bytes(effect)
    (textures / "P_Synthetic_Glow.TGA").write_bytes(effect_fixture.from_hex(effect_fixture.TEXTURE_FIXTURE))
    (textures / "eawr_scene_red.dds").write_bytes(scene_fixture.dds_bytes((220, 30, 30, 255)))
    (xml / "eawr_space_objects.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<SpaceProps>\n'
        f'  <SpaceProp Name="{space.SKY_OBJECT}">\n    <Space_Model_Name>{space.SKY_MODEL}</Space_Model_Name>\n'
        '  </SpaceProp>\n'
        f'  <SpaceProp Name="{HOST_OBJECT}">\n    <Space_Model_Name>{HOST_MODEL}</Space_Model_Name>\n'
        '  </SpaceProp>\n</SpaceProps>\n', encoding="utf-8")
    return root
