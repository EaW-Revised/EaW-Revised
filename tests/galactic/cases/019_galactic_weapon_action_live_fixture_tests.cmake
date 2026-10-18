eawr_add_test_case(
    NAME galactic_weapon_action_live_fixture_tests
    SOURCES weapon_action_live_fixture_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_view.cpp"
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_snapshot_stub.cpp"
    LIBS eawr::campaign eawr::sim_galactic eawr::platform
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    TESTS galactic_weapon_action_live_fixture
)

eawr_add_python_contract(python_presentation_renderer_galactic_fleet_runtime tests.presentation.renderer.test_viewer_galactic_fleet_runtime)
set_tests_properties(python_presentation_renderer_galactic_fleet_runtime PROPERTIES LABELS gpu)

eawr_add_python_contract(python_presentation_renderer_galactic_radar_runtime tests.presentation.renderer.test_viewer_galactic_radar_runtime)
set_tests_properties(python_presentation_renderer_galactic_radar_runtime PROPERTIES LABELS gpu)

eawr_add_python_contract(python_presentation_renderer_command_bar_offsets_runtime tests.presentation.renderer.test_viewer_command_bar_offsets_runtime)
set_tests_properties(python_presentation_renderer_command_bar_offsets_runtime PROPERTIES LABELS gpu)
