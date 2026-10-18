eawr_add_test_case(
    NAME galactic_clock_input_tests
    SOURCES clock_input_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    TESTS galactic_clock_input_contracts
)
