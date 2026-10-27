eawr_add_test_case(
    NAME sim_headless_live_report_tests
    SOURCES sim_headless_live_report_tests.cpp
    LIBS eawr::platform
)

add_test(NAME sim_headless_live_report_contract
    COMMAND sim_headless_live_report_tests "${CMAKE_CURRENT_BINARY_DIR}/sim-headless-live-report.json")
