eawr_add_test_case(
    NAME galactic_hero_actions_tests
    SOURCES hero_actions_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_hero_actions_contracts
)
