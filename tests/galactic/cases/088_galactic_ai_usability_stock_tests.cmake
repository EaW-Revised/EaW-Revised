eawr_add_test_case(
    NAME galactic_ai_usability_stock_tests
    SOURCES ai_usability_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc" "${PROJECT_SOURCE_DIR}/src/campaign"
    LIBS eawr::campaign eawr::checkpoint eawr::platform
)

eawr_add_game_data_test(NAME galactic_ai_usability_stock COMMAND galactic_ai_usability_stock_tests)
set_tests_properties(galactic_ai_usability_stock PROPERTIES LABELS game-data ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=1")
