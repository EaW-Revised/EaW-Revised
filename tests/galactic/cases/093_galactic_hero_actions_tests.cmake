eawr_add_galactic_area_case(
    AREA galactic_area_actions
    NO_ARGS

    NAME galactic_hero_actions_tests
    SOURCES hero_actions_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_hero_actions_contracts
)
