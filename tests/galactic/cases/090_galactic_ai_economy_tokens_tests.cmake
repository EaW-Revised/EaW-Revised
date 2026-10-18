eawr_add_test_case(
    NAME galactic_ai_economy_tokens_tests
    SOURCES ai_economy_tokens_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::platform
    TESTS galactic_ai_economy_tokens_contracts
)

set_tests_properties(galactic_ai_economy_tokens_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
eawr_add_game_data_test(NAME galactic_ai_economy_tokens_stock COMMAND galactic_ai_economy_tokens_tests --installed)
set_tests_properties(galactic_ai_economy_tokens_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
