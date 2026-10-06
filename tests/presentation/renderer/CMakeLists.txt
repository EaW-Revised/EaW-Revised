find_package(Threads REQUIRED)
add_executable(eawr_renderer_contracts
    main.cpp
    renderer_resource_tests.cpp
    renderer_submission_tests.cpp
    renderer_surface_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/presentation/godot/renderer_contract.cpp"
)
target_include_directories(eawr_renderer_contracts PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/presentation/godot"
)
target_link_libraries(eawr_renderer_contracts PRIVATE eawr::core eawr::sim Threads::Threads)
target_compile_features(eawr_renderer_contracts PRIVATE cxx_std_20)
eawr_target_project_warnings(eawr_renderer_contracts)
add_test(NAME renderer_contracts COMMAND eawr_renderer_contracts)

# WP-43 legacy/ family adapters (src/presentation/godot/legacy/).
add_executable(eawr_legacy_family_contracts
    legacy_family_tests.cpp
    legacy_registry_tests.cpp
    legacy_bindings_tests.cpp
    legacy_colorize_tests.cpp
    legacy_dx8_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/presentation/godot/renderer_contract.cpp"
)
target_include_directories(eawr_legacy_family_contracts PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/presentation/godot"
)
target_link_libraries(eawr_legacy_family_contracts PRIVATE eawr::core eawr::sim)
target_compile_features(eawr_legacy_family_contracts PRIVATE cxx_std_20)
eawr_target_project_warnings(eawr_legacy_family_contracts)
add_test(NAME legacy_family_contracts COMMAND eawr_legacy_family_contracts)

# The production GodotRenderer resource-churn probe (tests/presentation/
# renderer/churn) is built separately against the pinned godot-cpp cache; its
# report verifier always runs, its graphical run skips unless
# EAWR_GODOT_RESOURCE_CHURN_RUNTIME_TEST is set.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(NAME interdictor_visibility_gpu_contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_interdictor_visibility.py")
add_test(NAME skirmish_setup_gpu_contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_skirmish_setup.py")
add_test(
    NAME viewer_profile_argument_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_profile_argument.py"
)
add_test(
    NAME renderer_resource_churn_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_resource_churn_runtime.py"
)
# The WP-08 renderer fault probe (tests/presentation/renderer/faults) is built
# the same way. Its report and engine-output verifiers always run; the Godot
# runs skip unless EAWR_GODOT_RENDERER_FAULT_RUNTIME_TEST is set.
add_test(
    NAME renderer_fault_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_renderer_fault_runtime.py"
)

# The WP-43 legacy/ family probe (tests/presentation/renderer/legacy) is built
# the same way; its report verifier always runs, its graphical run skips
# unless EAWR_GODOT_LEGACY_FAMILY_RUNTIME_TEST is set.
add_test(
    NAME legacy_family_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_legacy_family_runtime.py"
)

add_executable(eawr_map_fog_contracts
    fog_mode_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/fog_mode.cpp"
)
target_include_directories(eawr_map_fog_contracts PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(eawr_map_fog_contracts PRIVATE eawr::core eawr::sim)
target_compile_features(eawr_map_fog_contracts PRIVATE cxx_std_20)
target_compile_definitions(eawr_map_fog_contracts PRIVATE
    EAWR_REPLAY_FIXTURE_PATH="${PROJECT_SOURCE_DIR}/tests/replay/fixtures/original-v1.eawr-replay")
eawr_target_project_warnings(eawr_map_fog_contracts)
add_test(NAME map_fog_contracts COMMAND eawr_map_fog_contracts)

add_executable(eawr_render_profile_contracts render_profile_tests.cpp)
target_include_directories(eawr_render_profile_contracts PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
target_compile_features(eawr_render_profile_contracts PRIVATE cxx_std_20)
eawr_target_project_warnings(eawr_render_profile_contracts)
add_test(NAME render_profile_contracts COMMAND eawr_render_profile_contracts)

add_executable(eawr_space_fog_contracts
    space_fog_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/viewer/src/space_fog.cpp"
)
target_include_directories(eawr_space_fog_contracts PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(eawr_space_fog_contracts PRIVATE eawr::scene eawr::sim eawr::core)
target_compile_features(eawr_space_fog_contracts PRIVATE cxx_std_20)
eawr_target_project_warnings(eawr_space_fog_contracts)
add_test(NAME space_fog_contracts COMMAND eawr_space_fog_contracts)

# Tier labels (cmake/EawrTestTiers.cmake).
add_test(NAME battle_cursor_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_battle_cursors.py")

# Each file runs its CPU contracts; graphical exercises retain their opt-in gates.
eawr_add_python_contract(python_presentation_renderer_projectile_expiry tests.presentation.renderer.test_projectile_expiry)
eawr_add_python_contract(python_presentation_renderer_atlas_overlay tests.presentation.renderer.test_atlas_overlay)
eawr_add_python_contract(python_presentation_renderer_asteroid_orders tests.presentation.renderer.test_asteroid_orders)
eawr_add_python_contract(python_presentation_renderer_batchmesh_alpha tests.presentation.renderer.test_batchmesh_alpha)
eawr_add_python_contract(python_presentation_renderer_batchmesh_alpha_fog tests.presentation.renderer.test_batchmesh_alpha_fog)
eawr_add_python_contract(python_presentation_renderer_battle_input tests.presentation.renderer.test_battle_input)
eawr_add_python_contract(python_presentation_renderer_battle_load tests.presentation.renderer.test_battle_load)
eawr_add_python_contract(python_presentation_renderer_camera_contract tests.presentation.renderer.test_camera_contract)
eawr_add_python_contract(python_presentation_renderer_camera_input tests.presentation.renderer.test_camera_input)
eawr_add_python_contract(python_presentation_renderer_capture_size tests.presentation.renderer.test_capture_size)
eawr_add_python_contract(python_presentation_renderer_carrier_arrival tests.presentation.renderer.test_carrier_arrival)
eawr_add_python_contract(python_presentation_renderer_effect_mode tests.presentation.renderer.test_effect_mode)
eawr_add_python_contract(python_presentation_renderer_effect_mode_capture tests.presentation.renderer.test_effect_mode_capture)
eawr_add_python_contract(python_presentation_renderer_font_mode tests.presentation.renderer.test_font_mode)
eawr_add_python_contract(python_presentation_renderer_hull_preview tests.presentation.renderer.test_hull_preview NEEDS PIL)
eawr_add_python_contract(python_presentation_renderer_hero_squadrons tests.presentation.renderer.test_hero_squadrons)
eawr_add_python_contract(python_presentation_renderer_hyperspace_icons tests.presentation.renderer.test_hyperspace_icons)
eawr_add_python_contract(python_presentation_renderer_input_routing tests.presentation.renderer.test_input_routing)
eawr_add_python_contract(python_presentation_renderer_live_session tests.presentation.renderer.test_live_session)
eawr_add_python_contract(python_presentation_renderer_battle_music tests.presentation.renderer.test_battle_music)
eawr_add_python_contract(python_presentation_renderer_map_attached_effects tests.presentation.renderer.test_map_attached_effects)
eawr_add_python_contract(python_presentation_renderer_map_camera tests.presentation.renderer.test_map_camera NEEDS PIL)
eawr_add_python_contract(python_presentation_renderer_map_fog tests.presentation.renderer.test_map_fog)
eawr_add_python_contract(python_presentation_renderer_map_foliage tests.presentation.renderer.test_map_foliage)
eawr_add_python_contract(python_presentation_renderer_map_free_camera tests.presentation.renderer.test_map_free_camera NEEDS PIL)
eawr_add_python_contract(python_presentation_renderer_map_mode tests.presentation.renderer.test_map_mode)
eawr_add_python_contract(python_presentation_renderer_game_io_failures tests.presentation.renderer.test_game_io_failures)
eawr_add_python_contract(python_presentation_renderer_map_particles tests.presentation.renderer.test_map_particles)
eawr_add_python_contract(python_presentation_renderer_pad_capture tests.presentation.renderer.test_pad_capture)
eawr_add_python_contract(python_presentation_renderer_merchant_purchase tests.presentation.renderer.test_merchant_purchase)
eawr_add_python_contract(python_presentation_renderer_particle_texture_origin tests.presentation.renderer.test_particle_texture_origin)
eawr_add_python_contract(python_presentation_renderer_production_contract tests.presentation.renderer.test_production_contract)
eawr_add_python_contract(python_presentation_renderer_render_profile tests.presentation.renderer.test_render_profile NEEDS PIL)
eawr_add_python_contract(python_presentation_renderer_replay_selected_factions tests.presentation.renderer.test_replay_selected_factions)
eawr_add_python_contract(python_presentation_renderer_runtime_renderer tests.presentation.renderer.test_runtime_renderer)
eawr_add_python_contract(python_presentation_renderer_space_camera tests.presentation.renderer.test_space_camera NEEDS PIL)
eawr_add_python_contract(python_presentation_renderer_space_fog tests.presentation.renderer.test_space_fog)
eawr_add_python_contract(python_presentation_renderer_space_map_mode tests.presentation.renderer.test_space_map_mode)
eawr_add_python_contract(python_presentation_renderer_space_meshadditive_sky tests.presentation.renderer.test_space_meshadditive_sky)
eawr_add_python_contract(python_presentation_renderer_space_meshgloss_sky tests.presentation.renderer.test_space_meshgloss_sky)
eawr_add_python_contract(python_presentation_renderer_tactical_hud tests.presentation.renderer.test_tactical_hud)
eawr_add_python_contract(python_presentation_renderer_ui_gallery tests.presentation.renderer.test_ui_gallery)
eawr_add_python_contract(python_presentation_renderer_ui_movies tests.presentation.renderer.test_ui_movies)
eawr_add_python_contract(python_presentation_renderer_viewport_aspect tests.presentation.renderer.test_viewport_aspect)

eawr_label_tests()
