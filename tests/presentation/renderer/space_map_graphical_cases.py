"""Cases for test_space_map_mode; collected by its legacy facade."""

import xml.etree.ElementTree as ET

from space_map_test_support import (
    ALDERAAN_BLOCKER, ALDERAAN_SPACE, COVERAGE, NONE,
    REFERENCE_MAPS, ROOT, SPACE_STATUSES, SpaceMapRunner,
    _reject_constant, decode_png, fixture, json,
    math, mode_source, os, pathlib,
    read, read_pgm, scene_fixture, source_text,
    strict_json, struct, subprocess, sys,
    tempfile, unittest, zlib,
)


class SpaceGraphicalCases:
    from space_map_material_cases import (
        test_compiled_scene_material_aliases,
        test_compiled_scene_additive2x,
        test_scene_specular_glow_colorize,
        test_scene_skinned_additive_alpha,
        test_scene_compiled_shields,
        test_scene_deathstar,
        test_scene_deathstar_stock_alpha_identity,
    )

    from space_map_environment_cases import (
        test_compiled_environment_material_routes,
        test_sunlit_mc80_shadow_acne,
        test_shadow_report_default_and_debug_ship,
        test_debug_ship_hardpoint_states,
        test_station_markers_attach_no_effects,
        test_supported_population_upload_failure_fails_run_with_placement,
        test_coruscant_attached_effects_tick_in_environment_view,
        test_coruscant_prewarmed_asteroid_density,
        test_coruscant_props_tumble_through_their_idle_clips,
        test_coruscant_nebula_moves_on_the_effect_clock,
        test_coruscant_live_nebula_has_visible_coverage,
        test_other_nebula_backdrops_move_in_lit_views,
        test_planet_map_marker_models_are_not_environment,
        _place_at,
        test_background_authored_space_unit_is_drawn_at_its_pose,
        test_ordinary_space_unit_is_not_a_background_scene_object,
        test_coruscant_default_pose_is_the_tactical_opening_pose,
        test_foc_reference_map_is_default_on_foc_install,
    )

    from space_map_sky_cases import (
        test_synthetic_primary_sky_draws_each_surface_with_its_own_texture,
        test_rigid_parent_sky_moves_in_predeclared_regions_repeatably,
        test_texture_swap_changes_only_that_surface,
        test_negative_controls_invalidate_their_evidence,
        test_unsupported_primary_sky_continues,
        test_missing_tactical_cameras_fails_before_sky,
        test_nonfinite_camera_fails_with_strict_json,
        test_unwritable_requested_artifacts_fail_and_release,
        test_hidden_bone_visibility_fails_closed,
        test_occluder_and_lifecycle_controls_pass,
        test_lit_foreground_excludes_the_sky_from_shadows,
        test_land_path_refuses_space_options,
        test_pinned_space_reference_map_passes_or_names_its_blocker,
    )
