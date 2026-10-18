eawr_add_test_case(
    NAME galactic_uninvolved_conflict_ui_tests
    SOURCES uninvolved_conflict_ui_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::presentation
)

eawr_add_game_data_test(NAME galactic_uninvolved_conflict_ui_stock COMMAND galactic_uninvolved_conflict_ui_tests 12500)
set_tests_properties(galactic_uninvolved_conflict_ui_stock PROPERTIES LABELS game-data TIMEOUT 7200 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
