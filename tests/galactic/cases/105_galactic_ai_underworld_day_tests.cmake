eawr_add_galactic_area_case(
    AREA galactic_area_ai_observation

    NAME galactic_ai_underworld_day_tests
    SOURCES ai_underworld_day_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
)

eawr_add_game_data_test(NAME galactic_ai_underworld_day_stock COMMAND galactic_area_ai_observation galactic_ai_underworld_day_tests 5400)
eawr_add_game_data_test(NAME galactic_ai_underworld_day_workers COMMAND galactic_area_ai_observation galactic_ai_underworld_day_tests --equality 1800)
set_tests_properties(galactic_ai_underworld_day_stock galactic_ai_underworld_day_workers PROPERTIES LABELS "game-data;observation"
    TIMEOUT 14400 RUN_SERIAL TRUE ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
