eawr_add_galactic_area_case(
    AREA galactic_area_session
    NO_ARGS

    NAME galactic_fleet_intelligence_tests
    SOURCES fleet_intelligence_tests.cpp
    LIBS eawr::sim_galactic eawr::platform
    TESTS galactic_fleet_intelligence
)
