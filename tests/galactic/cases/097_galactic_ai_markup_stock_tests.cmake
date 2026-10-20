eawr_add_galactic_area_case(
    AREA galactic_area_ai_access

    NAME galactic_ai_markup_stock_tests
    SOURCES ai_markup_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::campaign eawr::checkpoint eawr::platform
)

eawr_add_game_data_test(NAME galactic_ai_markup_stock COMMAND galactic_area_ai_access galactic_ai_markup_stock_tests --installed "${CMAKE_CURRENT_BINARY_DIR}/markup-stock")
set_tests_properties(galactic_ai_markup_stock PROPERTIES LABELS game-data TIMEOUT 1800)
eawr_add_game_data_test(NAME galactic_ai_markup_drift_stock COMMAND galactic_area_ai_access galactic_ai_markup_stock_tests --drift "${CMAKE_CURRENT_BINARY_DIR}/markup-drift" 27000)
set_tests_properties(galactic_ai_markup_drift_stock PROPERTIES LABELS "game-data;release-gate" TIMEOUT 14400 RUN_SERIAL TRUE)
set_tests_properties(python_presentation_renderer_galactic_hero_actions_runtime PROPERTIES LABELS gpu)
