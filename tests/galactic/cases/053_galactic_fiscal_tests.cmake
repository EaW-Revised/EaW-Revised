eawr_add_test_case(
    NAME galactic_fiscal_tests
    SOURCES fiscal_tests.cpp
    LIBS eawr::checkpoint eawr::platform
    TESTS galactic_fiscal_contracts
)
