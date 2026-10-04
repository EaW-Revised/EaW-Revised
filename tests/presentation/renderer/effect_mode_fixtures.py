"""Wholly original effect-mode fixtures and the graphical run helper.

Shared by test_effect_mode.py (structural contracts, fixture regeneration with
--write-fixtures) and test_effect_mode_capture.py (opt-in graphical runs).
"""

import json
import os
import pathlib
import re
import struct
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[3]
FIXTURES = ROOT / "tests/presentation/particles/fixtures"
EFFECT_FIXTURE = FIXTURES / "synthetic-effect.alo.hex"
TEXTURE_FIXTURE = FIXTURES / "synthetic-glow.tga.hex"
INVENTORY = ROOT / "plan/inventories/particle-render-families.json"
LEDGER = ROOT / "plan/inventories/particle-plugins.json"
EFFECT_LOGICAL_PATH = "data/art/models/p_synthetic_effect.alo"
PARENT_EFFECT_LOGICAL_PATH = "data/art/models/p_synthetic_parent.alo"
RELEASE_EFFECT_LOGICAL_PATH = "data/art/models/p_synthetic_parent_release.alo"
PROXY_EFFECT_LOGICAL_PATH = "data/art/models/p_synthetic_proxy_effect.alo"
PROXY_HOST_LOGICAL_PATH = "data/art/models/p_synthetic_proxy_host.alo"
PROXY_ANIMATION_LOGICAL_PATH = "data/art/models/p_synthetic_proxy_host.ala"
VISIBILITY_ANIMATION_LOGICAL_PATH = "data/art/models/p_synthetic_visibility_host.ala"
VISIBILITY_ATTACH = PROXY_HOST_LOGICAL_PATH + ":socket"

FORBIDDEN = re.compile(r"[A-Za-z]:[\\/]|SteamLibrary|steamapps|workshop", re.IGNORECASE)


# --- Wholly original fixture generation --------------------------------------
#
# The effect is a legacy (0x900) particle system written from the chunk layout
# the CPU parser accepts. Every value below is invented for this test.

def _chunk(kind: int, payload: bytes, group: bool = False) -> bytes:
    return struct.pack("<II", kind, len(payload) | (0x80000000 if group else 0)) + payload


def _mini(kind: int, payload: bytes) -> bytes:
    return struct.pack("<BB", kind, len(payload)) + payload


def _text(value: str) -> bytes:
    return value.encode("ascii") + b"\0"


def _group(shape: int, *, low=(0.0, 0.0, 0.0), high=(0.0, 0.0, 0.0), sphere=0.0,
           point=(0.0, 0.0, 0.0)) -> bytes:
    data = struct.pack("<I3f3f", shape, *low, *high)
    data += struct.pack("<ffIfIf", 0.0, sphere, 0, 0.0, 0, 0.0)
    data += struct.pack("<3f", *point)
    assert len(data) == 64
    return _chunk(0x1100, _chunk(0x1101, data), True)


def _track(first: float, last: float, colour: bool, keys=()) -> bytes:
    if colour:
        header = _mini(2, bytes([round(first * 255)])) + _mini(3, bytes([round(last * 255)]))
    else:
        header = _mini(2, struct.pack("<f", first)) + _mini(3, struct.pack("<f", last))
    header += _mini(4, struct.pack("<I", 0))
    body = b"".join(
        _mini(5, struct.pack("<If", round(value * 255), time) if colour
              else struct.pack("<ff", value, time))
        for time, value in keys)
    return _chunk(0, header) + _chunk(1, body)


def _emitter(name: str, *, blend: int, texture: str, position, velocity, rgb,
             size=(2.0, 5.0), heat=False, world=False, tail=False, tail_size=0.0,
             rate=24, lifetime=1.2, birth_child=None, death_child=None,
             start_delay=None) -> bytes:
    properties = b"".join([
        _mini(0x04, struct.pack("<I", blend)),
        _mini(0x0F, struct.pack("<f", lifetime)),
        _mini(0x2A, struct.pack("<I", rate)),
        _mini(0x10, struct.pack("<I", 1)),
        _mini(0x3B, bytes([1 if heat else 0])),
        _mini(0x2E, bytes([1 if world else 0])),
        _mini(0x41, bytes([1 if tail else 0])),
        _mini(0x42, struct.pack("<f", tail_size)),
        _mini(0x46, bytes([0])),
    ])
    if start_delay is not None:
        # Legacy emitter start delay (seconds); absent unless requested.
        properties += _mini(0x24, struct.pack("<f", start_delay))
    groups = velocity + _group(0, point=(lifetime, 0.0, 0.0)) + position
    tracks = b"".join([
        _track(rgb[0], rgb[0], True), _track(rgb[1], rgb[1], True),
        _track(rgb[2], rgb[2], True), _track(1.0, 0.0, True, keys=((0.5, 0.8),)),
        _track(size[0], size[1], False), _track(0.0, 0.0, False), _track(0.1, 0.1, False),
    ])
    body = (_chunk(2, properties) + _chunk(0x03, _text(texture)) + _chunk(0x16, _text(name))
            + _chunk(0x29, groups, True) + _chunk(1, tracks, True))
    if birth_child is not None or death_child is not None:
        links = (_mini(0x37, struct.pack("<I", death_child if death_child is not None else 0xFFFFFFFF))
                 + _mini(0x39, struct.pack("<I", birth_child if birth_child is not None else 0xFFFFFFFF)))
        body += _chunk(0x36, links)
    return _chunk(0x700, body, True)


def build_effect() -> bytes:
    glow = "p_synthetic_glow.tga"
    emitters = b"".join([
        _emitter("billboard-additive", blend=1, texture=glow,
                 position=_group(0, point=(-6.0, 0.0, 0.0)),
                 velocity=_group(3, sphere=3.0), rgb=(1.0, 0.55, 0.2)),
        _emitter("xy-alpha", blend=2, texture=glow, world=True,
                 position=_group(0, point=(6.0, 0.0, 0.0)),
                 velocity=_group(3, sphere=2.5), rgb=(0.3, 0.7, 1.0), size=(3.0, 6.0)),
        _emitter("kite-additive", blend=1, texture=glow, tail=True, tail_size=3.0,
                 position=_group(0, point=(0.0, 0.0, -3.0)),
                 velocity=_group(1, low=(-1.5, -1.5, 5.0), high=(1.5, 1.5, 8.0)),
                 rgb=(0.9, 1.0, 0.5), size=(1.5, 1.0)),
        _emitter("heat", blend=10, texture=glow, heat=True,
                 position=_group(0, point=(0.0, 0.0, 3.0)),
                 velocity=_group(3, sphere=1.0), rgb=(1.0, 1.0, 1.0), rate=8),
        _emitter("depth-sprite", blend=5, texture=glow,
                 position=_group(0, point=(0.0, 0.0, 0.0)),
                 velocity=_group(3, sphere=1.0), rgb=(1.0, 1.0, 1.0), rate=8),
    ])
    root = (_chunk(0, _text("synthetic-effect")) + _chunk(1, struct.pack("<I", 5))
            + _chunk(0x800, emitters, True) + _chunk(2, b"\x01"))
    return _chunk(0x900, root, True)


def build_parent_effect(leave_particles: bool = True) -> bytes:
    """Wholly original V1 parent, birth trail, and one-shot death burst.

    `leave_particles` is the system flag (root chunk 2): on detach, set drains
    the live particles and child chains, clear releases the instance at once.
    """
    glow = "p_synthetic_glow.tga"
    emitters = b"".join([
        _emitter("parent", blend=1, texture=glow, rate=1, lifetime=0.4,
                 birth_child=1, death_child=2,
                 position=_group(0, point=(-3.0, 0.0, 0.0)),
                 velocity=_group(0, point=(3.0, 0.0, 0.0)), rgb=(1.0, 0.9, 0.2)),
        _emitter("birth-trail", blend=1, texture=glow, rate=12, lifetime=1.0,
                 position=_group(0, point=(0.0, 0.0, 0.0)),
                 velocity=_group(0, point=(0.0, 0.0, 0.0)), rgb=(0.2, 0.7, 1.0)),
        _emitter("death-burst", blend=1, texture=glow, rate=5, lifetime=1.2,
                 position=_group(0, point=(1.0, 0.0, 0.0)),
                 velocity=_group(3, sphere=3.0), rgb=(1.0, 0.3, 0.1)),
    ])
    root = (_chunk(0, _text("synthetic-parent-effect")) + _chunk(1, struct.pack("<I", 3))
            + _chunk(0x800, emitters, True) + _chunk(2, b"\x01" if leave_particles else b"\x00"))
    return _chunk(0x900, root, True)


def build_proxy_effect() -> bytes:
    """One V1 emitter per mesh mode, with a burst every 0.2 source seconds."""
    emitters = []
    for mode, colour in ((1, (1.0, 0.4, 0.2)), (2, (0.2, 1.0, 0.4)),
                         (3, (0.3, 0.5, 1.0))):
        original = _emitter(f"mesh-{mode}", blend=1, texture="p_synthetic_glow.tga",
                            position=_group(0), velocity=_group(0), rgb=colour,
                            size=(1.5, 1.5), rate=1, lifetime=1.5)
        # Insert source mini-chunks into the first properties chunk of each emitter.
        body = original[8:]
        assert struct.unpack_from("<I", body)[0] == 2
        length = struct.unpack_from("<I", body, 4)[0]
        additions = b"".join((
            _mini(0x34, struct.pack("<I", mode)),
            _mini(0x3C, struct.pack("<f", 0.1)),
            _mini(0x07, b"\x01"),
            _mini(0x25, struct.pack("<f", 0.2)),
            _mini(0x26, struct.pack("<I", 1)),
            _mini(0x27, struct.pack("<I", 5)),
        ))
        body = _chunk(2, body[8:8 + length] + additions) + body[8 + length:]
        emitters.append(_chunk(0x700, body, True))
    root = (_chunk(0, _text("synthetic-proxy-effect")) + _chunk(1, struct.pack("<I", 3))
            + _chunk(0x800, b"".join(emitters), True) + _chunk(2, b"\x01"))
    return _chunk(0x900, root, True)


def build_proxy_host(*, duplicate_proxy=False) -> bytes:
    """Original asymmetric ALO: decoy mesh, two ordered owner submeshes, proxy child."""
    bones = []
    for name, parent, xyz in (("root", -1, (0.0, 0.0, 0.0)),
                              ("owner", 0, (2.0, -1.0, 1.0)),
                              ("socket", 1, (0.0, 0.0, 2.0))):
        matrix = (1.0, 0.0, 0.0, xyz[0], 0.0, 1.0, 0.0, xyz[1],
                  0.0, 0.0, 1.0, xyz[2])
        data = struct.pack("<iI12f", parent, 1, *matrix)
        bones.append(_chunk(0x202, _chunk(0x203, _text(name))
                            + _chunk(0x205, data), True))
    skeleton = _chunk(0x200, _chunk(0x201, struct.pack("<I", len(bones)))
                      + b"".join(bones), True)

    def vertex(position, normal):
        return struct.pack("<3f3f8f3f3f4f4I4f", *position, *normal,
                           *([0.0] * 8), *([0.0] * 3), *([0.0] * 3),
                           1.0, 1.0, 1.0, 1.0, 0, 0, 0, 0,
                           0.0, 0.0, 0.0, 0.0)

    def submesh(points, normal):
        vertices = b"".join(vertex(point, normal) for point in points)
        return (_chunk(0x10100, _chunk(0x10101, _text("synthetic")), True)
                + _chunk(0x10000,
                         _chunk(0x10001, struct.pack("<II", 3, 1))
                         + _chunk(0x10002, _text("original"))
                         + _chunk(0x10005, vertices)
                         + _chunk(0x10004, struct.pack("<3H", 0, 1, 2)), True))

    def mesh(name, pieces):
        info = struct.pack("<I3f3fIII", len(pieces), -10, -10, -10,
                           10, 10, 10, 0, 0, 0)
        return _chunk(0x400, _chunk(0x401, _text(name)) + _chunk(0x402, info)
                      + b"".join(pieces), True)

    decoy = mesh("decoy", [submesh(((40, 0, 0), (41, 0, 0), (40, 1, 0)), (0, 0, 1))])
    owner = mesh("first-owner", [
        submesh(((-1, -1, 0), (1, -1, 0), (0, 1, 0)), (0, 0, 1)),
        submesh(((2, 0, 1), (3, 0, 1), (2, 1, 1)), (0, 1, 0)),
    ])
    later = mesh("later-owner", [submesh(((60, 0, 0), (61, 0, 0), (60, 1, 0)), (0, 0, 1))])
    header = _mini(1, struct.pack("<I", 3)) + _mini(4, struct.pack("<I", 2 if duplicate_proxy else 1))
    connections = [_chunk(0x601, header)]
    for object_index, bone_index in ((0, 0), (1, 1), (2, 1)):
        connections.append(_chunk(0x602, _mini(2, struct.pack("<I", object_index))
                                  + _mini(3, struct.pack("<I", bone_index))))
    proxy = _chunk(0x603, _mini(5, _text("spark")) + _mini(6, struct.pack("<I", 2)))
    connections.append(proxy)
    if duplicate_proxy:
        connections.append(proxy)
    return skeleton + decoy + owner + later + _chunk(0x600, b"".join(connections), True)


def build_proxy_animation() -> bytes:
    metadata = (_mini(4, _text("owner")) + _mini(5, struct.pack("<I", 1))
                + _mini(6, struct.pack("<3f", 0, 0, 0))
                + _mini(7, struct.pack("<3f", 4 / 65535, 0, 0))
                + _mini(8, struct.pack("<3f", 1, 1, 1))
                + _mini(9, struct.pack("<3f", 0, 0, 0)))
    track = _chunk(0x1002, _chunk(0x1003, metadata)
                   + _chunk(0x1004, struct.pack("<3H3H", 0, 0, 0, 65535, 0, 0)), True)
    info = (_mini(1, struct.pack("<I", 2)) + _mini(2, struct.pack("<f", 1.0))
            + _mini(3, struct.pack("<I", 1)))
    return _chunk(0x1000, _chunk(0x1001, info) + track, True)


# Visibility clip: 31 stored frames at 10 fps (3.0 s). The socket travels +6
# ALO X over the clip and is visible on frames 0-5 and 14-23, hidden on 6-13
# and 24-30, so a 90-frame run at 1/30 s hides, reappears and hides again.
VISIBILITY_CLIP_FRAMES = 31
VISIBILITY_HIDDEN_FRAMES = tuple(range(6, 14)) + tuple(range(24, 31))


def build_visibility_animation() -> bytes:
    frames = VISIBILITY_CLIP_FRAMES
    metadata = (_mini(4, _text("socket")) + _mini(5, struct.pack("<I", 2))
                + _mini(6, struct.pack("<3f", 0, 0, 0))
                + _mini(7, struct.pack("<3f", 6 / 65535, 0, 0))
                + _mini(8, struct.pack("<3f", 1, 1, 1))
                + _mini(9, struct.pack("<3f", 0, 0, 0)))
    translation = b"".join(struct.pack("<3H", round(65535 * index / (frames - 1)), 0, 0)
                           for index in range(frames))
    bits = bytearray((frames + 7) // 8)
    for index in range(frames):
        if index not in VISIBILITY_HIDDEN_FRAMES:
            bits[index // 8] |= 1 << (index % 8)
    track = _chunk(0x1002, _chunk(0x1003, metadata) + _chunk(0x1004, translation)
                   + _chunk(0x1007, bytes(bits)), True)
    info = (_mini(1, struct.pack("<I", frames)) + _mini(2, struct.pack("<f", 10.0))
            + _mini(3, struct.pack("<I", 1)))
    return _chunk(0x1000, _chunk(0x1001, info) + track, True)


def build_glow() -> bytes:
    """A 16x16 32-bit TGA with a radial falloff, stored top-left first."""
    size = 16
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, size, size, 32, 0x28)
    pixels = bytearray()
    for y in range(size):
        for x in range(size):
            dx = (x + 0.5) / size * 2.0 - 1.0
            dy = (y + 0.5) / size * 2.0 - 1.0
            weight = max(0.0, 1.0 - (dx * dx + dy * dy) ** 0.5)
            value = round(255 * weight)
            pixels += bytes([value, value, value, value])  # B, G, R, A
    return header + bytes(pixels)


def to_hex(data: bytes, title: str) -> str:
    lines = [f"# {title}"]
    for offset in range(0, len(data), 32):
        lines.append(" ".join(f"{value:02x}" for value in data[offset:offset + 32]))
    return "\n".join(lines) + "\n"


def from_hex(path: pathlib.Path) -> bytes:
    data = bytearray()
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        data.extend(int(token, 16) for token in line.split())
    return bytes(data)


EFFECT_TITLE = "synthetic particle effect, wholly original, generated by test_effect_mode.py"
GLOW_TITLE = "synthetic 16x16 radial glow TGA, wholly original, generated by test_effect_mode.py"


def install_fixture(root: pathlib.Path) -> pathlib.Path:
    data = root / "GameData" / "Data"
    (data / "Art" / "Models").mkdir(parents=True)
    (data / "Art" / "Textures").mkdir(parents=True)
    (data / "Art" / "Models" / "P_Synthetic_Effect.ALO").write_bytes(from_hex(EFFECT_FIXTURE))
    (data / "Art" / "Models" / "P_Synthetic_Parent.ALO").write_bytes(build_parent_effect())
    (data / "Art" / "Models" / "P_Synthetic_Parent_Release.ALO").write_bytes(
        build_parent_effect(leave_particles=False))
    (data / "Art" / "Models" / "P_Synthetic_Proxy_Effect.ALO").write_bytes(build_proxy_effect())
    (data / "Art" / "Models" / "P_Synthetic_Proxy_Host.ALO").write_bytes(build_proxy_host())
    (data / "Art" / "Models" / "P_Synthetic_Proxy_Host.ALA").write_bytes(build_proxy_animation())
    (data / "Art" / "Models" / "P_Synthetic_Visibility_Host.ALA").write_bytes(build_visibility_animation())
    (data / "Art" / "Textures" / "P_Synthetic_Glow.TGA").write_bytes(from_hex(TEXTURE_FIXTURE))
    (data / "MegaFiles.xml").write_text(
        "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
    return root



# --- Graphical -------------------------------------------------------------------

DETACH_FRAME = 6


def _capture_path(name: str, temporary: pathlib.Path) -> pathlib.Path:
    """Retained under EAWR_EFFECT_CAPTURE_DIR (keep it in the ignored out/ tree)."""
    retained = os.environ.get("EAWR_EFFECT_CAPTURE_DIR")
    directory = pathlib.Path(retained) if retained else temporary
    directory.mkdir(parents=True, exist_ok=True)
    return directory / f"{name}.png"


def _run(game_root: pathlib.Path, logical_path: str, report: pathlib.Path,
         mod_root: str | None = None, extra=()) -> tuple[dict, subprocess.CompletedProcess]:
    executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
    assert executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary"
    command = [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-effect", logical_path, "--eawr-game-root", str(game_root),
               "--eawr-report", str(report), *extra]
    if mod_root:
        command += ["--eawr-mod-root", mod_root]
    completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    assert report.is_file(), completed.stdout
    return json.loads(report.read_text(encoding="utf-8")), completed
