eawr_add_test_case(
    NAME galactic_ai_execution_tests
    SOURCES ai_execution_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::script_foc eawr::campaign eawr::platform
    TESTS galactic_ai_execution_contracts
)

eawr_add_game_data_test(NAME galactic_ai_execution_stock COMMAND galactic_ai_execution_tests --installed)
set_tests_properties(galactic_ai_execution_stock PROPERTIES LABELS game-data)

# WGA-227..231/234: direct fleet admission, ordinary hop recomputation and native continuation.
