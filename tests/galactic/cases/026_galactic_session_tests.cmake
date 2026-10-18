eawr_add_test_case(
    NAME galactic_session_tests
    SOURCES session_tests.cpp
    LIBS eawr::sim_galactic eawr::platform
    TESTS galactic_session_contracts
)
