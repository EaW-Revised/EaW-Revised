eawr_add_test_case(
    NAME galactic_income_tests
    SOURCES income_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_income_contracts
)
