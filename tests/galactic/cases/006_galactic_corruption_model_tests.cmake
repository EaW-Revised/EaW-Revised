eawr_add_test_case(
    NAME galactic_corruption_model_tests
    SOURCES corruption_model_tests.cpp
    LIBS eawr::sim_galactic
    TESTS galactic_corruption_model_contracts
)
