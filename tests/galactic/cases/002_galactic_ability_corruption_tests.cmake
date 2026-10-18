eawr_add_test_case(
    NAME galactic_ability_corruption_tests
    SOURCES ability_corruption_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ability_corruption_contracts
)
