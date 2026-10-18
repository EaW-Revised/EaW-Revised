eawr_add_test_case(
    NAME galactic_ability_destruction_tests
    SOURCES ability_destruction_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ability_destruction_contracts
)
