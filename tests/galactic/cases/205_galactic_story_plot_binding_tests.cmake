eawr_add_galactic_area_case(
    AREA galactic_area_ai_movement

    NAME galactic_story_plot_binding_tests
    SOURCES story_plot_binding_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_story_plot_binding_contracts
)
