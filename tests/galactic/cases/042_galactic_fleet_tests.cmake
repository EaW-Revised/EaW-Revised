eawr_add_galactic_area_case(
    AREA galactic_area_campaign_economy
    CONST_ARGV

    NAME galactic_fleet_tests
    SOURCES fleet_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_fleet_contracts
)
