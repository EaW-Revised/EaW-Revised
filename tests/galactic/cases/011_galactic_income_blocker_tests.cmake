eawr_add_test_case(
    NAME galactic_income_blocker_tests
    SOURCES income_blocker_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_income_blocker_contracts
)
