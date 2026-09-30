"""Wholly original land map with two distinct, spatially separated particle ALOs."""

from __future__ import annotations

import pathlib
import struct
import sys

import scene_fixture

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
import test_effect_mode as effects  # noqa: E402

MAP_LOGICAL_PATH = scene_fixture.MAP_LOGICAL_PATH
EFFECT_NAMES = ("eawr_map_fx_red.alo", "eawr_map_fx_cyan.alo")
PLACEMENTS = [
    ("EAWR_SCENE_TOWER", (320.0, 240.0, 0.0), (0.0, 0.0, 0.0)),
    ("EAWR_MAP_FX_RED", (160.0, 120.0, 0.0), (0.0, 0.0, 0.0)),
    ("EAWR_MAP_FX_CYAN", (480.0, 360.0, 0.0), (0.0, 0.0, 45.0)),
]


def particle_bytes(name: str, colour: tuple[float, float, float], *,
                   texture: str = "p_synthetic_glow.tga", blend: int = 1,
                   rate: int = 40, heat: bool = False, lifetime: float = 4.0,
                   start_delay: float | None = None, speed: float = 1.0) -> bytes:
    emitter = effects._emitter(
        name, blend=blend, texture=texture,
        position=effects._group(0, point=(8.0, 0.0, 8.0)),
        velocity=effects._group(3, sphere=speed), rgb=colour,
        size=(5.0, 8.0), rate=rate, lifetime=lifetime, heat=heat, start_delay=start_delay)
    root = (effects._chunk(0, effects._text(name))
            + effects._chunk(1, struct.pack("<I", 1))
            + effects._chunk(0x800, emitter, True)
            + effects._chunk(2, b"\x01"))
    return effects._chunk(0x900, root, True)


def write_fixture_root(root: pathlib.Path, *, missing_texture: bool = False,
                       unsupported: bool = False, missing_model: bool = False,
                       inactive: bool = False, fog_compatible: bool = False,
                       blends: tuple[int, int] = (1, 1),
                       placements=None, heat: bool = False) -> pathlib.Path:
    original = (scene_fixture.OBJECTS, scene_fixture.PLACEMENTS,
                scene_fixture.MODELS, scene_fixture.TEXTURES)
    try:
        scene_fixture.OBJECTS = {
            "EAWR_SCENE_TOWER": ("eawr_scene_tower.alo", "1.5"),
            "EAWR_MAP_FX_RED": (EFFECT_NAMES[0], None),
            "EAWR_MAP_FX_CYAN": (EFFECT_NAMES[1], "1.5"),
        }
        scene_fixture.PLACEMENTS = PLACEMENTS if placements is None else placements
        tower = original[2]["eawr_scene_tower.alo"]
        if fog_compatible:
            tower = [("BatchMeshGloss.fx", texture, x, y, height)
                     for _, texture, x, y, height in tower]
        scene_fixture.MODELS = {"eawr_scene_tower.alo": tower}
        scene_fixture.TEXTURES = {
            "eawr_scene_red.dds": original[3]["eawr_scene_red.dds"],
            "eawr_scene_ground.dds": original[3]["eawr_scene_ground.dds"],
        }
        scene_fixture.write_fixture_root(root)
    finally:
        (scene_fixture.OBJECTS, scene_fixture.PLACEMENTS,
         scene_fixture.MODELS, scene_fixture.TEXTURES) = original
    data = root / "GameData" / "Data" / "Art"
    models = data / "Models"
    models.joinpath(EFFECT_NAMES[0]).write_bytes(particle_bytes(
        "eawr-map-red", (1.0, 0.2, 0.1), blend=blends[0]))
    if not missing_model:
        models.joinpath(EFFECT_NAMES[1]).write_bytes(particle_bytes(
            "eawr-map-cyan", (0.1, 0.8, 1.0),
            texture="eawr_missing_glow.tga" if missing_texture else "p_synthetic_glow.tga",
            blend=10 if heat else 5 if unsupported else blends[1],
            heat=heat, rate=0 if inactive else 40))
    (data / "Textures" / "P_Synthetic_Glow.TGA").write_bytes(effects.build_glow())
    return root
