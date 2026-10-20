eawr_add_galactic_area_case(
    AREA galactic_area_session
    NO_ARGS

    NAME galactic_session_tests
    SOURCES session_tests.cpp
    LIBS eawr::sim_galactic eawr::platform
    TESTS galactic_session_contracts
)
