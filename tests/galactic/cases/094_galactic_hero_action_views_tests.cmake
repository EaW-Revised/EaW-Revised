eawr_add_galactic_area_case(
    AREA galactic_area_actions
    NO_ARGS

    NAME galactic_hero_action_views_tests
    SOURCES hero_action_views_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_hero_action_views_contracts
)
