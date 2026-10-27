eawr_add_test_case(
    NAME galactic_scene_tests
    SOURCES galactic_scene_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/scene_content.cpp"
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::presentation eawr::ui_data eawr::ui_model
    TESTS galactic_scene_contracts
)
eawr_add_game_data_test(NAME galactic_scene_faction_colours_stock COMMAND galactic_scene_tests --stock-colours)

eawr_add_python_contract(python_presentation_renderer_galaxy_scene_runtime tests.presentation.renderer.test_viewer_galaxy_scene_runtime)
set_tests_properties(python_presentation_renderer_galaxy_scene_runtime PROPERTIES LABELS gpu)
eawr_add_python_contract(python_presentation_renderer_galaxy_start_runtime tests.presentation.renderer.test_viewer_galaxy_start_runtime)
set_tests_properties(python_presentation_renderer_galaxy_start_runtime PROPERTIES LABELS gpu)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME galactic_viewer_mode_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_galactic_viewer_mode.py"
)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
