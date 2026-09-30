"""The translation units of each split viewer source, read as one text.

Source-reading contract tests check what a mode does, not which of its files
holds the text, so they read every unit of the mode together.
"""
from __future__ import annotations

import pathlib

SRC = pathlib.Path(__file__).resolve().parents[3] / "apps/viewer/src"

MODE_SOURCES = {
    "camera_input": ("camera_binding_config.cpp", "camera_input.cpp", "camera_input_internal.hpp"),
    "effect_mode": ("effect_mode.cpp", "effect_mode_internal.hpp", "effect_mode_probe.cpp",
                    "effect_mode_report.cpp"),
    "land_look": ("land_look.cpp", "land_look_shaders.hpp"),
    "map_mode": ("map_mode.cpp", "map_mode_hud.cpp", "map_mode_internal.hpp", "map_mode_particles.cpp",
                 "map_mode_report.cpp", "map_mode_scene.cpp"),
    "space_environment": ("space_environment.cpp", "space_environment_fog.cpp", "space_environment_internal.hpp",
                          "space_environment_report.cpp", "space_environment_view.cpp"),
    "viewer_host": ("viewer_host.cpp", "viewer_host_camera.cpp", "viewer_host_exercises.cpp",
                    "viewer_host_internal.hpp", "viewer_host_report.cpp"),
}


def mode_source(mode: str) -> str:
    return "".join((SRC / name).read_text(encoding="utf-8") for name in MODE_SOURCES[mode])
