eawr_add_test_case(
    NAME galactic_ability_ruins_tests
    SOURCES ability_ruins_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ability_ruins_contracts
)
