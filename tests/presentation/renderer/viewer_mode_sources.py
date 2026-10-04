"""The translation units of each split viewer source, read as one text.

Source-reading contract tests check what a mode does, not which of its files
holds the text, so they read every unit of the mode together.
"""
from __future__ import annotations

import pathlib
import re

SRC = pathlib.Path(__file__).resolve().parents[3] / "apps/viewer/src"
ROOT = SRC.parents[2]


class _MultiMemberSource(str):
    """Text whose logical translation unit now spans physical members.

    Membership checks keep their original whole-source scope. Positions across
    members reflect the manifest tuple, not execution order: callers must read
    one physical member before making a positional assertion.
    """

    def __new__(cls, text: str, members: tuple[str, ...]):
        result = super().__new__(cls, text)
        result.members = members
        return result

    def _refuse_position(self, operation: str):
        raise ValueError(f"{operation} requires one physical source member; "
                         f"the bundle spans {', '.join(self.members)}")

    def index(self, *args, **kwargs):
        self._refuse_position("index")

    def find(self, *args, **kwargs):
        self._refuse_position("find")

    def split(self, *args, **kwargs):
        self._refuse_position("split")

    def partition(self, *args, **kwargs):
        self._refuse_position("partition")

    def rindex(self, *args, **kwargs):
        self._refuse_position("rindex")

    def rfind(self, *args, **kwargs):
        self._refuse_position("rfind")

    def rsplit(self, *args, **kwargs):
        self._refuse_position("rsplit")

    def rpartition(self, *args, **kwargs):
        self._refuse_position("rpartition")

    def __getitem__(self, key):
        return _MultiMemberSource(super().__getitem__(key), self.members)

    def __add__(self, other):
        if not isinstance(other, str):
            return NotImplemented
        return _MultiMemberSource(super().__add__(other), self.members + getattr(other, "members", ()))

    def __radd__(self, other):
        if not isinstance(other, str):
            return NotImplemented
        return _MultiMemberSource(other + str(self), getattr(other, "members", ()) + self.members)

    def replace(self, *args, **kwargs):
        return _MultiMemberSource(super().replace(*args, **kwargs), self.members)

    def strip(self, *args, **kwargs):
        return _MultiMemberSource(super().strip(*args, **kwargs), self.members)

    def lstrip(self, *args, **kwargs):
        return _MultiMemberSource(super().lstrip(*args, **kwargs), self.members)

    def rstrip(self, *args, **kwargs):
        return _MultiMemberSource(super().rstrip(*args, **kwargs), self.members)

    def lower(self):
        return _MultiMemberSource(super().lower(), self.members)

    def upper(self):
        return _MultiMemberSource(super().upper(), self.members)

    def casefold(self):
        return _MultiMemberSource(super().casefold(), self.members)

# Keys retain the original reader scope. Production moves extend only the
# corresponding tuple, so a contract never searches unrelated source files.
SOURCE_BUNDLES = {
    "apps/viewer/src/world_ui_view.cpp": (
        "apps/viewer/src/world_ui_view.cpp",
        "apps/viewer/src/world_ui_prepare.cpp",
        "apps/viewer/src/world_ui_groups.cpp",
        "apps/viewer/src/world_ui_bars.cpp",
        "apps/viewer/src/world_ui_report.cpp",
        "apps/viewer/src/world_ui_internal.hpp",
    ),
    "apps/viewer/src/viewer_host_report.cpp": ("apps/viewer/src/viewer_host_report.cpp", "apps/viewer/src/viewer_host_report_scene.cpp"),
    "apps/viewer/src/effect_mode_report.cpp": ("apps/viewer/src/effect_mode_report.cpp", "apps/viewer/src/effect_mode_report_probe.cpp"),
    "apps/viewer/src/space_fog_units.cpp": ("apps/viewer/src/space_fog_units.cpp", "apps/viewer/src/space_fog_units_compose.cpp"),
    "apps/viewer/src/viewer_host_exercises.cpp": ("apps/viewer/src/viewer_host_exercises.cpp", "apps/viewer/src/viewer_host_exercises_renderer.cpp", "apps/viewer/src/viewer_host_exercises_effects.cpp"),
    "apps/viewer/src/ui_gallery_mode.cpp": ("apps/viewer/src/ui_gallery_mode.cpp", "apps/viewer/src/ui_gallery_controls.cpp", "apps/viewer/src/ui_gallery_report.cpp", "apps/viewer/src/ui_gallery_internal.hpp"),
    "apps/viewer/src/camera_binding_config.cpp": ("apps/viewer/src/camera_binding_config.cpp", "apps/viewer/src/camera_binding_tokens.cpp", "apps/viewer/src/camera_binding_values.cpp", "apps/viewer/src/camera_binding_table.cpp", "apps/viewer/src/camera_binding_config_internal.hpp"),
    "apps/viewer/src/battle_input.cpp": (
        "apps/viewer/src/battle_input.cpp",
        "apps/viewer/src/battle_input_cards.cpp",
        "apps/viewer/src/battle_input_commands.cpp",
        "apps/viewer/src/battle_input_pick.cpp",
        "apps/viewer/src/battle_input_production.cpp",
        "apps/viewer/src/battle_input_replay.cpp",
        "apps/viewer/src/battle_input_report.cpp",
    ),
    "apps/viewer/src/live_session_view.cpp": (
        "apps/viewer/src/live_session_view.cpp",
        "apps/viewer/src/live_session_config.cpp",
        "apps/viewer/src/live_session_prepare.cpp",
        "apps/viewer/src/live_session_report.cpp",
        "apps/viewer/src/live_session_death.cpp",
        "apps/viewer/src/live_session_economy.cpp",
        "apps/viewer/src/live_session_fighters.cpp",
        "apps/viewer/src/live_session_frame.cpp",
    ),
    "apps/viewer/src/battle_effects.cpp": (
        "apps/viewer/src/battle_effects.cpp",
        "apps/viewer/src/battle_effects_prepare.cpp",
        "apps/viewer/src/battle_effects_projectiles.cpp",
        "apps/viewer/src/battle_effects_impacts.cpp",
        "apps/viewer/src/battle_effects_report.cpp",
        "apps/viewer/src/battle_effects_internal.hpp",
    ),
    "apps/viewer/src/battle_audio.cpp": (
        "apps/viewer/src/battle_audio.cpp",
        "apps/viewer/src/battle_audio_prepare.cpp",
        "apps/viewer/src/battle_audio_events.cpp",
        "apps/viewer/src/battle_audio_report.cpp",
        "apps/viewer/src/battle_audio_internal.hpp",
    ),
    "apps/viewer/src/unit_emitters.cpp": (
        "apps/viewer/src/unit_emitters.cpp",
        "apps/viewer/src/unit_emitters_prepare.cpp",
        "apps/viewer/src/unit_emitters_frame.cpp",
        "apps/viewer/src/unit_emitters_report.cpp",
        "apps/viewer/src/unit_emitters_internal.hpp",
    ),
    "apps/viewer/src/map_mode.cpp": ("apps/viewer/src/map_mode.cpp", "apps/viewer/src/map_mode_ready.cpp", "apps/viewer/src/map_mode_ready_terrain.cpp", "apps/viewer/src/map_mode_ready_population.cpp",
                                  "apps/viewer/src/map_mode_live.cpp", "apps/viewer/src/map_mode_camera_probe.cpp",
                                  "apps/viewer/src/map_mode_ready_internal.hpp"),
    "apps/viewer/src/map_mode_scene.cpp": ("apps/viewer/src/map_mode_scene.cpp",
                                         "apps/viewer/src/map_mode_placements.cpp",
                                         "apps/viewer/src/map_mode_units.cpp"),
    "apps/viewer/src/map_mode_internal.hpp": ("apps/viewer/src/map_mode_assets.hpp",
                                            "apps/viewer/src/map_mode_types.hpp",
                                            "apps/viewer/src/map_mode_assets.cpp",
                                            "apps/viewer/src/map_mode_internal.hpp"),
    "apps/viewer/src/map_mode_report.cpp": ("apps/viewer/src/map_mode_report.cpp", "apps/viewer/src/map_mode_report_scene.cpp", "apps/viewer/src/map_mode_report_effects.cpp", "apps/viewer/src/map_mode_report_live.cpp"),
    "apps/viewer/src/space_environment.cpp": ("apps/viewer/src/space_environment.cpp", "apps/viewer/src/space_environment_config.cpp", "apps/viewer/src/space_environment_prepare.cpp", "apps/viewer/src/space_environment_probe.cpp"),
    "apps/viewer/src/space_environment_report.cpp": ("apps/viewer/src/space_environment_report.cpp", "apps/viewer/src/space_environment_report_surfaces.cpp"),
    "apps/viewer/src/space_environment_view.cpp": ("apps/viewer/src/space_environment_view.cpp", "apps/viewer/src/space_environment_surfaces.cpp", "apps/viewer/src/space_environment_effects.cpp", "apps/viewer/src/space_environment_internal.hpp"),
    "apps/viewer/src/space_populate.cpp": ("apps/viewer/src/space_populate.cpp", "apps/viewer/src/space_populate_compose.cpp", "apps/viewer/src/space_populate_materials.cpp", "apps/viewer/src/space_populate_animation.cpp", "apps/viewer/src/space_populate_report.cpp", "apps/viewer/src/space_populate_internal.hpp"),
    "apps/viewer/src/viewer_host.cpp": ("apps/viewer/src/viewer_host.cpp", "apps/viewer/src/viewer_host_ready.cpp", "apps/viewer/src/viewer_host_config.cpp", "apps/viewer/src/viewer_host_scene.cpp", "apps/viewer/src/viewer_host_process.cpp"),
    "apps/viewer/src/viewer_host_camera.cpp": ("apps/viewer/src/viewer_host_camera.cpp", "apps/viewer/src/viewer_host_camera_setup.cpp", "apps/viewer/src/viewer_host_camera_probe.cpp", "apps/viewer/src/viewer_host_camera_steps.cpp"),
    "src/presentation/godot/renderer_upload.cpp": (
        "src/presentation/godot/renderer_upload.cpp",
        "src/presentation/godot/renderer_uniforms.cpp",
        "src/presentation/godot/renderer_materials.cpp",
        "src/presentation/godot/renderer_mesh_upload.cpp",
    ),
    "src/presentation/godot/ui/tactical_hud.cpp": (
        "src/presentation/godot/ui/tactical_hud.cpp",
        "src/presentation/godot/ui/tactical_hud_shell.cpp",
        "src/presentation/godot/ui/tactical_hud_build.cpp",
        "src/presentation/godot/ui/tactical_hud_resources.cpp",
        "src/presentation/godot/ui/tactical_hud_report.cpp",
        "src/presentation/godot/ui/tactical_hud_internal.hpp",
    ),
}

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
    return combined_source(*(f"apps/viewer/src/{name}" for name in MODE_SOURCES[mode]))


def combined_source(*paths: str) -> str:
    """Keep established aggregate scopes and propagate a newly split bundle's guard."""
    texts = tuple(source_text(path) for path in paths)
    text = "".join(texts)
    members = tuple(member for path in paths for member in SOURCE_BUNDLES.get(path, (path,)))
    unique_members = tuple(dict.fromkeys(members))
    if len(unique_members) != len(members):
        # A shared private header can belong to both the original mode scope
        # and a newly split member. Its physical text still contributes once.
        text = "".join((ROOT / member).read_text(encoding="utf-8") for member in unique_members)
    if any(isinstance(part, _MultiMemberSource) for part in texts):
        return _MultiMemberSource(text, unique_members)
    return text


def source_text(path: str) -> str:
    """Read a source's explicit move bundle, or the unchanged single file."""
    if path == "apps/viewer/CMakeLists.txt":
        return viewer_build_source()
    members = SOURCE_BUNDLES.get(path, (path,))
    text = "".join((ROOT / name).read_text(encoding="utf-8") for name in members)
    return _MultiMemberSource(text, members) if len(members) > 1 else text


def viewer_build_source() -> str:
    """Read the viewer manifest and only the shared lists it consumes.

    Keep build-membership assertions scoped to the viewer. Root-only lists in
    the same include files are deliberately not part of this source text.
    """
    text = (ROOT / "apps/viewer/CMakeLists.txt").read_text(encoding="utf-8")
    lists = {}
    for name in re.findall(r'include\("\$\{EAWR_ROOT\}/([^"\n]+/sources\.cmake)"\)', text):
        manifest = ROOT / name
        contents = manifest.read_text(encoding="utf-8")
        for variable, body in re.findall(r"set\((EAWR_\w+_SOURCES)\s+(.*?)\)", contents, re.DOTALL):
            paths = re.findall(r'"\$\{CMAKE_CURRENT_LIST_DIR\}/([^"\n]+)"', body)
            lists[variable] = "\n".join((manifest.parent / member).relative_to(ROOT).as_posix()
                                        for member in paths)
    return re.sub(r"\$\{(EAWR_\w+_SOURCES)\}", lambda match: lists[match[1]], text)
