eawr_add_galactic_area_case(
    AREA galactic_area_ai_observation
    NO_ARGS

    NAME galactic_ai_synchronized_day_tests
    SOURCES ai_synchronized_day_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/script/foc"
    LIBS eawr::checkpoint eawr::platform
)

eawr_add_game_data_test(NAME galactic_ai_synchronized_day_stock COMMAND galactic_area_ai_observation galactic_ai_synchronized_day_tests)
set_tests_properties(galactic_ai_synchronized_day_stock PROPERTIES LABELS "game-data;observation" TIMEOUT 3600 RUN_SERIAL TRUE
    ENVIRONMENT "EAWR_VERIFY_GALACTIC_HASH=0")

# #3944: seeded idle-human day run of the stock Underworld AI (Underworld is an AI seat in Sandbox_Equal_Footing_Empire).
