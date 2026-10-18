eawr_add_test_case(
    NAME galactic_fleet_intelligence_tests
    SOURCES fleet_intelligence_tests.cpp
    LIBS eawr::sim_galactic eawr::platform
    TESTS galactic_fleet_intelligence
)
