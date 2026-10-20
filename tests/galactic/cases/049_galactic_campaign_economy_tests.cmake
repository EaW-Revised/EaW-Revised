eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions
    CONST_ARGV

    NAME galactic_campaign_economy_tests
    SOURCES campaign_economy_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
)

add_test(NAME galactic_campaign_economy_contracts COMMAND galactic_area_campaign_transitions galactic_campaign_economy_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
