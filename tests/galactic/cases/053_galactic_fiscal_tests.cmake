eawr_add_galactic_area_case(
    AREA galactic_area_checkpoint
    NO_ARGS

    NAME galactic_fiscal_tests
    SOURCES fiscal_tests.cpp
    LIBS eawr::checkpoint eawr::platform
    TESTS galactic_fiscal_contracts
)
