eawr_add_test_case(
    NAME galactic_ability_host_tests
    SOURCES ability_host_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_ability_host_contracts
)
