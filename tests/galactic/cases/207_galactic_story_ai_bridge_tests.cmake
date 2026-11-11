eawr_add_galactic_area_case(
    AREA galactic_area_campaign_transitions

    NAME galactic_story_ai_bridge_tests
    SOURCES story_ai_bridge_tests.cpp
    LIBS eawr::campaign eawr::platform eawr::checkpoint
)

add_test(NAME galactic_story_ai_bridge_contracts COMMAND galactic_area_campaign_transitions galactic_story_ai_bridge_tests "${CMAKE_CURRENT_BINARY_DIR}/story-ai-bridge")
