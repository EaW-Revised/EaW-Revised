eawr_add_galactic_area_case(
    AREA galactic_area_campaign_economy
    NO_ARGS

    NAME galactic_remake_tolerance_tests
    SOURCES remake_tolerance_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_remake_tolerance_contracts
)
