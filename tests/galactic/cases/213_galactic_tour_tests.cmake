eawr_add_galactic_area_case(
    AREA galactic_area_hud_models
    NO_ARGS

    NAME galactic_tour_tests
    SOURCES galactic_tour_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::sim_galactic eawr::ui_data
    TESTS galactic_tour_contracts
)
