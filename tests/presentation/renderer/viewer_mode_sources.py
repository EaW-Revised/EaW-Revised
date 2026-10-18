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
    "apps/viewer/src/lan_lobby_screen.cpp": ('apps/viewer/src/lan_lobby_screen.cpp', 'apps/viewer/src/lan_lobby_screen_internal.hpp', 'apps/viewer/src/lan_lobby_screen_list.cpp', 'apps/viewer/src/lan_lobby_screen_chat.cpp', 'apps/viewer/src/lan_lobby_screen_staging.cpp', 'apps/viewer/src/lan_lobby_screen_options.cpp', 'apps/viewer/src/lan_lobby_screen_refresh.cpp'),
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
    "apps/viewer/src/ui_gallery_mode.cpp": ("apps/viewer/src/ui_gallery_mode.cpp", "apps/viewer/src/ui_gallery_controls.cpp", "apps/viewer/src/ui_gallery_box.cpp", "apps/viewer/src/ui_gallery_load_game.cpp", "apps/viewer/src/ui_gallery_credits.cpp", "apps/viewer/src/ui_gallery_report.cpp", "apps/viewer/src/ui_gallery_internal.hpp"),
    "apps/viewer/src/camera_binding_config.cpp": ("apps/viewer/src/camera_binding_config.cpp", "apps/viewer/src/camera_binding_tokens.cpp", "apps/viewer/src/camera_binding_values.cpp", "apps/viewer/src/camera_binding_table.cpp", "apps/viewer/src/camera_binding_config_internal.hpp"),
    "apps/viewer/src/battle_input.cpp": (
        "apps/viewer/src/battle_input.cpp",
        "apps/viewer/src/battle_input_cards.cpp",
        "apps/viewer/src/battle_input_special.cpp",
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
        "apps/viewer/src/skirmish_prepare.cpp",
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
    # Sources the refactor window moves (tickets T6-T14, T16-T21). These entries are identity mappings:
    # each logical path lists its physical members, today just the file itself, so they change no text
    # and only reserve the key. A split PR adds the new files to the tuple, so a test that reads the
    # logical path keeps its scope (test_bundle_members_exist checks the members). Tests read these
    # through source_text(); tools/rig/rig_case_preflight.py (live_session_config.cpp, T13) reads a
    # staged repo copy by literal path and is the T13 worker's to convert.
    "apps/viewer/src/live_session_view.hpp": (
        "apps/viewer/src/live_session_view.hpp",
        "apps/viewer/src/live_session_options.hpp",
        "apps/viewer/src/live_session_slots.hpp",
        "apps/viewer/src/live_session_economy_view.hpp",
        "apps/viewer/src/live_session_lan.hpp",
        "apps/viewer/src/live_session_report.hpp",
    ),
    "apps/viewer/src/live_session_prepare.cpp": ("apps/viewer/src/live_session_prepare.cpp",),
    "apps/viewer/src/live_session_config.cpp": ("apps/viewer/src/live_session_config.cpp",),
    "apps/viewer/src/live_session_fighters.cpp": ("apps/viewer/src/live_session_fighters.cpp",),
    "apps/viewer/src/live_session_report.cpp": ("apps/viewer/src/live_session_report.cpp",),
    "apps/viewer/src/live_session_frame.cpp": ("apps/viewer/src/live_session_frame.cpp",),
    "apps/viewer/src/space_populate_compose.cpp": ("apps/viewer/src/space_populate_compose.cpp",),
    "apps/viewer/src/lan_match.cpp": ("apps/viewer/src/lan_match.cpp",),
    "apps/viewer/src/lan_match.hpp": ("apps/viewer/src/lan_match.hpp",),
    "src/sim/tactical/session_step_commands.cpp": ("src/sim/tactical/session_step_commands.cpp", "src/sim/tactical/session_cmd_orders.cpp", "src/sim/tactical/session_cmd_lanes.cpp", "src/sim/tactical/session_cmd_economy.cpp", "src/sim/tactical/session_cmd_abilities.cpp", "src/sim/tactical/session_cmd_targeted.cpp"),
    "src/sim/tactical/session_step_fighters.cpp": ("src/sim/tactical/session_step_fighters.cpp",),
    "src/sim/tactical/session_state.cpp": (
        "src/sim/tactical/session_state.cpp",
        "src/sim/tactical/session_state_units.cpp",
        "src/sim/tactical/session_state_economy.cpp",
        "src/sim/tactical/session_state_combat.cpp",
        "src/sim/tactical/session_state_squadrons.cpp",
        "src/sim/tactical/session_state_fog.cpp",
        "src/sim/tactical/session_state_abilities.cpp",
        "src/sim/tactical/session_state_paths.cpp",
        "src/sim/tactical/session_state_world.cpp",
        "src/sim/tactical/session_state_players.cpp",
    ),
    "src/sim/tactical/session_impl.hpp": (
        "src/sim/tactical/session_impl.hpp",
        "src/sim/tactical/session_store_outcome.hpp",
        "src/sim/tactical/session_store_abilities.hpp",
        "src/sim/tactical/session_store_bonuses.hpp",
        "src/sim/tactical/session_store_diagnostics.hpp",
        "src/sim/tactical/session_store_economy.hpp",
        "src/sim/tactical/session_store_rules.hpp",
        "src/sim/tactical/session_store_environment.hpp",
        "src/sim/tactical/session_store_healing.hpp",
        "src/sim/tactical/session_store_publication.hpp",
        "src/sim/tactical/session_store_squadrons.hpp",
        "src/sim/tactical/session_store_visibility.hpp",
        "src/sim/tactical/session_store_reinforcements.hpp",
        "src/sim/tactical/session_store_paths.hpp",
        "src/sim/tactical/session_store_projectiles.hpp",
        "src/sim/tactical/session_store_station_spawn.hpp",
        "src/sim/tactical/session_store_registry.hpp",
        "src/sim/tactical/session_store_commands.hpp",
    ),
    "src/sim/tactical/replay_parse.cpp": ("src/sim/tactical/replay_parse.cpp", "src/sim/tactical/replay_parse_header.cpp", "src/sim/tactical/replay_parse_setup.cpp", "src/sim/tactical/replay_parse_records.cpp", "src/sim/tactical/replay_parse_internal.hpp"),
    "include/eawr/units/unit_tables.hpp": ("include/eawr/units/unit_tables.hpp", "include/eawr/units/unit_tables_common.hpp", "include/eawr/units/unit_tables_combat.hpp", "include/eawr/units/unit_tables_abilities.hpp", "include/eawr/units/unit_tables_spawner.hpp", "include/eawr/units/unit_tables_galactic.hpp", "include/eawr/units/unit_tables_presentation.hpp"),
    "src/units/unit_tables_profiles.cpp": (
        "src/units/unit_tables_profiles.cpp",
        "src/units/unit_tables_abilities_nested.cpp",
        "src/units/unit_tables_abilities_special.cpp",
        "src/units/unit_tables_abilities_bonuses.cpp",
    ),
    "src/units/unit_motion.cpp": ("src/units/unit_motion.cpp",),
    "include/eawr/campaign/game_session.hpp": ("include/eawr/campaign/game_session.hpp",),
    "src/campaign/game_session.cpp": ("src/campaign/game_session.cpp",),
    "src/campaign/live_setup.cpp": ("src/campaign/live_setup.cpp",),
    "src/campaign/auto_resolve_live.cpp": ("src/campaign/auto_resolve_live.cpp",),
    "src/campaign/galactic_ai_runtime.cpp": ("src/campaign/galactic_ai_runtime.cpp",),
    "src/campaign/space_entry.cpp": ("src/campaign/space_entry.cpp",),
    "src/campaign/space_entry_stock.cpp": ("src/campaign/space_entry_stock.cpp",),
    "src/campaign/conflict_policy.cpp": ("src/campaign/conflict_policy.cpp",),
}

MODE_SOURCES = {
    "camera_input": ("camera_binding_config.cpp", "camera_input.cpp", "camera_input_internal.hpp"),
    "effect_mode": ("effect_mode.cpp", "effect_mode_internal.hpp", "effect_mode_probe.cpp",
                    "effect_mode_report.cpp"),
    "galactic_mode": ("galactic/galactic_mode.cpp", "galactic/galactic_mode_internal.hpp",
                      "galactic/galactic_mode_seats.hpp",
                      "galactic/galactic_scene.cpp", "galactic/galactic_report.cpp",
                      "galactic/galaxy_view.cpp", "galactic/galaxy_snapshot_stub.cpp",
                      "galactic/scene_content.cpp", "galactic/galactic_layers.cpp",
                      "galactic/route_lines.cpp", "galactic/galactic_routes.cpp",
                      "galactic/planet_hud.cpp", "galactic/galactic_hud.cpp",
                      "galactic/galactic_fleet_hud.cpp", "galactic/fleet_hud_fixture.hpp",
                      "galactic/orbit_hud.cpp", "galactic/orbit_hud.hpp", "galactic/orbit_hud_fixture.hpp",
                      "galactic/radar_hud.cpp", "galactic/radar_hud.hpp", "galactic/galactic_radar_hud.cpp",
                      "galactic/galactic_save.cpp", "galactic/galactic_save.hpp",
                      "galactic/galactic_handoff.cpp", "galactic/galactic_retreat.cpp"),
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
        contents = re.sub(r"#[^\n]*", "", manifest.read_text(encoding="utf-8"))
        for variable, body in re.findall(r"set\((EAWR_\w+_SOURCES)\s+(.*?)\)", contents, re.DOTALL):
            # Generated at configure time, with no repository source text to read.
            # The reader has always omitted this build-identity translation unit.
            body = body.replace('"${EAWR_BUILD_IDENTITY_CPP}"', "")
            token = r'"\$\{CMAKE_CURRENT_LIST_DIR\}/([^"\n]+)"|\$\{(EAWR_\w+_SOURCES)\}'
            if re.sub(token, "", body).strip():
                raise ValueError(f"Unsupported source expression in {name}: {variable}")
            lists[variable] = "\n".join(
                (manifest.parent / member).relative_to(ROOT).as_posix() if member
                else "${" + nested + "}"
                for member, nested in re.findall(token, body))

    def expand(contents: str) -> str:
        return re.sub(r"\$\{(EAWR_\w+_SOURCES)\}",
                      lambda match: expand(lists[match[1]]), contents)

    return expand(text)
