eawr_add_test_case(
    NAME galactic_corruption_actions_tests
    SOURCES corruption_actions_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_corruption_actions_contracts
)
