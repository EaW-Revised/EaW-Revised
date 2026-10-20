eawr_add_galactic_area_case(
    AREA galactic_area_campaign_economy
    NO_ARGS

    NAME galactic_income_tests
    SOURCES income_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_income_contracts
)
