eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions

    NAME galactic_story_sleep_stock_tests
    SOURCES story_sleep_stock_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
)

eawr_add_game_data_test(NAME galactic_story_sleep_stock COMMAND galactic_area_campaign_transitions galactic_story_sleep_stock_tests "${CMAKE_CURRENT_BINARY_DIR}/story-sleep-stock")
set_tests_properties(galactic_story_sleep_stock PROPERTIES LABELS game-data TIMEOUT 600)
