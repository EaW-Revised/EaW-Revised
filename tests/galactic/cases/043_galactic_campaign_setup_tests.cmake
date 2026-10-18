eawr_add_test_case(
    NAME galactic_campaign_setup_tests
    SOURCES campaign_setup_tests.cpp
    LIBS eawr::campaign eawr::checkpoint eawr::platform
    TESTS galactic_campaign_setup_contracts
)
