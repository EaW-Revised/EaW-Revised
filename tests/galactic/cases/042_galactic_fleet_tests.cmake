eawr_add_test_case(
    NAME galactic_fleet_tests
    SOURCES fleet_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_fleet_contracts
)
