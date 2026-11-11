# Async AI A2.1: the sim->AI inbox keeps every delivery kind in arrival order and its saved block reloads to the same queue (synthetic).
eawr_add_galactic_area_case(
    AREA galactic_area_ai_execution
    NO_ARGS

    NAME galactic_ai_lane_inbox_tests
    SOURCES ai_lane_inbox_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::platform
    TESTS galactic_ai_lane_inbox_contracts
)
