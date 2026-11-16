eawr_add_galactic_area_case(
    AREA galactic_area_ai_observation
    NO_ARGS

    NAME galactic_ai_control_switch_tests
    SOURCES ai_control_switch_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
)

# #6917: the control-switch story reward on the stock campaign, saved and loaded around the window that applies it.
eawr_add_game_data_test(NAME galactic_ai_control_switch_stock COMMAND galactic_area_ai_observation galactic_ai_control_switch_tests)
set_tests_properties(galactic_ai_control_switch_stock PROPERTIES LABELS "game-data;observation" TIMEOUT 3600 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
