eawr_add_test_case(
    NAME galactic_hero_action_views_tests
    SOURCES hero_action_views_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_hero_action_views_contracts
)
