eawr_add_test_case(
    NAME galactic_conflict_policy_tests
    SOURCES conflict_policy_tests.cpp
    LIBS eawr::campaign
    TESTS galactic_conflict_policy_contracts
)
