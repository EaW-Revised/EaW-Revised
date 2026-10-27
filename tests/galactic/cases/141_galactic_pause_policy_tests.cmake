eawr_add_test_case(
    NAME galactic_pause_policy_tests
    SOURCES pause_policy_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
)
add_test(NAME galactic_pause_policy_contracts COMMAND galactic_pause_policy_tests)
