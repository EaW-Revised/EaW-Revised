eawr_add_test_case(
    NAME galactic_ability_theft_tests
    SOURCES ability_theft_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ability_theft_contracts
)
