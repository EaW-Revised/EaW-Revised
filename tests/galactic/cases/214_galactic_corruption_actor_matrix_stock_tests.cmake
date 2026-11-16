eawr_add_test_case(
    NAME galactic_corruption_actor_matrix_stock_tests
    SOURCES corruption_actor_matrix_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::presentation
)

eawr_add_game_data_test(NAME galactic_corruption_actor_matrix_stock COMMAND galactic_corruption_actor_matrix_stock_tests)
set_tests_properties(galactic_corruption_actor_matrix_stock PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
