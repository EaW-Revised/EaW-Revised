eawr_add_test_case(
    NAME galactic_corruption_views_tests
    SOURCES corruption_views_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_corruption_views_contracts
)
