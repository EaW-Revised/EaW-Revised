eawr_add_galactic_area_case(
    AREA galactic_area_hud_models
    NO_ARGS

    NAME galactic_planet_hud_tests
    SOURCES planet_hud_tests.cpp
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/galactic/planet_hud.cpp"
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::sim_galactic eawr::ui_data
    TESTS galactic_planet_hud_contracts
)

eawr_add_python_contract(python_presentation_renderer_planet_hover_panel_runtime tests.presentation.renderer.test_viewer_planet_hover_panel_runtime)
set_tests_properties(python_presentation_renderer_planet_hover_panel_runtime PROPERTIES LABELS gpu)
