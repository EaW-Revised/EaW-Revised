"""Wholly synthetic land map with ordered attached-effect admission controls."""

from __future__ import annotations

import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "presentation" / "renderer"))
import test_effect_mode as effect_fixture  # noqa: E402
import scene_fixture  # noqa: E402
import map_effect_fixture  # noqa: E402

MAP_LOGICAL_PATH = "data/art/maps/eawr_attached_synthetic.ted"
HOST_NAME = "EAWR_ATTACHED_HOST"
PROXIES = (
    ("eawr_attached_spark_ALT0_LOD0", 1, True),
    ("eawr_attached_spark_ALT1_LOD0", 1, True),
    ("eawr_attached_spark_ALT0_LOD1", 1, True),
    ("eawr_attached_hidden", 1, False),
    ("eawr_attached_unresolved", 1, True),
    ("eawr_attached_capacity", 1, True),
)
# #288: the host yaw 90 plus the fixed model turn: the socket offset turns a half turn.
EXPECTED_EMITTER_ORIGIN = (154.0, 117.0, 60.0)
# The corpus-naming idle clip the scene builder observes for the host
# (--eawr-map-effect-animation idle only): EffectMode's 31-frame, 10 fps
# visibility script on NamedSocket, starting at its bind translation and
# moving +6 ALO X. At 1/30 s it hides on samples 18-41 and 72-89.
IDLE_CLIP_NAME = "EAWR_ATTACHED_HOST_IDLE_00.ALA"
IDLE_HIDDEN_FRAMES = effect_fixture.VISIBILITY_HIDDEN_FRAMES
IDLE_CLIP_FRAMES = effect_fixture.VISIBILITY_CLIP_FRAMES
# Short-lived residuals, so a drain started at sample 18 finishes in the run.
IDLE_EFFECT_LIFETIME = 0.5


def idle_clip(hidden_frames=IDLE_HIDDEN_FRAMES) -> bytes:
    frames = IDLE_CLIP_FRAMES
    mini, chunk = scene_fixture._mini, scene_fixture._chunk
    metadata = (mini(4, scene_fixture._cstring("NamedSocket")) + mini(5, struct.pack("<I", 1))
                + mini(6, struct.pack("<3f", 4.0, 2.0, 40.0))
                + mini(7, struct.pack("<3f", 6 / 65535, 0, 0))
                + mini(8, struct.pack("<3f", 1, 1, 1))
                + mini(9, struct.pack("<3f", 0, 0, 0)))
    translation = b"".join(struct.pack("<3H", round(65535 * index / (frames - 1)), 0, 0)
                           for index in range(frames))
    bits = bytearray((frames + 7) // 8)
    for index in range(frames):
        if index not in hidden_frames:
            bits[index // 8] |= 1 << (index % 8)
    track = chunk(0x1002, chunk(0x1003, metadata) + chunk(0x1004, translation)
                  + chunk(0x1007, bytes(bits)), True)
    info = (mini(1, struct.pack("<I", frames)) + mini(2, struct.pack("<f", 10.0))
            + mini(3, struct.pack("<I", 1)))
    return chunk(0x1000, chunk(0x1001, info) + track, True)


def _top_chunks(data: bytes):
    offset = 0
    while offset < len(data):
        kind, size = struct.unpack_from("<II", data, offset)
        length = 8 + (size & 0x7FFFFFFF)
        yield kind, data[offset:offset + length]
        offset += length
    assert offset == len(data)


def mixed_mesh_effect() -> bytes:
    """Three mesh emitters and one independent ordinary emitter."""
    def emitters(effect: bytes) -> bytes:
        root = next(raw for kind, raw in _top_chunks(effect) if kind == 0x900)
        return next(raw[8:] for kind, raw in _top_chunks(root[8:]) if kind == 0x800)

    mesh = emitters(effect_fixture.build_proxy_effect())
    ordinary = emitters(map_effect_fixture.particle_bytes("ordinary", (1.0, 0.7, 0.2)))
    root = (scene_fixture._chunk(0, b"mixed-mesh\0")
            + scene_fixture._chunk(1, struct.pack("<I", 4))
            + scene_fixture._chunk(0x800, mesh + ordinary, True)
            + scene_fixture._chunk(2, b"\x01"))
    return scene_fixture._chunk(0x900, root, True)


def host_alo(*, with_mesh: bool = True) -> bytes:
    """Host cube owned by Root, the proxies' immediate parent. Without a mesh
    owner the cube hangs from its own bone at Root's origin: Root owns no mesh,
    but the rigid cube is drawn in the same place, clear of the emitters."""
    mesh = next(raw for kind, raw in _top_chunks(scene_fixture.alo_bytes(
        [("BatchMeshGloss.fx", "eawr_scene_red.tga", 15.0, 15.0, 15.0)])) if kind == 0x400)
    bones = []
    skeleton_bones = [("Root", -1, True, (0.0, 0.0, 0.0)),
                      ("NamedSocket", 0, True, (4.0, 2.0, 40.0))]
    if not with_mesh:
        skeleton_bones.append(("MeshHolder", 0, True, (0.0, 0.0, 0.0)))
    for name, parent, visible, xyz in skeleton_bones:
        matrix = (1.0, 0.0, 0.0, xyz[0], 0.0, 1.0, 0.0, xyz[1],
                  0.0, 0.0, 1.0, xyz[2])
        record = scene_fixture._chunk(0x203, scene_fixture._cstring(name))
        record += scene_fixture._chunk(0x205, struct.pack("<iI12f", parent, int(visible), *matrix))
        bones.append(scene_fixture._chunk(0x202, record, True))
    skeleton = scene_fixture._chunk(0x200,
        scene_fixture._chunk(0x201, struct.pack("<I", len(bones))) + b"".join(bones), True)
    header = scene_fixture._mini(1, struct.pack("<I", 1))
    header += scene_fixture._mini(4, struct.pack("<I", len(PROXIES)))
    connections = scene_fixture._chunk(0x601, header)
    connections += scene_fixture._chunk(0x602,
        scene_fixture._mini(2, struct.pack("<I", 0))
        + scene_fixture._mini(3, struct.pack("<I", 0 if with_mesh else 2)))
    for name, bone, visible in PROXIES:
        data = scene_fixture._mini(5, scene_fixture._cstring(name))
        data += scene_fixture._mini(6, struct.pack("<I", bone))
        data += scene_fixture._mini(7, struct.pack("<I", 0 if visible else 1))
        connections += scene_fixture._chunk(0x603, data)
    return skeleton + mesh + scene_fixture._chunk(0x600, connections, True)


def ted_bytes() -> bytes:
    root = scene_fixture._mini(0, struct.pack("<I", 0x0201))
    root += scene_fixture._mini(1, struct.pack("<I", 1))
    width, height = scene_fixture.TERRAIN_WIDTH, scene_fixture.TERRAIN_HEIGHT
    header = (scene_fixture._mini(0, struct.pack("<I", width))
              + scene_fixture._mini(1, struct.pack("<I", height))
              + scene_fixture._mini(4, struct.pack("<I", width * height))
              + scene_fixture._mini(5, struct.pack("<I", 1)))
    material = scene_fixture._chunk(3,
        scene_fixture._mini(0x0C, scene_fixture._cstring("eawr_scene_ground.tga")))
    plane = struct.pack("<hBB", 0, 0, 255) * (width * height)
    terrain = (scene_fixture._chunk(0, header)
               + scene_fixture._chunk(2, material, True)
               + scene_fixture._chunk(5, plane))
    record = (scene_fixture._mini(0, struct.pack("<I", 500))
              + scene_fixture._mini(1, struct.pack("<I", scene_fixture.type_crc(HOST_NAME)))
              + scene_fixture._mini(4, struct.pack("<3f", 160.0, 120.0, 0.0))
              + scene_fixture._mini(5, struct.pack("<3f", 0.0, 0.0, 90.0)))
    objects = scene_fixture._chunk(1100,
        scene_fixture._chunk(1113, scene_fixture._chunk(1200, record), True), True)
    main = (scene_fixture._chunk(256, b"", True)
            + scene_fixture._chunk(257, terrain, True)
            + scene_fixture._chunk(266, plane)
            + scene_fixture._chunk(267, b"", True)
            + scene_fixture._chunk(258, scene_fixture._chunk(1, objects, True), True))
    return struct.pack("<II", 0, len(root)) + root + scene_fixture._chunk(1, main, True)


def write_fixture_root(root: pathlib.Path, *, idle: bool = False,
                       idle_hidden_frames=IDLE_HIDDEN_FRAMES, effect_start_delay=None,
                       effect_texture=None, mesh_effect: bool = False,
                       host_mesh: bool = True, mixed_effect: bool = False) -> pathlib.Path:
    data = root / "GameData" / "Data"
    maps, models, textures, xml = (data / "Art" / "Maps", data / "Art" / "Models",
                                   data / "Art" / "Textures", data / "XML")
    for directory in (maps, models, textures, xml):
        directory.mkdir(parents=True, exist_ok=True)
    (maps / "EAWR_ATTACHED_SYNTHETIC.TED").write_bytes(ted_bytes())
    (models / "EAWR_ATTACHED_HOST.ALO").write_bytes(host_alo(with_mesh=host_mesh))
    options = {"lifetime": IDLE_EFFECT_LIFETIME} if idle else {}
    if effect_start_delay is not None:
        options["start_delay"] = effect_start_delay
    if effect_texture is not None:
        options["texture"] = effect_texture
    effect = (mixed_mesh_effect() if mixed_effect else effect_fixture.build_proxy_effect() if mesh_effect else
              map_effect_fixture.particle_bytes("eawr-attached-spark", (1.0, 0.7, 0.2), **options))
    if idle:
        (models / IDLE_CLIP_NAME).write_bytes(idle_clip(idle_hidden_frames))
    for name, _, _ in PROXIES:
        if name != "eawr_attached_unresolved":
            (models / f"{name}.alo").write_bytes(effect)
    (textures / "P_Synthetic_Glow.TGA").write_bytes(effect_fixture.from_hex(effect_fixture.TEXTURE_FIXTURE))
    (textures / "eawr_scene_red.dds").write_bytes(scene_fixture.dds_bytes((220, 30, 30, 255)))
    (textures / "eawr_scene_ground.dds").write_bytes(scene_fixture.dds_bytes((30, 60, 30, 255)))
    (xml / "GameObjectFiles.xml").write_text(
        "<Game_Object_Files><File>eawr_attached_objects.xml</File></Game_Object_Files>", encoding="utf-8")
    for filename, tag in (("HardpointDataFiles.xml", "Hard_Point_Files"),
                          ("FactionFiles.xml", "Faction_Files"),
                          ("CampaignFiles.xml", "Campaign_Files"),
                          ("SFXEventFiles.xml", "SFXEvent_Files")):
        (xml / filename).write_text(f"<{tag}></{tag}>\n", encoding="utf-8")
    (xml / "eawr_attached_objects.xml").write_text(
        '<GroundStructures><GroundBuildable Name="EAWR_ATTACHED_HOST">'
        '<Land_Model_Name>eawr_attached_host.alo</Land_Model_Name>'
        '<Scale_Factor>1.5</Scale_Factor>'
        '</GroundBuildable></GroundStructures>', encoding="utf-8")
    (data / "MegaFiles.xml").write_text(
        "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
    return root
