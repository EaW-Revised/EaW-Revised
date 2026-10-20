eawr_add_galactic_area_case(
    AREA galactic_area_session
    NO_ARGS

    NAME galactic_clock_tests
    SOURCES clock_tests.cpp
    LIBS eawr::sim_galactic eawr::platform
    TESTS galactic_clock_contracts
)
