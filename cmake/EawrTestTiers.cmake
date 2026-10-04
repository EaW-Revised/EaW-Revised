# CTest tiers (#307, docs/worker-offload.md#test-tiers). Every test carries exactly one tier label:
#   fast      the default: a few seconds at most on a warm build tree
#   slow      about five seconds or more on a warm build tree
#   gpu       has a graphical part that runs on a GPU host when opted in; CTest runs its CPU part
#   ci-tools  the tests of tools/offload, tools/common and tools/rig (tests/ci)
# Iteration (windows_build.py / linux_build.py --iterate) runs every tier but ci-tools, and ci-tools
# too when the change touches the tooling; merge validation runs every test.
#
# Each directory that registers tests ends with eawr_label_tests(); eawr_check_test_tiers() at the
# end of the root list file fails the configure when one does not.

set(EAWR_SLOW_TESTS
    
    python_presentation_godot_qualify_package_runtime
    python_presentation_p1_capture_compare
    python_presentation_p1_capture_migration_pair
    python_validation_p1_capture_build_manifest
    python_validation_p1_capture_windows_prototype_capture
    fidelity_trace_comparator_contracts
    foc_ai_turn_668
    foc_burn_battle
    foc_ai_economy
    foc_plan_battle
    foc_schedule_957
    foc_soak_615
    foc_soak_664
    inventory_contracts
    lua_numeric_sequences
    math_evidence_comparator_contracts
    path_bench_melee_workers
    path_bench_owner_work
    replay_hash_comparator_contracts
    roster_gate_game_data
    scene_inventory_contracts
    sim_boundary_fixtures
    sim_boundary_supplemental
    tactical_formation_contracts
    tactical_replay_contracts
    tag_applied_check_smoke
    tag_coverage_m2_scene
    unit_census_game_data
    vfs_supplemental
)
set(EAWR_GPU_TESTS
    python_presentation_space_environment_effect_graphical
    python_presentation_renderer_atlas_overlay
    python_presentation_renderer_batchmesh_alpha
    python_presentation_renderer_batchmesh_alpha_fog
    python_presentation_renderer_battle_input
    python_presentation_renderer_battle_load
    python_presentation_renderer_camera_contract
    python_presentation_renderer_camera_input
    python_presentation_renderer_capture_size
    python_presentation_renderer_effect_mode
    python_presentation_renderer_effect_mode_capture
    python_presentation_renderer_font_mode
    python_presentation_renderer_hull_preview
    python_presentation_renderer_input_routing
    python_presentation_renderer_live_session
    python_presentation_renderer_map_attached_effects
    python_presentation_renderer_map_camera
    python_presentation_renderer_map_fog
    python_presentation_renderer_map_foliage
    python_presentation_renderer_map_free_camera
    python_presentation_renderer_map_mode
    python_presentation_renderer_map_particles
    python_presentation_renderer_particle_texture_origin
    python_presentation_renderer_render_profile
    python_presentation_renderer_runtime_renderer
    python_presentation_renderer_space_camera
    python_presentation_renderer_space_fog
    python_presentation_renderer_space_map_mode
    python_presentation_renderer_space_meshadditive_sky
    python_presentation_renderer_space_meshgloss_sky
    python_presentation_renderer_tactical_hud
    python_presentation_renderer_ui_gallery
    python_presentation_renderer_ui_movies
    battle_cursor_runtime
    fog_godot_runtime
    fog_renderer_godot_runtime
    hull_asset_shadow_probe_contracts
    legacy_family_runtime
    lighting_renderer_probe_contracts
    renderer_fault_runtime
    renderer_resource_churn_runtime
)

# Labels every test of the calling directory with its tier (appended to labels it already has).
# DEFAULT names the tier of a test that is in neither table (fast unless given).
function(eawr_label_tests)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "DEFAULT" "")
    if(NOT arg_DEFAULT)
        set(arg_DEFAULT fast)
    endif()
    get_property(tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS tests)
        set(tier "${arg_DEFAULT}")
        if(test IN_LIST EAWR_SLOW_TESTS)
            set(tier slow)
        elseif(test IN_LIST EAWR_GPU_TESTS)
            set(tier gpu)
        endif()
        set_property(TEST "${test}" APPEND PROPERTY LABELS "${tier}")
    endforeach()
    set_property(DIRECTORY PROPERTY EAWR_TEST_TIERS_APPLIED TRUE)
endfunction()

function(_eawr_collect_test_directories directory out_unlabelled out_tests)
    set(unlabelled "${${out_unlabelled}}")
    set(all_tests "${${out_tests}}")
    get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
    get_property(applied DIRECTORY "${directory}" PROPERTY EAWR_TEST_TIERS_APPLIED)
    if(tests AND NOT applied)
        list(APPEND unlabelled "${directory}")
    endif()
    list(APPEND all_tests ${tests})
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        _eawr_collect_test_directories("${child}" unlabelled all_tests)
    endforeach()
    set(${out_unlabelled} "${unlabelled}" PARENT_SCOPE)
    set(${out_tests} "${all_tests}" PARENT_SCOPE)
endfunction()

# Root list file, after every add_subdirectory: every directory with tests labelled them.
function(eawr_check_test_tiers)
    set(unlabelled "")
    set(all_tests "")
    _eawr_collect_test_directories("${CMAKE_SOURCE_DIR}" unlabelled all_tests)
    if(unlabelled)
        list(JOIN unlabelled "\n  " listed)
        message(FATAL_ERROR "These directories register tests without a tier label; end them with "
                            "eawr_label_tests() (cmake/EawrTestTiers.cmake):\n  ${listed}")
    endif()
    foreach(test IN LISTS EAWR_SLOW_TESTS EAWR_GPU_TESTS)
        if(NOT test IN_LIST all_tests AND NOT test MATCHES "^sim_boundary_")
            message(FATAL_ERROR "cmake/EawrTestTiers.cmake names ${test}, which no directory registers")
        endif()
    endforeach()
endfunction()
