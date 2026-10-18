eawr_add_test_case(
    NAME galactic_replay_tests
    SOURCES replay_tests.cpp
    LIBS eawr::sim_galactic eawr::platform
    TESTS galactic_replay_contracts
)
