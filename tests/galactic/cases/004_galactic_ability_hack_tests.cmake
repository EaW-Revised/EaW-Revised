eawr_add_test_case(
    NAME galactic_ability_hack_tests
    SOURCES ability_hack_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ability_hack_contracts
)
