eawr_add_test_case(
    NAME galactic_campaign_economy_tests
    SOURCES campaign_economy_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
)

add_test(NAME galactic_campaign_economy_contracts COMMAND galactic_campaign_economy_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
