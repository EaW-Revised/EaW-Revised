eawr_add_galactic_area_case(
    AREA galactic_area_hud_models
    NO_ARGS

    NAME galactic_story_hud_state_tests
    SOURCES story_hud_state_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::sim_galactic eawr::ui_data
    TESTS galactic_story_hud_state_contracts
)
