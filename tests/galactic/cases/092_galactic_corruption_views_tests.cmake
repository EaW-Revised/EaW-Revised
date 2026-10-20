eawr_add_galactic_area_case(
    AREA galactic_area_actions
    NO_ARGS

    NAME galactic_corruption_views_tests
    SOURCES corruption_views_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_corruption_views_contracts
)
