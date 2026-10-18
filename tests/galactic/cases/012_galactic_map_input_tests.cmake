eawr_add_test_case(
    NAME galactic_map_input_tests
    SOURCES map_input_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_view.cpp"
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/galaxy_snapshot_stub.cpp"
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::sim_galactic
    TESTS galactic_map_input_contracts
)

eawr_add_python_contract(python_presentation_renderer_galaxy_input_runtime tests.presentation.renderer.test_viewer_galaxy_input_runtime)
set_tests_properties(python_presentation_renderer_galaxy_input_runtime PROPERTIES LABELS gpu)
