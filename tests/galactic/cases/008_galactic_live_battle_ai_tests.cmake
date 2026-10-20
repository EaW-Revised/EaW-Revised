eawr_add_galactic_area_case(
    AREA galactic_area_live_campaign

    NAME galactic_live_battle_ai_tests
    SOURCES live_battle_ai_tests.cpp
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/apps/viewer/src"
    LIBS eawr::campaign eawr::platform eawr::presentation
)

eawr_add_game_data_test(NAME galactic_live_battle_ai_native_off COMMAND galactic_area_live_campaign galactic_live_battle_ai_tests)
set_tests_properties(galactic_live_battle_ai_native_off PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
eawr_add_game_data_test(NAME galactic_live_battle_ai_native_on COMMAND galactic_area_live_campaign galactic_live_battle_ai_tests --galactic-ai-on)
set_tests_properties(galactic_live_battle_ai_native_on PROPERTIES LABELS game-data SKIP_REGULAR_EXPRESSION "SKIPPED:")
