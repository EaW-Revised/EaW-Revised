eawr_add_test_case(
    NAME galactic_steal_drop_stock_tests
    SOURCES steal_drop_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::presentation
)

eawr_add_game_data_test(NAME galactic_steal_drop_stock COMMAND galactic_steal_drop_stock_tests)
set_tests_properties(galactic_steal_drop_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
