eawr_add_galactic_area_case(
    AREA galactic_area_campaign_economy
    CONST_ARGV

    NAME galactic_campaign_setup_tests
    SOURCES campaign_setup_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_campaign_setup_contracts
)
