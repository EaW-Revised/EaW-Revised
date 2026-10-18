eawr_add_test_case(
    NAME galactic_clock_tests
    SOURCES clock_tests.cpp
    LIBS eawr::sim_galactic eawr::platform
    TESTS galactic_clock_contracts
)
