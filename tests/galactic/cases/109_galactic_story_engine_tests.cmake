eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions

    NAME galactic_story_engine_tests
    SOURCES story_engine_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
)

add_test(NAME galactic_story_engine_contracts COMMAND galactic_area_campaign_transitions galactic_story_engine_tests "${CMAKE_CURRENT_BINARY_DIR}/story-engine")

eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions
    NO_ARGS

    NAME galactic_story_engine_stock_tests
    SOURCES story_engine_stock_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
)

eawr_add_game_data_test(NAME galactic_story_engine_stock COMMAND galactic_area_campaign_transitions galactic_story_engine_stock_tests)
set_tests_properties(galactic_story_engine_stock PROPERTIES LABELS game-data TIMEOUT 1800)
