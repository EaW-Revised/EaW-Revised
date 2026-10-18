eawr_add_test_case(
    NAME galactic_ai_funding_live_tests
    SOURCES ai_funding_stock_tests.cpp
    LIBS eawr::script_foc eawr::platform
    TESTS galactic_ai_funding_live_contracts
)

# Six-owner composition is infrastructure; stock runtime/victory remains separate.
