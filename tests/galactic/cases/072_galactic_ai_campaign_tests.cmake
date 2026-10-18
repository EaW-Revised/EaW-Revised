eawr_add_test_case(
    NAME galactic_ai_campaign_tests
    SOURCES ai_campaign_stock_tests.cpp
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
    TESTS galactic_ai_campaign_runtime_contracts
)
