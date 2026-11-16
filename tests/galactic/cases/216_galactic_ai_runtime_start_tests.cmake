eawr_add_galactic_area_case(
    AREA galactic_area_ai_observation
    NO_ARGS

    NAME galactic_ai_runtime_start_tests
    SOURCES ai_runtime_start_stock_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
)

# #7128 regression: a real stock galactic campaign start (tactical AI globals + galactic freestore on one scheduler) must not
# hit a duplicate binding.
eawr_add_game_data_test(NAME galactic_ai_runtime_start_stock COMMAND galactic_area_ai_observation galactic_ai_runtime_start_tests)
set_tests_properties(galactic_ai_runtime_start_stock PROPERTIES LABELS "game-data;observation" TIMEOUT 1200
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")
