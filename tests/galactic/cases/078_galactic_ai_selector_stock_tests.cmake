eawr_add_test_case(
    NAME galactic_ai_selector_stock_tests
    SOURCES ai_selector_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::platform
)

eawr_add_game_data_test(NAME galactic_ai_selector_stock COMMAND galactic_ai_selector_stock_tests)
set_tests_properties(galactic_ai_selector_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
eawr_add_game_data_test(NAME galactic_ai_selector_normal_stock COMMAND galactic_ai_selector_stock_tests --normal)
set_tests_properties(galactic_ai_selector_normal_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
set_tests_properties(galactic_ai_selector_runtime_contracts PROPERTIES ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
