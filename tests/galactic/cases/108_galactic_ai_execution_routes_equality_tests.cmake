# #4309: execution route build equals the all-pairs formulation (rows, matrices, refusals) on randomized graphs.
eawr_add_galactic_area_case(
    AREA galactic_area_ai_execution

    NAME galactic_ai_execution_routes_equality_tests
    SOURCES ai_execution_routes_equality_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::platform
    TESTS galactic_ai_execution_routes_equality
)
